# Report: workspace directory change re-saves every chat

**Work branch:** `v5/fix-workspace-move-chat-resave` at **`9ab2dcdb`** (base `uam/v5.2-sync-main`; merges cleanly into `uam/v5.2-performance`).
Commits: `fbb2f310` (fix), `93e847da` (review round 1), `9ab2dcdb` (re-check round 2). No PR, issue, label or comment was created.

## Change

`RenameFolderById` (`src/app/chat_lifecycle_service.cpp`), plus the path helper `FolderDirectoryRelativeSubpath` (`src/common/paths/app_paths.{h,cpp}`):

- Re-points only chats in the edited folder whose recorded directory (or a worktree chat's source directory) is the old root or below it, keeping the subpath. Paths are compared as written first, then resolved. Remote paths use the host's normalised form, are never resolved on this computer, and keep their recorded spelling.
- A chat already at or inside the new root keeps its directory. When the old and new roots contain one another, only chats at the old root itself follow, so narrowing then widening back reverses the move.
- Saves only the re-pointed chats, so an unrelated chat that can't be saved no longer blocks the change. If a re-pointed chat fails to save, the change is refused naming that chat, the re-pointed chats already saved are written back, and the folder keeps its directory.
- Chats left on another directory are counted in one status line, e.g. "Folder settings saved. 3 chats in this workspace use directories outside the old folder and were left unchanged."
- Chats in other folders are never touched. `chat_repository.cpp` is unchanged.
- Docs: `docs/sidebar/all-chats/workspace-folders.md`, marked **Needs verification**. It records two known gaps: chats that earlier versions left on old paths are not migrated, and a re-pointed local chat keeps its native session.

## Tests (`tests/core_folder_import_tests.cpp`)

- `WorkspaceDirectoryChangeRepointsOnlyChatsUnderTheOldRoot`
- `WorkspaceDirectoryChangeRollsBackWhenAnAffectedChatCannotBeSaved`
- `WorkspaceDirectoryChangeLeavesChatsAlreadyInsideTheNewRoot` (narrow, then widen back)
- `WorkspaceDirectoryChangeReportsWorktreeChatWhoseSourceIsElsewhere`
- `RemoteWorkspaceDirectoryChangeRepointsSubdirectoryChatsWithNativeReset`

**What was run (Linux):**
- `g++ -std=c++20 -fsyntax-only` on `chat_lifecycle_service.cpp`, `app_paths.cpp` and the new test bodies: no errors outside the stand-in CEF headers. The CEF host is blocked, so stand-in headers were used.
- A scratch program linking the real `app_paths.cpp` and the repoint helper's source text: 21 of 21 path cases passed.
- `python3 scripts/check_docs.py`: passed.
- `git clang-format` applied.

**Not run:** the native test binary and the full CTest gates. The desktop/test build refuses Linux, so these need macOS or Windows. Also not checked: live app behaviour, a real network share, live SSH.

## Review rounds

- **Round 1** (independent claude-opus-5-5, fresh context, diff vs `origin/uam/v5.2-sync-main`): 6 suggested (1 blocker, 3 should-fix, 2 nits), 6 implemented.
  - Blocker: nested roots rewrote chats already in the new root.
  - Should-fix: worktree chats miscounted; remote paths resolved locally; docs overstated runs.
  - Nit: shadowed variable, fixed in code.
  - Nit: local re-pointed chats keep their native session. Recorded as a known gap in the docs rather than changed in code, because it is pre-existing behaviour and outside the requested scope.
- **Round 2** (one fresh-context claude-opus-5-5 re-check of the round-1 fixes): all 6 earlier findings verified fixed. 3 new suggested (2 should-fix, 1 nit), 3 implemented.
  - Nested roots still invented paths and couldn't be reversed.
  - The worktree exclusion was too broad.
  - A check was computed twice.
- Round-2 fixes were not re-reviewed, because the brief allowed one re-check.

## Issue preview

**Title:** `bug(workspaces): changing a workspace directory re-saves every chat and misses subfolders`

Changing a workspace folder's directory saves every chat in the app. Any single save failure blocks the change, and only chats whose saved path exactly matches the old directory are moved. Reported from a real install after its repositories moved to a network share.

**Reproduction:** Have a workspace with chats whose working directory is the workspace directory or a subfolder of it, plus any chat anywhere whose save fails. Change the workspace's directory in folder settings.
**Expected:** Only chats under the old directory are moved, keeping their subfolder. Unrelated chats are not saved and can't block the change.
**Actual:** Every chat is saved, and any failure gives "Failed to persist chat workspace updates; folder was kept". Subfolder chats keep the old path and keep launching there.

**Environment:** `uam/v5.2-sync-main`. The user's OS is not recorded.

**Evidence:**
- User-reported: bulk rewrites of 582, 348 and 344 chats within single minutes. 32 chats fail to save on every run. 499 chats in share-path workspaces still record home-folder paths: 75 of those paths no longer exist and 424 point to a different directory.
- Seen in the code: `SaveChatHistories(app, app.chats)` and the exact-match `FolderDirectoryMatches` in `RenameFolderById` (`src/app/chat_lifecycle_service.cpp`). `ResolveWorkspaceRootPath` uses the chat's own path before the folder's (`src/common/paths/workspace_root.h`).

**Reproduction status:** Not reproduced in a running app; confirmed by reading the code.

**Acceptance criteria:**

- [ ] Only changed chats are saved and rolled back.
- [ ] Chats at or under the old directory move with their subfolder kept.
- [ ] Chats left on another directory are reported in one line.
- [ ] An unrelated chat failing to save doesn't block the change; a moved chat failing to save rolls the change back.
- [ ] Chats outside the workspace are never changed.

**Review:** Self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

**Title:** `fix(workspaces): re-point only chats under a moved workspace root`
**Base:** `uam/v5.2-sync-main` ← **Head:** `v5/fix-workspace-move-chat-resave` (`9ab2dcdb`)

Changing a workspace's directory used to save every chat in the app, and was refused if any of them failed to save. It also moved only chats whose path exactly matched the old directory.

Now only chats in that workspace at or under the old directory are moved, keeping their subfolder. Remote paths are compared as the host sees them. A chat already inside the new directory stays put. When the two directories contain one another, only chats at the old directory itself move, so moving the folder back reverses the change.

Only the moved chats are saved. If one of them fails to save, the change is refused, the chat is named, and the chats already saved are written back. Chats left on another directory are counted in the save status, and other workspaces are never touched.

This adds the path helper `FolderDirectoryRelativeSubpath` and updates the workspace-folders doc, which records the remaining gaps.

Related to: the issue above (not yet filed).

**Proof:**
- Syntax-only compiles of `chat_lifecycle_service.cpp`, `app_paths.cpp` and the new tests on Linux, using stand-in CEF headers: no errors.
- A scratch program ran the real `app_paths.cpp` and the matching code from the service file: 21 of 21 path cases passed.
- `python3 scripts/check_docs.py` passed.
- **Not run:** the 5 new native cases and the full native run, because the build refuses Linux. They need macOS or Windows.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 9 suggested, 9 implemented.

- Round 1: 6 suggested, 6 implemented. The native-session nit was handled as a documented known gap, not a code change.
- Round 2: 3 suggested, 3 implemented.

Claude review of the final revision and David's approval are still pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## Screens needing Before/After capture

There is no React/UI code change. Only the native status text shown after a folder edit changes. Optional captures:

- Workspace folder settings after a directory change: the status line, including the "… left unchanged" count.
- The refusal message when a moved chat can't be saved.
