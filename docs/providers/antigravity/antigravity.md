# Antigravity CLI

Antigravity CLI is fully in scope for structured Chat and native CLI View, subject to the same shared requirements as every other provider. Structured chat uses its native stream JSON adapter; CLI View runs the interactive agy terminal. Missing native capabilities require a UAM equivalent or an explicit implementation gap, not a release-scope exemption. Common behaviour is defined once in the [provider agreements](../providers.md); David’s [scope confirmation](../../documentation-review.md#antigravity-scope) does not certify every implementation route.

**Assessment:** source routes and retained base evidence only, 2026-10-16. Implemented is not live verification or David’s acceptance. Consult the [status definitions](../../documentation-rules.md#agreements-and-implementation) and [V5 log](<../../../V5 Improvement Change log.md>).

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Implemented | Native stream JSON launches, normalises text/tools and retains conversation identity; live platform acceptance remains separate. |
| `terminal-view` | Both | Implemented | Native agy CLI has a launch path; interactive approvals stay with the provider. |
| `terminal-sync` | Both | Partial | Saved IDs support native resume, but provider-native history import for terminal-to-Chat refresh is unavailable. |
| `chat-lifecycle` | UAM | Implemented | Shared create/select/rename/delete and catalogue services; cleanup acceptance is separate. |
| `concurrency` | Both | Implemented | Managed independent provider instances; real host combinations remain unverified. |
| `branching` | UAM | Partial | Shared branching is in scope; exact context replay and empty-chat behaviour retain the shared implementation gaps. |
| `editing-messages` | UAM | Partial | Shared edit/branch recovery is in scope; Antigravity continuation and recovery cases still need verification. |
| `mcp-connections` | Both | Partial | Private additional-directory setup exists; discovery does not establish permission to invoke tools. |
| `skills` | UAM | Partial | Shared resource/index delivery exists; attached-skill discovery and live model compliance remain open. |
| `attachments` | Both | Needs verification | Verify structured file/image consumption and remote delivery rather than assuming native terminal support is enough. |
| `compaction` | Unverified | Needs verification | Native /compact is unavailable; a UAM equivalent satisfying the shared compaction agreement is not established. |
| `computer-use` | Both | Needs verification | UAM-managed setup exists; verify the local controller and policy route for structured Antigravity work. |
| `side-chats` | UAM | Implemented | Shared related conversations; no automatic worktree request. |
| `models` | Both | Partial | Configured model selection reaches both launch paths; a complete runtime discovery catalogue is not established. |
| `agent-selection` | Both | Needs verification | Shared UAM agent delivery is in scope; native constraint mapping and structured execution require verification. |
| `agent-history` | Unverified | Needs verification | Complete addressable child history and a UAM equivalent are not established. |
| `resume` | Both | Implemented | Structured conversation IDs are captured and passed with --conversation for saved-session resume; live Windows/SSH acceptance remains separate. |
| `reasoning` | Provider | Needs verification | Any native effort control and its UAM selection/persistence mapping need verification. |
| `fast-mode` | Unverified | Needs verification | An actual native speed tier or UAM equivalent is not established. |
| `permissions` | Both | Partial | Native Plan/Accept Edits and denial reporting exist; interactive callbacks and complete four-mode UAM equivalence remain gaps. |
| `usage` | Both | Partial | Token/history and independent local account quota-summary reads exist; existing account eligibility, custom commands, remote acquisition and platform acceptance remain limits. Expired grants without registration fields require refresh in the provider CLI. |
| `memory` | UAM | Implemented | Shared policy, recall and extraction; inherited choices and remote publication retain their boundaries. |
| `central-setup` | UAM | Implemented | Shared immutable launch resources; running sessions retain their captured setup. |
| `remote-execution` | Both | Needs verification | SSH helper route exists; this trial does not establish live platform/CLI combinations or helper acceptance. |

**Known UI gap:** provider availability settings still label Antigravity “Terminal sessions only.” The label conflicts with the confirmed scope; structured adapter presence does not establish that all selectors and settings expose it correctly.

**Source:** [provider runtime](../../../src/common/provider/antigravity/cli/antigravity_cli_provider_runtime.cpp), [shared source map](../../reference/source-map.md) and [retained parity evidence](../../reference/provider-runtime-parity.md). Detailed acceptance and existing owners stay in the V5 log.
