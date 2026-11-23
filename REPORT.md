# Report: #902 native sub-chat import gaps

**Work branch:** `v5/feat-902-subchat-import-gaps`, pushed.
**Final SHA:** `c81c2d3b85825f47bf239eff8d3d0f4f4894d6fa`, two commits on `origin/uam/v5.2-sync-main` @ `6f235eb9`:
- `52645665`: SSH backfill under saved parents, its fixture test and docs
- `c81c2d3b`: review fix (two more guard checks in the test)

## Where #902 stood on the base
- **Already done in V5.1.0 (#1007 and earlier):** all four review follow-ups. These are the deleted-journal record (`passive|<reference>`), the count of selected histories, the OpenCode 2-only `subagent` match, and tests for a parent that fails staging and for the remote Codex and OpenCode child routes.
- **Still open:** five provider gaps. This branch closes part of one: "Backfill for remote chats".

## What changed
A Chat Data scan you start now also imports the missing sub-chats of a saved Codex or OpenCode chat on an SSH host (`StartSavedParentBackfill`).
- **Why only after an earlier scan:** a scan you start reads only histories on this computer. So the SSH children are the ones this run's earlier scan of that host found, at startup or by a background retry.
- **Conditions:** the host must still be the same connection with its helper ready, and the parent's workspace must be the folder that scan read.
- **Staging:** each child is read again from the host through the existing remote route.
- **Unchanged:** startup scans and background retries still never add sub-chats.
- **Docs:** the import page's implementation note, the child-session reference, and the Codex and OpenCode import rows (still Needs verification).

## Still open on #902, and why
- **Antigravity child links:** the step that names a child conversation is in a format with no public specification. Nothing was guessed.
- **Claude Code, Gemini and Copilot children on SSH hosts:** SSH scans never find these providers' parent sessions, and the helper can't list Claude Code or Copilot children. Gemini has a child listing (`ListGeminiChildren`). New helper calls are needed, checked against each CLI's remote layout.
- **Gemini children with inline images:** the save route for images requires the parent to be saved already, which isn't true during an import. Fixing this means restructuring the import pipeline.
- **Nested OpenCode children:** finding grandchildren means reading a child during staging and adding items to an import that is already running.
- **OpenCode 2 export's parent field:** needs a live OpenCode 2 CLI.
- **Remote backfill:** it still reaches only hosts that this run of UAM has already scanned.

## Review rounds
1 independent reviewer, 1 iteration.

- **Round 1 (independent claude-opus-5-5, `agent-reports/review-R2`, #902 section):** 2 suggested, 1 implemented, 0 blockers, 0 should-fixes. Verdict: approve.
  1. Nit, implemented: the test now also covers a helper that is no longer ready and a workspace folder whose directory changed since the scan.
  2. Nit, declined: it asks for an edit to the #902 checklist, which the owner will make.
- **No re-check:** the follow-up only added test checks, so no second round was needed.

## Proof (what was actually run)
- `python3 scripts/check_docs.py`: passed (203 pages, 1508 links, 6 providers × 32 capabilities).
- GCC `-std=c++20 -fsyntax-only` with all providers on, on Linux:
  - `src/app/startup_native_import_service.cpp`: 0 errors.
  - `tests/empty_native_import_tests.cpp` (13 cases): 0 errors, compiled against the real source headers with a small stand-in for the test harness, because the real harness needs the CEF headers.
- **Not run:** the native build and `uam_core_tests`, including the new case. CMake refuses Linux; they need macOS or Windows. No live SSH host or provider CLI was used.

## Screens needing Before/After capture
None. Nothing on screen changes: the change is native import logic, so sub-chats simply appear under their saved SSH parent after a scan you start.

## Issue previews
None.

## PR preview
**Title:** `feat(chat-data): backfill SSH sub-chats of saved parents on a manual scan`

A Chat Data scan you start now imports the missing sub-chats of saved Codex and OpenCode chats on SSH hosts, not only chats on this computer.
- It takes the children that this run's earlier scan of that host found, while the host is the same, ready connection and the parent's workspace is the folder that scan read.
- Each child is read again from the host before it is saved.
- Startup scans and background retries still never add sub-chats.
- The import page, the child-session reference, and the Codex and OpenCode import rows are updated.

The other four gaps on #902 stay open, for the reasons recorded in `docs/reference/native-child-sessions.md`.

**Proof:**
- `python3 scripts/check_docs.py` passed.
- On Linux, GCC syntax-only compiles found no errors in the changed service file or the whole test file (13 cases), with all providers on.
- Not run: the native build and `uam_core_tests`, including `ManualScanBackfillsRemoteSubchatsOfSavedParentsThroughTheRunner`. They need macOS or Windows. No live SSH host or provider CLI was used.

**Review:** 1 independent reviewer, 1 iteration. Independent claude-opus-5-5: 2 suggested, 1 implemented (the other is a checklist edit left to the owner); 0 blockers, 0 should-fixes.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session, scheduled task), on behalf of David Taylor.

Refs #902
