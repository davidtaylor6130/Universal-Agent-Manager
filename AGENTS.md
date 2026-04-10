# AGENTS.md

## Build Commands

```bash
# Build with dependencies (fetches SDL2, ImGui, llama.cpp)
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON
cmake --build Builds --config Release

# Windows: must initialize MSVC first
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON

# Custom dependencies
cmake -S . -B Builds -DUAM_FETCH_DEPS=OFF -DIMGUI_DIR=/path/to/imgui
```

## Critical: Build Directory Restriction

CMake **enforces** the build directory must be inside `Builds/`. Use one of:
- `cmake -S . -B Builds`
- `cmake -S . -B Builds/<name>`

CLion default `cmake-build-*` directories are also accepted.

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Or use the helper launcher
./run_uam.sh

# Windows
.\Builds\Release\universal_agent_manager.exe

# Custom data root on macOS
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager
```

## Provider Disable Flags

Disable providers at build time. Disabled providers are completely excluded from the binary.

```bash
# Disable all external CLI providers (Ollama Engine only)
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON \
  -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF \
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF
```

| Provider | Flag |
|----------|------|
| Gemini Structured | `-DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF` |
| Gemini CLI | `-DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF` |
| Codex CLI | `-DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF` |
| Claude CLI | `-DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF` |
| OpenCode CLI | `-DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF` |
| OpenCode Local | `-DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF` |
| Ollama Engine | `-DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF` |

**Note:** `opencode-local` requires both `opencode-cli` AND `ollama-engine` enabled.

## Tests

```bash
cmake -S . -B Builds/tests -DUAM_FETCH_DEPS=ON -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

Tests use a custom framework in `tests/core_tests.cpp` (not Catch2/GTest).

## Code Style

- **Brace style**: Allman
- **Indentation**: 4 spaces, tabs for indentation
- **Column limit**: 10000
- Use `.clang-format` in project root

## Architecture

- Entry point: `src/main.cpp` → `src/app/application.cpp` → `src/app/application.h`
- Providers: `src/common/provider/*/`
- Terminal runtime: `src/common/runtime/terminal/` (libvterm on macOS, ConPTY on Windows)
- Data storage: `<data-root>/chats/` (JSON files)

## Data Root Resolution

1. `UAM_DATA_DIR` env var (if set)
2. `<cwd>/data`
3. OS default app-data location
4. Temp fallback
