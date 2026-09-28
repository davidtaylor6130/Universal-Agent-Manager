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

Failed initialization or session setup stops the stale structured transport and clears pending
setup state. Unsent prompts remain queued for reconnection. Invalid saved-session IDs still use
the provider's existing fresh-session retry before this terminal failure path.

**OpenCode native activity remains incomplete:** the native TUI uses cursor-based footer
updates that generic prompt scanning cannot interpret. The source now reports unknown activity
as Connected rather than claiming Busy or Ready. Unknown activity prevents automatic interruption,
idle shutdown and provider replacement, and keeps an active machine awake. This does not identify
actual completion. An isolated OpenCode 1.18.20 check confirmed exact-session busy/idle events and
recovery snapshots through the TUI worker's authenticated loopback API; the production observer,
reconnect/input handling and remote transport remain unimplemented. Installed alpha18 retains the
older behavior. Static status icons are intentional; restoring animation would not fix activity.

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

**OpenCode native session binding:** new terminal sessions obtain an explicit ID through
ACP initialization and `session/new`, close that setup process, and save the ID before launching
the configured native CLI with `--session`. Setup sends no model prompt or MCP configuration.
Reattachments share setup; stopping or switching to structured chat cancels it. Failed saves
retain the ID for retry and prevent CLI launch. Session-selection and fork flags in provider
configuration are rejected so they cannot override the saved binding. The former UAM-history
guessing scans have been removed. OpenCode 1.18.20 was verified to open and resume the same empty
session across an isolated application restart without creating a duplicate.

Remote creation uses the same handshake over the existing runner transport, with a unique
temporary process and a 60-second transient lease. Completion revalidates the chat and host
configuration before saving the ID. Cleanup errors retain a known ID; an unknown ID after a lost
creation response remains an error and may leave an empty native session. The 30-second handshake
deadline does not bound connection, write or cleanup RPC timeouts. Framed fake-SSH tests cover
Windows-form paths, correlated responses, cancellation, provider failures and cleanup failures;
actual SSH execution remains unverified.

OpenCode terminal launches require enabled resume support and a nonblank resume argument on both
local and remote hosts. Invalid configuration is rejected before runtime handoff or SSH launch,
so a terminal cannot silently start a different session while retaining the saved UAM binding.

Previously unbound native conversations and session changes made inside the native CLI still
require separate verification/recovery. An isolated macOS GUI check with OpenCode 1.18.20
generated two native turns against a deterministic loopback model fixture, imported both into
Chat, and retained all four messages and the same native ID across application restart and CLI
resume. The profile contained no inherited provider credentials; upstream model service behavior
was not tested.

New local Codex terminals still discover sessions through the native index. Repeated records for
one ID are deduplicated; genuinely different candidates in the same workspace remain ambiguous.
This does not prove ownership when concurrent unbound terminals see the same first candidate.
An isolated Codex 0.153.3 probe could not create an empty durable session for later native resume:
`thread/start` returned an ID, but a fresh process could not resume it before any prompt was sent.
Exact concurrent native-session binding therefore remains incomplete.
When saving a discovered Codex binding fails, the existing deferred-save queue retries the same
in-memory session ID, including after the terminal closes. It does not rediscover a replacement ID.

Returning from Terminal to Chat requests a native transcript refresh for bound local or remote
Codex and OpenCode sessions. Ordinary history reads retain their cached path. Refreshes preserve
active structured work, discard superseded exports, revalidate chat/host identity, and save before
replacing the cached transcript. Shorter snapshots retain the existing history. Frontend race and
native persistence tests cover this path. The local OpenCode GUI check above also verifies native
generation-to-Chat refresh. Local export passes the session ID before `--pure`: OpenCode ignores
an ID after `--` and opens an interactive picker, which previously caused refresh timeouts.
Both local and remote export reject option-like session IDs before launching a process.
Codex and actual remote generation-to-Chat verification remain open.
Superseded root and child refreshes cancel active remote Codex/OpenCode polls and request cleanup
of their temporary export process. Requests canceled before execution do not start a bridge.
Connection, write, acknowledgement and cleanup RPCs retain their existing timeouts; these changes
do not establish a total cancellation deadline on an unavailable host.

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

Codex model discovery follows `nextCursor` until completion, preserving the last complete catalog
while pages arrive. Failed or malformed pages and repeated cursors discard partial discovery;
completed discovery does not replace the model selected by an already-ready chat. The protocol is
specified in the [Codex app-server model list](https://learn.chatgpt.com/docs/app-server#list-models-modellist).

Sub-agent rows always retain the original tool details. Full nested chat history is loaded only when
the provider supplies an addressable child session; otherwise the visible tool call/result is the
complete history available from that provider.

Local Codex child opening reads the matching rollout in the background from active history or
archives, then validates the parent and applies the snapshot on the GUI thread. New-session detection only considers active history.
Local rollout imports preserve function and custom-tool calls, their arguments, and matched outputs
using Codex call IDs. Built-in shell and web-search records retain their action details and native
status; shell output also matches legacy IDs. Structured output arrays are retained as JSON in the
tool details. Plaintext reasoning summaries populate existing thinking blocks; encrypted-only
records are skipped. Other rollout item types still need coverage.
The viewer re-imports child history at completion; bulk sidebar imports still exclude Codex children.
Deletion records prevent a refresh from recreating a deleted child. Continuous native-file updates
remain unverified: periodic `getChatMessages` reads the UAM transcript. Local OpenCode child
opening and explicit refresh now export native history in the background, validate the parent
again on completion, and apply deletion-aware imports without overwriting active child work.
Refreshes also reject changes to the child transcript during export and preserve matching local
attachment/context details when history grows. Failed opens restore the full previous in-memory
chat and leave previously saved child files intact. When an import resolves to a different child
than the cached session link suggested, selection and rollback use the actual imported child.
Missing native timestamps retain known activity times on refresh.
The export/import services are fixture-tested and the CEF path compiles; live GUI/provider
verification remains outstanding. Remote Codex and OpenCode child opening/refresh reuse the
remote transcript readers, then revalidate the source chat and host connection configuration.
Imports preserve host identity, saved titles, deletion records and active child work. Shared
service tests cover both providers with Linux and Windows paths; live SSH/GUI verification
remains outstanding. Other providers do not yet expose remote child history through this path.
See [chat access](../src/cef/chat_config_handlers.cpp) and [message hydration](../src/cef/chat_handlers.cpp).

OpenCode structured launches currently deny its `task` tool because child-session permission
requests can hang. Recognition and history rendering do not establish full provider-spawned
sub-agent parity while that launch restriction remains. See
[the OpenCode launch environment](../src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp).
Verified against installed OpenCode 1.18.20: its tagged
[permission handler](https://raw.githubusercontent.com/anomalyco/opencode/v1.18.20/packages/opencode/src/acp/permission.ts)
returns without answering when the requesting child session is not registered.
[Upstream fix PR 37902](https://github.com/anomalyco/opencode/pull/37902) remains open as of
2026-09-05. Recheck this path before enabling the tool; a closed issue alone is insufficient.

Structured permission events are mediated by UAM: providers start in their restrictive request mode,
and UAM Default, YOLO, Auto Decide, and AI Review decide what happens to each normalized request.
Plan is a hard read-only ceiling and cannot be widened by another mode. Terminal fallback is an opaque
provider-controlled PTY, so it does not claim this mediation and never receives UAM-generated native
bypass flags.

## Platform boundary

Remote process, terminal and file requests encode paths as UTF-8 and decode them explicitly at
the runner. Workspace resolution, attachment staging and directory browsing use the same
conversion helpers, avoiding Windows system-code-page conversions. Existing bridge and terminal
tests now exercise Unicode paths; Windows execution remains a separate release gate.

Direct protocol probes on macOS, 2026-09-05, used the installed CLIs with temporary homes and
workspaces, no inherited credentials, and no model prompts:

- Codex 0.153.3 accepted initialization, `model/list`, and `thread/start` using UAM's request
  shapes. Resuming the newly created empty thread returned `no rollout found for thread id`,
  which UAM's existing invalid-session recovery recognizes. Resume with persisted turns remains
  unverified by this probe. The handshake follows the
  [Codex app-server protocol](https://learn.chatgpt.com/docs/app-server#initialization).
- OpenCode 1.18.20 accepted initialization, `session/new`, mode/model changes through both
  dedicated methods and `session/set_config_option`, `session/load`, and `session/resume`.
  It reported model and mode choices through `configOptions`, without legacy `models`/`modes`
  objects. The shared parser now applies their current values to runtime state while retaining
  the user's saved selections for startup reconciliation. Loading the empty session reset the
  provider's choices to its defaults; this probe did not test persistence after a model turn.

These checks exercise the installed providers directly, not the UAM GUI, streaming, permissions,
or SSH transport. OpenCode required a local socket outside the command sandbox.

Subsequent isolated GUI tests exercised native Codex/OpenCode output and theme/error recovery.
MacOS terminal launch now acquires a controlling TTY in a fresh bootstrap process before executing
the provider. A real-process regression verifies foreground ownership, inherited signal reset and
resize-triggered SIGWINCH. Expanding and restoring the isolated Session Audit window also
redrew the native OpenCode layout correctly. Replacing the user-used Input Audit build remains
subject to its pending restart approval.

Codex 0.153.3 ignores Escape after final-answer text starts streaming, while Ctrl+C still interrupts.
Timed tests reproduce this through the compiled UAM terminal runtime with both keyboard modes,
matching [upstream issue #35348](https://github.com/openai/codex/issues/35348).
UAM retains native key passthrough; no released provider fix has been verified.

| Capability | macOS | Windows |
|---|---|---|
| Structured process transport | Supported and covered by native tests | Supported by the shared process interface and compile guards |
| Terminal transport | Supported through `openpty` and controlling-TTY bootstrap | Supported through ConPTY |
| Provider command construction | Shared and covered by provider contract tests | Shared and covered by provider contract tests |
| Live release verification | Required before release | Required on a Windows runner before release |

Provider code must not invoke OS process or terminal APIs directly. Structured runtimes use the
platform process service; terminal runtimes use the platform terminal service.

Shared terminal handlers now dispatch provider launch validation, child environment, native session
creation, resume preparation, ID discovery and input-prompt recognition through `IProviderRuntime`.
Concrete implementations own those policies. Core, terminal and remote tests exercise the public
interface, and `uam_provider_terminal_boundary` rejects implementation-header imports in shared
terminal consumers and tests. App/config/CEF-chat/ACP consumers still contain provider details;
this scoped guard does not establish complete repository-wide isolation.

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
