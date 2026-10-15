# Antigravity CLI

Antigravity CLI uses native terminal in the recorded release scope. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. The base contains structured-stream code, but repository instructions still define a terminal-only release boundary. Keep that distinction until [David confirms the scope](../../documentation-review.md#antigravity-scope).

**Assessment:** source routes and retained base evidence only, 2026-10-16. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | None | Not applicable | Explicit terminal-only release exception; structured code presence is not accepted scope. |
| `terminal-view` | Both | Implemented | Native agy CLI has a launch path; interactive approvals stay with the provider. |
| `terminal-sync` | Unverified | Needs verification | Native terminal transcript synchronization and exact ownership are not established. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | Unverified | Needs verification | Shared or native code is not enough to establish the terminal-only user outcome. |
| `editing-messages` | Unverified | Needs verification | Shared or native code is not enough to establish the terminal-only user outcome. |
| `mcp-connections` | Both | Partial | Private additional-directory setup exists; discovery does not establish permission to invoke tools. |
| `skills` | UAM | Partial | Shared resource/index delivery exists; attached-skill discovery and live model compliance remain open. |
| `attachments` | Unverified | Needs verification | Shared or native code is not enough to establish the terminal-only user outcome. |
| `compaction` | Unverified | Needs verification | Shared or native code is not enough to establish the terminal-only user outcome. |
| `computer-use` | Unverified | Needs verification | Shared setup exists, but a terminal-only computer-control route satisfying the UAM agreement is not established. |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Provider | Needs verification | Native/configured terminal selection exists; no accepted structured model-discovery claim. |
| `agent-selection` | Unverified | Needs verification | Shared or native code is not enough to establish the terminal-only user outcome. |
| `agent-history` | Unverified | Needs verification | Addressable native child history in the terminal release scope is not established. |
| `resume` | Both | Needs verification | Saved-ID terminal resume path exists; exact continuation needs verification. |
| `reasoning` | Provider | Needs verification | Any native effort control and its UAM selection/persistence mapping need verification. |
| `fast-mode` | Unverified | Needs verification | An actual native speed tier or UAM equivalent is not established. |
| `permissions` | Provider | Partial | Opaque terminal approvals stay native; UAM mediation of the four shared choices is not established. |
| `usage` | Both | Partial | Token/history routes exist; full account allowance figures are not established. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Partial | SSH helper route exists; live platform/CLI combinations and helper acceptance remain open. |

**Source:** [provider runtime](../../../src/common/provider/antigravity/cli/antigravity_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
