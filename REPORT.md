# SVN snapshot parity: final report

All three gaps are fixed and the independent review loop is done; the new native tests were not run (desktop and test targets refuse Linux, so they were only syntax-checked).

**Branch:** `v5/fix-svn-snapshot-parity`, final SHA `a21cd30dc0e0dc625fbde191d1639714867da3c0`. Its three commits:
- `7c4e3aa1`: the fix
- `48ee054d`: round-1 review fixes
- `a21cd30d`: round-2 nits

## Review rounds

Each round used a separate fresh-context Opus reviewer (claude-opus-5-5, requested as `opus`).

### Round 1: 10 suggested, 8 implemented

The reviewer found 1 blocker, 3 should-fix and 6 nits.

**Implemented:**
- **Blocker:** the fake-SVN snapshot test was missing `ScopedCleanSvnStatus`.
- **Update fence:** SVN Update no longer holds snapshot chats of the working copy. That hold also blocked their prompts and launches. Instead, porting back into a copy being updated is refused, and an update is refused while a port is running.
- **Snapshot detection:** the commit panel and SVN routing now use the same rule as snapshot creation (`DescribeLocalSnapshotSource`). This also covers unborn repositories and folders below an empty `.git`.
- An SVN update of a chat whose isolated copy is not an SVN snapshot is now declined.
- The patch fallback applies only to managed snapshots.
- The port-back notice is on every fallback outcome.
- The panel note no longer mentions SVN actions that are hidden.
- A resumed update keeps its VCS type (regression test fails without the fix).

**Declined:**
- **Goal review runs `svn status` synchronously on the UI thread.** Moving it to a worker would change goal-loop scheduling that another agent owns. Recorded as a known gap. Git-enabled SVN workspaces already ran it before this branch.
- **No handler-level test of the merge-to-patch handoff.** The CEF handler test programs build only on macOS CI. Noted as a gap.

### Round 2 (re-check): 6 suggested (all nits), 4 implemented

**Implemented:**
- An update is refused only for a real port in flight (`git_port_requests`), not for checkpoints.
- Docs wording now matches the code.
- Corrected a header comment.
- New core case: an SVN update of an ordinary Git-worktree chat is declined.

**Declined:**
- An unreadable snapshot source is labelled "folder". Low impact, and the reviewer was unsure it matters.
- Chats created mid-update are not held. This was pre-existing behaviour, not a regression.

## Issue previews

### Issue preview 1

`bug(worktrees): SVN and plain-folder snapshot chats cannot port back under Merge or Squash`

With the Git port method set to Merge or Squash, chats in a local snapshot (on by default for SVN working copies and plain folders) are refused with "Merge and squash require an owned local Git worktree." Their changes cannot be ported back unless the user switches the method to Patch.

**Reproduction:** Enable SVN with "Local snapshot Git" on, set Port changes to Merge, start a chat in an SVN working copy, make a change, then port changes back.
**Expected:** The changes reach the working copy. A snapshot has no shared history with its source, so it uses the patch path and says so.
**Actual:** Refused. `HandlePortChatWorktreeChanges` routes to `PortCommittedChanges`, and `StageCommittedPort` refuses any `managed_repository`, SVN or non-Git status (`src/app/git_worktree_service.cpp` ~2799, 2983). No test covered it.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** Found by code inspection.
**Reproduction status:** not run in the app.

**Acceptance criteria:**

- [ ] Merge and Squash port snapshot chats back as a patch, with a one-line notice
- [ ] Nothing is staged and no port receipt is written for snapshots
- [ ] local-snapshots.md documents it, and a test covers it

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

### Issue preview 2

`bug(commit-panel): SVN snapshot chats show a Git panel with no SVN history or update`

SVN chats in a local snapshot show the SVN logo, but the commit panel shows only the hidden Git copy. Its commits never reach SVN, and SVN history, Update and the branch graph are missing.

**Reproduction:** Open the commit panel on a chat in an SVN working copy with "Local snapshot Git" on.
**Expected:** The panel says it is a local Git snapshot, explains that publishing to SVN happens after porting back, and offers read-only SVN history and Update for the source working copy.
**Actual:** A "GIT" panel for the copy, with no SVN actions. `ResolveWorkspaceRootPath` resolves to the worktree (`src/common/paths/workspace_root.h` ~110). Status detects only Git there (`src/app/vcs_commit_service.cpp` ~2576). The rail logo uses the source (`UI-V2/src/components/layout/AppShell.tsx` ~791).

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** Found by code inspection.
**Reproduction status:** not run in the app.

**Acceptance criteria:**

- [ ] The panel is labelled "Local snapshot (Git)", with a note about publishing to SVN after porting back
- [ ] SVN history and Update run against the source working copy

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

### Issue preview 3

`bug(goals): goal reviewer gets Git-only reads in SVN working copies`

The goal reviewer prompt names only `git status` and `git diff`, even in an SVN working copy. With Git disabled, the reviewer gets no changed-file list at all.

**Reproduction:** Run a goal in a non-snapshot SVN working copy, or with Git disabled.
**Expected:** The prompt names reads for the active system, and the change list appears when any VCS is enabled.
**Actual:** Git commands only. `ReviewWorkspaceChanges` hard-codes `VcsType::Git` and returns early unless Git is enabled (`src/common/runtime/acp/acp_goal_loop.cpp` ~61-63). The prompt text is fixed (`src/app/goal_service.cpp` ~1008).

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** Found by code inspection.
**Reproduction status:** not run with a live provider.

**Acceptance criteria:**

- [ ] SVN workspaces are given `svn status` and `svn diff --internal-diff`
- [ ] The change list is gated on any enabled VCS

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

`fix(vcs): give SVN and plain-folder snapshot chats port-back, panel and reviewer parity`

Fixes three gaps for chats in a local snapshot, the hidden Git copy UAM makes of SVN working copies and plain folders:

- **Port back:** under Merge or Squash, snapshots port back as a patch with a one-line notice instead of being refused. Nothing is staged or recorded first.
- **Commit panel:** the copy is labelled "Local snapshot (Git)", with a note that publishing to SVN happens after porting back. SVN history and Update SVN run against the source working copy. Porting back into that copy is refused while it is updated.
- **Goal reviewer:** it is told the reads for the workspace's VCS (`svn status`, `svn diff --internal-diff` for SVN), and its change list is gated on any enabled system.

A snapshot is recognised by the same rule snapshot creation uses.

Closes #<issue 1>, closes #<issue 2>, closes #<issue 3>.

<Before/After captures needed: see below.>

**Proof:**

- **UI-V2, final state:**
  - `npx vitest run` passed: 240 files, 4,218 tests, including the new snapshot-panel, resumed-update and transport cases.
  - The resumed-update test fails without its fix.
  - `npx tsc --noEmit -p .` and `npm run build` passed.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Runner:** the runner-only build of `uam_runner` passed. sqlite.org was blocked, so a locally supplied SQLite amalgamation was used.
- **Native, not run.** The desktop and test targets refuse Linux. All changed native sources and test files pass `g++ -std=c++20 -fsyntax-only` against stub CEF headers. The round-2 reviewer also ran `clang++ -Wall -Wshadow` on the main files, with no errors. These new cases have not run:
  - `GitWorktreePortSnapshotFallsBackToPatchForMergeAndSquash`
  - `GitWorktreeSvnUpdateDeclinesIsolatedChatThatIsNotSvnSnapshot`
  - `SvnSnapshotReadsAndUpdatesSourceWorkingCopyAndPortsBackAsPatchUnderMerge`
  - `GoalReviewPromptNamesReadsForTheWorkspaceVcs`
- **Real SVN** (svn 1.14.3, local `file://` repository): the patch path and the `svn log` and `svn update` steps were reproduced by hand, and a reviewer replayed them. The reviewer's read-only check accepts `svn status` and `svn diff --internal-diff` and refuses plain `svn diff`.
- **Still needed:**
  - macOS and Windows native tests and the full core run
  - an in-app check of the panel and port flow
  - a test of the CEF merge-to-patch handoff

**Known gap:** the goal reviewer's change list is read synchronously on the UI thread. It is recorded in `docs/documentation-review.md`.

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 (fresh context, round 1): suggested 10; implemented 8.
- Agent 2: claude-opus-5-5 (fresh context, re-check): suggested 6; implemented 4.
- Declined items and reasons are listed under Review rounds above.
- Required Claude review of the React UI changes and David's acceptance are pending.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## Screens needing Before/After capture

- Commit panel on an SVN snapshot chat: the "Local snapshot (Git)" label, the note, the SVN history tab and the Update SVN button.
- The same panel with SVN disabled: label and shorter note, no SVN actions.
- Commit panel on a plain-folder snapshot chat: label and note.
- The chat's port result showing the patch notice, with Port changes set to Merge.
- Settings › Version Control › Port changes description.
