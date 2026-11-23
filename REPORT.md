# Report: v5/fix-folder-load-overwrite-fence

**Work branch:** `v5/fix-folder-load-overwrite-fence` at `0e2198ef8fd1eb3800fdb3881256e60d84cad4f4` (3 commits on `uam/v5.2-sync-main` 6f235eb9; merges cleanly into `origin/uam/v5.2-performance`). No PR opened.

## Change
- `uam::io::TryReadTextFileStatus` returns Read, Missing or Error. A read error is never treated as an empty file.
- `src/common/utils/store_load_fence.h` holds the load result (Missing, Valid, RecoveredFromBackup, Unrecovered), a shared loader that falls back from the primary to the validated `.bak`, and a process-wide save fence keyed by the store's file path (UTF-8).
- `ChatFolderStore` (`folders.txt`) and `ResourceCollectionService` (`resource_collections.json`) report that result. Only loads that become the live saved state arm the fence (`StoreFenceMode::Arm`): startup, and replay of a pending deletion at startup. While a file is fenced, `Save`, `PrepareSave` and `PublishPreparedSave` all refuse it.
- Also while fenced:
  - workspace deletion is refused before its transaction is written;
  - legacy import doesn't merge folders from a damaged file on either side;
  - managed-worktree retention doesn't prune.
- Startup shows one plain-line notice in the status line and writes it to the diagnostics log.
- Docs: "Store recovery rules" in `docs/architecture/source-of-truth.md`, with links from the collections and folder-recovery pages.

## Review rounds
- **Round 1 (independent claude-opus-5-5, fresh context): 10 suggested, 10 implemented.**
  - 1 blocker: the MSVC fence key could throw on non-ANSI paths.
  - 3 should-fix:
    - passing reads armed the fence;
    - a workspace deletion while fenced could block every later startup;
    - worktree retention used the partial lists.
  - 6 nits: `.bak` removal wording, joined notice, read-status comment, extra tests, doc precision and links, the legacy-root partial import.
  - Nit 8's case for which notice startup chooses wasn't added, because it needs CEF. It's noted under Not verified instead.
- **Round 2 (fresh-context claude-opus-5-5 re-check of the round 1 fixes): 3 suggested, 2 implemented.**
  - **Implemented:** a stale comment, and the warning now names the `.bak` when only the backup is damaged.
  - **Declined:** naming the deletion-staging copy in the startup-exit message. It was marked optional, the exit follows the existing corrupt-intent path, and the docs already record that there's no in-app recovery action yet.

## Issue preview
**Title:** `bug(storage): unreadable folders/collections file is overwritten by the next save`

When `folders.txt` or `resource_collections.json` can't be read or validated and has no valid `.bak`, the load returns partial or empty data without saying so. The next save then overwrites the saved file, so workspace definitions are lost and their chats drop out of the sidebar.

**Reproduction:** Write a truncated `folders.txt` (a format header with no `folders_complete=1` line), or invalid JSON to `resource_collections.json`, with no `.bak`. Load the store, then save it.
**Expected:** The load reports an unrecoverable file, the save is refused, and the file is unchanged.
**Actual:** The save succeeds and replaces the file with the partial or empty list.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; all platforms.

**Evidence:** `chat_folder_store.cpp` (~137) parses an invalid primary when there is no backup. `ReadTextFile` (`io_utils.h` ~179) returns "" on an I/O error. `resource_collection_service.cpp` (~306) returns `{}` on failure. The writers at `folder_handlers.cpp` ~364, `organization_service.cpp` ~69 and `resource_collection_handlers.cpp` ~330 then persist that state. Settings already refuses this case (`settings_store.cpp` ~893–918).
**Reproduction status:** Reproduced on Linux with a standalone program built against `uam/v5.2-sync-main`: both stores overwrote the damaged file.

**Acceptance criteria:**
- [ ] An invalid primary with no `.bak` is not overwritten by any save.
- [ ] An I/O read error is not treated as an empty file.
- [ ] A valid `.bak` still recovers.
- [ ] The user sees one plain-line notice.

**Review:** Self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session, scheduled routine), on behalf of David Taylor.

## PR preview
**Title:** `fix(storage): fence folder and collection saves after an unrecovered load`

Workspace and collection loads now report missing, valid, recovered from `.bak`, or unrecovered. A read error is no longer mistaken for an empty file. When the startup load is unrecovered, every save of that file is refused for the rest of the process, so partial or empty state can't overwrite the saved copy. In that state, workspace deletion is refused up front, legacy import doesn't merge damaged folder files, and managed-worktree retention doesn't prune. Startup shows one plain-line notice. A valid `.bak` still recovers and saves normally, and a blank legacy file still loads as empty. The source-of-truth doc records the rules and notes that there is no in-app recovery action yet. This PR doesn't change how chats with missing workspaces are displayed. Related to the issue above.

**Proof:**
- **New tests:** on Linux, the 10 cases in `tests/store_load_fence_tests.cpp` pass. They ran in a standalone program built from that file with a minimal test harness, linking only the two stores and a stub for one unused platform call.
- **Before and after:** a damaged-file reproduction overwrote both files on `uam/v5.2-sync-main`. On this branch both saves were refused and both files were unchanged.
- **Syntax checks:** g++ `-fsyntax-only` is clean for `chat_folder_store.cpp`, `resource_collection_service.cpp`, `local_chat_bundle_service.cpp` and `worktree_retention_service.cpp`.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Not run:**
  - `uam_core_tests`, the full native suite, CTest and CI. CMake refuses Linux.
  - A compile of `application.cpp` and `chat_lifecycle_service.cpp`, which need CEF. Both were reviewed by eye.
  - A Windows run.
  - Any GUI check.
  - A test of which notice startup chooses.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 13 suggested, 12 implemented. The declined item was an optional recovery hint in the startup-exit message.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## Screens needing Before/After capture
- **Startup notice:** start the app with a damaged `folders.txt` and no `.bak`, on macOS or Windows.
  - **Before:** the workspaces are missing from the sidebar and there's no notice.
  - **After:** the notice reads "Could not load workspaces: folders.txt is unreadable or invalid…". The workspaces it could parse still show.
- **Refused workspace action:** do the same with damaged files. Toggling, reordering or deleting a workspace, and creating a collection, now fail with a message instead of overwriting the file.
