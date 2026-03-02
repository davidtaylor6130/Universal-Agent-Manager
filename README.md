# Universal Agent Manager (Gemini CLI + Dear ImGui)

## Executive Summary
- Purpose: provide a polished desktop control surface that lets developers or teams monitor Gemini CLI chat sessions inside a lightweight UI while preserving Gemini’s native session files.
- Business value: enables quicker review, file contextualization, and terminal-driven troubleshooting without forcing team members to memorize CLI arguments or drop into a shell for every session.
- Intended audience: developers or support engineers who already run Gemini CLI in projects and want a structured history, workspace file attachments, and optional terminal replay without leaving the desktop.

## What the App Does
1. Loads Gemini’s native `~/.gemini/tmp/<project>/chats` directory (auto-detected through `GEMINI_HOME` or file system heuristics) and exposes every session in a left-hand folder list.
2. Lets you group chats into folders, attach workspace files, edit titles, and update Gemini command templates/flags from the right-hand pane.
3. Houses a structured conversation view with bubble-style history plus a real embedded `gemini` terminal (via `libvterm`) so you can rerun or poke a session with its own prompt/resume arguments.
4. Tracks Gemini CLI executions asynchronously (non-blocking UI) and automatically refreshes the session history after each run, ensuring local metadata stays in sync with Gemini’s JSON logs.

## Architecture & Data Flow
- UI metadata (folders, attached files, overrides, settings) is stored under `<data-root>/folders.txt`, `<data-root>/chats/<chat-id>/meta.txt`, and `<data-root>/settings.txt`. You can override the data root via `UAM_DATA_DIR` for per-project or enterprise storage.
- Each chat mirrors a Gemini native session whenever possible. Messages are replayed from Gemini’s JSON export (`*.json`) so the app never duplicates Gemini logs; it only writes extra metadata and per-chat linked file lists.
- Gemini commands are templated via settings with placeholders `{prompt}`, `{files}`, `{resume}`, and optional `{flags}`. CLI flags can also specify YOLO mode or arbitrary shell arguments (split intelligently) before being passed to `gemini`.
- CLI terminal mode spins up `libvterm` + a pseudo-tty to run `gemini` with the same template flags, letting you interact with Gemini exactly as you would in a real terminal while still syncing the chat history afterward.

## Setup & Build Notes
1. **Requirements**: CMake 3.20+, a C++20 compiler, system OpenGL headers/libs, and Gemini CLI on `PATH`. On macOS, SDL + OpenGL are bundled automatically; on Linux you may have to install `libsdl2-dev` or similar.
2. **Option A (self-contained)**: `cmake -S . -B build -DUAM_FETCH_DEPS=ON && cmake --build build -j`. This pulls SDL2/ImGui, builds the vendored `libvterm`, and links everything statically.
3. **Option B (custom ImGui/SDL)**: `cmake -S . -B build -DUAM_FETCH_DEPS=OFF -DIMGUI_DIR=/absolute/path/to/imgui && cmake --build build -j` plus make sure SDL2 is discoverable. `libvterm` is always built from `third_party/libvterm`.
4. **Running**: `./build/universal_agent_manager`. Override the app data folder (useful for CI or clean profiles) with `UAM_DATA_DIR=/tmp/uam-data ./build/universal_agent_manager`.

## Operational Workflow
1. Launch Gemini CLI or run a command in the same project directory so the app can detect `~/.gemini/tmp/<project>` and load existing sessions.
2. Create folders, add metadata, and attach files from the right pane before sending prompts. Attached files become part of the command text passed to Gemini as a markdown-style list.
3. Compose prompts in the bottom input area; `Ctrl+Enter` or the `Send` button immediately constructs the configured command, appends `--resume <session-id>` when a native session exists, and runs it async.
4. After Gemini returns, the app reloads all JSON logs, reconciles metadata (e.g., retain attachments, folder assignments), and if needed rewinds edited user messages directly inside native Gemini session JSON.

## Known Constraints & Workarounds
- **Session detection delay**: The app looks for `~/.gemini/tmp/<project>/chats/*.json`. If Gemini hasn’t been run yet, you’ll see a “native session directory not found yet” message—just run Gemini once and restart the UI or hit “Structured View” to refresh.
- **Native session editing**: Editing a previous user message trims Gemini’s JSON to keep history accurate. This can fail if a terminal session is still running; stop the terminal before rewinding.
- **File attachments**: App does **not** upload files. It only records local file paths (no copies). Avoid deleting/moving attached files while a chat references them because the app doesn’t track file renames.
- **Gemini CLI compatibility**: If Gemini changes its flag set, you can use the template editor to inject `{flags}` or fully rewrite the command string. The YOLO toggle simply adds `--yolo`.
- **Terminal mode**: CLI panel uses `openpty` + `fork` and therefore only runs on Unix-like systems. macOS (Bundled) and Linux are supported. Windows is not currently supported until a PTY/terminal layer can be ported.

## Launch Goals & Evaluation Checklist
| Goal | Success Criteria |
| --- | --- |
| Productivity | Managers can browse all Gemini chats in one place, add context files, and iterate on prompts without consulting CLI history files manually. |
| Reliability | Gemini history refresh triggers after each request, and edits persist locally even if the native JSON jumps ahead. |
| Security | Gemini sessions remain on disk (no remote capture); metadata is limited to paths/settings in the local data root. |
| Support Readiness | CLI panel doubles as a debugging terminal, so raises/testing can be reproduced without dropping into an external shell. |

Suggested validation steps:
1. Run `gemini` once in a project so the tmp/chats directory exists, open the app, and verify sessions populate.
2. Attach files and send a prompt; confirm the constructed command in “Current Command” matches expectations and Gemini output re-syncs.
3. Switch to CLI Console, interact with the embedded terminal, then kill Gemini; confirm the terminal shuts down gracefully and status updates.

## Development Notes
- Fonts: ImGui favours `/Library/Fonts/Inter*.ttf` or fallback system fonts; adjust in `ConfigureFonts` if targeting Windows or Linux.
- Theme: The app defines a bespoke dark palette (see `ApplyModernTheme`) so UI colors remain consistent between macOS and Linux builds.
- Persistence: `ChatFolderStore`, `ChatRepository`, and `SettingsStore` each write simple key-value files, making it easy to back up, inspect, or version control local metadata.

Feel free to share this repo with the team and let me know if you want help packaging a build or drafting a one-pager for the backlog. 
