# Goals

A goal is explicitly requested continuing work with recorded progress and, when specified, a budget. UAM should retain its state across interruptions and distinguish active work, waiting for the user, blocked work and achieved completion. Automatic continuation must remain bounded by the user’s instructions, stop conditions, permissions and budget; ordinary chat input must not silently become a goal or automatically resume unrelated held input.

[Agents](../settings/central-setup/agents.md) · [Current session reference](../reference/architecture/provider-sessions.md).

[Goal Loops settings](../settings/goal-loops.md).

Provider integrations should let an agent inspect its chat’s current goal and request explicit goal creation through UAM’s permission boundary, while UAM supplies the objective, progress context and supported lifecycle controls. Native goals use a provider adapter where available; they must not start a second competing continuation loop or bypass user-owned pause, stop, budget or completion review. Shared goal inspection and approval-gated creation are existing routes, not proof of complete native lifecycle interoperability.

Continuation and review context may compact repeated objectives, task lists and empty state fields, while retaining distinct tasks, verification requirements, budgets and user stop conditions. Attached skill and computer-use context remains associated with the original authored inputs when requests are batched.

Generated small-model goals and their authored input share the complete initial admission save on local and SSH hosts before provider input. Goal preparation alone grants no delivery authority. Ready v3 small-model SSH delivery and direct steering additionally retain the exact goal owner, command and iteration through the full marker receipt and final check against saved process authority; changed or missing ownership refuses that continuation. The owner’s authoritative goal metadata can supply this scope while its transcript remains unloaded. Automatic small-model quota claim and settlement retain that full active-goal scope even when the provider runtime is absent; an objective edit does not inherit an earlier claim’s authority. Cold launch additionally retains that exact goal scope through its owned process/control save; legacy transport and native goal lifecycle controls retain their own publication boundaries and pending verification.
