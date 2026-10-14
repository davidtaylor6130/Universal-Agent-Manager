# Chats, imports and workspaces

Chat services own creation, selection, rename, branch, folder and deletion behavior. A chat's workspace determines its files, provider context and optional Git worktree. Native provider-history import links external sessions to UAM chat identities.

## Flow and invariants

`ChatDomainService` and `ChatLifecycleService` coordinate operations over the chat repository and folder store. Startup/native imports map provider-native identities to local chats; failures must leave the original source intact. Worktree preparation selects a per-chat working tree when configured. Workspace leases and ownership checks protect concurrent save and deletion operations. Folders are one level deep as defined by the release slice.

When editing these paths, trace chat ID, workspace path, provider-native ID and worktree path separately. Do not assume they share a lifetime. Keep destructive cleanup tied to the owning chat and test failed imports, concurrent writes and deletion cleanup.

Startup filtering retains metadata ownership for saved agent-run transcripts even though the ordinary sidebar hides them. The cache belongs to one data root, refreshes from persisted hidden owners, and captures resolved native IDs before an agent runtime is removed. It contributes ownership sets rather than partial replacement summaries; sidebar refreshes preserve matching resolved links until saved metadata supplies a newer native ID. Saved transcript bodies remain unchanged.

Codex discovery reads through the first visible authored user message without collecting transcript or tool state. Valid metadata-only and context-only sessions are ineligible, while complete staging still validates the entire source before publication. Startup diagnostics retain at most 20 display entries but log every sanitized error, including failures after that limit.

Local OpenCode imports privately spool complete exports and map one message at a time into staged storage. Startup decisions retain spool ownership until selection or disposal, and guarded publication rejects incomplete exports. Memory follows the largest complete record, including session metadata or a message; existing refresh and child-history callers retain complete mapped vectors. Remote transcript transport remains separately bounded. Cancellation revokes publication while active staging retains its spool until the reader finishes.

Provider-agent Markdown imports expand leading `~/` paths through the platform path service and validate the generated canonical definition before atomic publication. Invalid definitions, including duplicate skill attachments, cannot replace previous agent bytes or leave a new invalid file. The native import handler retains selected skills and explicit workspace access; global scope needs no chat, while workspace scope requires an available chat. Provider security fields remain blocked and omitted provider fields require acknowledgment. The settings global-picker flow and live GUI acceptance remain pending.

## SVN before first launch

`svn_update_before_chat` defaults to OFF. An explicit new local chat captures `svn_update_pending` when SVN and the update setting are enabled. Existing chats, native imports, historical edit/branch children, selected existing worktrees, temporary side chats and subagents do not acquire that intent. Turning the setting OFF skips the update and preserves normal local Git preparation.

The owned preparation worker checks for the nearest SVN working copy, runs a bounded noninteractive update, checks conflicts, then prepares the existing managed Git snapshot when automatic isolation is enabled. Non-SVN sources skip SVN commands. Update-only chats use the same preparation task and owned save barrier. Concurrent tasks for the same checkout wait; canceled or stale owners cannot publish launch metadata.

Atomic `.svn-launch` receipts bind chat ID, creation time, canonical source, checkout root, URL and repository UUID. Completed receipts avoid repeating a successful update after a failed save or restart. An interrupted receipt blocks another update until the working copy and receipt are manually verified; timeout and cancellation are not evidence that SVN made no changes. There is no automatic rollback of an SVN update.

Managed Git branches, checkpoints and commits remain local. Patch porting checks source conflicts and applies changes back to the original working-copy files. SVN commit remains explicit; scheduling added or deleted paths in SVN is outside this toggle and remains a separate pending capability.

Verified linked-worktree imports group under the canonical main workspace even when a branch-only relative directory does not exist in the main checkout. The native working directory remains the linked-worktree directory, and distinct nested repositories retain their ownership boundary.

## Ownership and checks

- Chat lifecycle/search/title: `src/app/chat_domain_service.*`, `src/app/chat_lifecycle_service.*`, `src/app/chat_search_service.*`, `src/app/chat_title_service.*`.
- Chat storage and identity: `src/common/chat/`, `src/app/native_session_link_service.*`.
- Import and workspace/worktree: `src/app/startup_native_import_service.*`, `src/app/git_worktree_service.*`, `src/app/worktree_launch_preparation_service.*`, `src/common/paths/workspace_lease.h`.
- Focused checks: `tests/startup_native_import_tests.cpp`, `tests/native_import_failure_tests.cpp`, `tests/workspace_lease_tests.cpp`, `tests/deletion_workspace_ownership_tests.cpp`, `tests/git_worktree_chat_base_override_tests.cpp`.

No import or worktree performance budget is recorded. Keep operations asynchronous where the UI expects it and preserve cleanup behavior on every failure path.
