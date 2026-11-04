# Agents

An agent is a reusable definition of role, instructions, allowed skills and execution constraints, with shared UAM selection across providers. Native provider-agent imports must preserve supported restrictions and explicitly reject or acknowledge fields that cannot be represented; importing a read-only definition must not silently make it writable. Build/Plan choices, provider-spawned subagents and user-created side chats are related but distinct concepts.

[Provider agent selection](../../providers/agent-selection.md) · [Subagent history](../../providers/agent-history.md) · [Side chats](../../providers/side-chats.md).

Built-in definitions are read-only; custom definitions can be created, edited or explicitly deleted with their supported role, workspace access and skill attachments. Favourites and the configured cycling shortcut select Build, Plan and eligible custom agents without weakening imported restrictions. [Central Setup](central-setup.md) owns shared default agents/skills; a saved definition does not retroactively replace setup captured by a running session.

## Built-in delegation

Build and Plan are available as both primary and delegated UAM agents without creating a custom definition. Build inherits the parent’s permission and workspace ceilings; Plan remains read-only. UAM-managed delegation does not require widening the parent to YOLO; a captured Plan or read-only caller keeps that ceiling on its children. Custom definitions marked primary-only remain unavailable for delegation. Existing sessions retain their captured definitions and restrictions when the shared defaults change.

Delegation still requires UAM tool permission, an eligible provider on the chat’s execution host and, for nested helpers, the parent’s captured delegate allowlist.
