# Goals

A goal is explicitly requested continuing work with recorded progress and, when specified, a budget. UAM should retain its state across interruptions and distinguish active work, waiting for the user, blocked work and achieved completion. Automatic continuation must remain bounded by the user’s instructions, stop conditions, permissions and budget; ordinary chat input must not silently become a goal or automatically resume unrelated held input.

The configured [iteration cap](../settings/goal-loops.md) applies before any further UAM-managed worker continuation, including continuation after a transient blocker. Reaching the cap blocks the goal instead of starting another iteration; terminal blockers retain their own stop reason. **Implementation gap:** Review-driven continuation enforces this bound; [stalled-loop auto-resume](../documentation-review.md#stalled-goal-loop-cap) still needs a cap admission check.

[Agents](../settings/central-setup/agents.md) · [Current session reference](../reference/architecture/provider-sessions.md).

[Goal Loops settings](../settings/goal-loops.md).

Provider integrations should let an agent inspect its chat’s current goal and request explicit goal creation through UAM’s permission boundary, while UAM supplies the objective, progress context and supported lifecycle controls. Native goals use a provider adapter where available; they must not start a second competing continuation loop or bypass user-owned pause, stop, budget or completion review. Shared goal inspection and approval-gated creation are existing routes, not proof of complete native lifecycle interoperability.

Continuation and review context may compact repeated objectives, task lists and empty state fields, while retaining distinct tasks, verification requirements, budgets and user stop conditions. Attached skill and computer-use context remains associated with the original authored inputs when requests are batched.
