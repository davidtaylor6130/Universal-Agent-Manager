# Chats, imports and workspaces

> Engineering reference retained from the trial base (`3a7c8477`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../../README.md); its agreements and [documented corrections](../../documentation-review.md) take precedence over stale claims here.

Chat services own creation, selection, rename, branch, folder and deletion behavior. A chat's workspace determines its files, provider context and optional Git worktree. Native provider-history import links external sessions to UAM chat identities.

## Flow and invariants

`ChatDomainService` and `ChatLifecycleService` coordinate operations over the chat repository and folder store. Startup/native imports map provider-native identities to local chats; failures must leave the original source intact. Worktree preparation selects a per-chat working tree when configured. Workspace leases and ownership checks protect concurrent save and deletion operations. Folders are one level deep as defined by the release slice.

When editing these paths, trace chat ID, workspace path, provider-native ID and worktree path separately. Do not assume they share a lifetime. Keep destructive cleanup tied to the owning chat and test failed imports, concurrent writes and deletion cleanup.

Startup filtering retains metadata ownership for saved agent-run transcripts even though the ordinary sidebar hides them. The cache belongs to one data root, refreshes from persisted hidden owners, and captures resolved native IDs before an agent runtime is removed. It contributes ownership sets rather than partial replacement summaries; sidebar refreshes preserve matching resolved links until saved metadata supplies a newer native ID. Saved transcript bodies remain unchanged.

Codex discovery reads through the first visible authored user message without collecting transcript or tool state. Valid metadata-only and context-only sessions are ineligible, while complete staging still validates the entire source before publication. Startup diagnostics retain at most 20 display entries but log every sanitized error, including failures after that limit.

Local OpenCode imports privately spool complete exports and map one message at a time into staged storage. Startup decisions retain spool ownership until selection or disposal, and guarded publication rejects incomplete exports. Memory follows the largest complete record, including session metadata or a message; existing refresh and child-history callers retain complete mapped vectors. Remote transcript transport remains separately bounded. Cancellation revokes publication while active staging retains its spool until the reader finishes. Structured export parsing receives stdout only while stderr is drained separately with bounded memory. Startup discovery continues through sorted session IDs using a list fingerprint and finite remaining-entry budget; a changed list, exhausted budget or lack of progress stops continuation instead of repeating pages.

New selected or direct remote OpenCode imports validate a complete bounded native export before first publication. Discovery remains metadata-only, so unknown empty candidates can remain in the chooser until selected. A verified `messages: []` skips publication without a tombstone, allowing the same native ID to import later after receiving content. Unknown, malformed, incomplete or paginated, unmapped, oversized, canceled and failed exports remain unpublished and retryable. Startup retires only a confirmed-empty selection. Existing owned or active chats remain untouched, and publication revalidates host, workspace, data root, deletion state, ownership and chat incarnation after worker completion.

Provider-agent Markdown imports expand leading `~/` paths through the platform path service and validate the generated canonical definition before atomic publication. Invalid definitions, including duplicate skill attachments, cannot replace previous agent bytes or leave a new invalid file. The native import handler retains selected skills and explicit workspace access; global scope needs no chat, while workspace scope requires an available chat. Unsupported provider security fields remain blocked; a bounded native OpenCode permission subset has a provider-bound import path described in [Permissions and notifications](permissions-notifications.md#provider-bound-agent-imports). Omitted provider fields such as name still require acknowledgment. The settings global-picker flow and live GUI acceptance remain pending.


Selected OpenCode historical-compaction repair is classification-only and uses the exact primary through `LoadLocalChatBounded`; it never substitutes a backup or reimports the whole native session. The bounded loader preserves normal summary metadata overlay while limiting actual primary and summary reads. Publication retains the existing original backup and chat/summary/queue revision guards. Its detailed proof and admission rules are described in [Provider sessions](provider-sessions.md#opencode-compaction-synchronization).

## Local Git and SVN setting scopes

The `No VCS` section contains the `Enable local Git files` row. The existing SVN section contains the `Local snapshot Git` row. Each defaults ON independently; these master controls affect local Git snapshot behavior only and do not disable remote Git operations. Turning local snapshots off does not delete existing worktrees. Existing partial or interrupted owned receipts fail closed; damaged `.git` metadata does not trigger a hidden fallback. Remote Git behavior is unchanged. The corresponding controls are documented separately so the SVN update option and local snapshot option remain distinguishable.

## SVN before first launch

`svn_update_before_chat` defaults to OFF. An explicit new local chat captures `svn_update_pending` when SVN and the update setting are enabled. Existing chats, native imports, historical edit/branch children, selected existing worktrees, temporary side chats and subagents do not acquire that intent. Turning the setting OFF skips the update and preserves normal local Git preparation.

The owned preparation worker checks for the nearest SVN working copy, runs a bounded noninteractive update, checks conflicts, then prepares the existing managed Git snapshot when automatic isolation is enabled. Non-SVN sources skip SVN commands. Update-only chats use the same preparation task and owned save barrier. Concurrent tasks for the same checkout wait; canceled or stale owners cannot publish launch metadata.

Atomic `.svn-launch` receipts bind chat ID, creation time, canonical source, checkout root, URL and repository UUID. Completed receipts avoid repeating a successful update after a failed save or restart. An interrupted receipt blocks another update until the working copy and receipt are manually verified; timeout and cancellation are not evidence that SVN made no changes. There is no automatic rollback of an SVN update.

Managed Git branches, checkpoints and commits remain local. Patch porting checks source conflicts and applies changes back to the original working-copy files. Explicit local SVN commits schedule selected new and missing files after fresh status and containment checks, add parent directories with empty depth, and commit with empty depth to preserve unselected descendants. Deleted directories require individual file selection. Scheduling remains local on failure; verify repository publication before retrying a failed commit response. Remote SVN scheduling and real-checkout acceptance remain pending.

Verified linked-worktree imports group under the canonical main workspace even when a branch-only relative directory does not exist in the main checkout. The native working directory remains the linked-worktree directory, and distinct nested repositories retain their ownership boundary.

## Ownership and checks

- Chat lifecycle/search/title: `src/app/chat_domain_service.*`, `src/app/chat_lifecycle_service.*`, `src/app/chat_search_service.*`, `src/app/chat_title_service.*`.
- Chat storage and identity: `src/common/chat/`, `src/app/native_session_link_service.*`.
- Import and workspace/worktree: `src/app/startup_native_import_service.*`, `src/app/git_worktree_service.*`, `src/app/worktree_launch_preparation_service.*`, `src/common/paths/workspace_lease.h`.
- Focused checks: `tests/startup_native_import_tests.cpp`, `tests/native_import_failure_tests.cpp`, `tests/workspace_lease_tests.cpp`, `tests/deletion_workspace_ownership_tests.cpp`, `tests/git_worktree_chat_base_override_tests.cpp`.

No import or worktree performance budget is recorded. Keep operations asynchronous where the UI expects it and preserve cleanup behavior on every failure path.

## Branch selection and deletion

The branch picker lists literal local Git branches for an owned isolated worktree. A background read-only preflight checks the branch and workspace before changing any runtime. Active turns and pending input reject switching; idle ACP sessions stop cooperatively, and checkout waits for confirmed exit and saved history. A running terminal requires an explicit stop. Checkout rechecks ownership under an exclusive workspace lease and uses `--no-overwrite-ignore` to preserve ignored files; Git versions that reject this option fail the switch.

The worker publishes prepared branch metadata against the captured storage revision. Failed publication attempts to restore the previous branch, and a failed restoration reports that the worktree needs inspection. Discard and port cleanup delete a branch ref only when the original chat-owned branch and path match, preserving selected pre-existing refs. SVN branch switching is unavailable. Creating another chat in a selected existing worktree validates the source chat, expected local directory and host; it copies workspace identity without creating another isolated worktree and rejects automatic routing or a base-branch override.

Deletion publishes transient `deletionProgress` through full updates and fingerprints while its query remains pending through background cleanup. Total counts deduplicated selected chats and workspaces; completed items have finished local storage and native cleanup, including shared-history ownership protection. Staging and metadata commit do not increase the count. Publication uses real counts at most every 50 ms plus final completion, without recursive filesystem counting on the UI thread.

Progress and final publication require the current task, bridge lifetime and data root. Committed cleanup retains its owned snapshot after query or bridge closure. Terminal paths omit progress, and committed deletion still returns success with a recovery status when cleanup cannot finish. Native cleanup failures retain transaction evidence for restart recovery. Private fixtures are in `tests/deletion_progress_tests.cpp`; this contract does not record user deletion acceptance.

Native history refresh applies deletion tombstones within the data root and workspace scope before overlay reconciliation or direct saves. Unreadable deletion records stop refresh; when tombstones exist, an unscoped native session also stops refresh. Asynchronous native history request identities include the data root so an old-root response cannot populate the current database.

## Sidebar render ownership

`FolderTree` retains equal branch-family ID arrays across metadata-only catalogue updates and keeps Shift-click anchors in the existing row reference. `SessionItem` reacts to pane layout changes only when its family’s pane indexes or the pane count change; family changes refresh from the latest stored layout before paint. Selection ranges still follow visible DOM rows, and selected-chat/status subscriptions remain live. Focused render regressions are in `FolderTree.render.test.tsx` and `SessionItem.test.tsx`.

Runtime grace/deadline displays in chat headers and terminal panes share `useCountdownClock`: one wake at grace expiry, updates aligned to displayed-second boundaries while visible, and no scheduled updates past the deadline or after runtime Stop. Focus and visible-window events refresh wall-clock changes without duplicating timers. `RuntimeTimeout.test.tsx` verifies render counts through grace and expiry, clock adjustment, activity reset and Stop. The serialized `FolderTree.performance.test.tsx` enforces the existing 250 ms selection budget with 1,000 mounted rows.
