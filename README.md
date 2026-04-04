<h1>
  <img src="assets/app_icon.png" alt="Universal Agent Manager icon" width="36" valign="middle" />
  Universal Agent Manager (UAM)
</h1>

A local-first desktop application for managing CLI-driven AI agent workflows across multiple providers.

[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Windows-blue)](https://github.com/davidtaylor6130/Universal-Agent-Manager)
[![Language](https://img.shields.io/badge/language-C%2B%2B20-green)](https://github.com/davidtaylor6130/Universal-Agent-Manager)
[![UI Framework](https://img.shields.io/badge/UI-Dear%20ImGui-purple)](https://github.com/ocornut/imgui)
[![License](https://img.shields.io/badge/license-UAML%20v1.0-orange)](LICENSE)

## Support Matrix

### Provider Feature Support

| Provider | ID | Structured | CLI | Interactive | Universal History |
|----------|:--:|:---------:|:---:|:-----------:|:----------------:|
| **Gemini Structured** | `gemini-structured` | ✅ | ❌ | 🔜 | 🔜🛠️ |
| **Gemini CLI** | `gemini-cli` | ❌ | ✅ | 🔜 | 🔜🛠️ |
| **Codex CLI** | `codex-cli` | ❌ | ✅ | 🔜 | 🔜 |
| **Claude CLI** | `claude-cli` | ❌ | ✅ | 🔜 | 🔜 |
| **OpenCode CLI** | `opencode-cli` | ❌ | ✅ | 🔜 | 🔜 |
| **OpenCode Local** | `opencode-local` | ❌ | ✅ | 🚫 | 🔜 |
| **Ollama Engine** | `ollama-engine` | ✅ | ❌ | 🚫 | 🔜 |

### Symbol Legend

| Symbol | Status | Meaning |
|:------:|:------:|---------|
| ✅ | **Current** | Implemented and working |
| ❌ | **Current** | Not available for this provider |
| 🔜 | **Future** | Planned, not yet implemented |
| 🔜🛠️ | **Future** | In active development |
| 🚫 | **Future** | Explicitly not planned |

### View Definitions

| View | Description |
|------|-------------|
| **Structured View** | Simple LLM query (e.g., `gemini -p "prompt"`). No tool use, no agents, just a prompt → response. Chat bubble UI with history. |
| **CLI View** | Embedded terminal (libvterm/ConPTY) running the provider's CLI directly. Full terminal experience, real-time output, progress bars, streaming. |
| **Interactive** *(future)* | CLI View but with a polished UI overlay. Terminal power with chat UI convenience — stream output to bubbles, capture tool calls, show progress, but still have full CLI capability. |

### What is Interactive Mode?

**Structured View** = Single-shot prompt → response. Like calling a function with text in, text out.

**CLI View** = Full terminal running the provider's CLI. Real terminal window inside the app, streaming output, multi-turn, interactive prompts.

**Interactive Mode** (future) = The best of both. CLI power with UI polish:
- Terminal output streams into chat bubbles
- Tool calls captured and displayed beautifully
- Progress indicators and streaming text
- But still full CLI capability underneath

### History & Storage

| Mode | Purpose | Notes |
|------|---------|-------|
| **Local History** | **UAM source of truth** | All chats stored in `<data-root>/chats/`. This is where chats live long-term. |
| **Native History** | **Active session only** | Provider's native format (e.g., Gemini JSON). Only used while actively using the AI. Not for long-term storage. |
| **Path Bootstrap** | **Active session only** | Injects `@.gemini/path` references. Only during active sessions, not for storage. |
| **Universal History** | **Cross-provider chats** | UAM-normalized chat format that works across CLI agents. One chat, swap providers. |

### What is Universal Chat History?

UAM stores chats in its own format, enabling provider switching:

- **Start a chat with Gemini CLI**
- **Switch mid-conversation to Claude or Codex**
- **Continue the same chat with a different provider**

Context and conversation history are preserved across providers.

**How it works (architecture-driven):**
- UAM stores chats in its own normalized format
- Tool calls, decisions, and context are captured uniformly
- When you switch providers, UAM reconstructs context for the new CLI

**Current support:**
- Gemini: **Partial** — basic conversation continuity works
  - 🛠️ Tool use: In progress
  - 🛠️ Sub-agents: In progress
- Other providers: **Future** — core switching works, deeper integration planned

> ⚠️ **Important:** Native History and Path Bootstrap are temporary — they exist only while you're actively using the AI. For long-term chat storage, always use Local History (UAM's format).

## Quick Start

```bash
# Build with dependencies
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON
cmake --build Builds --config Release

# Run
./Builds/universal_agent_manager
```

## Key Features

- **Multi-Provider Support** — Seamlessly switch between Gemini, Codex, Claude, OpenCode, and Ollama
- **Flexible Views** — Structured chat UI or embedded terminal for each provider
- **Local-First Storage** — JSON-based local storage with no cloud dependencies
- **Provider Profiles** — Configure providers via `providers.txt` without modifying code
- **Workspace Templates** — Materialize markdown templates into workspace `.gemini` directories
- **RAG Support** — Optional retrieval-augmented generation via Ollama engine

## Project Goals

- Local-first operation with JSON-based state
- Auditable behavior with explicit command execution
- Provider-native history when an adapter is available
- No cloud backend, no telemetry, no sync service
- Reproducible workspace-driven CLI runs

## Architecture

### Diagram Legend

```text
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   Subgraph      │ ──▶ │   Component     │ ──▶ │   Component     │
│   (Module)      │     │   (Class/File)  │     │   (Class/File)  │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

```mermaid
flowchart TB
  subgraph APP["Boot + App Shell"]
    MAIN["main.cpp"]
    APPLICATION["Application"]
    STATE["uam::AppState"]
    MAIN --> APPLICATION --> STATE
  end

  subgraph PROVIDERS["Provider Runtime"]
    PROFILES["ProviderProfileStore"]
    RUNTIME["ProviderRuntime"]
    REGISTRY["ProviderRuntimeRegistry"]
    IPR["IProviderRuntime"]
    GBASE["GeminiBaseProviderRuntime"]
    GSTRUCT["GeminiStructuredProviderRuntime"]
    GCLI["GeminiCliProviderRuntime"]
    CODEX["CodexCliProviderRuntime"]
    CLAUDE["ClaudeCliProviderRuntime"]
    OC_BASE["OpenCodeBaseProviderRuntime"]
    OPENCODE["OpenCodeCliProviderRuntime"]
    OPLocal["OpenCodeLocalProviderRuntime"]
    OLLAMA_RT["OllamaEngineProviderRuntime"]
    PROFILES --> RUNTIME --> REGISTRY --> IPR
    REGISTRY --> GSTRUCT & GCLI & CODEX & CLAUDE & OPENCODE & OPLocal & OLLAMA_RT
    GBASE --> IPR
    GSTRUCT & GCLI --> GBASE
    OC_BASE --> IPR
    OPENCODE & OPLocal --> OC_BASE
  end

  subgraph HISTORY["History + Persistence"]
    LOCAL["LocalChatStore"]
    CHAT_REPO["ChatRepository"]
    GEM_HISTORY["GeminiJsonHistoryStore"]
    SETTINGS["SettingsStore"]
    FOLDERS["ChatFolderStore"]
    TEMPLATES["MarkdownTemplateCatalog"]
    IPR --> LOCAL
    GSTRUCT & GCLI --> GEM_HISTORY
    LOCAL --> CHAT_REPO
    STATE --> SETTINGS & FOLDERS & TEMPLATES
  end

  subgraph PLATFORM["Platform Services"]
    FACTORY["PlatformServicesFactory"]
    ITERM["IPlatformTerminalRuntime"]
    IPROC["IPlatformProcessService"]
    IFD["IPlatformFileDialogService"]
    WTR & MTR["TerminalRuntimes"] --> ITERM
    WPS & MPS["ProcessServices"] --> IPROC
  end
```

## Data Layout

```
<data-root>/
├── settings.txt
├── folders.txt
├── providers.txt
└── chats/
    ├── <chat-id-1>.json    # Full chat session (metadata + messages)
    ├── <chat-id-2>.json
    └── ...
```

### Data Root Resolution

1. `UAM_DATA_DIR` environment variable (if set)
2. `<current-working-directory>/data`
3. OS default app-data location
4. Temp fallback

## Dependencies

- CMake 3.20+
- C++20 compiler
- OpenGL
- SDL2
- Dear ImGui
- libvterm (vendored)
- libcurl
- llama.cpp (fetched via `UAM_FETCH_LLAMA_CPP`, required for Ollama Engine)

## Build

```bash
# Self-contained (fetches dependencies)
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON
cmake --build Builds --config Release

# Custom dependencies
cmake -S . -B Builds -DUAM_FETCH_DEPS=OFF -DIMGUI_DIR=/path/to/imgui
cmake --build Builds --config Release
```

### Runtime Options

| Option | Default | Description |
|--------|---------|-------------|
| `UAM_FETCH_DEPS` | ON | Fetch SDL2 and Dear ImGui |
| `UAM_FETCH_LLAMA_CPP` | ON | Fetch pinned llama.cpp fork |
| `UAM_BUILD_TESTS` | OFF | Build test executable |

## ⚠️ Critical: Provider Disable Flags

**If you do not want UAM to be able to call or use a specific provider, you MUST disable it at build time.** Disabled providers are completely excluded from the binary and cannot be invoked.

### Disable All External Provider Calls

To build UAM with **no external CLI providers** (only Ollama Engine):

```bash
cmake -S . -B Builds -DUAM_FETCH_DEPS=ON \
  -DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF \
  -DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF \
  -DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF
cmake --build Builds --config Release
```

### Disable Individual Providers

| Provider | ID | CMake Flag |
|----------|----|------------|
| Gemini Structured | `gemini-structured` | `-DUAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF` |
| Gemini CLI | `gemini-cli` | `-DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF` |
| Codex CLI | `codex-cli` | `-DUAM_ENABLE_RUNTIME_CODEX_CLI=OFF` |
| Claude CLI | `claude-cli` | `-DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF` |
| OpenCode CLI | `opencode-cli` | `-DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF` |
| OpenCode Local | `opencode-local` | `-DUAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF` |
| Ollama Engine | `ollama-engine` | `-DUAM_ENABLE_RUNTIME_OLLAMA_ENGINE=OFF` |

> **Note:** `opencode-local` requires both `opencode-cli` and `ollama-engine` to be enabled.

### Tests

```bash
cmake -S . -B Builds/tests -DUAM_FETCH_DEPS=ON -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

## Run

```bash
# macOS
./Builds/universal_agent_manager

# Windows
.\Builds\Release\universal_agent_manager.exe

# Custom data root
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager
```

## Platform Notes

| Platform | Minimum Version | Terminal Implementation |
|----------|-----------------|------------------------|
| macOS | Current | libvterm (openpty/fork/execvp) |
| Windows | Windows 10 1809+ | ConPTY (CreatePseudoConsole) |

## License

This project is licensed under the Universal Agent Manager License (UAML) v1.0.
See [LICENSE](LICENSE) for full terms.

- Copyright remains with David Taylor (davidtaylor6130)
- Free to use and modify
- Cannot be sold as-is
- Redistribution requires attribution
