# Agents

An agent is a reusable definition of role, instructions, allowed skills and execution constraints, with shared UAM selection across providers. Native provider-agent imports must preserve supported restrictions and explicitly reject or acknowledge fields that cannot be represented; importing a read-only definition must not silently make it writable. Build/Plan choices, provider-spawned subagents and user-created side chats are related but distinct concepts.

[Provider agent selection](../../providers/agent-selection.md) · [Subagent history](../../providers/agent-history.md) · [Side chats](../../providers/side-chats.md).

Built-in definitions are read-only; custom definitions can be created, edited or explicitly deleted with their supported role, workspace access and skill attachments. Favourites and the configured cycling shortcut select Build, Plan and eligible custom agents without weakening imported restrictions. [Central Setup](central-setup.md) owns shared default agents/skills; a saved definition does not retroactively replace setup captured by a running session.

## Built-in delegation

Build and Plan are available as both primary and delegated UAM agents without creating a custom definition. Build inherits the parent’s permission and workspace ceilings; Plan remains read-only. UAM-managed delegation does not require widening the parent to YOLO; a captured Plan or read-only caller keeps that ceiling on its children. Custom definitions marked primary-only remain unavailable for delegation. Existing sessions retain their captured definitions and restrictions when the shared defaults change.

Delegation still requires UAM tool permission, an eligible provider on the chat’s execution host and, for nested helpers, the parent’s captured delegate allowlist.

## Importing provider agents

Settings › Agents imports one native agent Markdown file from OpenCode, GitHub Copilot CLI, Gemini CLI or Claude Code after a preview; UAM does not import Codex or Antigravity agent files. The install scope defaults to All workspaces, and This workspace needs an open workspace chat. UAM checks for an agent with that ID in the chosen scope before the action is enabled; if one exists, the action becomes **Replace agent…** and overwrites it only after a confirmation that names what would change, such as becoming delegate-only, gaining write access or losing skills. A replacement still cannot remove or change a saved provider permission policy. A refused import reports one plain notice line naming the agent and the backend's reason, such as an existing agent or a policy that needs read access, and keeps the form for a retry. A successful import says which scope received it, and whether it is delegate-only; if the user is still on the Agents page, Settings then opens Central Setup with the agent's row scrolled into view, focused and briefly highlighted.

Implementation note: Claude Code, Gemini CLI and Copilot agent files currently import as delegate-only agents, because the importer gives them no chat-agent mode; OpenCode keeps the file's mode. The importer is outside this page's UI agreement.

## Agent store and pickers

Central Setup's agent store lists the agents saved for all workspaces and, while a chat is open, the agents saved in that chat's workspace, each with an All workspaces or This workspace badge; a workspace agent with the same name as a global one is listed as well and overrides it in that workspace, and the two rows' action names then include their scope. Shared defaults offer only agents saved for all workspaces. Sub-agents carry a Delegate-only badge in the store and in the desktop chat agent pickers, where they follow the chat agents but cannot be selected; the agent cycle and favourites skip them, and the backend still refuses them as a chat's agent.

Implementation gaps: workspace agents open read-only in the store, because it saves agents for all workspaces; they are edited or removed in the workspace's `.uam/agents` files. The phone apps' agent pickers still list chat agents only.
