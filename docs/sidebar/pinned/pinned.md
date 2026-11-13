# Pinned Chats

Pinning keeps a chat easy to find in a separate persistent Pinned Chats section. Pinning is independent of running status, completion and workspace membership; unpinning does not delete or stop it. Snooze can temporarily park attention without erasing the pin, and the chat returns to the appropriate pinned and active views when it wakes.

Pin changes appear immediately while saving. If overlapping saves fail, restore the last confirmed saved pin rather than an earlier unsaved change; an older failure must not replace a newer successful save. While an older save remains unresolved, a failed current change returns to the confirmed value until that older save is acknowledged. Report the current failed change through the [shared failure notification](../../ui/notifications.md) and preserve other chat details.

Pinned chats can be put in any order: drag a row and the others slide aside to show where it lands (see [motion](../../ui/motion.md)), or focus it and press Arrow Up/Down, which moves it one place, keeps focus on it and announces the new position. Dropping it on a chat pane opens it there instead, as dragging any chat does. The order is saved in the app's settings. Until it is first changed, and for chats pinned since, the list shows the most recently used first, with newly pinned chats above the ones already placed. A snoozed pin keeps its place while it is hidden; an unpinned chat loses it, so pinning it again puts it on top. Search shows a filtered view, so rows are not reordered while searching, and a list of more than 2,000 chats keeps the default order. The phone companion shows the same order.

[Sidebar](../sidebar.md) · [Snoozing](../snoozed/snoozed.md).
