# Permissions

UAM exposes four consistent permission choices: Default asks before commands and changes, Accept Edits automatically permits workspace file edits, AI Review uses the provider’s supported automatic review or UAM’s reviewer while retaining decisions that need user judgement, and YOLO enables the least restrictive supported behaviour. Map provider terminology to these meanings; a similarly named native setting is not enough. Plan/read-only and managed restrictions remain ceilings. A native terminal uses the provider’s own controls and must not imply that UAM mediates every action.

Leaving Plan requires an explicit decision on the current native review request. Show only supported actions, keep managed and read-only ceilings, persist the explicit decision before delivering it, retain the Plan ceiling until an exact native confirmation, and persist the confirmed mode before treating the transition as finished. Repeated or expired replies must not authorize another review or fail a later turn.

## Captured read restrictions

For a captured read-only agent or goal-review turn, manual decisions, automatic approvals, saved-response recovery and displayed choices share the same runtime ceiling. Native launch settings must retain that ceiling, so the working chat’s edit permission cannot broaden its goal reviewer. Only supported one-time read actions and explicit denial or cancellation are available; persistent and session-wide grants cannot widen the restriction, even under YOLO. UI filtering preserves the original native request and offers, and the backend validates every response again. Recognized non-action questions remain answerable; ordinary native Plan exit retains its separate current-request decision and confirmation flow. Unknown action or reply shapes must not acquire approval from their labels; retain a supported denial or Cancel, with Cancel as the safe default.

Command approval requires the actual typed native command; a display description, reason or read-looking label is not command authority. The conservative literal filter rejects unsupported forms, and admitted Git diff/log/show forms require explicit `--no-ext-diff` and `--no-textconv`. Admitted SVN reads (status, info, log, list, cat, blame, propget/proplist and diff) exclude `--config-dir`/`--config-option`, and SVN diff requires `--internal-diff` so no configured diff command runs. Even an admitted command such as Git status trusts the resolved executable, repository configuration and execution environment, including pager, index-refresh and filesystem-monitor behavior; this is not an operating-system sandbox.

Recovery must validate a saved answer against the fresh native request and current captured ceiling before delivery. An earlier approval cannot authorize an action outside the current captured ceiling. A fresh safe denial or cancellation may replace that stale answer, but the new decision must save before wire delivery; a failed save restores the previous receipt and pending request rather than sending an unrecorded answer.

**Implementation: Partial overall.** The captured-read response guard, typed command provenance, one-time automatic choice, decoded replay validation, atomic safe-answer replacement and offered-choice projection are implemented with protocol verification and scoped source review, including the [OpenCode question distinction](../opencode/opencode.md#repeated-call-questions). Broader provider and environment coverage retains its explicit limits. Desktop choice and journey acceptance remains inconclusive; live-provider, Windows, SSH and user acceptance remain pending. See the [current gaps](../../documentation-review.md#provider-parity-decisions) and [engineering boundary](../../reference/architecture/permissions-notifications.md#captured-read-response-boundary).

- [Default](default.md)
- [Accept Edits](auto-accept.md)
- [AI Review](ai-review.md)
- [YOLO](yolo.md)

[Permission settings](../../settings/permission-defaults.md) define configuration. [Permission architecture](../../architecture/permissions-notifications.md) and the [provider checklists](../providers.md#provider-checklists) record implementation routes and limitations.
