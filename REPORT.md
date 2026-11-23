# #353 Close remote workspace parity gaps — report

- **Work branch:** `v5/fix-353-remote-workspace-parity`
- **Final SHA:** `5ea7889711e68a577b26bb26cbf0339ecd847611` (commits `9115f23`, `5ea7889` on `origin/uam/v5.2-sync-main`)

## Gaps named by #353

| Gap | Status |
|---|---|
| Target-side project memory | Already done before this branch (V5.1.0 close-out). Runner memory script passes. |
| Remote native-history rescans | Already done before this branch (#761). Not changed. |
| Moving remote chats between workspace folders | Route already existed. Fixed: an SSH chat in (or preparing) its own Git worktree is now refused instead of being relabelled while the provider kept using the old worktree. Documented. |
| Remote folder attachments | Route already existed. Fixed: helper `directory.list` drops a trailing separator (distinct parent); the controller accepts the helper's folder only if it is absolute and names the requested folder. Documented. |

## Review rounds

| Round | Source | Suggested | Implemented | Declined |
|---|---|---|---|---|
| 1 | Independent Opus review (`agent-reports/review-R2`, #353 section) | 4 (2 should-fix, 2 nits) | 3 | 1 |
| 2 | Fresh-context Opus re-check of `5ea7889` | 1 (nit, no blockers or should-fixes) | 0 | 1 |

Declined:
- **R1 #1 (split into separate issues):** directed by the orchestrator not to create issues; two issue previews are below instead.
- **R2 nit (UNC `..` can pop the server name in `LexicalRemotePath`):** nit only, and the step applies only blockers and should-fixes. Edge case: a UNC request containing `..` past the share. Candidate follow-up: treat `//server/` as the root, or refuse UNC requests containing `..`.

R1 #2 note: covered by a native test that lists through the in-process runner handler and asserts the match check refuses relative, empty and different replies; not a full staging-handler run (that function is file-private).

## Issue previews

### Move SSH chats between workspace folders on the same computer

**Scope**
Split from #353. An SSH chat can move to another workspace folder registered on the same computer. It keeps its history, goals and computer; its next provider session starts fresh in the new folder with the earlier conversation handed over.

**Acceptance criteria**
- The helper confirms the destination folder exists on the selected computer first; nothing falls back to a controller path.
- Moving to another computer is refused; the computer stays part of the chat's identity.
- Refused while running, with queued, pending or held input, or in (or preparing) its own Git worktree. A refusal shows its reason and leaves the saved chat unchanged.
- Known gap: attachment copies staged in the old folder are not moved or cleaned up, and older messages pointing at them don't resolve from the new folder (local moves too).
- Live SSH acceptance on Linux and Windows hosts.

**Status:** Implemented on `v5/fix-353-remote-workspace-parity` with native fixtures. Needs verification: CI compile, live SSH, David's acceptance.

### Attach folders on the SSH host to a chat message

**Scope**
Split from #353. In an SSH chat, Attach remote directory browses the selected computer through its helper and adds the chosen folder as a reference, recorded with the absolute path the helper confirmed. Nothing is copied.

**Acceptance criteria**
- The folder is always chosen on the selected computer, never from the controller's disk.
- The helper's reply must be absolute on the host and name the requested folder after normalisation; otherwise the attachment is refused and nothing is recorded.
- The helper reports a folder without a trailing separator and with a distinct parent.
- Folder references are never deleted by chip removal or chat deletion.
- Delivery to all six providers is checked on a live host.

**Status:** Implemented on `v5/fix-353-remote-workspace-parity` with helper fixtures. Needs verification: CI compile, live SSH per provider, David's acceptance.

## PR preview

(David's pr-format file was not found in the repository or this session; this follows the layout used in earlier rounds.)

**Title:** fix(remote): close remote workspace parity gaps for chat moves and folder attachments (#353)

**Summary**
- Refuse moving an SSH chat that is in, or preparing, its own Git worktree; folder, workspace, worktree and native session stay unchanged.
- Helper `directory.list` strips a trailing separator, so the folder and its parent are distinct (fixes Up in the remote browser and the saved attachment path).
- Remote folder attachments accept the helper's folder only when it is absolute on the host and names the requested folder (`LexicalRemotePath`, `RemoteDirectoryMatchesRequest`).
- Docs: SSH workspace menu actions in `docs/chat/composer/workspace-actions.md` and `docs/chat/composer/attachments.md`, marked Needs verification. Capability rows unchanged: no provider's status changes.

**Tests added**
- Native: `MoveRemoteWorktreeChatRefusesWithoutChangingSavedIdentity`, `RemoteDirectoryAttachmentAcceptsOnlyTheRequestedHelperDirectory`
- Runner script: `scripts/test_remote_runner_directories.py`, added to the Linux runner CI job

**Proof (what was run, Linux)**
- `cmake -S . -B Builds/runner -DUAM_RUNNER_ONLY=ON -DCMAKE_BUILD_TYPE=Release` + `cmake --build Builds/runner --target uam_runner`: pass (SQLite 3.54.1 local stand-in for the pinned 3.54.0, since sqlite.org was blocked; no repository change)
- `scripts/test_remote_runner_directories.py`: pass; fails without the helper fix
- `scripts/test_remote_runner_memory.py`, `scripts/test_remote_runner_closed_bridge.py`: pass (3/3 runner scripts run)
- Standalone compile of `execution_host_config.h` with the new helpers' assertions: pass
- `python3 scripts/check_docs.py`: pass (203 pages, 6 providers × 32 capabilities)
- Not run: native core tests (need CEF; macOS/Windows only), compatibility and SSH-loopback runner scripts, frontend, live SSH

**Review:** 2 rounds — round 1: 4 suggested, 3 implemented, 1 declined (issue split, per orchestrator); round 2: 1 nit suggested, 0 implemented, 1 declined (nit only). No open blockers or should-fixes.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

## Screens needing Before/After capture

No React code changed. These existing screens now show different native outcomes:
- Chat composer → Workspace actions menu of an SSH chat in its own Git worktree → Move chat: now shows the refusal "A chat in its own Git worktree cannot be moved to another workspace. Start a new chat in that workspace instead."
- Attach remote directory browser: Up after opening a folder typed with a trailing `/` now goes to the real parent.
