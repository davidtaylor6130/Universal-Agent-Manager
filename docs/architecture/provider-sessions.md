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
