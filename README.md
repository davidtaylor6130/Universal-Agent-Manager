# Universal Agent Manager (Gemini CLI + Dear ImGui)

Desktop UI for managing Gemini CLI chat sessions with a classic layout:
- Left pane: previous chats
- Main pane: switchable `Structured UI` or `CLI Console` conversation view
- Right pane: attached files + Gemini CLI command settings

## Features
- Session list with create/select/delete
- Uses Gemini CLI native session storage from `~/.gemini/tmp/<project>/chats`
- Local app storage is only used for UI metadata overrides (e.g. attached files/settings)
- Async Gemini CLI execution (UI stays responsive)
- Embedded real terminal emulator (`libvterm`) for interactive Gemini CLI sessions
- Per-chat file attachments
- Configurable Gemini command template with placeholders:
  - `{prompt}`
  - `{files}`
  - `{resume}`

Default command template:
- `gemini -p {prompt}`

## Requirements
- CMake 3.20+
- C++20 compiler
- OpenGL
- Gemini CLI installed and available on `PATH`

## Build

### Option A: Fetch dependencies automatically

```bash
cmake -S . -B build -DUAM_FETCH_DEPS=ON
cmake --build build -j
```

### Option B: Use local Dear ImGui + system SDL2

```bash
cmake -S . -B build -DUAM_FETCH_DEPS=OFF -DIMGUI_DIR=/absolute/path/to/imgui
cmake --build build -j

`libvterm` is vendored under `third_party/libvterm` and built automatically.
```

## Run

```bash
./build/universal_agent_manager
```

Optional data directory override:

```bash
UAM_DATA_DIR=/absolute/path/to/data ./build/universal_agent_manager
```

## Notes on Gemini CLI
- Ensure `gemini` runs from your shell first.
- If your Gemini CLI uses a different syntax, update **Command Template** in the right pane.
- The app now resumes native Gemini sessions automatically by appending `--resume <session-id>` when available.
- On startup and after each response, session history is reloaded from Gemini's own chat files.
- In `CLI Console` mode, the center panel runs a real interactive `gemini` terminal instance.
