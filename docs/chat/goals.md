# Goals

A goal is explicitly requested continuing work with recorded progress and, when specified, a budget. UAM should retain its state across interruptions and distinguish active work, waiting for the user, blocked work and achieved completion. Automatic continuation must remain bounded by the user’s instructions, stop conditions, permissions and budget; ordinary chat input must not silently become a goal or automatically resume unrelated held input.

[Agents](../settings/central-setup/agents.md) · [Current session reference](../reference/architecture/provider-sessions.md).

[Goal Loops settings](../settings/goal-loops.md).

Provider integrations should let an agent inspect its chat’s current goal and request explicit goal creation through UAM’s permission boundary, while UAM supplies the objective, progress context and supported lifecycle controls. Native goals use a provider adapter where available; they must not start a second competing continuation loop or bypass user-owned pause, stop, budget or completion review. Shared goal inspection and approval-gated creation are existing routes, not proof of complete native lifecycle interoperability.

Continuation and review context may compact repeated objectives, task lists and empty state fields, while retaining distinct tasks, verification requirements, budgets and user stop conditions. Attached skill and computer-use context remains associated with the original authored inputs when requests are batched.

Completed shared goal reviews and their single JSON repair count reported token usage once before UAM admits another repair or worker turn. Reaching the token budget holds the goal before applying a review decision, including a completion decision; unknown usage stays unknown and must not reuse an earlier turn’s count or infer tokens from response length. Cumulative usage requires a matching native session and a known starting total from adapter-validated receipts, or a newly created review context; replacement sessions cannot charge an earlier receipt, and startup retry begins a new receipt without assuming an empty native history. Replayed older cumulative snapshots cannot reduce already observed review usage, and display telemetry cannot establish a repair baseline. Cancellation and changed user controls retain their existing continuation gates. Native provider-managed goal budgets remain under their own adapter contract.

**Implementation note:** Reviewer accounting and cumulative-receipt corrections have focused native verification across all six adapters and independent source/test review; broader runtime/platform verification and user acceptance remain pending.
