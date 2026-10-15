# Chat lifecycle

Users can create, select, rename, delete and pin chats without losing another chat’s history or runtime state. Deletion must respect related chats, native-history ownership and workspace ownership, show progress for meaningful background cleanup, and report incomplete cleanup honestly. Selecting a chat must restore its own saved context without sending a prompt. Display organisation does not determine whether a provider process is running.

[Sidebar](../sidebar/sidebar.md) · [Workspace ownership](../workspaces/workspaces.md) · [Persistence](../architecture/app-state-persistence.md).
