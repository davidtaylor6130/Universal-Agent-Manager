# #521 and #522 first slices: report

**Branch:** `v5/design-521-522-svn-mirror-adapters` (pushed). **Final SHA:** `700e6aedc67b3b52445470714b17b9ed4c43c062`, two commits on `origin/uam/v5.2-sync-main` (`c252f9a9`), one per issue:

- `1e6ba181` docs(vcs): design a UAM-owned Git mirror for SVN checkouts (#521)
- `700e6aed` docs(vcs): specify the VCS adapter protocol with a conformance runner (#522)

Neither commit contains UAM product code. Besides docs and Python tools, the only change is four `CMakeLists.txt` lines that register the self-test as a CTest. Decisions recorded with date 2026-11-23 in `docs/documentation-review.md` (#520, #521, #522), cross-referenced from `docs/architecture/decisions.md#existing-vcs-panel`.

## What was built

**#521:** `docs/reference/svn-git-mirror-design.md` and the throwaway spike `tools/svn-git-mirror-spike/spike.py` (referenced from the note). The note covers:

- where the mirror lives: a bare repository in the data root, a private bridge checkout per SVN path, percent-encoded upstream refs, an authoritative revision map, a state file and a publish journal;
- an exact import of SVN's versioned files with keywords unexpanded;
- the `svn:ignore`/`svn:global-ignores` mapping, with SVN's own check before every add;
- an explicit, one-way pull with a fixed write order, which repairs a stop or refuses a ref moved outside UAM;
- ordered commit replay with a `uam:git-commit` revision property for recovery;
- every failure case, where nothing is lost;
- branch publish with `svn copy`, following the #520 decision;
- spike results and open questions for David.

The spike ran against a local `file://` repository: **23 of 23 checks pass**.

**#522:** three pieces.

- `docs/reference/vcs-adapter-protocol.md`, protocol version 1:
  - JSON-RPC over stdio.
  - The handshake declares capabilities by `VcsCapabilityId` names. Version 1 allows read-only `status`, `diff`, `history` (with `revisionDiff`) and `branchList`; write methods must be refused.
  - Actions only open an http(s) URL or show a file inside the workspace in the file manager; neither can run anything.
- `tools/vcs-adapter/conformance.py`, the conformance runner. It contains:
  - crashes, timeouts, oversized or deeply nested output, and stdout noise;
  - stray child processes, whose process tree it kills;
  - stderr floods.

  It also checks capability honesty, path safety, error handling, actions, shutdown and that the workspace is unchanged.
- `tools/vcs-adapter/sample_folder_adapter.py`, which **passes all 15 checks**, and `tools/vcs-adapter/selftest.py`, which **passes 30 of 30** (CTest `uam_vcs_adapter_conformance`).

## Review rounds

- **Round 1, independent claude-opus-5-5 with a fresh context:** 25 suggested (1 blocker, 14 should-fix, 10 nits); 24 implemented and 1 declined. The declined one was nit 25's `Claude-Session:` commit trailer, because your instruction says commits end with the Co-Authored-By line; the other half of that nit, the `decisions.md` cross-reference, was added. The blocker: `openFile` through the OS default handler could launch programs from a repository, so file actions now only show the item in the file manager.
- **Round 2, one fresh-context claude-opus-5-5 re-check:** 16 suggested (0 blockers, 6 should-fix, 10 nits); 14 implemented and 2 implemented in part:
  - **#2, the Windows Job Object.** Not added to the runner, because I cannot test Windows here. The `taskkill /T` limit is stated in the spec, the spec requires a Job Object of UAM itself, and the orphan-after-crash test runs on macOS and Linux only.
  - **#13, host names in error messages.** URLs and IP addresses are now flagged. Bare host names cannot be told from other dotted text, so the spec leaves that check to authors.
  - Nit #15 (`--targets` for `svn status`) was done another way: `svn status` does not accept `--targets`, so paths are passed in chunks.

## New issue previews

None proposed. The open questions for David are recorded in the two design pages and in the documentation review, under the #521 and #522 issues.

## PR preview

**Title:** `docs(vcs): design the SVN Git mirror and the VCS adapter protocol`

Related to #521 and #522. These are first slices only, with no UAM product code.

**#521.** The design note covers how UAM builds and owns a Git mirror of an SVN checkout, so chats work in ordinary Git worktrees and SVN is touched only by an explicit, one-way Pull or by Publish. It sets out the import of exactly SVN's versioned files, the `svn:ignore` mapping with SVN as the final check, and the write order that recovers from a stop and refuses a ref moved by an agent. It also covers ordered commit replay with a revision property for recovery and the failure cases in which nothing is lost. A throwaway spike tests the design against a disposable `file://` repository.

**#522.** Version 1 of a read-only JSON-RPC-over-stdio protocol for external version-control adapters. Capabilities are declared by `VcsCapabilityId` names, and action buttons can only open http(s) URLs or show a workspace file in the file manager. It comes with a conformance runner, a sample folder adapter that passes it, and a self-test of 26 deliberate faults, registered as a CTest.

The decisions David made on 2026-11-23 for #520, #521 and #522 are recorded in the documentation review.

**Proof:** All run on Linux with Python 3.13, SVN 1.14.3 and Git 2.43.0.

- `python3 tools/svn-git-mirror-spike/spike.py`: 23 of 23 checks passed, including ordered publish, out-of-date and different-file races, recovery after a stop, a moved ref, ignore handling and branch publish.
- `python3 tools/vcs-adapter/selftest.py`: 30 of 30 cases passed in about 41 s. Three copies run at once next to four busy CPU processes also passed.
- `python3 scripts/check_docs.py`: passed, on the branch and on the first commit alone.

Not run:

- The native configure. The desktop configure refuses Linux, and the runner-only configure fails downloading SQLite through the sandbox proxy (the same on the unchanged base). So the CTest registration was checked by reading only.
- Windows and macOS, and real SVN servers.
- No UI-V2 or C++ code changed.

**Review:** 2 independent reviewers, 2 iterations. Independent claude-opus-5-5: 25 suggested, 24 implemented (the `Claude-Session` trailer was declined per the task's commit-trailer instruction). Independent claude-opus-5-5 re-check: 16 suggested, 14 implemented, 2 in part (no Windows Job Object in the runner, which is stated as a limit; host names in adapter messages are flagged only as URLs and IP addresses).

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code harness), on behalf of David Taylor.

**Screens needing Before/After capture:** none; no UI changed.
