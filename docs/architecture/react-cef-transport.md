# React and CEF transport

The React app runs inside the native CEF window. React sends action envelopes through `window.cefQuery`; C++ validates and dispatches them. Native state and terminal output return through `window.uamPush`.

## Flow and invariants

`UI-V2/src/ipc/cefBridge.ts` wraps request/reply transport. `UamQueryHandler` maps action names to handlers, which call app services. `StateSerializer` returns initial state; `cef_push` sends `stateUpdate` or `cliOutput`. The store registers the push callback at module load, before React effects, so the first native push is not lost. Keep action payloads and responses explicit and version-compatible across both ends.

CEF accepts bridge messages only from the trusted bundled page and main frame. Keep bridge work asynchronous when a handler can block. For a new action, update the native dispatch/handler, frontend action caller and relevant transport tests together.

## Ownership and checks

- Frontend transport and state: `UI-V2/src/ipc/cefBridge.ts`, `UI-V2/src/store/useAppStore.ts`, `UI-V2/src/store/push/`.
- Native dispatch and replies: `src/cef/uam_query_handler.*`, `src/cef/*_handlers.cpp`, `src/cef/state_serializer.*`, `src/cef/cef_push.*`.
- Focused checks: `UI-V2/src/store/cpp/`, `UI-V2/src/ipc/companionBridge.test.ts`, `tests/uam_control_transport_tests.cpp`, `tests/state_workspace_snapshot_tests.cpp`.

No stable latency budget is defined for the full bridge. `UI-V2/src/test/interactionBudgets.ts` sets a 250 ms budget for its explicitly covered frontend interactions; do not generalize it to all IPC calls.
