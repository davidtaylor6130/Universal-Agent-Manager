# Provider runtime parity

This matrix tracks the built-in provider contract against the Codex and Gemini baseline. A
provider-specific protocol may expose different wire events, but every runtime must use the shared
UAM chat model and lifecycle.

Status meanings:

- **Supported**: implemented through the provider runtime or a shared UAM service.
- **Upstream limitation**: the provider CLI does not expose the capability on the protocol UAM uses.
- **Not applicable**: the provider protocol does not need a separate implementation.
- **Defect**: a known UAM parity gap. No defect is accepted as release-ready.

## Runtime paths

| Capability | Gemini CLI | Codex CLI | Claude Code | OpenCode CLI | Copilot CLI |
|---|---|---|---|---|---|
| Structured chat | Supported (ACP) | Supported (app-server) | Supported (stream JSON) | Supported (ACP) | Supported (ACP) |
| Terminal fallback | Supported | Supported | Supported | Supported | Supported |
| Concurrent instances | Supported | Supported | Supported | Supported | Supported |
| Start, stop, cancel | Supported | Supported | Supported; stop terminates the stream because the protocol has no wire cancel | Supported | Supported |
| Unexpected disconnect recovery | Supported | Supported | Supported | Supported | Supported |
| Malformed output isolation | Supported | Supported | Supported | Supported | Supported |
| Bounded diagnostics | Supported | Supported | Supported | Supported | Supported |

## Chats and persistence

| Capability | Gemini CLI | Codex CLI | Claude Code | OpenCode CLI | Copilot CLI |
|---|---|---|---|---|---|
| UAM history save/load | Supported | Supported | Supported | Supported | Supported |
| Native session resume | Supported | Supported | Supported | Supported when ACP returns a session ID | Supported when ACP returns a session ID |
| Native history overlay | Supported | Not applicable | Not applicable | Not applicable | Not applicable |
| Rename, delete, select, create | Supported | Supported | Supported | Supported | Supported |
| Pin and branch | Supported | Supported | Supported | Supported | Supported |
| Workspace folders | Supported | Supported | Supported | Supported | Supported |
| Durable memory files | Supported | Supported | Supported | Supported | Supported |
| Provider/model/mode persistence | Supported | Supported | Supported | Supported | Supported |

Chat management, folders, memory, and normalized persistence are shared services. Provider runtimes
only own provider launch arguments, wire protocol handling, and native-session identity.

## Structured interaction

| Capability | Gemini CLI | Codex CLI | Claude Code | OpenCode CLI | Copilot CLI |
|---|---|---|---|---|---|
| Streaming assistant text | Supported | Supported | Supported | Supported | Supported |
| Reasoning/thought display | Supported when emitted | Supported when emitted | Supported when emitted | Supported when emitted | Supported when emitted |
| Tool calls and results | Supported | Supported | Supported | Supported | Supported |
| Interactive permissions | Supported | Supported | Upstream limitation; print mode requires an external `--permission-prompt-tool` MCP integration | Supported | Supported |
| User-input requests | Supported when emitted | Supported | Upstream limitation; stream JSON has no equivalent request event | Supported when emitted | Supported when emitted |
| Runtime model discovery | Supported when emitted | Supported, with cached catalog fallback | Upstream limitation; stream JSON reports the active model only | Supported when emitted, with configured catalog fallback | Supported when emitted |
| Runtime model selection | Supported | Supported | Supported on the next stream launch | Supported | Supported |
| Approval/planning modes | Supported | Supported | Supported on the next stream launch | Supported when emitted | Supported when emitted |
| Sub-agent recognition | Supported | Supported | Supported | Supported | Supported |
| Expandable sub-agent history | Supported when a native child session ID is emitted | Supported when a native child session ID is emitted | Supported when a native child session ID is emitted | Supported when a native child session ID is emitted | Supported when a native child session ID is emitted |

Sub-agent rows always retain the original tool details. Full nested chat history is loaded only when
the provider supplies an addressable child session; otherwise the visible tool call/result is the
complete history available from that provider.

Structured permission events are mediated by UAM: providers start in their restrictive request mode,
and UAM Default, YOLO, Auto Decide, and AI Review decide what happens to each normalized request.
Plan is a hard read-only ceiling and cannot be widened by another mode. Terminal fallback is an opaque
provider-controlled PTY, so it does not claim this mediation and never receives UAM-generated native
bypass flags.

## Platform boundary

| Capability | macOS | Windows |
|---|---|---|
| Structured process transport | Supported and covered by native tests | Supported by the shared process interface and compile guards |
| Terminal transport | Supported through `openpty` | Supported through ConPTY |
| Provider command construction | Shared and covered by provider contract tests | Shared and covered by provider contract tests |
| Live release verification | Required before release | Required on a Windows runner before release |

Provider code must not invoke OS process or terminal APIs directly. Structured runtimes use the
platform process service; terminal runtimes use the platform terminal service.

## Release checks

Run the following before declaring parity complete:

```bash
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

On Windows, initialize MSVC before the CMake commands and perform a live structured and terminal
smoke test for every installed provider.
