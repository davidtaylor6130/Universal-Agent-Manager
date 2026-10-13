# Chats, imports and workspaces

Chat services own creation, selection, rename, branch, folder and deletion behavior. A chat's workspace determines its files, provider context and optional Git worktree. Native provider-history import links external sessions to UAM chat identities.

## Flow and invariants

`ChatDomainService` and `ChatLifecycleService` coordinate operations over the chat repository and folder store. Startup/native imports map provider-native identities to local chats; failures must leave the original source intact. Worktree preparation selects a per-chat working tree when configured. Workspace leases and ownership checks protect concurrent save and deletion operations. Folders are one level deep as defined by the release slice.

When editing these paths, trace chat ID, workspace path, provider-native ID and worktree path separately. Do not assume they share a lifetime. Keep destructive cleanup tied to the owning chat and test failed imports, concurrent writes and deletion cleanup.

## Ownership and checks

- Chat lifecycle/search/title: `src/app/chat_domain_service.*`, `src/app/chat_lifecycle_service.*`, `src/app/chat_search_service.*`, `src/app/chat_title_service.*`.
- Chat storage and identity: `src/common/chat/`, `src/app/native_session_link_service.*`.
- Import and workspace/worktree: `src/app/startup_native_import_service.*`, `src/app/git_worktree_service.*`, `src/app/worktree_launch_preparation_service.*`, `src/common/paths/workspace_lease.h`.
- Focused checks: `tests/startup_native_import_tests.cpp`, `tests/native_import_failure_tests.cpp`, `tests/workspace_lease_tests.cpp`, `tests/deletion_workspace_ownership_tests.cpp`, `tests/git_worktree_chat_base_override_tests.cpp`.

No import or worktree performance budget is recorded. Keep operations asynchronous where the UI expects it and preserve cleanup behavior on every failure path.
