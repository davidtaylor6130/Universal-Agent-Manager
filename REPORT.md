# Report: hidden chats with a missing workspace

**Branch:** `v5/fix-hidden-chats-missing-workspace` at **`a803e42aa17e51b4b69dfd43121b8969f7797aad`**. It has three commits on `uam/v5.2-sync-main`. Only this branch and this report branch were pushed. I opened no PR or issue and left no comments.

## Change
- **The sidebar is not what hid the chats on this branch.** The code already lists chats whose workspace ID no saved workspace has under Unsorted, with Rebuild workspace folders available (`buildChatSearchModelFromGroups`, from #731). An end-to-end test, from a native-shaped state push through `FolderTree`, passed before any change. I added regression tests and a comment only.
- **Folders are now saved before the chats that move or are copied.** This covers the startup mixed-machine split (`MigrateWorkspaceFolderOwnership`), Rebuild workspace folders, and the previous-version data copy (`ImportLegacyDataRoot`). The copy now copies no chats when its folder save fails.
- **A failed chat save rolls back every moved chat.** The old folder list comes back only when all of them are restored. Memory keeps any chat whose move could not be undone on disk.
- **Startup diagnostic.** Startup logs one line with the number of chats naming missing workspace IDs and the IDs with per-ID counts. IDs are capped at 20, each at 64 characters, with no paths or titles. Saved chats are never changed.
- **Docs:** folder recovery, All Chats and source of truth.

## Cause
- **Shown to be possible, now fixed:**
  - The split and Rebuild workspace folders saved chats before `folders.txt`. Stopping between the two writes left chats naming a workspace that was never saved.
  - The previous-version copy copied chats first, and only recorded an error when its folder save then failed.
- **Not proven to be the install's cause.** The reviewer rates a bulk previous-version copy as a better fit than the split for 12 IDs and 487 chats under one ID, at medium confidence.
- **Not fixed:**
  - `folders.txt` that fails validation silently loads the older `.bak`, and the next folder save persists that list.
  - Two open leads for why the chats were hidden on the install: branch chats whose root no longer exists are never listed, and the installed build may predate #731.
- The new startup line will show the IDs on the affected install.

## Tests (only what I ran)
| Check | Result |
|---|---|
| `npm ci` | passed |
| `npx vitest run` | 240 files, 4218 tests passed (after commit 1; no UI files changed later) |
| `npx tsc --noEmit -p .` | passed |
| `npm run build` | passed |
| `python3 scripts/check_docs.py` | passed after each round |
| `g++ -std=c++20 -fsyntax-only` with stubbed CEF headers | no errors on changed lines |

- **New Vitest:** `FolderTree.missingWorkspace.test.tsx`, plus one case in `chatSearch.test.ts`.
- **New C++, not compiled or run:**
  - `InterruptedMixedMachineWorkspaceSplitNeverLeavesChatsNamingAnUnsavedWorkspace`
  - `InterruptedUnsortedWorkspaceRebuildNeverLeavesChatsNamingAnUnsavedWorkspace`
  - `UnsortedWorkspaceRecoveryPutsMovedChatsBackWhenAChatSaveFails`
  - `DanglingWorkspaceFolderReferencesAreCountedWithoutChangingChats`
  - `LegacyDataRootImportCopiesNoChatsWhenItsWorkspacesCannotBeSaved`
  - Renamed: `UnsortedWorkspaceRecoveryLeavesChatsUntouchedWhenFolderSaveFails`.
- **Native build and tests have not run.** CMake refuses Linux and the CEF host is blocked here. Run `uam_core_tests` with these cases, then the full native gates, on macOS or Windows.

## Review rounds
All rounds were fresh-context Opus (`claude-opus-5-5`) subagents.

**Round 1 (full diff): 8 suggested, 7 implemented.**
- 1 blocker: the interrupted-rebuild test failed by construction. Fixed.
- 2 should-fix: the previous-version copy saved chats before folders and ignored the UI re-save result; memory disagreed with disk after an incomplete rollback. Both fixed.
- 5 nits: a chat-save-failure test, a test rename, log wording for internal chats plus ID length, and status-line punctuation, all implemented.
- Declined: native trims folder IDs but the frontend compares raw IDs. This existed before the change, hides nothing, and is out of scope.

**Round 2 (re-check of the fixes): 7 suggested, 5 implemented.**
- 2 should-fix: no test for the copy's failure path, and inaccurate wording on the UI re-save failure. Both fixed.
- 3 nits implemented: rollback rewrites every moved chat, the missing-original guard is restored, and the doc wording is corrected.
- Declined: a test for the incomplete-rollback branch. Failing the second write after a successful first one can't be arranged portably without another test hook.
- Declined: a UTF-8 boundary when cutting IDs to 64 characters. It only affects one diagnostic line.

## Issue preview
**Title:** `bug(sidebar): chats whose workspace no longer exists disappear from All Chats`

Chats whose saved workspace ID matches no saved workspace disappeared from the desktop sidebar, including Unsorted. Rebuild workspace folders was therefore unreachable and the chats looked deleted. Reported by David from a real install.

**Reproduction:** Exact steps not recorded. Precondition: saved chats whose folder ID is missing from `folders.txt`.
**Expected:** They appear under Unsorted with Rebuild workspace folders available, and their saved data is unchanged.
**Actual (reported):** They are missing from every sidebar section.

**Environment:** Installed version and OS not recorded. Code examined: `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:**
- Reported: 666 saved chats, with primaries and summaries on disk, reference 12 folder IDs that exist nowhere. One ID holds 487 chats.
- Observed in code: the startup split, Rebuild workspace folders and the previous-version data copy each wrote chats before `folders.txt`. Stopping between the writes, or a failed folder save in the copy, leaves dangling IDs.
- Hypothesis: `folders.txt` falling back to an older `.bak`.

**Reproduction status:** On `6f235eb9`, a test pushing native-shaped state with dangling IDs through the store into `FolderTree` shows the chats under Unsorted, with recovery working. The hiding was not reproduced, and the install's real cause is unconfirmed.

**Acceptance criteria:**
- [ ] On the affected install, the 666 chats appear under Unsorted and Rebuild workspace folders opens.
- [ ] Startup logs the missing IDs with counts and no paths.
- [ ] Stopping the split, recovery or previous-version copy between its writes never leaves a saved chat naming an unsaved workspace.

**Review:** Self-triage only; the report was not independently reviewed.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code harness), on behalf of David Taylor.

## PR preview
**Title:** `fix(workspaces): save workspaces before moved or copied chats and log missing workspace IDs`

The startup split of mixed-machine workspaces, Rebuild workspace folders and the previous-version data copy each wrote chats before `folders.txt`. A process stopped between the writes, or a failed folder save in the copy, left chats naming a workspace that was never saved. All three now save the folder list first, and the copy copies no chats when that save fails. A failed chat save puts every moved chat back, keeps the added workspaces when that is incomplete, and keeps memory matching disk. Startup logs one line with the missing workspace IDs and their chat counts, without paths. The desktop sidebar already lists such chats under Unsorted; this PR adds regression coverage for that, not a display change.

<Visual evidence: capture pending. See the screen list below.>

**Proof:**
- `npm ci`, `npx vitest run` (240 files, 4218 tests), `npx tsc --noEmit -p .`, `npm run build` and `python3 scripts/check_docs.py` all passed.
- The new Vitest cases also pass without the change; they protect existing behaviour.
- Native: only a syntax check with g++ against stubbed CEF headers, with no errors on changed lines. The native build and the five new C++ cases have not run, because CMake refuses Linux. They must run on macOS or Windows before merge.

**Review:** 1 independent reviewer, 2 iterations. Independent `claude-opus-5-5`: 15 suggested, 12 implemented. Declined: the trimmed-ID mismatch (existed before this change), the incomplete-rollback test (not portable), and UTF-8 cutting in the diagnostic line. The required Claude review on the PR and David's approval are still pending.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code harness), on behalf of David Taylor.

Related to <issue number once filed>. This only partly addresses the report, since the install's actual cause is unconfirmed.

## Screens to capture (Before/After)
1. Sidebar All Chats on the affected install: the Unsorted header with its count, expanded.
2. The Unsorted actions menu showing Rebuild workspace folders.
3. The Rebuild workspace folders dialog preview for these chats.

On a build of this branch, Before and After should look the same, because the sidebar code didn't change. The useful comparison is the current install against a build of this branch.
