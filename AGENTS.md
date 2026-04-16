# AGENTS.md

## Release Slice

This repository is currently scoped to the Gemini CLI release slice. Keep the app focused on React/CEF, xterm.js terminal sessions, Gemini CLI save/resume, chat rename/delete/select/create, one-level workspace folders, and multiple concurrent CLI instances on macOS and Windows.

Unsupported providers, structured prompt UI, RAG, templates, VCS panels, local engines, Dear ImGui, and checked-in frontend build output are intentionally removed.

## Build Commands

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run build

cmake -S . -B Builds
cmake --build Builds --config Release
```

Windows must initialize MSVC first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B Builds
cmake --build Builds --config Release
```

## Critical: Build Directory Restriction

CMake enforces the build directory must be inside `Builds/`. Use one of:

- `cmake -S . -B Builds`
- `cmake -S . -B Builds/<name>`

CLion default `cmake-build-*` directories are also accepted.

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Windows
.\Builds\Release\universal_agent_manager.exe

# Custom data root on macOS
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager
```

## Tests

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

Tests use a custom framework in `tests/core_tests.cpp` and Vitest for `UI-V2`.

## Code Style

- Brace style: Allman
- Indentation: 4 spaces, tabs for indentation
- Column limit: 10000
- Use `.clang-format` in the project root

## Architecture

- Entry point: `src/main.cpp` -> `src/app/application.cpp` -> `src/app/application.h`
- React UI: `UI-V2/src`
- CEF bridge: `src/cef/uam_query_handler.cpp`
- Gemini provider: `src/common/provider/gemini/`
- Terminal runtime: `src/common/runtime/terminal/` plus platform services
- Data storage: `<data-root>/chats/` JSON files

## Data Root Resolution

1. `UAM_DATA_DIR` env var
2. `<cwd>/data`
3. OS default app-data location
4. Temp fallback
