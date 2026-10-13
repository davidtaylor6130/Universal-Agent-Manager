# App state and persistence

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
