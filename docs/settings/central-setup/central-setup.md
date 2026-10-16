# Central Setup

Central Setup is UAM’s one shared definition of instructions, skills, agents and related resources, adapted for each provider rather than maintained separately in every CLI. At provider launch, UAM prepares the selected resources on the executing computer, including SSH, without overwriting user-owned workspace or provider-home configuration. Existing sessions keep their captured setup until a supported restart or explicit update; saving shared resources must not pretend they have already reached a running provider.

- [Instructions](instructions.md)
- [Skills](skills.md)
- [Agents](agents.md)
- [MCP Servers](../mcp-servers.md)

[Engineering reference](../../reference/central-configuration.md).

Shared defaults can select instruction files, the default new-chat agent, skills on every agent and agents made available to providers. Those defaults reference the same central definitions rather than copying independent editable versions into each provider. [Instructions](instructions.md), [Skills](skills.md) and [Agents](agents.md) own resource editing.
