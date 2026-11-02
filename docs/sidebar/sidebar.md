# Sidebar

The sidebar organises conversations by attention, pinning and workspace, independently of the pane layout. Keep distinct Active Chats and Pinned Chats sections rather than replacing them with a filter; Snoozed and All Chats provide the corresponding organisation paths. One conversation can have several organisational relationships, but its identity, branch family and selected state must remain consistent. Grouping must stay responsive with large histories.

Resting the pointer or keyboard focus on a local chat row for about 150 ms starts reading that chat's history in the background so a following click opens sooner; a quick pass across rows starts nothing. This warm-up never selects the chat, shows its content, marks it read or changes its recency or Active Chats placement. At most two warm-ups run at once, already-loaded or loading chats are skipped, remote (SSH) chats are not warmed because their read can start a provider history request, and only the four most recent unopened warm chats are kept in memory.

- [Active Chats](active/active.md)
- [Snoozed chats](snoozed/snoozed.md)
- [Pinned Chats](pinned/pinned.md)
- [All Chats](all-chats/all-chats.md)
- [Workspace folders](all-chats/workspace-folders.md)

[Search](search.md) · [Collections](collections.md).

[Chat/workspace actions](management.md) · [Custom icons](custom-icons.md) · [Folder recovery](../workspaces/folder-recovery.md).
