# Universal Agent Manager

Universal Agent Manager is currently a Gemini CLI release slice for macOS and Windows. The app packages a React UI inside CEF and embeds Gemini CLI sessions through xterm.js.

This slice is intentionally narrow:

- Gemini CLI only. Legacy provider IDs are normalized to `gemini-cli`.
- Multiple concurrent Gemini CLI terminal instances.
- Chat create, select, rename, delete, save, and resume.
- Resume through Gemini native history when a native session id is available.
- Local UAM metadata for chat titles, folder/workspace roots, selected chat, theme, window, and sidebar state.
- One-level folders used as workspace roots for Gemini history discovery and CLI working directories.

Removed surfaces include alternate providers, structured prompt mode, RAG, templates, VCS panels, local engines, Dear ImGui, and checked-in frontend build output.

## Requirements

- CMake 3.20+
- C++20 compiler
- Node.js and npm
- Gemini CLI available on `PATH`
- macOS with Xcode command line tools, or Windows with MSVC Build Tools initialized

## Frontend

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
```

`UI-V2/node_modules/`, `UI-V2/dist/`, and TypeScript build info files are generated output and are not committed.

## Build

CMake enforces build directories under `Builds/`:

```bash
cmake -S . -B Builds
cmake --build Builds --config Release
```

On Windows, initialize MSVC first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B Builds
cmake --build Builds --config Release
```

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Windows
.\Builds\Release\universal_agent_manager.exe

# Optional custom data root on macOS
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager
```

Do not open `UI-V2/dist/index.html` directly. The frontend is packaged into the CEF shell and expects the native bridge.

## Tests

```bash
cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

The core tests use the custom framework in `tests/core_tests.cpp`.

## Data Layout

```text
<data-root>/
  settings.txt
  folders.txt
  chats/
    <chat-id>.json
```

Data root resolution:

1. `UAM_DATA_DIR`
2. `<current-working-directory>/data`
3. OS default app-data location
4. Temp fallback

## Architecture

- Entry point: `src/main.cpp`
- App shell: `src/app/application.cpp`
- CEF bridge: `src/cef/uam_query_handler.cpp`
- React UI: `UI-V2/src`
- Gemini provider runtime: `src/common/provider/gemini/cli`
- Gemini native history loader: `src/common/provider/gemini/base`
- Terminal runtime: `src/common/runtime/terminal` plus platform services
- Local persistence: `src/common/chat`, `src/common/config`

CEF bridge actions in this slice:

- `getInitialState`
- `selectSession`
- `createSession`
- `renameSession`
- `deleteSession`
- `createFolder`
- `renameFolder`
- `deleteFolder`
- `toggleFolder`
- `browseFolderDirectory`
- `startCliTerminal`
- `stopCliTerminal`
- `resizeCliTerminal`
- `writeCliInput`
- `setTheme`

## Manual Release Checks

1. Create two chats in different workspace folders and start both terminals.
2. Type into both terminals and verify output routes to the correct session.
3. Stop one terminal and verify the other keeps running.
4. Rename a chat, restart, and verify the title persists.
5. Resume a prior Gemini chat and verify the native session id is used where available.
6. Restart and verify sidebar chats restore from local metadata plus Gemini history discovery.
