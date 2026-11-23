# History-scan UI-thread freeze: final report

**Work branch:** `v5/perf-history-scan-off-ui-thread`
**Final SHA:** `4394e81d83f219c4918c5a6098b0c4ad0bc85218`
**Commits:** `95c1cc07` (the fix), `9690cdc6` (round-1 fixes), `4394e81d` (round-2 fixes)

## What moved off the UI thread
- **On the scan worker now:** splitting out sub-chats and their keys, filtering (via the non-mutating `ClassifyStartupImportCandidates`), owner resolution, workspace planning, startup deferral, identity keys, and keys and owners for saved chats that sub-chats name.
- **Snapshot:** the worker plans from a memory-only snapshot taken on the UI thread when the scan starts. It holds folders, settings, chat ownership fields without transcripts, hidden and excluded chats, resolved IDs, live chats and planned workspaces.
- **Merge:** the UI thread compares a fingerprint and merges only if it still matches. The fingerprint also covers a decline revision. If it doesn't match, the same results are planned again on the worker. Stale results are never merged.
- **Fewer filesystem calls:** `ImportedWorkspaceFolderIndex` resolves each registered folder once per pass. A shared `NativeWorkspaceCache` handles identity keys, and is reused when the same results are planned again.

## Independent review (fresh-context Opus sub-agents, claude-opus-5-5)
- **Round 1 (full diff):** 8 suggested (0 blocker, 2 should-fix, 6 nit), 8 implemented.
  - Should-fixes: the decline revision is now hashed into the fingerprint, and replans are logged and reuse resolved workspaces.
  - Nits:
    - Sub-chats survive a planning failure.
    - A failed scan no longer starts a pointless replan.
    - `Refresh` handles a folder list that shrank.
    - The fingerprint is computed once.
    - Docs no longer overstate per-workspace resolution.
    - Docs no longer use change-history wording.
  - No replan cap was added. The cap was optional, and capping would mean merging stale results; the reference docs say plainly that replans are uncapped.
- **Round 2 (re-check of the round-1 fixes):** 2 suggested (0 blocker, 1 should-fix, 1 nit), 2 implemented.
  - Should-fix: a failed scan's empty plan now keeps the child keys already resolved for the same state, so linking stays memory-only.
  - Nit: the replan log line is written on the first replan and each power-of-two replan.
- **Total:** 10 suggested, 10 implemented, 0 declined.

## Tests
New native tests in `tests/startup_native_import_tests.cpp`, none run:
- `StartupNativePlanResolvesEachRegisteredFolderOncePerPass`
- `StartupNativeMergeDoesNoFileSystemProbingOnTheAppThread`
- `StartupNativeStalePlanIsReplannedInsteadOfMerged`
- `StartupNativeDeclineMakesAnEarlierPlanStale`

## Unverified
- Nothing was compiled or run on macOS or Windows: no `uam_core_tests`, full native inventory or CTest. CMake refuses Linux, and the test binary needs the CEF wrapper and the platform services.
- Linux checks only:
  - Syntax-only compiles of all touched files and their callers.
  - A scratch program comparing the old and new folder lookups: 43,200 comparisons, 0 mismatches.
  - `python3 scripts/check_docs.py` passed.
- No before/after "history" poll-stage timings on the 2,379-chat data root.
- Packaged-app check not done.
- Edge cases:
  - Keys for staged candidates are computed when they're found, so a directory created or removed between passes could let a duplicate through.
  - When the merged results are dropped, temporary OpenCode export files for sessions that weren't offered are still deleted on the UI thread.
  - Saving a newly found child link still writes on the UI thread.
  - Replans are uncapped.

## Screens needing Before/After capture
None. This is a native-only change with no React or UI code. The visible effect is that the window stays responsive during discovery. A before/after timing of the "history" poll stage is the evidence to capture.

## Issue preview
**Title:** `bug(import): history discovery results freeze the window for 15 s to over 2 min`

A large native-history discovery freezes the whole window while its results are processed on the CEF UI thread.

**Reproduction:** Start UAM with many saved chats (reported: 2,379), some in workspaces on a network volume. Wait for startup native-history discovery to finish.
**Expected:** The window stays responsive while results are filtered and listed.
**Actual:** The window freezes. The "history" poll stage took 41,927 ms, then 14,548 ms, then 122,512 ms (about 36 minutes after startup). Each stall followed a burst of "Startup native history:" log lines.

**Environment:** `uam/v5.2-sync-main`; a real install with 2,379 chats, some workspaces on a network volume. OS not recorded.

**Evidence:** The timings and log pattern come from David's install log. Code reading on `uam/v5.2-sync-main` shows that once the scan future is ready, `StartupNativeImportService::Poll` runs these on the UI thread:
- `FilterStartupImportCandidates`, which calls `LoadNativeImportIndex` and `StartupImportWorkspaceOwner` for each candidate
- `LinkOwnedChildren`
- `NativeIdentityKeyForHistoryImport` for each candidate
- `PlanImportWorkspace` for each chat

`FindImportedWorkspaceFolder` runs `weakly_canonical` on every registered folder on every call, plus a VCS-root walk. That is roughly candidates × folders × path depth filesystem calls. A code comment in `PlanImportWorkspace` already noted this. Hypothesis: the network volume makes each call slow.
**Reproduction status:** Not reproduced in a controlled run. A Linux cloud session cannot run the desktop app.

**Acceptance criteria:**
- [ ] Discovery filtering, owner resolution, identity keys and workspace planning run off the UI thread.
- [ ] The UI thread merges only results planned against the current state, and never merges stale ones.
- [ ] Each registered folder is resolved once per discovery pass.
- [ ] The "history" poll stage no longer stalls on the reported data root.

**Review:** Self-triaged from the log and code reading; no independent review of the report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330, scheduled routine), on behalf of David Taylor.

## PR preview
**Title:** `perf(import): plan history discovery on the scan worker`

Startup and explicit native-history discovery used to filter, resolve owners, compute identity keys and plan new workspaces on the CEF UI thread once the scan finished. Each candidate re-resolved every registered folder. The scan worker now does all of this from a snapshot copied at launch. The UI thread checks a fingerprint, then merges, or replans if the app changed or a session was declined, so stale results are never merged. A shared `ImportedWorkspaceFolderIndex` resolves each registered folder once per pass, and planning shares one `NativeWorkspaceCache`. Docs updated: the native-imports agreement and the chats/imports engineering reference.

**Proof:**
- Syntax-only compile (`g++ -fsyntax-only`, C++20) of all touched files and their callers, on Linux.
- A scratch program comparing the old `FindImportedWorkspaceFolder` with the new index: 43,200 comparisons over 400 random folder sets (nested repos, SVN, linked worktree, managed bucket, symlink, missing and remote paths), 0 mismatches.
- `python3 scripts/check_docs.py` passed.
- New tests added but **not run**: `StartupNativePlanResolvesEachRegisteredFolderOncePerPass`, `StartupNativeMergeDoesNoFileSystemProbingOnTheAppThread`, `StartupNativeStalePlanIsReplannedInsteadOfMerged`, `StartupNativeDeclineMakesAnEarlierPlanStale`.
- Not run: `uam_core_tests`, the full native inventory and CTest (CMake refuses Linux; macOS/Windows needed).
- Not measured: before/after "history" poll-stage timings on the 2,379-chat data root.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 10 suggested, 10 implemented.

- Agent 1: claude-opus-5-5 — round 1 suggested 8, implemented 8; round 2 suggested 2, implemented 2.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
