# Chat permission defaults

Chat Defaults and provider defaults select the initial [four chat approval modes](../providers/permissions/permissions.md) and the provider/model used for UAM AI Review. Native automatic review is used only through an established mapping; failed or uncertain review retains a user decision, and Plan/read-only restrictions remain ceilings. Defaults and explicit chat choices keep their provider/environment inheritance. They do not grant microphone, screen or other OS access, and a reviewer selection must not be mistaken for approval of a pending action.

[Chat Defaults](chat-defaults.md) · [Environment scope](environment-scope.md) · [OS permissions](permissions.md) · [Permission architecture](../architecture/permissions-notifications.md).

Changing a default, selecting YOLO or answering a manual prompt cannot widen a [captured read-only agent or goal-review restriction](../providers/permissions/permissions.md#captured-read-restrictions). Such a turn permits supported one-time read approvals and denial or cancellation; persistent grants remain unavailable. Ordinary native Plan transitions continue to require their own explicit current-request decision and native confirmation.

**Implementation: Partial.** Native Claude auto and confirmed Codex auto-review routes coexist with UAM reviewer handling; the captured-read final guard, displayed choices and saved-response checks are implemented with protocol verification and scoped source review. Broader environment scoping is outstanding, desktop choice acceptance remains inconclusive, and live-provider, Windows, SSH and user acceptance remain pending. See [current architecture](../architecture/permissions-notifications.md) and [scope](environment-scope.md).
