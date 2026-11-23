# Agents

An agent is a reusable definition of role, instructions, allowed skills and execution constraints, with shared UAM selection across providers. Native provider-agent imports must preserve supported restrictions and explicitly reject or acknowledge fields that cannot be represented; importing a read-only definition must not silently make it writable. Build/Plan choices, provider-spawned subagents and user-created side chats are related but distinct concepts.

[Provider agent selection](../../providers/agent-selection.md) · [Subagent history](../../providers/agent-history.md) · [Side chats](../../providers/side-chats.md).

## Importing provider agents

Settings can find native agents in each provider's global and workspace folders (Claude Code `.claude/agents`, Gemini CLI `.gemini/agents`, Copilot CLI `.copilot/agents` and `.github/agents`, OpenCode `agent`/`agents` folders and `agent`/`agents` entries in `opencode.json` or `opencode.jsonc`) and offers them as a pick-list; a typed file path remains available. Codex CLI and Antigravity have no native agent files, so they offer nothing to import. Import reads a closed YAML frontmatter subset (nested maps, lists and block text, without anchors, aliases, tags or merges) and every refusal names the offending field. Restrictions are kept, never widened: OpenCode denies stay denies, `allow` and an enabled tool become `ask` so UAM still mediates them, and a pattern map with any deny denies the whole tool. A source that cannot edit (denied edit tools, a read-only tool list or a `plan` permission mode) imports only with read access. Rules that read access cannot express bind the agent to local OpenCode 1.x; preview says so, and refuses such an import while the local OpenCode is 2.x. Tool lists, turn limits and other provider fields UAM cannot represent are listed and require acknowledgment; hooks, worktree isolation and remote Gemini agents are refused. Descriptions are folded to one line and shortened, with a visible note, beyond 1,024 bytes.

Built-in definitions are read-only; custom definitions can be created, edited or explicitly deleted with their supported role, workspace access and skill attachments. Favourites and the configured cycling shortcut select Build, Plan and eligible custom agents without weakening imported restrictions. [Central Setup](central-setup.md) owns shared default agents/skills; a saved definition does not retroactively replace setup captured by a running session.

## Built-in delegation

Build and Plan are available as both primary and delegated UAM agents without creating a custom definition. Build inherits the parent’s permission and workspace ceilings; Plan remains read-only. UAM-managed delegation does not require widening the parent to YOLO; a captured Plan or read-only caller keeps that ceiling on its children. Custom definitions marked primary-only remain unavailable for delegation. Existing sessions retain their captured definitions and restrictions when the shared defaults change.

Delegation still requires UAM tool permission, an eligible provider on the chat’s execution host and, for nested helpers, the parent’s captured delegate allowlist.
