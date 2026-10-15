# Codex CLI

Codex CLI uses app-server over stdio. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. Native capabilities depend on the installed CLI, selected model and execution environment; shared UAM routes do not imply that every native variant is supported.

**Assessment:** source routes and retained base evidence only, 2026-10-16. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Implemented | Native protocol normalised into UAM messages. |
| `terminal-view` | Both | Implemented | Native CLI in an embedded PTY. |
| `terminal-sync` | Both | Partial | Native rollout refresh exists; concurrent first-session binding remains ambiguous. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | UAM | Partial | Shared branching exists; empty-chat and compacted-context follow-ups retain outstanding work/acceptance. |
| `editing-messages` | UAM | Partial | Delivered edits branch and known-unsent edits have a route; recovery cases retain outstanding acceptance. |
| `mcp-connections` | Both | Implemented | Shared launch setup supplies connections; transport and observed status vary. |
| `skills` | UAM | Partial | Shared resource/index delivery exists; attached-skill discovery and live model compliance remain open. |
| `attachments` | Both | Needs verification | Context and delivery routes exist; complete file/image consumption across local and SSH is unverified. |
| `compaction` | Both | Partial | Manual local compaction and state handling exist; remote/manual parity and acceptance remain open. |
| `computer-use` | Both | Partial | Auto uses local UAM; explicit native Codex is an unprobed preview, not an automatic fallback. |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Both | Implemented | Provider discovery or configured catalogue with saved selection; current availability must be checked. |
| `agent-selection` | Both | Partial | Shared selection/adaptation exists; native import restrictions and GUI acceptance remain open. |
| `agent-history` | Both | Partial | Tool recognition and addressable children vary; full native/remote history is not established. |
| `resume` | Both | Implemented | UAM history plus a returned native session ID; invalid or unavailable IDs require an explicit outcome. |
| `reasoning` | Both | Implemented | Advertised reasoning levels and saved effort reach Codex request configuration. |
| `fast-mode` | Both | Implemented | Supported service tiers are parsed and persisted; selection reaches Codex request configuration. |
| `permissions` | Both | Implemented | Native requests, explicit decisions and confirmed native auto-review mapping coexist with UAM policy. |
| `usage` | Both | Partial | Token/history routes exist; full account allowance figures are not established. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Partial | SSH helper route exists; live platform/CLI combinations and helper acceptance remain open. |

**Source:** [provider runtime](../../../src/common/provider/codex/cli/codex_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
