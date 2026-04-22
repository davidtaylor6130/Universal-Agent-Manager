# UAM Project Guide

## Project Overview

Universal Agent Manager (UAM) is a native desktop AI chat and agent application. The C++ backend owns provider runtimes, local persistence, process management, and the CEF window. The React frontend in `UI-V2/` is loaded from bundled files inside CEF and talks to C++ through `window.cefQuery` and `window.uamPush`.

The active release slice supports Gemini CLI and Codex CLI. Both have structured chat paths through ACP-style stdio runtimes and an xterm.js CLI fallback path.

## Architecture

```text
src/                                 C++ backend (CMake target: universal_agent_manager)
  main.cpp                           CEF multiprocess entry and Application::Run
  app/
    application.*                    App lifecycle, CEF init, polling loop
    chat_domain_service.*            Chat CRUD, selection, branching, sorting
    chat_lifecycle_service.*         Chat/folder delete and runtime cleanup
    memory_service.*                 Durable memory extraction and recall preface
    native_session_link_service.*    Native session/thread id linking
    persistence_coordinator.*        Save/load chats, folders, and settings
    provider_resolution_service.*    Provider lookup for chats and defaults
    runtime_orchestration_services.* Native history sync and provider request dispatch
  cef/
    uam_cef_app.*                    CefApp, command line switches, renderer router
    uam_cef_client.*                 CefClient, navigation, context menu, keyboard
    uam_query_handler.*              JS to C++ bridge action dispatch
    state_serializer.*               AppState to frontend JSON
    cef_push.*                       PushStateUpdate / PushCliOutput
  common/
    chat/                            Local chat/folder storage and branch metadata
    config/                          Settings and frontend action metadata
    models/                          Chat, message, folder, provider, memory models
    paths/                           Data-root and provider-history paths
    provider/gemini/                 Gemini CLI runtime and JSON history loader
    provider/codex/                  Codex CLI runtime and thread-id helpers
    runtime/acp/                     Gemini ACP and Codex app-server session runtime
    runtime/terminal/                PTY launch, polling, lifecycle, sync helpers
    platform/                        macOS and Windows process/PTY services

UI-V2/                               React frontend (Vite + TypeScript)
  src/
    App.tsx                          Root component and copy fallback install
    ipc/cefBridge.ts                 window.cefQuery wrapper and CEF detection
    store/useAppStore.ts             Zustand store, CEF bootstrap, uamPush handler
    components/layout/               AppShell, Sidebar, MainPanel
    components/views/                ChatView and CLIView
    components/sidebar/              Folder tree, search, new-chat modal
    components/settings/             SettingsModal
    components/shared/               Logo and theme toggle
    types/                           session, message, provider
    utils/                           Clipboard and theme helpers
```

## CEF IPC Protocol

### JS to C++

```json
{ "action": "<name>", "payload": { "...": "..." }, "requestId": "optional-id" }
```

Current bridge actions:

| Action | Purpose |
| --- | --- |
| `getInitialState` | Return full serialized app state |
| `selectSession` | Select a chat |
| `createSession` | Create a chat in a workspace folder with a provider |
| `renameSession` | Rename a chat |
| `setChatPinned` | Pin or unpin a chat |
| `setChatProvider` | Switch between enabled providers |
| `setChatModel` | Set ACP/Codex model id for a chat |
| `setChatApprovalMode` | Set ACP approval mode (`default` or `plan`) |
| `setChatMemoryEnabled` | Toggle memory extraction/recall per chat |
| `setMemorySettings` | Persist memory defaults, idle delay, recall budget, worker bindings |
| `deleteSession` | Delete a chat and reconcile branch children |
| `createFolder` | Create a one-level workspace folder |
| `renameFolder` | Rename/update a workspace folder |
| `deleteFolder` | Delete a folder and its chats |
| `toggleFolder` | Collapse/expand a folder |
| `browseFolderDirectory` | Native folder picker |
| `startCliTerminal` | Start the xterm.js CLI fallback PTY |
| `stopCliTerminal` | Stop a CLI terminal |
| `resizeCliTerminal` | Resize the PTY |
| `writeCliInput` | Send bytes to the PTY |
| `sendAcpPrompt` | Send a structured chat prompt |
| `cancelAcpTurn` | Cancel an active structured turn |
| `resolveAcpPermission` | Resolve a provider permission request |
| `resolveAcpUserInput` | Answer provider-requested user input |
| `stopAcpSession` | Stop a structured provider process |
| `writeClipboardText` | Native clipboard write, limited to 1 MiB |
| `setTheme` | Persist UI theme |

C++ returns raw JSON in `cb->Success()`. `UI-V2/src/ipc/cefBridge.ts` wraps that into the frontend response shape.

### C++ to JS

| Type | Purpose |
| --- | --- |
| `stateUpdate` | Full serialized app state |
| `cliOutput` | Base64 PTY output bytes for xterm.js |

The store registers `window.uamPush` at module load time so early `OnLoadEnd` pushes are not missed.

## Provider Runtimes

- `gemini-cli`: interactive command `gemini`, structured command `gemini --acp`, native history in Gemini JSON files under the workspace-mapped Gemini tmp directory.
- `codex-cli`: interactive command `codex --no-alt-screen` or `codex resume --no-alt-screen <thread>`, structured command `codex app-server --listen stdio://`, local JSON chat storage.

CMake options:

```bash
-DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON
-DUAM_ENABLE_RUNTIME_CODEX_CLI=ON
```

Both are enabled by default. Removed runtime flags for old structured providers, Claude, OpenCode, Ollama, and RAG intentionally fail configuration.

## Build System

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds
cmake --build Builds --config Release
```

CMake downloads nlohmann/json and the platform CEF binary with FetchContent, builds `UI-V2`, and copies the frontend into the packaged app:

- macOS: `Contents/Resources/UI-V2/dist/`
- Windows: next to the executable at `UI-V2/dist/`

The canonical app outputs are:

```text
Builds/universal_agent_manager.app
Builds/Release/universal_agent_manager.exe
```

Tests:

```bash
cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

## Persistence

```text
<data-root>/
  settings.txt
  folders.txt
  chats/
    <chat-id>.json
    <chat-id>.json.bak
  memory/
    Failures/
      AI_Failures/
      User_Failures/
    Lessons/
      AI_Lessons/
      User_Lessons/
```

Data root resolution:

1. `UAM_DATA_DIR`
2. `<cwd>/data`
3. OS default app-data location
4. Temp fallback

Workspace-local memories are stored at `<workspace>/.codex/memories/`.

## Security Notes

- Bridge messages are accepted only from the trusted bundled `index.html` URL and the main frame.
- Renderer-side `window.cefQuery` is injected only for the trusted UI URL.
- External `http`, `https`, `mailto`, `ftp`, and `tel` navigation is blocked in CEF and handed to the OS.
- Chromium `allow-file-access-from-files` and `disable-web-security` are currently enabled because the packaged React UI is served from `file://`.
- JavaScript clipboard access is enabled, and app copy paths prefer the native `writeClipboardText` bridge in CEF.
- DevTools/view-source shortcuts are blocked; the context menu is reduced to Copy when there is selected text.

See `docs/security-enterprise.md` for the enterprise checklist and known gaps.

## Common Gotchas

1. `UI-V2/dist/` in the source tree is not the packaged output. CMake writes the build-tree frontend bundle and copies it into the native app.
2. `window.uamPush` must stay registered outside React effects so initial state pushes are not missed.
3. Gemini history discovery depends on workspace folders and Gemini's project tmp mapping.
4. Codex resume only uses valid Codex thread ids; invalid ids fall back to a new thread.
5. CEF helper bundles and codesigning on macOS are generated by the CMake post-build steps.
6. The build directory restriction is intentional: use `Builds/` or a CLion `cmake-build-*` directory.
