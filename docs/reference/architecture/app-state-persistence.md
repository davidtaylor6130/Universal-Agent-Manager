# App state and persistence

> Engineering reference retained from the trial base (`3a7c8477`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../../README.md); its agreements and [documented corrections](../../documentation-review.md) take precedence over stale claims here.

UAM keeps chat and settings data locally and projects a snapshot into React. `Application` owns the native app lifecycle and `PersistenceCoordinator` coordinates loading and saving. Chat, folder and settings models are shared by app services and serializers.

## Flow and invariants

At startup, paths resolve through `UAM_DATA_DIR`, the working directory, the OS app-data path, then a temporary fallback. The coordinator loads settings, folders and chats; `StateSerializer` emits frontend state. Mutations go through domain services and repositories, then persist to the data root. Chat files have `.bak` companions. The backend is the authority for persisted values; frontend state is a working view reconciled from native replies and pushes.

Keep workspace ownership explicit when loading or saving chats. Do not move app data into the UI bundle or treat `UI-V2/dist` as source. A new persisted field needs model normalization, settings serialization, frontend state/action mapping and focused round-trip coverage.

## Ownership and checks

- Native lifecycle and orchestration: `src/app/application.*`, `src/app/persistence_coordinator.*`.
- Models, chat repository and settings: `src/common/models/`, `src/common/chat/`, `src/common/config/`.
- Data paths and state JSON: `src/common/paths/`, `src/cef/state_serializer.*`.
- Focused checks: `tests/core_tests.cpp`, `tests/state_workspace_snapshot_tests.cpp`, `tests/workspace_chat_save_ticket_tests.cpp`, `tests/storage_retention_tests.cpp`.

No measured whole-app persistence latency budget is recorded here. Keep writes bounded and preserve failure handling; use the focused tests before changing file formats or save ownership.

Cold summary regeneration atomically publishes caches derived from primary chat files without disk
durability syncs. Missing, corrupt or stale caches fall back to the primary file. Normal chat saves,
prepared saves and metadata-only summary updates remain durable; those summaries can hold recency,
attention and snooze values absent from the primary. The cold 1,000-chat catalogue has a one-second
service budget in `tests/interaction_performance_tests.cpp`, separate from whole-app startup.

## Computer-scoped defaults

`host_scoped_settings` persists optional provider defaults, the default new-chat provider and MCP server lists for configured SSH computers. Missing overrides inherit global values; an explicitly empty MCP list disables inherited servers for that computer. `scopeExecutionHostId` selects a configured SSH host, while an omitted, empty or `*` scope selects global settings. Invalid or removed hosts cannot become save targets, and loading settings removes obsolete host overrides.

`hostScopedSettingsValues` exposes resolved defaults to the frontend. Scoped saves change the selected host without replacing global settings; frontend saves send only changed provider fields and ignore replies for an obsolete scope. MCP projection filters servers belonging to other hosts, and provider context preparation uses the resolved launch host. New chats explicitly assigned to SSH computers initialize and fall back to the effective host defaults while preserving explicit user choices. Automatically routed chats retain creation-time inheritance markers for model, reviewer, reasoning and service tier when the host is unknown, then resolve those fields after host assignment. Failed assignment persistence restores the prior values and inheritance markers.
