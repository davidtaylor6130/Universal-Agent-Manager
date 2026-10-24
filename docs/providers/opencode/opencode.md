# OpenCode CLI

OpenCode CLI uses ACP. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. Native capabilities depend on the installed CLI, selected model and execution environment; shared UAM routes do not imply that every native variant is supported.

**Assessment:** source routes and retained automated evidence only, 2026-10-17. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Partial | Native protocol normalised into UAM messages. Shared [input delivery](../structured-chat.md#input-delivery) is Partial. |
| `terminal-view` | Both | Implemented | Bound ordinary local and SSH TUI launches observe their own authenticated native status; custom, ambiguous and unadmitted input states remain Unknown; [Windows workspace spelling](#workspace-identity) retains separate live acceptance. |
| `terminal-sync` | Both | Partial | Explicit native session binding and export refresh exist; older unbound and remote live cases remain open. [Shared history recency](../../chat/native-imports.md#history-recency) remains under verification. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | UAM | Implemented | Guarded chosen-prefix branches preserve source history, compacted context and branch-owned originals; supported local regeneration exists, while native/model/SSH continuation and acceptance retain their explicit limits. |
| `editing-messages` | UAM | Implemented | Definitely-unsent edits save locally; delivered edits branch with independently selected files and captured skills; unsupported recorded-source delivery refuses explicitly and visual/platform/user acceptance remains pending. |
| `mcp-connections` | Both | Implemented | Shared launch setup supplies connections; transport and observed status vary. |
| `skills` | UAM | Implemented | Complete resources and indexes are delivered locally or over SSH; attached requests use captured instructions and immutable references, subject to native permissions and model compliance. |
| `attachments` | Both | Partial | Native input uses executing-host files or ACP blocks; local saved-original Retry additionally checks owned native server/current-model vision evidence. Commands, remote saved-original Retry and unadmitted companions remain gaps; live/platform acceptance is pending. |
| `compaction` | Both | Implemented | Native metadata and verified summary identities drive active state, collapsed summaries, historical repair and compacted replay; unavailable structured manual control is not offered, and live/platform acceptance remains pending. |
| `computer-use` | UAM | Implemented | Local UAM companion route exists; SSH desktop control is outside the current supported scope. UAM data-folder writer fencing is implemented in source; platform runtime verification remains pending ([shared storage boundary](../../computer-use.md)). |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Both | Implemented | Provider discovery or configured catalogue with saved selection; current availability must be checked. |
| `agent-selection` | Both | Partial | Shared selection/adaptation exists; native import restrictions and GUI acceptance remain open. Shared [built-in delegation](../../settings/central-setup/agents.md#built-in-delegation) retains parent ceilings. |
| `agent-history` | Both | Partial | Addressable child export and authenticated same-process approval relay support ordinary delegation while Plan and bound-policy restrictions remain. Shared inline presentation remains pending integration and acceptance. |
| `subagents` | Both | Partial | UAM nested catalogue and child-only pane source is unit-verified; Supported native child Chat controls are retained; [same-child follow-up, lineage and acceptance remain partial](../../chat/subagents.md). Shared [active-turn wait](../../chat/subagents.md#active-turn-result-wait) is source-integrated with focused private fixtures; its [reserved UAM server budget](#uam-tool-deadline) is source-implemented for the checked version, while canonical verification, effective native deadlines and live local/SSH invocation remain unverified. |
| `goals` | UAM | Partial | Shared goal inspection, approval-gated creation and bounded continuation exist; native lifecycle interoperability and full runtime verification remain outstanding. |
| `resume` | Both | Implemented | UAM history plus a returned native session ID; invalid or unavailable IDs require an explicit outcome. [Data-folder transfer](../../settings/storage.md#application-data-folder): UAM Partial; same-object IO, owner timestamp preservation, retained bootstrap/receipt reads and modern/legacy remote and managed-marker presence admission implemented in source, live activation remains held. |
| `reasoning` | Both | Implemented | Native advertised effort and variant config options retain their exact choices and dispatch through ACP configuration controls. |
| `fast-mode` | Unverified | Needs verification | No speed-tier guarantee inferred from model selection; verify an actual provider/UAM equivalent. |
| `permissions` | Both | Partial | ACP parent actions and authenticated child approvals retain the [implemented captured-read boundary](../permissions/permissions.md#captured-read-restrictions) and [qualified question identity](#repeated-call-questions), with protocol verification and scoped source review; [Windows workspace spelling](#workspace-identity) retains exact native IDs and read ceilings with live acceptance pending. Shared [UAM-managed delegation](../../settings/central-setup/agents.md#built-in-delegation) retains parent and captured read ceilings, while provider-native Task permissions remain separate; environment scoping and desktop/live/platform acceptance remain open. |
| `usage` | Both | Partial | Token/history routes exist; no independent account-quota reader is integrated, and local request counts do not establish a provider allowance. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Needs verification | SSH helper route exists; this trial does not establish live platform/CLI combinations or helper acceptance. |

## Version compatibility

OpenCode V1 and V2 require separately verified launch and native-service integration. The local Alpha150 workaround uses V1.19.4; V2.1.2 returned command-line help instead of ACP JSON when launched with the existing V1 companion arguments. V2 compatibility remains open, including its session, configuration, MCP, history, permission and cancellation routes. Passing the minimum-version check does not establish support for a newer major version, and a desktop-only update does not verify remote helpers.

## Workspace identity

OpenCode structured permissions and native Terminal View bind to the same owned workspace and exact native session identities. For an SSH Windows host, ordinary absolute DOS paths may differ only in corresponding slash/backslash separator spelling while drive case, component bytes, component order and separator positions stay exact. This rule does not expand aliases, case, dot components, device or UNC paths; byte-identical paths retain their existing admission. Local canonical checks and other hosts retain their existing comparison. Source implements this spelling comparison; matching desktop/helper delivery and Windows live acceptance remain pending.

## UAM tool deadline

For checked OpenCode 1.19.3 launches with enabled UAM control, UAM's private process profile gives its reserved `uam-control` server a 120-second tool-call budget when no explicit inline reserved definition exists. This per-server policy takes precedence over native experimental fallback, including a larger fallback; other servers and global settings are unchanged. Explicit inline reserved settings, including shorter or larger budgets, are retained; a conflicting injected owned server refuses setup. Effective native precedence and long-call execution remain unverified. SSH coverage requires a newly matching deployed helper: older helpers can ignore this optional setup hint, so a desktop update alone does not establish remote coverage or acceptance.

## Repeated-call questions

The built-in `doom_loop` question is distinct from an MCP action whose generated name is also `doom_loop`. Recognition of the inspected OpenCode 1.18.35 question requires its native tool-and-input metadata; the authenticated child snapshot also needs matching tool patterns and no action tool link. Unknown or changed shapes retain denial or cancellation. This identifies a trusted producer shape, not origin attestation: plugins can imitate it, and ACP does not retain every native snapshot field. This distinction is implemented with protocol verification and scoped source review; recognizing this question grants no provider-native Task permission and does not relax captured read ceilings or broader native policy limits. Desktop, live-provider, Windows, SSH and user acceptance remain pending.

**Source:** [provider runtime](../../../src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
