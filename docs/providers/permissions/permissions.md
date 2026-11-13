# Permissions

UAM exposes four consistent permission choices: Default asks before commands and changes, Accept Edits automatically permits workspace file edits, AI Review uses the provider’s supported automatic review or UAM’s reviewer while retaining decisions that need user judgement, and YOLO enables the least restrictive supported behaviour. Map provider terminology to these meanings; a similarly named native setting is not enough. Plan/read-only and managed restrictions remain ceilings. A native terminal uses the provider’s own controls and must not imply that UAM mediates every action.

Leaving Plan requires an explicit decision on the current native review request. Show only supported actions, keep managed and read-only ceilings, persist the explicit decision before delivering it, retain the Plan ceiling until an exact native confirmation, and persist the confirmed mode before treating the transition as finished. Repeated or expired replies must not authorize another review or fail a later turn.

## A chat's permission level

A chat's permission level changes only when someone changes it: the composer chip or `/permissions`, the phone companion, or an explicit Plan exit decision. Switching provider keeps the chat's level and its Plan or Default mode instead of applying the new provider's Settings default ([#754](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/754)). A provider that answers its own permission prompts (Antigravity) cannot show or change UAM's levels, so any level above Default becomes Default when switching to or from it, never a less safe level. The desktop switch dialog says so before confirming; the phone has no switch dialog. When a live mode change fails, the chat goes back to its previous mode and level and the failure the user sees names both. Every change is written to the diagnostic log with the chat ID, the old and new values and its source (user, companion, rollback, switch, migration or plan approval), never prompt text.

**Implementation: Implemented for the shared switch, rollback and logging routes**; live-provider, Windows, SSH and user acceptance remain pending.

## Captured read restrictions

For a captured read-only agent or goal-review turn, manual decisions, automatic approvals, saved-response recovery and displayed choices share the same runtime ceiling. Native launch settings must retain that ceiling, so the working chat’s edit permission cannot broaden its goal reviewer. Only an explicit write access grants write: a missing, unknown or unreadable saved access value is treated as read-only. The provider-native feature preference changes which instructions an agent uses, never its read ceiling. Only supported one-time read actions and explicit denial or cancellation are available; persistent and session-wide grants cannot widen the restriction, even under YOLO. UI filtering preserves the original native request and offers, and the backend validates every response again. Recognized non-action questions remain answerable; ordinary native Plan exit retains its separate current-request decision and confirmation flow. Unknown action or reply shapes must not acquire approval from their labels; retain a supported denial or Cancel, with Cancel as the safe default.

Command approval requires the actual typed native command; a display description, reason or read-looking label is not command authority. The conservative literal filter rejects unsupported forms, and admitted Git diff/log/show forms require explicit `--no-ext-diff` and `--no-textconv`. Admitted SVN reads (status, info, log, list, cat, blame, propget/proplist and diff) exclude `--config-dir`/`--config-option`, and SVN diff requires `--internal-diff` so no configured diff command runs. Even an admitted command such as Git status trusts the resolved executable, repository configuration and execution environment, including pager, index-refresh and filesystem-monitor behavior; this is not an operating-system sandbox.

## Launch ceilings

An agent can only do what its UAM settings allow. Plan, read-only agent and goal-review launches must not load provider configuration that approves tools without asking UAM: repository or user allow rules, saved approvals, hooks, plugins and provider-side permission flags. Where a provider cannot be neutralised on a host, UAM refuses that launch with an explanation rather than starting it unbounded. Permission-bypass options in provider extra flags never reach a launch; options that grant tools or change the permission mode are removed under a ceiling. CLI View opens Plan and read-only chats in the provider's native read mode, or refuses when the terminal has none UAM can set, and Computer Use is never attached to these launches. A user can still leave Plan inside a native terminal, so CLI View is not a UAM-mediated boundary.

**Implementation: Partial.** Each provider's [launch route and residual limits](../../reference/architecture/permissions-notifications.md#launch-configuration-ceiling) are recorded in the engineering reference, with argv and environment regression tests; live-provider, Windows, SSH and user acceptance remain pending.

Recovery must validate a saved answer against the fresh native request and current captured ceiling before delivery. An earlier approval cannot authorize an action outside the current captured ceiling. A fresh safe denial or cancellation may replace that stale answer, but the new decision must save before wire delivery; a failed save restores the previous receipt and pending request rather than sending an unrecorded answer.

**Implementation: Partial overall.** The captured-read response guard, typed command provenance, one-time automatic choice, decoded replay validation, atomic safe-answer replacement and offered-choice projection are implemented with protocol verification and scoped source review, including the [OpenCode question distinction](../opencode/opencode.md#repeated-call-questions). Broader provider and environment coverage retains its explicit limits. Desktop choice and journey acceptance remains inconclusive; live-provider, Windows, SSH and user acceptance remain pending. See the [current gaps](../../documentation-review.md#provider-parity-decisions) and [engineering boundary](../../reference/architecture/permissions-notifications.md#captured-read-response-boundary).

- [Default](default.md)
- [Accept Edits](auto-accept.md)
- [AI Review](ai-review.md)
- [YOLO](yolo.md)

[Permission settings](../../settings/permission-defaults.md) define configuration. [Permission architecture](../../architecture/permissions-notifications.md) and the [provider checklists](../providers.md#provider-checklists) record implementation routes and limitations.
