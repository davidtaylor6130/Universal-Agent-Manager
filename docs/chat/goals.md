# Goals

A goal is explicitly requested continuing work with recorded progress and, when specified, a budget. UAM should retain its state across interruptions and distinguish active work, waiting for the user, blocked work and achieved completion. Automatic continuation must remain bounded by the user’s instructions, stop conditions, permissions and budget; ordinary chat input must not silently become a goal or automatically resume unrelated held input.

[Agents](../settings/central-setup/agents.md) · [Current session reference](../reference/architecture/provider-sessions.md).

[Goal Loops settings](../settings/goal-loops.md).

Provider integrations should let an agent inspect its chat’s current goal and request explicit goal creation through UAM’s permission boundary, while UAM supplies the objective, progress context and supported lifecycle controls. Native goals use a provider adapter where available; they must not start a second competing continuation loop or bypass user-owned pause, stop, budget or completion review. Shared goal inspection and approval-gated creation are existing routes, not proof of complete native lifecycle interoperability.
