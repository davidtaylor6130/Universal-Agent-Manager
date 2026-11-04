# Pinned Chats

Pinning keeps a chat easy to find in a separate persistent Pinned Chats section. Pinning is independent of running status, completion and workspace membership; unpinning does not delete or stop it. Snooze can temporarily park attention without erasing the pin, and the chat returns to the appropriate pinned and active views when it wakes.

Pin changes appear immediately while saving. If overlapping saves fail, restore the last confirmed saved pin rather than an earlier unsaved change; an older failure must not replace a newer successful save. While an older save remains unresolved, a failed current change returns to the confirmed value until that older save is acknowledged. Report the current failed change through the [shared failure notification](../../ui/notifications.md) and preserve other chat details.

[Sidebar](../sidebar.md) · [Snoozing](../snoozed/snoozed.md).
