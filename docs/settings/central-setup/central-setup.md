# Central Setup

Central Setup is UAM’s one shared definition of instructions, skills, agents and related resources, adapted for each provider rather than maintained separately in every CLI. At provider launch, UAM prepares the selected resources on the executing computer, including SSH, without overwriting user-owned workspace or provider-home configuration. Existing sessions keep their captured setup until a supported restart or explicit update; saving shared resources must not pretend they have already reached a running provider.

- [Instructions](instructions.md)
- [Skills](skills.md)
- [Agents](agents.md)
- [MCP Servers](../mcp-servers.md)

[Engineering reference](../../reference/central-configuration.md).
