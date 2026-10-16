# GitHub Copilot CLI

GitHub Copilot CLI uses ACP. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. Native capabilities depend on the installed CLI, selected model and execution environment; shared UAM routes do not imply that every native variant is supported.

**Assessment:** source routes and retained base evidence only, 2026-10-16. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Implemented | Native protocol normalised into UAM messages. |
| `terminal-view` | Both | Implemented | Native CLI in an embedded PTY. |
| `terminal-sync` | Both | Needs verification | Native terminal/history continuity needs exact-provider evidence. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | UAM | Partial | Shared branching exists; empty-chat and compacted-context follow-ups retain outstanding work/acceptance. |
| `editing-messages` | UAM | Partial | Delivered edits branch and known-unsent edits have a route; recovery cases retain outstanding acceptance. |
| `mcp-connections` | Both | Implemented | Shared launch setup supplies connections; transport and observed status vary. |
| `skills` | UAM | Partial | Shared resource/index delivery exists; attached-skill discovery and live model compliance remain open. |
| `attachments` | Both | Needs verification | Context and delivery routes exist; complete file/image consumption across local and SSH is unverified. |
| `compaction` | Both | Partial | Compaction state may be emitted; a complete manual/native/UAM fallback route is not established. |
| `computer-use` | UAM | Implemented | Local UAM companion route exists; SSH desktop control is outside the current supported scope. |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Both | Implemented | Provider discovery or configured catalogue with saved selection; current availability must be checked. |
| `agent-selection` | Both | Partial | Shared selection/adaptation exists; native import restrictions and GUI acceptance remain open. |
| `agent-history` | Both | Partial | Tool recognition and addressable children vary; full native/remote history is not established. |
| `resume` | Both | Implemented | UAM history plus a returned native session ID; invalid or unavailable IDs require an explicit outcome. |
| `reasoning` | Both | Implemented | Advertised reasoning options and effort configuration have a native route; unsupported choices stay unavailable. |
| `fast-mode` | Unverified | Needs verification | No speed-tier guarantee inferred from model selection; verify an actual provider/UAM equivalent. |
| `permissions` | Both | Implemented | ACP requests map to UAM policy; native-terminal internals remain provider-owned. |
| `usage` | Both | Partial | Token/history and independent local SDK account.getQuota routes exist; custom commands, remote acquisition and platform acceptance remain limited. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Partial | SSH helper route exists; live platform/CLI combinations and helper acceptance remain open. |

**Source:** [provider runtime](../../../src/common/provider/copilot/cli/copilot_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
