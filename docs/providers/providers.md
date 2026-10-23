# Providers

Providers are the AI CLI runtimes UAM integrates through programmable interfaces such as ACP, app-server or stream JSON, with a native terminal where available. UAM supports Gemini CLI, Codex CLI, Claude Code CLI, OpenCode CLI, GitHub Copilot CLI and Antigravity CLI, with both structured Chat and native CLI View in scope for every provider. UAM supplies shared features when a provider lacks an equivalent. Provider implementation is judged by the outcomes below, not just successful process launch or protocol connection.

## Required capabilities

Every provider checklist must account for every row. Conditional features must clearly report unavailable choices rather than inventing provider capabilities. Antigravity follows the same shared requirements as the other five providers; its native stream JSON protocol is a structured-chat implementation route, not a scope exception. David’s [scope confirmation](../documentation-review.md#antigravity-scope) does not mark unverified capabilities complete.

| Capability ID | Agreement |
|---|---|
| `structured-chat` | [Structured chat](structured-chat.md) |
| `terminal-view` | [CLI terminal view](terminal-view/terminal-view.md) |
| `terminal-sync` | [Chat and terminal continuity](terminal-view/session-continuity.md) |
| `chat-lifecycle` | [Create, select, rename and delete](../chat/lifecycle.md) |
| `concurrency` | [Concurrent instances](concurrency.md) |
| `branching` | [Branching](branching.md) |
| `editing-messages` | [Editing messages](editing-messages.md) |
| `mcp-connections` | [MCP connections](mcp-connections.md) |
| `skills` | [Skills](skills.md) |
| `attachments` | [Images and files](attachments.md) |
| `compaction` | [Compaction](compaction.md) |
| `computer-use` | [Computer use](computer-use.md) |
| `side-chats` | [Side chats](side-chats.md) |
| `models` | [Model discovery and selection](models.md) |
| `agent-selection` | [Agent selection](agent-selection.md) |
| `agent-history` | [Provider subagent history](agent-history.md) |
| `subagents` | [Delegated child workflow](../chat/subagents.md) |
| `goals` | [Continuing goals](../chat/goals.md) |
| `resume` | [Save and resume](resuming.md) |
| `reasoning` | [Reasoning](reasoning.md) |
| `fast-mode` | [Fast mode](fast-mode.md) |
| `permissions` | [Four permission modes](permissions/permissions.md) |
| `usage` | [Usage figures](usage.md) |
| `memory` | [Memory](../memory/memory.md) |
| `central-setup` | [Central setup](../settings/central-setup/central-setup.md) |
| `remote-execution` | [SSH execution](../remote-environments/ssh.md) |

## Provider checklists

- [Gemini](gemini/gemini.md)
- [Codex](codex/codex.md)
- [Claude Code](claude-code/claude-code.md)
- [OpenCode](opencode/opencode.md)
- [Copilot](copilot/copilot.md)
- [Antigravity](antigravity/antigravity.md)

**Full implementation** means the applicable agreements are delivered, including UAM equivalents, with relevant verification and outstanding acceptance honestly reported. A checklist with Partial, Not implemented or Needs verification rows cannot support an unqualified full-implementation claim. Not applicable requires an explicit scope or conditional-capability reason. “Implemented” records a source route, not David's approval. [Status definitions](../documentation-rules.md#agreements-and-implementation) apply throughout.
