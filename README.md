# Universal Agent Manager

Universal Agent Manager is a macOS and Windows desktop app that packages a React UI inside CEF and connects it to provider CLIs through C++ runtime services.

The current release slice is intentionally focused:

- Gemini CLI and Codex CLI provider runtimes.
- Chat and CLI views for each session.
- ACP/stdout structured chat flows for Gemini (`gemini --acp`) and Codex (`codex app-server --listen stdio://`).
- xterm.js terminal fallback for interactive provider CLI sessions.
- Chat create, select, rename, delete, pin, provider switch, model selection, approval mode, save, resume, and branch metadata.
- One-level workspace folders used for provider working directories and Gemini history discovery.
- Multiple concurrent CLI terminal instances on macOS and Windows.
- Local chat metadata, app settings, folders, theme, window/sidebar state, and optional memory files.

Unsupported surfaces remain out of scope: non-Gemini/non-Codex providers, RAG engines, templates, VCS panels, local model engines, Dear ImGui, and checked-in frontend build output.

## Screenshots

### V2.0.2

Dark theme:

![Universal Agent Manager V2.0.2 dark theme](docs/images/V2.0.1-Dark.png)

Light theme:

![Universal Agent Manager V2.0.2 light theme](docs/images/V2.0.1-Light.png)

## Requirements

- CMake 3.20+
- C++20 compiler
- Node.js and npm
- Gemini CLI on `PATH` when using Gemini features
- Codex CLI on `PATH` when using Codex features
- macOS with Xcode command line tools, or Windows with MSVC Build Tools initialized

## Frontend

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
```

`UI-V2/node_modules/`, frontend build output, and TypeScript build info files are generated output and are not committed. CMake builds the frontend into the build tree before packaging it into the native app.

## Build

CMake enforces build directories under `Builds/`, except CLion default `cmake-build-*` directories:

```bash
cmake -S . -B Builds
cmake --build Builds --config Release
```

Provider runtime flags:

```bash
cmake -S . -B Builds -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON -DUAM_ENABLE_RUNTIME_CODEX_CLI=ON
```

Both provider runtimes are enabled by default. At least one must be enabled.

Gemini-only build:

```bash
npm --prefix UI-V2 ci
cmake -S . -B Builds/GeminiCLI \
  -DUAM_FETCHCONTENT_BASE_DIR=Builds/_deps \
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON \
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF
cmake --build Builds/GeminiCLI --config Release
```

On Windows, initialize MSVC first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
cmake -S . -B Builds
cmake --build Builds --config Release
```

On Windows for the Gemini-only build:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
cmake -S . -B Builds\GeminiCLI ^
  -DUAM_FETCHCONTENT_BASE_DIR=Builds\_deps ^
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON ^
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF ^
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF
cmake --build Builds\GeminiCLI --config Release
```

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Windows
.\Builds\Release\universal_agent_manager.exe

# Gemini-only macOS
open Builds/GeminiCLI/universal_agent_manager.app

# Gemini-only Windows
.\Builds\GeminiCLI\Release\universal_agent_manager.exe

# Optional custom data root on macOS
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager
```

Do not open `UI-V2/dist/index.html` directly. The frontend is packaged into the CEF shell and expects the native bridge.

## Tests

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

Native tests include `uam_core_tests` from `tests/core_tests.cpp` and the `uam_platform_ifdef_guard` CMake script test.

## Data Layout

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

Workspace-local memories are written under `<workspace>/.codex/memories/` using the same category layout.

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
- Gemini runtime: `src/common/provider/gemini/`
- Codex runtime: `src/common/provider/codex/`
- ACP runtime: `src/common/runtime/acp/`
- Terminal runtime: `src/common/runtime/terminal/` plus platform services
- Local persistence: `src/common/chat`, `src/common/config`
- Memory worker: `src/app/memory_service.cpp`

Security and enterprise deployment notes are tracked in `docs/security-enterprise.md`.

CEF bridge actions in this slice:

- `getInitialState`
- `selectSession`
- `createSession`
- `renameSession`
- `setChatPinned`
- `setChatProvider`
- `setChatModel`
- `setChatApprovalMode`
- `setChatMemoryEnabled`
- `setMemorySettings`
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
- `sendAcpPrompt`
- `cancelAcpTurn`
- `resolveAcpPermission`
- `resolveAcpUserInput`
- `stopAcpSession`
- `writeClipboardText`
- `setTheme`

## Manual Release Checks

1. Create Gemini and Codex chats in different workspace folders.
2. Send prompts in chat view and verify ACP output, tool calls, approvals, user input prompts, and cancellation route to the right session.
3. Start CLI fallback for two sessions, type into both terminals, and verify output stays scoped to the correct session.
4. Stop one terminal and verify the other keeps running.
5. Rename, pin, branch, and delete chats, then restart and verify metadata persists.
6. Resume prior Gemini and Codex chats and verify native session/thread ids are used where available.
7. Toggle memory settings and verify durable memory files are only written after idle extraction.
8. Restart and verify sidebar chats restore from local metadata plus Gemini history discovery.
