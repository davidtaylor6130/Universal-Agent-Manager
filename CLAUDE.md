# UAM — Claude Code Project Guide

## Project Overview

Universal Agent Manager (UAM) is a native desktop AI chat/agent application. The C++ backend runs provider runtimes, manages chat persistence, and hosts a CEF (Chromium Embedded Framework) window. The React frontend (UI-V2/) is served from `file://` inside that CEF window and communicates with C++ via `window.cefQuery` / `window.uamPush`.

---

## Architecture

```
src/                        C++ backend (CMake target: universal_agent_manager)
  main.cpp                  CEF multiprocess entry — CefExecuteProcess + Application::Run
  app/
    application.cpp/.h      App lifecycle, CEF init, polling loop
    chat_domain_service.*   Chat CRUD operations
    persistence_coordinator.*  Save/load chats and settings
    runtime_orchestration_services.*  Provider request dispatch
  cef/
    uam_cef_app.*           CefApp + CefBrowserProcessHandler + CefRenderProcessHandler
    uam_cef_client.*        CefClient (lifespan, load, display, context menu, keyboard)
    uam_query_handler.*     CefMessageRouterBrowserSide::Handler — all JS→C++ actions
    state_serializer.*      AppState → nlohmann::json
    cef_push.*              PushStateUpdate / PushStreamToken / PushCliOutput
    cef_includes.h          Single include for all CEF headers
  common/
    state/app_state.h       AppState, ChatSession, Message, ChatFolder, CliTerminalState
    runtime/terminal_polling.h  PollCliTerminal — reads PTY → PushCliOutput
    provider/               Provider runtime interfaces and implementations
    platform/               macOS/Windows PTY spawning

UI-V2/                      React frontend (Vite + TypeScript)
  src/
    App.tsx                 Root component — theme sync only (uamPush registered in store)
    ipc/cefBridge.ts        window.cefQuery wrapper; isCefContext() detection
    store/useAppStore.ts    Zustand store — state, actions, CEF bootstrap, uamPush handler
    types/                  session.ts, message.ts, provider.ts
    components/
      layout/               AppShell, Sidebar, MainPanel
      views/                StructuredView, CLIView, CodingAgentView
      input/                ProviderChiplets, MessageInput
      sidebar/              NewChatModal, ChatList
      settings/             SettingsModal
    mock/                   mockData.ts — used only in dev/browser mode
```

---

## CEF IPC Protocol

### JS → C++ (`window.cefQuery`)
```json
{ "action": "<name>", "payload": { ... } }
```

| Action | Payload |
|--------|---------|
| `getInitialState` | — |
| `selectSession` | `{ chatId }` |
| `createSession` | `{ title, folderId, providerId }` |
| `renameSession` | `{ chatId, title }` |
| `deleteSession` | `{ chatId }` |
| `sendMessage` | `{ chatId, content }` |
| `createFolder` | `{ title }` |
| `toggleFolder` | `{ folderId }` |
| `startCliTerminal` | `{ chatId }` |
| `writeCliInput` | `{ chatId, data }` |
| `setTheme` | `{ theme: "dark"\|"light" }` |

### C++ → JS (`window.uamPush(json)`)

| type | Fields |
|------|--------|
| `stateUpdate` | `data`: full CppAppState |
| `streamToken` | `chatId`, `token` |
| `streamDone` | `chatId` |
| `cliOutput` | `chatId`, `data` (base64 PTY bytes) |

**Important**: C++ returns raw payload JSON as the `cb->Success()` body — NOT wrapped in `{ok, data}`. The bridge (`cefBridge.ts`) detects this and wraps it automatically.

---

## Build System

### CEF Build (the one you run)
```bash
# Configure (once)
cmake -S . -B Builds/cef -DUAM_BUILD_TARGET=cef

# Build
cmake --build Builds/cef --config Release

# After React changes only (no C++ recompile needed):
cd UI-V2 && npm run build
rsync -a --delete UI-V2/dist/ Builds/cef/universal_agent_manager.app/Contents/Resources/UI-V2/dist/
```

The correct app to run is always:
```
Builds/cef/universal_agent_manager.app
```

**Note**: macOS Finder shows the `.app` folder's own mtime (creation/structure time), not the binary's mtime. The binary inside (`Contents/MacOS/universal_agent_manager`) is the authoritative timestamp. Always check it with `ls -la` if unsure whether a build is current.

### React Dev (browser preview)
```bash
cd UI-V2 && npm run dev
# Uses mock data — isCefContext() returns false in browser
```

---

## Key Design Decisions

### `window.uamPush` is registered in the store, not in a React component
`useAppStore.ts` registers `window.uamPush` synchronously at module load time (before React mounts). This ensures the C++ `OnLoadEnd` push — which fires before React's `useEffect` would run — is never missed.

### `isCefContext()` determines initial state
- In CEF: store starts with empty `[]`/`{}`/`null` and populates from `getInitialState` response
- In browser/dev: store starts with mock data from `mock/mockData.ts`

### View mode is React-only
C++ does not store `viewMode`. `deserializeState()` assigns `viewMode: 'cli'` for sessions whose `providerId` contains `'gemini'` or `'cli'`, otherwise `'structured'`.

### CEF renderer process requires `CefRenderProcessHandler`
`UamCefApp` inherits from both `CefBrowserProcessHandler` and `CefRenderProcessHandler`. The renderer handler's `OnWebKitInitialized()` creates `CefMessageRouterRendererSide`, which injects `window.cefQuery` into every page. Without this, `isCefContext()` always returns false.

### macOS CEF helper bundles
Four helper `.app` bundles inside `Contents/Frameworks/` are copies of the main binary:
- `universal_agent_manager Helper.app` — referenced as `browser_subprocess_path`
- `universal_agent_manager Helper (GPU).app`
- `universal_agent_manager Helper (Plugin).app`
- `universal_agent_manager Helper (Renderer).app`

Each has a symlink `Contents/Frameworks/Chromium Embedded Framework.framework → ../../../Chromium Embedded Framework.framework` so the CEF dylib resolves via `@executable_path/../Frameworks/`.

### No browser chrome
`UamCefClient` implements `CefContextMenuHandler` (clears all menus) and `CefKeyboardHandler` (blocks F12, Ctrl+Shift+I, Ctrl+U). The app is native-feeling — no Chromium address bar or DevTools shortcuts.

### Codesign
The main binary and each helper are ad-hoc signed with a JIT entitlement (`com.apple.security.cs.allow-jit`). The bundle itself is NOT fully signed (avoids sealed-resources errors from `.woff` fonts). UI dist lives in `Contents/Resources/UI-V2/dist/` (not `Contents/MacOS/`) to prevent codesign from treating font files as code objects.

---

## Provider System

- **gemini-cli**: `UsesCliOutput() = true`, `UsesNativeOverlayHistory() = true` — runs as interactive PTY terminal, output piped to xterm.js in `CLIView`
- **Structured providers**: API-based, stream tokens via `PushStreamToken` → React streaming bubbles
- Provider IDs containing `gemini` or `cli` auto-select `viewMode: 'cli'` on deserialization

---

## Common Gotchas

1. **`resp.ok` check after cefQuery**: C++ returns raw JSON, not `{ok:true, data:...}`. The bridge wraps it. Always verify `resp.ok && Array.isArray(resp.data?.chats)` before deserializing `getInitialState`.

2. **rsync after React-only changes**: cmake's POST_BUILD only copies the dist when C++ recompiles. For React-only changes, manually rsync `UI-V2/dist/` into the bundle.

3. **Finder timestamps are misleading**: The `.app` directory mtime reflects when the folder structure last changed, not when the binary was last rebuilt. Check the binary directly.

4. **CEF version**: `130.1.16+g6a9f117+chromium-130.0.6723.117` (Spotify CDN). Platform detected at configure time (`macosarm64` / `macosx64` / `windows64`).

5. **`--disable-gpu` removed**: GPU compositing is required for Metal rendering on macOS — removing those flags was a prior bug fix. Do not re-add them.
