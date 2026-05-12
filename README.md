# Universal Agent Manager

Universal Agent Manager is a macOS and Windows desktop app that runs a React/Vite UI inside CEF and connects it to agent CLIs through C++ runtime services.

The app is focused on local CLI-backed agent sessions:

- Built-in CLI providers for Gemini, Codex, Claude Code, OpenCode, and GitHub Copilot.
- Structured chat sessions over provider protocols: Gemini ACP, Codex app-server stdio, Claude stream JSON, OpenCode ACP, and Copilot ACP.
- xterm.js terminal fallback sessions for interactive provider CLIs.
- Chat create, select, rename, delete, pin, branch, provider switch, model selection, approval mode, save, and resume metadata.
- One-level workspace folders used for provider working directories and Gemini history discovery.
- Optional git worktree isolation, status, diff, commit, discard, and port workflows for chat workspaces.
- Local chat metadata, app settings, folders, theme, window/sidebar state, markdown store entries, and durable memory files.
- Multiple concurrent CLI and structured runtime sessions on macOS and Windows.

Removed or intentionally unsupported surfaces include RAG engines, local model engines, templates, Dear ImGui, Linux builds, and checked-in frontend build output.

## Screenshots

Dark theme:

![Universal Agent Manager dark theme](docs/images/V2.0.1-Dark.png)

Light theme:

![Universal Agent Manager light theme](docs/images/V2.0.1-Light.png)

## Requirements

- CMake 3.20+
- C++20 compiler
- Node.js and npm
- Internet access for the first native configure, because CMake fetches CEF and `nlohmann/json`
- macOS with Xcode command line tools, or Windows with MSVC Build Tools initialized
- Provider CLIs on `PATH` for the providers you use:
  - `gemini`
  - `codex`
  - `claude`
  - `opencode`
  - `copilot`

## Frontend

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
```

`UI-V2/node_modules/`, frontend build output, and TypeScript build info files are generated output and are not committed. CMake also builds the frontend into the native build tree before packaging it into the app.

## Build

CMake enforces build directories under `Builds/`, except for CLion default `cmake-build-*` directories:

```bash
cmake -S . -B Builds
cmake --build Builds --config Release
```

Provider runtime flags:

```bash
cmake -S . -B Builds \
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON \
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=ON \
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=ON \
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=ON \
  -DUAM_ENABLE_RUNTIME_COPILOT_CLI=ON
```

All built-in provider runtimes are enabled by default. At least one provider runtime must be enabled.

Gemini-and-Codex-only build:

```bash
npm --prefix UI-V2 ci
cmake -S . -B Builds/GeminiCodex \
  -DUAM_FETCHCONTENT_BASE_DIR=Builds/_deps \
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON \
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=ON \
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_COPILOT_CLI=OFF
cmake --build Builds/GeminiCodex --config Release
```

On Windows, initialize MSVC first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
cmake -S . -B Builds
cmake --build Builds --config Release
```

On Windows for the Gemini-and-Codex-only build:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
cmake -S . -B Builds\GeminiCodex ^
  -DUAM_FETCHCONTENT_BASE_DIR=Builds\_deps ^
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=ON ^
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=ON ^
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF ^
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF ^
  -DUAM_ENABLE_RUNTIME_COPILOT_CLI=OFF
cmake --build Builds\GeminiCodex --config Release
```

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Windows
.\Builds\Release\universal_agent_manager.exe

# Custom data root on macOS
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

Native tests include `uam_core_tests` from `tests/core_tests.cpp` and the `uam_platform_ifdef_guard` CMake script test. On macOS, a full `cmake --build Builds/tests --config Debug` can fail after compilation during CEF app bundle signing or verification; if that happens, build the isolated test target and run CTest:

```bash
cmake --build Builds/tests --target uam_core_tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

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
- Provider profiles and runtime registry: `src/common/provider/`
- Gemini runtime: `src/common/provider/gemini/`
- Codex runtime: `src/common/provider/codex/`
- Claude runtime: `src/common/provider/claude/`
- OpenCode runtime: `src/common/provider/opencode/`
- Copilot runtime: `src/common/provider/copilot/`
- ACP runtime: `src/common/runtime/acp/`
- Terminal runtime: `src/common/runtime/terminal/` plus platform services
- Local persistence: `src/common/chat`, `src/common/config`
- Memory services: `src/app/memory_service.cpp`, `src/app/memory_library_service.cpp`
- Workspace isolation: `src/app/git_worktree_service.cpp`
- VCS commit workflows: `src/app/vcs_commit_service.cpp`

Security and enterprise deployment notes are tracked in `docs/security-enterprise.md`.

## CEF Bridge

The UI talks to native code through `window.cefQuery`; native state updates are pushed back through `window.uamPush`.

Current bridge capabilities include:

- State and chat loading: initial state, chat selection, lazy message loading, and sidebar search.
- Chat lifecycle: create, rename, delete, pin, provider switch, model changes, Codex options, approval modes, auto-approve commands, and per-chat memory toggles.
- Settings: memory settings, editor settings, provider chat defaults, CLI provider version refresh/apply, theme, and clipboard writes.
- Folders and workspaces: create, rename, delete, toggle, browse, open workspace, and open workspace editor.
- Markdown store: browse/set store directory, list entries, create entries, and reveal entries.
- Memory library: list, create, delete, open roots, reveal entries, list scan candidates, and queue scans.
- Git/VCS: worktree status/create/discard/port, changed-file status, diffs, commits, and commit message generation.
- Terminal sessions: start, stop, resize, and write xterm.js CLI input.
- Structured sessions: stage attachments, send prompts, cancel turns, resolve permission and user-input requests, and stop ACP sessions.

## Manual Release Checks

1. Create chats for every enabled provider in different workspace folders.
2. Send prompts in chat view and verify structured output, tool calls, approvals, user input prompts, attachments, model selection, and cancellation route to the right session.
3. Start CLI fallback for two sessions, type into both terminals, and verify output stays scoped to the correct session.
4. Stop one terminal and verify the other keeps running.
5. Rename, pin, branch, and delete chats, then restart and verify metadata persists.
6. Resume prior Gemini and Codex chats and verify native session or thread ids are used where available.
7. Toggle memory settings and verify durable memory files are only written after idle extraction or manual scan.
8. Exercise git worktree create, discard, port, status, diff, and commit flows in a git workspace.
9. Restart and verify sidebar chats restore from local metadata plus Gemini history discovery.
