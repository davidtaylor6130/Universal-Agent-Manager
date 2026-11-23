# #521 and #522 first slices: report

**Branch:** `v5/design-521-522-svn-mirror-adapters` (pushed; PR #1140). **Final SHA:** `5d2c9d19a4490c9c139dbcf52d57961e0d8848d1`. Commits on `origin/uam/v5.2-sync-main` (`c252f9a9`), one per issue per round:

- `1e6ba181` docs(vcs): design a UAM-owned Git mirror for SVN checkouts (#521)
- `700e6aed` docs(vcs): specify the VCS adapter protocol with a conformance runner (#522)
- `dbca56e8` docs(vcs): apply David's answers to the SVN mirror design (#521)
- `740374bc` docs(vcs): apply David's answers to the adapter protocol (#522)
- `e3f88349` docs(vcs): settle recovery and interim readings in the SVN mirror design (#521)
- `832d60ef` docs(vcs): clarify adapter approval coverage (#522)
- `5d2c9d19` docs(vcs): settle a still-running publish and label the person-action reading (#521)

Neither issue's commits contain UAM product code. Besides docs and Python tools, the only change is four `CMakeLists.txt` lines that register the self-test as a CTest. No force-push was used.

## What was built

**#521:**

- **Design note:** `docs/reference/svn-git-mirror-design.md`.
- **Throwaway spike:** `tools/svn-git-mirror-spike/spike.py`, run against a local `file://` repository. **25 of 25 checks pass.**

The design covers:

- where the mirror lives: a bare repository keyed by repository root URL and UUID, shared by workspaces at their own revisions; a private bridge checkout per SVN path; percent-encoded upstream refs; an authoritative revision map; a state file; one publish journal per mirror;
- an exact import of SVN's versioned files with keywords unexpanded;
- the ignore mapping, with SVN's own check before every add;
- an explicit, one-way pull with a fixed write order, which repairs a stop or refuses a ref moved outside UAM;
- ordered commit replay with a `uam:git-commit` revision property (or, where a server refuses that, a one-line `UAM-Git-Commit` trailer);
- Publish stopping and asking for a Pull whenever SVN has newer revisions, including the race after a commit and recovery after a stop;
- every failure case, where nothing is lost;
- branch publish with `svn copy`, following #520;
- the approval rule for every change to SVN.

**#522:**

- **Protocol:** `docs/reference/vcs-adapter-protocol.md`, version 1.
  - Read-only JSON-RPC over stdio; capabilities are declared by `VcsCapabilityId` names.
  - File actions only reveal a workspace file in the file manager, and URLs are http or https only.
  - Approval covers the executable and its install folder, and any change asks again.
  - Adapters are local only, and the restart rule is the one you approved.
  - Writes a later version adds need approval under the remote-commit rule.
- **Conformance runner:** `tools/vcs-adapter/conformance.py`.
- **Sample folder adapter:** `tools/vcs-adapter/sample_folder_adapter.py`, which **passes all 15 checks**.
- **Self-test:** `tools/vcs-adapter/selftest.py`, **30 of 30 pass**; registered as the CTest `uam_vcs_adapter_conformance`.

## David's answers (2026-11-23)

All are recorded in both design pages' Decisions tables. They are closed in `docs/documentation-review.md` under "SVN through Git" and "Extension adapters", with this date.

The cross-cutting rule is defined once, on `docs/providers/permissions/permissions.md#remote-commits`. It says every action that permanently commits outside the person's computer needs explicit approval by default, even in YOLO, unless a hidden advanced setting turns that off. It is marked not built, and these point to it:

- the YOLO agreement;
- a review entry, "Approval for remote commits", that records the gap;
- `docs/architecture/decisions.md`;
- both designs.

**Open, pending David's decision:** three interim readings of the answers, recorded in the documentation review:

1. In this mode the default "working copy revision" for a new branch is the revision the chat's Git branch starts from.
2. A Publish an agent requests asks for approval once per commit.
3. The person's own Publish or Commit, from a control that shows the target and the content, is itself the approval.

## Review rounds

All rounds were run by independent claude-opus-5-5 agents with fresh contexts.

| Round | Suggested | Implemented |
|---|---|---|
| 1, original slices | 25 | 24 |
| 2, re-check | 16 | 14 fully, 2 in part |
| 3, answers commits | 10 | 10 |
| 4, re-check | 7 | 7 |

What was not fully implemented:

- **Round 1:** declined the `Claude-Session` commit trailer, because your instruction says commits end with the Co-Authored-By line.
- **Round 2, Windows Job Object:** not added to the runner, because Windows can't be tested here. The limit is stated in the spec, and the orphan-after-crash test runs on macOS and Linux only.
- **Round 2, host names in error messages:** URLs and IP addresses are flagged; bare host names are left to adapter authors.

The stop hook asked for a push before round 3's review had finished, so `740374bc` and `832d60ef` were pushed before their reviews completed. All later fixes went on top.

## New issue previews

None proposed.

## PR #1140 update

**Title:** `docs(vcs): design the SVN Git mirror and the VCS adapter protocol`

Related to #521 and #522. These are first slices only, with no UAM product code.

**#521.** The design note covers how UAM builds and owns a Git mirror of an SVN checkout. Chats work in ordinary Git worktrees, and SVN changes only through an explicit, one-way Pull or an approved Publish. Publish stops and asks for a Pull whenever SVN has newer revisions, and nothing is lost on any failure. A throwaway spike tests the design against a disposable `file://` repository.

**#522.** Version 1 of a read-only JSON-RPC-over-stdio protocol for external version-control adapters. Actions can only reveal a workspace file or open an http(s) URL. It comes with a conformance runner, a sample adapter that passes it, and a CTest self-test of 26 deliberate faults.

David's 2026-11-23 decisions and answers are recorded and their questions closed. His rule that remote commits need approval even in YOLO is recorded once on the permissions page, marked as not built. Three interim readings of his answers wait for his confirmation.

**Proof:** All run on Linux with Python 3.13, SVN 1.14.3 and Git 2.43.0.

- `python3 tools/svn-git-mirror-spike/spike.py`: 25 of 25 checks passed.
- `python3 tools/vcs-adapter/selftest.py`: 30 of 30 cases passed. Three copies run at once next to four busy CPU processes also passed.
- `python3 scripts/check_docs.py`: passed on the head and on individual intermediate commits.

Not run:

- The native configure. The desktop configure refuses Linux, and the runner configure fails downloading SQLite through the sandbox proxy (the same on the base). So the CTest registration was checked by reading only.
- Windows and macOS, and real SVN servers.

**Review:** 4 rounds by independent fresh-context agents. Independent claude-opus-5-5 (round 1): 25 suggested, 24 implemented (the `Claude-Session` trailer was declined per the task's commit-trailer instruction). Independent claude-opus-5-5 (round 2 re-check): 16 suggested, 14 implemented, 2 in part. Independent claude-opus-5-5 (round 3, answers commits): 10 suggested, 10 implemented. Independent claude-opus-5-5 (round 4 re-check): 7 suggested, 7 implemented.

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code harness), on behalf of David Taylor.

**Screens needing Before/After capture:** none; no UI changed.
