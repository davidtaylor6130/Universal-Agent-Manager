# Codex CLI

Codex CLI uses app-server over stdio. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. Native capabilities depend on the installed CLI, selected model and execution environment; shared UAM routes do not imply that every native variant is supported.

**Assessment:** source routes and retained base evidence only, 2026-10-16. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Partial | Native protocol normalised into UAM messages. Shared [input delivery](../structured-chat.md#input-delivery) is Partial. |
| `terminal-view` | Both | Implemented | Native CLI in an embedded PTY. |
| `terminal-sync` | Both | Partial | Exact native status replies bind new terminals and guarded local/SSH rollout refresh exists; live concurrent view-continuation acceptance remains pending. [Shared history recency](../../chat/native-imports.md#history-recency) remains under verification. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | UAM | Implemented | Guarded chosen-prefix branches preserve source history, compacted context and branch-owned originals; supported local regeneration exists, while native/model/SSH continuation and acceptance retain their explicit limits. |
| `editing-messages` | UAM | Implemented | Definitely-unsent edits save locally; delivered edits branch with independently selected files and captured skills; unsupported recorded-source delivery refuses explicitly and visual/platform/user acceptance remains pending. |
| `mcp-connections` | Both | Implemented | Shared launch setup supplies connections; transport and observed status vary. |
| `skills` | UAM | Implemented | Complete resources and indexes are delivered locally or over SSH; attached requests use captured instructions and immutable references, subject to native permissions and model compliance. |
| `attachments` | Both | Partial | Ordinary images use executing-host paths; explicit local saved-message Retry uses validated owner-scoped original bytes as native image URLs with complete-selection and stale-policy refusal; same-process SSH saved-original Retry has an adapter opt-in under the shared scoped ownership and durable-admission safeguards awaiting matching-helper runtime verification; cold or replacement-process remote Retry and live model/platform acceptance remain pending. |
| `compaction` | Both | Implemented | Native manual compaction uses owned local or durable protocol-3 SSH control delivery; live acceptance remains pending. |
| `computer-use` | Both | Partial | Auto uses local UAM; explicit native Codex is an unprobed preview, not an automatic fallback. |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Both | Implemented | Provider discovery or configured catalogue with saved selection; current availability must be checked. |
| `agent-selection` | Both | Partial | Shared selection/adaptation exists; native import restrictions and GUI acceptance remain open. Shared [built-in delegation](../../settings/central-setup/agents.md#built-in-delegation) retains parent ceilings. |
| `agent-history` | Both | Partial | Exact native child threads open local active/archived rollouts or SSH app-server history with reasoning, plans, compaction and tool results; every validated receiver is retained for individual selection, and Unknown-age child-detail enrichment requires safely matched operation identity under the [shared native-history rules](../agent-history.md), while integrated native and live acceptance remain pending. Shared inline presentation remains pending integration and acceptance. |
| `subagents` | Both | Partial | Nested catalogue and child-only panes retain native controls; eligible local named OpenAI managed children have verified read-only Plan same-thread continuation within the current controller lifetime, while [broader principals, providers, restart, native lineage and acceptance remain partial](../../chat/subagents.md). Shared [active-turn wait](../../chat/subagents.md#active-turn-result-wait) is source-integrated with focused private fixtures; canonical verification, this provider's long-call deadline and live local/SSH invocation remain unverified.  [Same-live parent continuation](../../chat/subagents.md#same-live-parent-result-continuation) is source-integrated for the local-only route; native and live verification remain pending. |
| `goals` | UAM | Partial | Shared goal inspection, approval-gated creation and bounded continuation exist; native lifecycle interoperability and full runtime verification remain outstanding. |
| `resume` | Both | Implemented | UAM history plus a returned native session ID; invalid or unavailable IDs require an explicit outcome. |
| `reasoning` | Both | Implemented | Advertised reasoning levels and saved effort reach Codex request configuration. |
| `fast-mode` | Both | Implemented | Supported service tiers are parsed and persisted; selection reaches Codex request configuration. |
| `permissions` | Both | Partial | Native requests, typed decisions and confirmed native auto-review coexist with UAM policy; the [captured-read boundary](../permissions/permissions.md#captured-read-restrictions) is implemented with protocol verification and scoped source review, while complete environment scoping and desktop/live/platform acceptance remain open. |
| `usage` | Both | Partial | Token/history and independent local account/rateLimits/read routes exist; custom commands, remote acquisition and platform acceptance remain limited. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Needs verification | SSH helper route exists; this trial does not establish live platform/CLI combinations or helper acceptance. |

**Source:** [provider runtime](../../../src/common/provider/codex/cli/codex_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
