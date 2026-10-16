# Permissions

UAM exposes four consistent permission choices: Default asks before commands and changes, Accept Edits automatically permits workspace file edits, AI Review uses the provider’s supported automatic review or UAM’s reviewer while retaining decisions that need user judgement, and YOLO enables the least restrictive supported behaviour. Map provider terminology to these meanings; a similarly named native setting is not enough. Plan/read-only and managed restrictions remain ceilings. A native terminal uses the provider’s own controls and must not imply that UAM mediates every action.

- [Default](default.md)
- [Accept Edits](auto-accept.md)
- [AI Review](ai-review.md)
- [YOLO](yolo.md)

[Permission settings](../../settings/permission-defaults.md) define configuration. [Permission architecture](../../architecture/permissions-notifications.md) and the [provider checklists](../providers.md#provider-checklists) record implementation routes and limitations.
