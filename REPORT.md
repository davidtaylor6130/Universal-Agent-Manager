# Report: #519 refactor(vcs), one version-control interface (branch-picker slice)

## Result

- **Work branch:** `v5/refactor-519-vcs-interface`
- **Final SHA:** `bca48e3f`, which merges `origin/uam/v5.2-performance` with no conflicts.
- **Commits:** `da59643e` (the refactor), `8a62e0e9` (review fixes), `bca48e3f` (base merge).
- **Scope:** this closes one item from the V5.1.0 close-out list. The chat branch picker's list, selection check, switch recheck and switch now go through `VcsProvider` for both Git and SVN, with no intended behaviour change.
- **Still open on #519:** worktrees behind the interface, "open in" actions, the two callers that still ask for Git by name (`vcs_handlers.cpp`, `acp_goal_loop.cpp`), greying out the picker in the UI, and splitting the Git and SVN implementations out of `vcs_commit_service.cpp`. These are recorded in `docs/architecture/vcs-boundaries.md`.

## Interface shape (`src/app/vcs_provider.h`)

- **`BranchesAreLocated()`:**
  - SVN: true. Choices are repository folders under an explicit parent, and each selection carries the working-copy context it was listed in.
  - Git: false. Choices are named branches.
- **Operations:**
  - `ListBranches`: list the choices for the chat's workspace.
  - `CheckBranchSelection`: check a choice when the picker submits it.
  - `CheckBranchSwitch`: recheck the workspace once the chat has stopped and the workspace is locked.
  - `SwitchBranch`: switch, or create and switch.
- **Types:** `VcsBranch`, `VcsBranchListing`, `VcsBranchSelection` and `VcsBranchOutcome`.
  - `VcsBranchOutcome.conflict` keeps each 409 vs 400 status code the same.
  - `VcsBranchOutcome.chat_updated` is set because Git saves the branch on the chat and SVN doesn't.
- **Helpers:** `VcsProviderForId` (exact declared id, otherwise Git) and `DeclaresCapability`.
- **`BranchCreate` is now used:** a system that doesn't declare it refuses creation with the same message as before.
- **Delegation:** Git delegates to `GitWorktreeService`, SVN to `SvnBranchService`. Neither service changed.
- **Files moved:** none.

## Review rounds

- **Round 1** (independent `claude-opus-5-5` reviewer, fresh context): 9 suggested (0 blocker, 2 should-fix, 7 nit), 8 implemented.
  - #1 (should-fix) was declined: the reviewer asked for a connected CEF handler test of the `listChatBranches`/`switchChatBranch` wire format. That harness can't be built or run on the Linux host this work ran on, and writing it blind risks a broken test. The reviewer checked bridge parity path by path and found no difference; it is checked by review only.
  - #2 (should-fix, the real-SVN case now runs without SSH), #3, #4, #5, #7 and #8 were done in `8a62e0e9`.
  - #6 (keep the SVN context type as the interface's alias) was accepted as is; the code comment already explains it. No change needed.
  - #9 (refresh the base) was done in `bca48e3f`.
- **No re-check round:** the fixes only touched tests, comments and docs (no product logic), so I did not run a second review.

## Issue previews

None. No new issues were filed.

## PR preview

**Title:** refactor(vcs): route the chat branch picker through VcsProvider (#519)

**Summary:** Branch list, selection check, switch recheck and switch go through `VcsProvider` instead of the bridge checking `vcsType == "svn"` and calling `GitWorktreeService` and `SvnBranchService` directly.
- The bridge keeps the wire format (`svnParent`, `svnContext`), waiting for the chat to stop, the workspace lock, and Git's first-message pending branch.
- Payloads, replies, status codes and messages are unchanged.
- `docs/architecture/vcs-boundaries.md` describes the interface as the extension point and lists the remaining #519 items.

**Review:** 1 independent reviewer, 1 iteration. Independent claude-opus-5-5: 9 suggested, 8 implemented (1 declined: the connected handler wire-format test can't be built on Linux; parity checked by review).

**Proof (only what was run, on Linux):**
- `python3 scripts/check_docs.py`: passed (203 pages, 1508 links, 6 providers × 32 capabilities).
- SVN branch service suite, built by hand with g++: unit suite passed, real `file://` integration passed (5 passes).
- Syntax-only compile against CEF headers (g++ 13; the reviewer also ran clang 18): 0 errors in `vcs_commit_service.cpp`, `chat_branch_handlers.cpp`, `vcs_handlers.cpp` and `tests/vcs_provider_tests.cpp`. The test file was compiled both with and without SSH.
- Throwaway driver: the SVN provider against a real `file://` repository passed all 28 checks (list, check, switch, stale-listing conflict, disabled-SVN refusal, chat fields unchanged).

**Not verified:**
- `uam_core_tests`, including the new `VcsBranchPickerRoutesThroughEachSystemsProvider` and `SvnBranchListCheckAndSwitchGoThroughTheSvnProvider`, was not built or run. Linux can't build the app's tests.
- The Git provider was not run, because it needs the macOS/Windows platform services.
- The bridge handlers were only compile-checked.
- Whether macOS CI has subversion installed decides whether the real-SVN case runs or prints SKIP.
- No UI change, so no UI review or acceptance is needed for this slice.

## Screens needing Before/After capture

None. Nothing in React/UI changed; the picker should look and behave exactly as before.
