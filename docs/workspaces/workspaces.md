# Workspaces

A workspace identifies the files a chat works on, on the local computer or a selected execution environment. Its display folder, actual working directory, repository identity and optional managed worktree must remain distinct. UAM may prepare isolated work when explicitly configured, but side chats and conversation branches do not automatically request a new worktree. Source files and user-owned branches survive failed preparation, switching and cleanup.

[New-chat workspace and starting branch](../chat/new-chat.md) · [Sidebar grouping](../sidebar/all-chats/workspace-folders.md) · [Version Control](../settings/version-control/version-control.md) · [Workspace actions](../chat/composer/workspace-actions.md) · [Engineering reference](../reference/architecture/chats-imports-workspaces.md).

[Checkpoints, rollback and workspace recovery](checkpoints.md).

[Rebuild workspace folders](folder-recovery.md) restores catalogue grouping independently of Git rollback.

[Native history import](../settings/chat-data.md#native-provider-histories) can stage an unregistered local source workspace for explicit review. Assignment names its original path without relocating history or creating directories; only selected import publishes the UAM workspace definition through the guarded folder-save path.
