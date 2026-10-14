# Provider sessions

The app supports Gemini CLI, Codex CLI, Claude Code CLI, OpenCode CLI, GitHub Copilot CLI and Antigravity CLI (terminal only). Provider runtimes hide provider-specific launch and history details behind a registry. Chats use a structured session when available and can use an xterm.js PTY fallback.

## Flow and invariants

The registry resolves a provider runtime. Each runtime builds structured, terminal and worker commands as supported. ACP and provider-specific protocols run over managed processes; updates become normalized messages and activity state. Terminal output streams as bytes to xterm.js and can be synchronized back to chat history. Resume identifiers must remain provider-valid; do not fabricate a native session ID after a failed resume. Keep all six provider implementations and the terminal-only Antigravity boundary.

Startup model discovery preserves cached and existing-chat scopes and the default provider's registered workspace scopes. After restoring chat selection, it also prewarms enabled structured providers for the initial New chat folder, using requested-folder, selected-chat-folder and first-folder priority. Discovery stays limited to two concurrent processes and rechecks host connection, workspace, availability and provider compatibility before launching. Terminal-only Antigravity is excluded, cached choices remain usable during refresh failure, and other previously unused provider/workspace pairs retain manual refresh.

Permissions for structured tool requests pass through the native permission layer before resolution. Stop, cancel, exit and resume state belong to the owning runtime/session, especially when multiple chats run concurrently.

## Ownership and checks

- Provider implementations and registry: `src/common/provider/`, `src/common/provider/runtime/`.
- ACP lifecycle/protocol: `src/common/runtime/acp/`.
- Terminal lifecycle, polling and history sync: `src/common/runtime/terminal/`.
- Session orchestration: `src/app/runtime_orchestration_services.*`, `src/app/runtime_activity.h`.
- Focused checks: `tests/acp_session_tests.cpp`, `tests/terminal_tests.cpp`, `tests/provider_availability_tests.cpp`, `tests/native_chat_identity_tests.cpp`, `tests/turn_finalization_ownership_tests.cpp`.

The 250 ms frontend interaction budget does not measure provider startup or response time. Process timeouts are operation-specific; preserve their existing values and ownership tests when changing launch or shutdown paths.

## Manual compaction and model limits

Local Codex manual compaction can start an idle or stopped structured session and wait for provider readiness. The pending request owns the chat incarnation, data root, provider, workspace and native session; stop, cancellation, deletion or an unrelated ownership change clears it. Preparation can transfer ownership only through its captured saved workspace task. Active turns, queued prompts, pending delivery and controls block admission. Compaction is maintenance, creates no user message or turn checkpoint, and reports `compacting` while waiting or running. Other providers and remote sessions retain their explicit unsupported response.

Model catalogs preserve optional `contextWindowTokens` and `maxOutputTokens` across cached and frontend projections. Structured provider metadata and OpenCode's model `limit.context` and `limit.output` supply these values. Only nonnegative integers within JavaScript's exact integer range are accepted; absent or invalid limits remain unknown. Context capacity and maximum output stay separate and are not inferred from one another.

Retry locates the latest authored user message after hydrating cold history. Empty assistant placeholders do not block retry when their content, thoughts and plan summaries contain only JavaScript `trim` whitespace, including its Unicode whitespace characters. Substantive output or malformed UTF-8 blocks retry. A later user message still rejects an obsolete retry request.

## Remote setup rejection

A token-bound proxy startup-failure marker preserves the setup error and unsent user input rather than reattaching to a nonexistent provider. For configuration rejected by `process.start`, that marker requires a confirmed missing process; lost start replies and surviving or foreign-owned processes retain recovery. ACP accepts the marker only for its current authority before initialization/session readiness or prompt delivery, so provider stderr cannot invalidate an established turn.

## Undelivered authored input

A definitively failed current-turn input can return to the held durable queue with its authored text, skill blocks, attachments and captured agent/security selection. History removal and queue publication share the full chat save; a failed save restores both, and existing future queue entries remain intact. Delivery uncertainty, provider output, imported history, branch-index dependencies, or the queue capacity limit prevent relocation. A held queue does not resume automatically.

Legacy rows marked `acp_prompt_not_sent` are not automatically migrated, deleted or resent. Native `unsentMessageIndices` identifies eligible known-unsent user rows and their empty assistant intervals for separate frontend queue presentation while preserving raw history indices; `recoverableUnsentMessageIndices` separately authorizes explicit retry. A fresh retry is limited to the latest eligible row in a wholly undelivered chat without a native session, active runtime, queue or unresolved remote-stop authority. Only that explicit retry captures the currently selected agent; stored legacy rows cannot recover an unknown original selection. Empty assistant placeholders retain the existing JavaScript-trim retry rule, and failures restore temporarily removed placeholders. Retry branch context omits earlier known-unsent attempts while retaining delivered history.

Legacy unsent input also supports explicit **Remove** through native `discardUnsent` admission. The request includes the raw message index and authoritative full-history digest, and the backend rechecks known-unsent eligibility, mutable loaded history, saves, Stop and worktree activity. It rejects provider output, uncertain delivery, prepared queue entries and queue references to that row; a safely identified older unsent row can be removed while later remote authority remains untouched.

Removal durably sets `acp_prompt_discarded` and rolls the flag back if the full chat save fails. Authored content, raw indices, queue entries, captured permissions, branch references and provider processes remain intact. The frontend obtains the full-history digest through an anchored one-row read, preserving the current page, then waits for durable acknowledgement before marking only the same still-current local row discarded; discarded rows are also excluded from search, navigation, future context and memory. They cannot be retried or migrated back to the queue. This removes local unsent intent from use; it does not retract submitted provider turns or delete copies in other family members.

The native authority lives in `src/common/runtime/acp/acp_session_runtime.cpp`, with the CEF projection in `src/cef/state_serializer.cpp`; focused regressions are in `tests/queued_prompt_hold_tests.cpp` and `tests/acp_session_tests.cpp`. Frontend projection does not grant delivery authority: read-only imports disable recovery, legacy Edit uses the existing explicit branch flow, and Retry is shown only for a native-authorized index.

A rejected prepared remote prefix can be recovered explicitly against its unchanged authored row after pending delivery and runtime work settle. Recovery captures the current selected agent into a held referenced entry; Resume validates the row and restores the captured capability before launching. Editing that entry updates raw text, retained attachments and queue revision in one full save with rollback. Remove and Clear may delete only an exclusively owned unsent tail without branch or surviving queue references; Stop preserves the raw input as interrupted history. Raw indices remain stable for ordinary projection and edits.

## Advertised model identity

ACP model selections use complete provider-advertised IDs as opaque values. An exact match wins; OpenCode alone may resolve a unique case-insensitive match to its exact advertised ID. Ambiguous aliases and unavailable IDs fail with an actionable selection error instead of falling back to another model or splitting an ID at a slash. Without an advertised model list, the requested ID remains unchanged; other providers retain their existing current-model and legacy selection behavior. OpenCode current-model equality does not bypass its live catalogue validation. Normal chat and goal-turn selections remain separate, and selection validation runs before provider model configuration through `src/common/runtime/acp/acp_model_json.h` and the ACP lifecycle/runtime paths.

## Resume response and durable admission

A setup or resume response may stage the next prompt before the durable receipt registers its wire request. Remote request retirement preserves that exact pending or committed staged entry only while its complete chat, provider, process, connection, turn, configuration, authored-input and delivery authority still matches. Cancelled, foreign, failed or stale packets retire normally. Prompt delivery retains the existing full durable receipt and ownership checks and registers one wire request; diagnostic rejection categories contain no authored text or configuration values.

## Progressive OpenCode tools

New OpenCode launches can request its native Code Mode only when the installed version checked for that exact execution host is 1.17.20 or 1.18.35. The private setup bundle carries an optional boolean `openCodeCodeMode` hint in manifest format 1; compatible setup applies `OPENCODE_EXPERIMENTAL_CODE_MODE` to that child only when neither its execution environment nor its launch overrides already defines the variable. Explicit false, true and empty values retain precedence, with environment names compared case-insensitively on Windows.

Code Mode provides compact native discovery and on-demand tool schemas; this is an OpenCode optimization, not a UAM gateway for every provider. Other providers, mandatory instructions, permission policies and configured MCP capabilities remain unchanged. The hint also works in a small MCP-only setup bundle when central instructions are disabled or MCP is inherited. Existing processes retain their launch environment on reconnect. Older helpers accept and ignore the optional hint and remain eager, so remote optimization needs a compatible helper root activated only after owned jobs are idle. Effective host/provider setup overrides use the existing asynchronous preparation worker, keeping memory-resource reads off the GUI thread; this also covers existing forced setup for Claude and Antigravity UAM control without enabling their MCP optimization. The selected model binds before immutable launch ownership is captured, using the same effective-setup predicate so asynchronous configuration does not invalidate its own launch. Session receipts remain asynchronously staged for global or effective setup instead of falling back to a full synchronous history save; existing ownership and durable-delivery checks still apply.

Central/default skill discovery already uses the installed skill index. Explicit attached skill snapshots remain bounded inline: replacing them with references is deferred until owned per-turn resource preparation can install exact captured files before warm SSH prompt admission. Queue and journal snapshots keep their original semantics; no local-only reference path is substituted.

## Ephemeral remote discovery cleanup

Model-discovery chats are hidden runtime contexts, not ordinary persisted user chats. Stop and its asynchronous receipt, failure, exit and restart settlement resolve that hidden context as well as visible chats, retaining the original execution host and remote ownership. Reaping waits while owned remote Stop is pending or unconfirmed, or process recovery remains unresolved; hidden discovery does not create a saved user-chat transcript.

For a registered hidden context on a nonlocal host, uncertain-exit cleanup requires its remote-process-exists marker and a nonempty retained control token. A failed or uncertain local proxy exit then retains that context and authority. Cleanup recovery resumes only that existing owned process instead of launching a replacement provider or replaying a prompt, then completes the normal Stop/removal lifecycle when confirmed. Unregistered contexts do not infer remote authority from an orphan runtime alone, and other user chats and their input remain outside this cleanup.

This lifecycle does not provide a durable hidden-cleanup journal across full app shutdown while the helper is unreachable. It cannot recover control tokens already lost by older builds or automatically remove the previously orphaned discovery jobs. Helper update remains subject to the existing busy and ownership guards until actual cleanup or quiescence is established.
