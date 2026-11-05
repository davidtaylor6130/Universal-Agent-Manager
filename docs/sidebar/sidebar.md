# Sidebar

The sidebar organises conversations by attention, pinning and workspace, independently of the pane layout. Keep distinct Active Chats and Pinned Chats sections rather than replacing them with a filter; Snoozed and All Chats provide the corresponding organisation paths. One conversation can have several organisational relationships, but its identity, branch family and selected state must remain consistent. Grouping must stay responsive with large histories.

Resting the pointer or keyboard focus on a local chat row for about 150 ms starts reading that chat's history in the background so a following click opens sooner; a quick pass across rows starts nothing. This warm-up does not select the chat, show its content, acknowledge its attention, update its last-opened time or move it in Active Chats. At most two warm-ups run at once, behind any explicit open. Chats that are selected, already loaded or loading, have a failed history read, have queued, held or running input, or run on a remote (SSH) computer are skipped; a remote read can start a provider history request. When more than four unopened warm chats accumulate, the oldest that is not processing is unloaded and its native copy released (a processing chat is unloaded once it stops, if still over the limit), and a warm-up that ends without a page releases its native copy too.

- [Active Chats](active/active.md)
- [Snoozed chats](snoozed/snoozed.md)
- [Pinned Chats](pinned/pinned.md)
- [All Chats](all-chats/all-chats.md)
- [Workspace folders](all-chats/workspace-folders.md)

[Search](search.md) · [Collections](collections.md).

[Chat/workspace actions](management.md) · [Custom icons](custom-icons.md) · [Folder recovery](../workspaces/folder-recovery.md).
