# Design note: SVN through a Git mirror that UAM owns

**Status:** design and throwaway spike for [issue #521](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/521). Nothing here is implemented in UAM, and nothing here is accepted. [David's decisions of 2026-11-23](../documentation-review.md#svn-through-git) set the frame: UAM builds and owns the Git mirror (no `git svn`, so no Perl SVN bindings), pulling SVN changes is always explicit and one-way, and a new SVN branch copies from the working copy's URL and revision by default, with an editable message UAM writes ([#520 decision](../documentation-review.md#svn-branch-creation)). The spike script is [`tools/svn-git-mirror-spike/spike.py`](../../tools/svn-git-mirror-spike/spike.py); its results are [below](#spike-results).

## What the person and the agents see

Turning the mode on for an SVN workspace gives each chat an ordinary Git worktree on its own branch. Agents use plain Git: status, diff, commit, branch, rebase. Two UAM actions cross over to SVN, and only when the person asks (whether agents may ask too is an [open question](#open-questions)):

- **Pull from SVN** brings new SVN revisions into the mirror. It never writes to SVN and never touches a chat's branch, worktree or uncommitted files.
- **Publish to SVN** turns a branch's new Git commits into SVN commits, one revision per commit, in order, with the original messages. A new Git branch is first created as an SVN branch with `svn copy`. Publishing ends with a pull of the revisions it wrote; if someone else committed in between, that pull also brings in their revisions, as part of the publish the person asked for.

The person's own SVN checkout is never modified by either action. It stays at its revision with its local edits until the person updates it with the existing commit panel **Update**.

## Layout

| Part | Where | Purpose |
|---|---|---|
| Mirror repository | `<data-root>/svn-mirrors/<repository UUID>/mirror.git` (bare) | Holds every imported revision and every chat branch. Not inside the SVN checkout: a `.git` there would make [`FindNearestVcsRoot`](../architecture/vcs-boundaries.md) treat the workspace as Git. |
| Bridge checkout | `<mirror>/bridges/<hash of the SVN path>` | A private SVN checkout of one repository path (trunk, or one branch) that is also a work tree of the mirror with its own index. Only UAM uses it, so it never holds anyone's uncommitted work. Pull updates it; publish replays commits in it. A hash names it, so two SVN paths can never share one. |
| Upstream ref | `refs/uam-svn/upstream/<SVN path>` | One Git commit per SVN revision that changed that path. Each path segment is percent-encoded (`branches/chat%20branchy%201%2E0`), because SVN allows names Git refs do not. Chat branches start from it and rebase onto it. |
| Revision map | `<mirror>/revmap.jsonl`, append-only, synced per line | The authority: SVN revision ↔ Git commit, per path, and which Git commit a published revision came from. |
| State file | `<mirror>/state.json`, replaced atomically | The last revision each path has been checked up to, including revisions that did not change it. |
| Publish journal | `<mirror>/publish-journal.json`, replaced atomically | The commit being published, its parent, the revision the bridge is at and the identity of the running `svn commit` (its process id with its start time, since an id alone can be reused), written before the commit and removed after the mirror has recorded the result. |
| Ignore rules | `mirror.git/info/exclude` | SVN's ignore rules as Git patterns, shared by every worktree and never published. |

Every imported revision is written in one order: the Git commit object, the revision-map line, the upstream ref (with an old-value check), then the state file. Before every pull and publish, UAM compares each upstream ref with the last revision-map entry for its path. A ref one step behind, or missing when the path has only its first entry (a stop while the path was being attached), is the trace of a stop between the map line and the ref, and is moved forward; a missing state entry is rebuilt from the map. Any other difference means something else moved the ref, for example an agent's `git update-ref` in its worktree, which shares the repository; UAM then refuses with one line ("The mirror of trunk was changed outside UAM, so nothing was pulled or published.") rather than trusting it, because a ref moved onto a chat's branch would make its commits look already published.

One mirror operation (pull, publish, branch creation) runs at a time per mirror, under a lock owned like the existing workspace leases. Chat worktrees stay usable throughout, because none of these operations writes to them.

## Creating the mirror

The mirror starts at the checkout's own revision, not at HEAD, so the chat sees what the person sees. The first commit is the repository content at that revision, not the checkout's working files:

- **Exactly SVN's versioned files.** The bridge's file list comes from `svn info -R`, not from ignore rules, so a versioned file that matches an ignore pattern (a committed `keep.o`) is included and unversioned or ignored files are not.
- **Keywords unexpanded.** An `svn:keywords` file is staged as the repository stores it (`$Id$`), otherwise every revision would change `$Id: … $` lines in Git and cause needless rebase conflicts. SVN still treats the contracted bridge file as unmodified.
- **The person's uncommitted SVN edits stay in their checkout.** They are not imported. Carrying them into a chat as uncommitted changes, as the managed snapshot does today, is an [open question](#open-questions).

History before the checkout's revision is not imported; Git log starts there. Each later revision becomes its own commit, authored as the SVN user (`name <name@repository-UUID>`) at the revision's date, with the SVN message followed by `UAM-SVN-Revision:` and `UAM-SVN-Path:` trailers.

For a workspace with this mode on, the mirror replaces the [managed local snapshot](../settings/version-control/local-snapshots.md): chats get mirror worktrees instead of snapshot copies.

## Ignore rules

SVN matches ignore patterns against an item's name only. The translation into `info/exclude` is:

| SVN rule | Git exclude line | Covers |
|---|---|---|
| `svn:ignore` pattern `p` on folder `d` | `/d/p` | Direct children of `d` only |
| `svn:global-ignores` pattern `p` on folder `d` (SVN 1.8+) | `/d/**/p` (`p` at the root) | Every descendant of `d` |
| Inherited `svn:global-ignores` from above the checkout | `p` | Everything |
| The client's `global-ignores` (from the SVN configuration, else SVN's default list) | `p` | Everything |
| A pattern containing `/` | dropped | SVN never matches it |
| A pattern starting with `#` or `!` | escaped with `\` | Git would read it as a comment or negation |

The folder part `d` is written literally: `*`, `?`, `[` and `\` in a folder name are escaped, so `lib[1]` means that folder and not `lib1`.

Git's exclude list is a convenience that stops agents adding ignored files by accident. SVN stays the authority: before publishing, UAM adds each new folder with `--depth empty` from the top down, checking each with `svn status --no-ignore`, then checks each new file the same way, and refuses the publish if any is ignored. This check is required because an explicit `svn add` adds an ignored file without complaint, and because SVN reports nothing about ignored files inside a new unversioned folder until the folder is added. The rules are rewritten after every pull, since `svn:ignore` is versioned.

Known differences in the patterns themselves: Git applies `core.ignoreCase` on macOS and Windows while SVN matching is case-sensitive, so Git ignores more there, and SVN's bracket and escape rules inside a pattern differ slightly from Git's, so either side can occasionally match a name the other does not. The SVN check before each commit decides in every case.

## Pull (explicit, one-way)

Pull lists the revisions that changed the mirrored path since the last one checked (`svn log -q` on the path), then, oldest first, updates the bridge to each revision, stages the exact versioned file set and records it in the order above. It writes nothing to SVN, and nothing outside the bridge, the upstream ref, the revision map, the state file and `info/exclude`. A chat brings the new revisions into its branch with an ordinary `git rebase` onto the upstream ref, in its own worktree, where a conflict is a normal Git conflict the agent or the person resolves. (A merge would also bring them in, but publish only accepts a linear branch.)

The spike stages the whole file list for each revision. A product version should stage only the paths `svn update` reports, because the full listing costs time in proportion to the tree size for every revision.

## Publish

### Before anything is written

Publish refuses the whole branch, writing nothing to SVN, when:

- the upstream ref does not match the revision map (see [layout](#layout));
- the branch does not contain the latest pulled revision (rebase first);
- a commit is a merge, changes no files (SVN cannot record an empty commit), adds a Git submodule, writes inside a `.svn` folder, or has a file name with a control character, which Git allows and SVN cannot store;
- a commit is already published (its id is in the revision map);
- a commit adds a path Git's copy of the ignore rules matches (for files forced in with `git add -f`);
- a commit writes content SVN would rewrite as it stores it: an expanded keyword (`$Id: … $`) in an `svn:keywords` file, or line endings that differ from the file's `svn:eol-style`. Otherwise SVN would store different content from the agent's commit, and the mirror could not tell that from someone else's change;
- SVN has a newer revision on the path than the last one pulled.

Every message the person sees is worded by UAM: one plain line naming the commit by its subject, never SVN's raw output, error codes or the repository's address, and shown through the notification system. For example: "SVN has newer changes on trunk; pull them and rebase before publishing." SVN's own output goes only to the diagnostic log. Any failure the checks did not foresee is handled the same way: the bridge is reset, the journal cleared, and the person told that publishing the commit failed and that the diagnostic log has the reason.

### Replaying one commit

For each commit, oldest first:

1. Write the journal entry (commit, parent, the bridge's revision).
2. Move the bridge from the parent's tree to the commit's tree with a two-tree `git read-tree -m -u`, then schedule the SVN side from `git diff-tree --no-renames`: new folders and files are checked against SVN's ignore rules and added, deleted files are `svn rm`ed, executable-bit changes become `svn:executable` changes, and a versioned folder the commit emptied is deleted (Git cannot show an empty folder, so the agent saw it go). Every path reaches `svn` after `--` and with a trailing `@`, or one per line in a `--targets` file, so a name like `icon@2x.png` is not read as a peg revision, a name like `-dash.txt` is not read as an option, and a long list stays under Windows' command-line limit.
3. Start `svn commit` with the Git commit's full message and the revision property `uam:git-commit=<commit id>`, and record its process id in the journal. Setting a revision property during a commit needs no server hook.
4. Pull up to the new revision. If the revision's tree equals the commit's tree and its parent is the previous upstream commit, the upstream ref fast-forwards to the agent's own commit, so the branch and the mirror share commit ids and the next publish has nothing to rebase.
5. Remove the journal entry.

Renames publish as a delete and an add; using `svn move` for an exact Git rename is a later improvement. A change between a file and a symbolic link is refused in the first version.

### When it goes wrong

| Situation | What SVN does | What the mirror does | What is kept |
|---|---|---|---|
| Someone changed the same file after the last pull | Refuses the commit (out of date); SVN commits are atomic, so nothing is written | Resets the bridge to the parent, stops, and says to pull, rebase and publish again | Earlier commits stay published; this and later commits stay on the branch unchanged |
| Someone changed a different file after the last pull | **Accepts** the commit: SVN only checks out-of-date per file, so the new revision combines both changes | The pull in step 4 imports the other revision and then this one as a new commit (its tree differs from the agent's), records which commit it came from, and stops publishing because a revision other than its own came in | The branch is unchanged; after a rebase onto the upstream ref, Git drops the published commit (same patch) and the rest can be published |
| SVN stores the commit with different content and nobody else committed | Accepts it | The same import, but with no other revision in between UAM says SVN stored the commit differently and asks for a rebase and a check, rather than blaming a newer change. The checks before publishing make this rare | As above |
| A new file is ignored by SVN | Would add it silently | Refuses before the commit and resets the bridge | Nothing is written to SVN |
| An upstream ref moved outside UAM | Nothing | Refuses before anything is written | Everything |
| UAM stops after `svn commit` and before recording it | The revision exists with `uam:git-commit` set | At the next start, the journal shows a publish in progress. While the recorded `svn` process (matched by id and start time) is still running, UAM leaves the bridge alone and says a previous publish is still running; once it has gone, UAM runs `svn cleanup` (a checkout an interrupted `svn` left behind refuses `revert` until cleaned), reverts the bridge, returns it to the journal's revision and pulls; the pull finds the property, so the revision is mapped to the commit and is never published twice | Everything |
| UAM stops after step 4 recorded the revision but before the journal was cleared | The revision exists | The same recovery: its pull finds nothing new, so UAM then brings the bridge up to the revision the mirror has recorded, rather than leaving it behind the ref | Everything |
| UAM stops before `svn commit`, or the commit never completed | Nothing | The same recovery finds no property; the commit is still unpublished on the branch | Everything |
| UAM stops during a pull | Nothing | The next operation repairs the ref from the revision map (see [layout](#layout)) and continues; no revision is imported twice | Everything |
| UAM stops after creating a new SVN branch and before mirroring it | The branch exists, with `uam:git-branch` set on its copy revision | Not handled by the spike: a retry fails because the branch exists. The product version should find the copy by that property and attach it | The Git branch and its commits |
| Something the checks did not foresee fails | Nothing is committed | Resets the bridge, clears the journal and gives one worded line; the raw output goes to the log | Everything |
| Authentication or network failure | Refuses the commit | Resets the bridge and says SVN refused the commit and that the diagnostic log has the reason | Everything |

No failure rewrites a chat branch, discards a commit or touches the person's checkout. The only ways a published revision can differ from its Git commit are the two "accepts" cases, and the mirror then imports what SVN really holds rather than claiming the agent's tree.

A server whose `start-commit` or `pre-commit` hook refuses unknown revision properties would refuse every publish. The fallback is a matching log message with the `UAM-SVN-Revision` trailer, which is weaker; this needs checking against real servers.

## Publishing a new branch

A Git branch with no SVN path yet is published by a server-side `svn copy` to the configured branches location, followed by the branch's commits on the new path. The branches location comes from **SVN repository layouts** in Settings, as [#520](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/520) defines it. Following the [#520 decision](../documentation-review.md#svn-branch-creation), UAM asks each time where to copy from and writes an editable default message.

In this mode, "the working copy's URL and revision" is read as the SVN path and revision the Git branch starts from (the revision mapped to the point where the branch left the upstream ref), because that is the content the branch's commits were made on. It can be newer than the revision of the person's own checkout, as in the spike (`/trunk@15` while the checkout was at r2). The copy revision then maps to that same Git commit, because a copy changes no content, and the commits replay without a rebase. Choosing trunk HEAD instead copies newer content, so the branch must first be rebased onto the pulled revisions. Whether this reading is right is an [open question](#open-questions).

## Spike results

`python3 tools/svn-git-mirror-spike/spike.py`, run on 2026-11-23 on Linux with SVN 1.14.3 and Git 2.43.0, against a disposable `file://` repository with the standard layout, `svn:ignore` (including on a folder named `lib[1]`), `svn:global-ignores`, a versioned file matching an ignore pattern, an executable script and an `svn:keywords` file. A second SVN user (`bob`) made the concurrent commits. All 23 checks passed:

| # | Check | Result |
|---|---|---|
| S1 | The mirror's first commit equals the checkout's revision: versioned `*.o` kept, ignored junk in the person's checkout left out, `$Id$` unexpanded | Pass |
| S2 | Translated Git excludes agree with `svn status --no-ignore` on 20 probe paths (15 ignored by both), including two inside `lib[1]` | Pass |
| S2b | A file inside a new unversioned folder: Git ignores it, while SVN reports only the folder | Pass (the reason publish adds folders before checking files) |
| S3 | Three commits published as consecutive revisions r3–r5, in order, with identical messages; they include `icon@2x.png` and `-dash.txt` | Pass |
| S3b | Each revision's exported tree equals its Git commit's tree | Pass |
| S3c | `svn:executable` set and removed from the Git mode; the folder Git emptied is deleted | Pass |
| S3d | The upstream ref fast-forwards to the agent's own commit ids | Pass |
| S4 | A file forced in with `git add -f` that SVN ignores is refused before any revision | Pass |
| S4b | With Git's excludes removed, SVN's own check still refuses it, and the bridge is clean | Pass |
| S5 | A same-file change between the check and the commit: SVN refuses it as out of date, the earlier commit stays published, the branch and the bridge are intact | Pass |
| S5b | Pull, rebase with a real Git conflict in the chat worktree, resolve, publish: both changes are in SVN | Pass |
| S6 | A different-file change between the check and the commit: SVN accepts, the mirror imports both revisions and stops | Pass |
| S6b | After a rebase, Git drops the published commit and the remaining one publishes | Pass |
| S7 | A simulated stop right after `svn commit`: recovery maps the revision from `uam:git-commit`, and publishing again writes nothing | Pass |
| S7b | A simulated stop after the publish recorded its revision but before the journal was cleared: recovery leaves the bridge at r13, the recorded revision, and the next commit publishes as r14 | Pass |
| S8 | A pull while a chat worktree has uncommitted and untracked files imports r15: only the upstream ref moves; the worktree, the chat branch and the person's checkout (still at r2 with its edit) are unchanged | Pass |
| S9 | A new Git branch published with `svn copy` to `branches/chat branchy 1.0` from `/trunk@15`, the revision the branch starts from (the person's checkout is at r2), then its commit; the encoded ref name is valid | Pass |
| S10 | Pull of 30 small revisions | Pass, 2.5 s (about 82 ms per revision) |
| S11 | A simulated stop between the revision-map line and the ref move: the next pull repairs the ref and imports nothing twice | Pass |
| S12 | An upstream ref moved by hand from a chat worktree: publish refuses and SVN is unchanged | Pass |
| S12b | With the ref restored, the same commit publishes | Pass |
| S13 | A commit with an expanded `$Id: … $` is refused before publishing | Pass |
| S14 | A file name with a tab is refused before publishing, in words, leaving the bridge clean and no journal | Pass |

Findings the design now relies on: an explicit `svn add` adds ignored files; SVN refuses an out-of-date commit only per file, so the different-file race must be detected after the commit; staging from the working files would put expanded keywords into Git; `@` and leading `-` in file names need `--`, a trailing `@` or `--targets`; and `svn propget --xml` names targets by absolute path.

**Not covered by the spike:** Windows and macOS (line endings under `svn:eol-style native` both at import and publish, `core.ignoreCase`, symbolic links, process-id checks), real servers over HTTP or `svn://` (authentication, hooks, revision-property policy), SSH workspaces, externals, sparse checkouts, auto-props that set `svn:keywords` or `svn:eol-style` on new files, large trees, locks (`svn:needs-lock`), renames, a stop during `svn cleanup` itself, and concurrent use by two UAM instances.

## Placement in UAM

Following the [VCS boundaries](../architecture/vcs-boundaries.md), the product version is a mirror service in `src/app` beside `SvnBranchService`, with SVN and Git commands run through injected argv-only runners so the same fake-runner tests apply. It depends on the remaining [#519](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/519) work (worktree, isolation and branch methods behind the VCS interface) and on [#520](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/520) SVN branch creation. Real-SVN tests follow `tests/svn_branch_integration.py`: registered only where `svn` and `svnadmin` exist, against `file://` repositories.

## Proposed slices

1. Mirror creation, pull and the ignore mapping, local workspaces only, with chats using mirror worktrees; no publish. Tests: S1, S2, S8, S11, S12 and incremental staging.
2. Publish to the existing path, with the journal and recovery. Tests: S3 to S7b, S13 and S14.
3. Branch publish through #520's branch creation. Test: S9.
4. SSH workspaces, renames with `svn move`, and Windows and macOS line-ending and case checks.

Each slice needs its own UI for the two actions and their errors, which needs Claude review and David's approval under [AGENTS.md](../../AGENTS.md).

## Open questions

- Should turning the mode on carry the person's uncommitted SVN edits into each new chat as uncommitted changes, as the managed snapshot does, or start chats from the clean revision?
- Is deleting an SVN folder that a published commit emptied right, or should empty folders stay in SVN?
- May agents call Pull and Publish themselves, each write approved by the person as proposed for the pull-request tool in [#514](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/514), or are both person-only?
- Publishing pulls the revisions it wrote, and with them any revision someone else committed meanwhile. Is that within "pulling is always explicit", or should publish stop and ask before importing anyone else's revision?
- For a new branch, is "the working copy's URL and revision" the revision the Git branch starts from (as above), or the revision of the person's own checkout?
- The mirror is keyed by repository UUID and starts at the first checkout's revision. How should a second workspace of the same repository at an earlier revision attach, and how should two repositories that share a UUID (copies made with `svnsync`) be kept apart, for example by keying on the root URL too?
- Where a server refuses custom revision properties, is recovery by log-message trailer acceptable?
