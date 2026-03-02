# Universal Agent Manager (Gemini CLI + Dear ImGui)

<img src="https://img.shields.io/badge/status-in-development-orange" alt="Status" /> <img src="https://img.shields.io/badge/platform-macOS%2FLinux%2FWindows-blue" alt="Platforms" />

![Universal Agent Manager hero](https://via.placeholder.com/1200x360?text=Universal+Agent+Manager+Desktop+UI)

**Tags:** `#desktop` `#GeminiCLI` `#ImGui` `#OpenGL` `#ConPTY` `#project-management`

## Executive Summary
- **Purpose:** deliver a desktop “wrapper” around Gemini CLI that keeps chat history in the background and exposes structured editing/listing tools, because the default Gemini CLI workflow lacked a good way to edit prompts or manage history.
- **Business value:** removes the friction of re-running Gemini or digging through raw files by persisting folders, metadata, and attachments while still honoring Gemini’s native session logs.
- **Audience:** developers (including the author) who rely on Gemini CLI and need a lightweight UI plug-in to keep folders, edits, and terminal replay within reach.

## What the App Does
_Built out of frustration with Gemini CLI’s lack of editable history, this pluginish wrapper keeps each conversation and its metadata synchronized so you can stop re-running commands just to pick up where you left off._
1. Auto-discovers Gemini sessions from `~/.gemini/tmp/<project>/chats` (via `GEMINI_HOME` or directory heuristics) and mirrors each chat inside a scripted folder and designer-friendly layout.
2. Tracks metadata such as chat titles, folders, attached files, and custom command templates/flags in `UAM_DATA_DIR` so each workspace can stay organized without touching Gemini logs.
3. Offers two center views: a bubble-style structured conversation that replays history with edit/repeat controls plus a fully embedded `gemini` terminal powered by `libvterm`.
4. Runs each Gemini command asynchronously, shows the live command preview, and resyncs the local state after every response so Gemini’s JSON history and UAM metadata stay consistent.

## Architecture & Data Flow
- UI metadata (folders, attachments, settings) is written to `<data-root>/folders.txt`, `<data-root>/chats/<chat-id>/meta.txt`, and `<data-root>/settings.txt`; overriding `UAM_DATA_DIR` lets you keep per-project data under version control or shared storage.
- Gemini messages are sourced from the native JSON logs (`*.json`), so UAM never duplicates the conversation data; it simply applies UI overrides and displays them with enhanced status indicators.
- Commands are templated with `{prompt}`, `{files}`, `{resume}`, and optional `{flags}` placeholders; additional flags (e.g., YOLO mode) are split safely into argv-style arguments before launching.
- The CLI panel uses pseudo-terminals: `openpty/fork` on macOS & Linux, and Windows ConPTY (`CreatePseudoConsole`/`ResizePseudoConsole`) so the `CLI Console` view matches native terminal behavior across desktop OSes.

## Status & Versions
| Version | Date | Highlights | Notes |
| --- | --- | --- | --- |
| 0.1.0 | 2026-02-28 | Initial prototype: chat folders, metadata storage, live Gemini CLI previews, structured view. | macOS/Linux only; CLI console uses `openpty`. |
| 0.2.0 | 2026-03-02 | Added Windows ConPTY support, CLI terminal resizing, and improved README for stakeholders. | Requires Windows 10 1809+ or Server 2019+. |

## Setup & Build Notes
1. **Requirements:** CMake 3.20+, a C++20 compiler, OpenGL headers/libs, SDL2, and Gemini CLI on `PATH`. On macOS the SDL/OpenGL stack is bundled through the `FetchContent` path; on Linux install `libsdl2-dev` or your pkg-config equivalent.
2. **Option A (self-contained):** `cmake -S . -B build -DUAM_FETCH_DEPS=ON && cmake --build build -j` pulls SDL2/ImGui, builds vendored `libvterm`, and links statically.
3. **Option B (custom sources):** `cmake -S . -B build -DUAM_FETCH_DEPS=OFF -DIMGUI_DIR=/absolute/path/to/imgui && cmake --build build -j` and ensure SDL2 is available via pkg-config/`find_package`.
4. **Running:** `./build/universal_agent_manager`. Override the application data folder (useful for pilots or testing) with `UAM_DATA_DIR=/tmp/uam-data ./build/universal_agent_manager` or the Windows path equivalent.

## Operational Workflow
1. Run Gemini CLI from the project so that `~/.gemini/tmp/<project>/chats/*.json` exists; UAM then imports each session into its foldered sidebar.
2. Organize chats via folders, attach workspace files via the right pane, and update prompts/flags before sending. Linked files appear in the preview and inside Gemini command text.
3. Send prompts via the composer (`Send` button or `Ctrl+Enter`). The app builds the configured command (including attached files, `{resume}` / `--resume` when native sessions exist) and executes it asynchronously while keeping UI responsive.
4. When Gemini produces output, UAM reloads the JSON files, reconciles metadata (attachments, folders), and, if the user edited a past prompt, trims the native JSON stream so the history stays coherent.

## Known Issues
- **Session detection race:** The first Gemini run must finish writing its session logs before UAM can find the directory; if UAM shows “session directory not found,” rerun Gemini or refresh the view.
- **Windows prerequisites:** The ConPTY surface requires Windows 10 1809 (Build 17763) / Server 2019 or later. Older builds lack the `CreatePseudoConsole` APIs, so the CLI console will stay disabled.
- **Attachment hygiene:** No file copies are made. If you move or delete attached files while a chat references them, the preview still lists the original path and Gemini sees whatever path is currently valid.
- **Interactive edit race:** Editing a prior user message while a CLI terminal is running may fail because the native Gemini session JSON cannot rewind a live process; stop the terminal beforehand.

## Launch Goals & Evaluation Checklist
| Goal | Success Criteria |
| --- | --- |
| Productivity | Browse Gemini chats, add context files, and iterate on prompts without manual log inspection. |
| Reliability | Gemini history refreshes after each request while edits persist locally even if the JSON log advances. |
| Security | Gemini sessions stay on disk; metadata remains in the chosen data root (no remote sync). |
| Support Readiness | Embedded CLI panel lets raises/testing be replayed without leaving the UI. |

Suggested validation steps:
1. Run `gemini` once so the tmp/chats directory exists, launch UAM, and confirm sessions show up in the sidebar.
2. Attach a file and send a prompt; confirm the “Current Command” preview matches the template and that Gemini output re-syncs the chat.
3. Switch to `CLI Console`, interact with the embedded terminal, then terminate Gemini; verify the terminal closes cleanly and the status line updates.

## Development Notes
- Fonts: ImGui prefers `/Library/Fonts/Inter*.ttf` or system fallbacks; adjust `ConfigureFonts` if targeting Windows/font bundles.
- Theme: `ApplyModernTheme` defines the dark palette so the UI stays consistent across platforms.
- Persistence: `ChatFolderStore`, `ChatRepository`, and `SettingsStore` write plain key-value data, making backups and audits straightforward.

Feel free to share this repo with the team and let me know if you want help packaging a build or drafting a backlog one-pager that highlights Windows support.
