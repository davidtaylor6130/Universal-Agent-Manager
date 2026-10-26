# Goals

A goal is explicitly requested continuing work with recorded progress and, when specified, a budget. UAM should retain its state across interruptions and distinguish active work, waiting for the user, blocked work and achieved completion. Automatic continuation must remain bounded by the user’s instructions, stop conditions, permissions and budget; ordinary chat input must not silently become a goal or automatically resume unrelated held input.

The configured [iteration cap](../settings/goal-loops.md) applies before any further UAM-managed worker continuation, including continuation after a transient blocker. Reaching the cap blocks the goal instead of starting another iteration; terminal blockers retain their own stop reason. **Implementation gap:** Review-driven continuation enforces this bound; [stalled-loop auto-resume](../documentation-review.md#stalled-goal-loop-cap) still needs a cap admission check.

[Agents](../settings/central-setup/agents.md) · [Current session reference](../reference/architecture/provider-sessions.md).

[Goal Loops settings](../settings/goal-loops.md).

Provider integrations should let an agent inspect its chat’s current goal and request explicit goal creation through UAM’s permission boundary, while UAM supplies the objective, progress context and supported lifecycle controls. Native goals use a provider adapter where available; they must not start a second competing continuation loop or bypass user-owned pause, stop, budget or completion review. Shared goal inspection and approval-gated creation are existing routes, not proof of complete native lifecycle interoperability.

Antigravity goals use UAM-managed continuation in the chat's saved native CLI conversation, with isolated Plan reviewers whose native IDs cannot replace the worker's identity, including their single bounded JSON repair. UAM owns progress, completion review, iteration limits and observed step-token accounting, including reviewer consumption. Pause/clear/replacement cancels the matching owned turn after the control's durable receipt and retains unrelated held input. Direct controls save their intent before cancellation and restore prior goal intent if that save fails; CEF controls retain their asynchronous receipt boundary. Interrupted or missing usage holds budgeted continuation across restart; ordinary Resume preserves counters, while explicit Restart requires observed process exit for an interrupted local metered turn before beginning the existing fresh UAM budget/progress cycle in the same native conversation. Reported-step enforcement can stop after a step exceeds the allowance; it is not a provider hard token cap. Native `/goal` commands and unsupported provider-owned controls remain refused, without silently changing ownership or claiming native cancellation. The [runtime reference](../reference/antigravity-runtime-goals.md) and [native bridge dependency](../documentation-review.md#antigravity-native-goals) retain those boundaries.

Continuation and review context may compact repeated objectives, task lists and empty state fields, while retaining distinct tasks, verification requirements, budgets and user stop conditions. Attached skill and computer-use context remains associated with the original authored inputs when requests are batched.

The first goal send includes the reference snapshots selected with the original authored submission, captured before goal creation waits. Automatic continuations must not borrow later composer selections. This follows [David's action-send contract decision](../documentation-review.md#referenced-context-action-sends) and the [composer context agreement](composer/attachments.md); implementation verification and acceptance remain separate.

## Inspection

Goal inspection reports the selected goal’s saved execution owner (`uam` or `provider`) separately from its creator provenance and UAM’s lifecycle authority. Legacy records without a recognized execution owner retain the UAM default. Inspection preserves saved native commands and provenance without starting or transferring execution.
