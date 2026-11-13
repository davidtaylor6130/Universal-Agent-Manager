# Workspaces

A workspace identifies the files a chat works on, on the local computer or a selected execution environment. Its display folder, actual working directory, repository identity and optional managed worktree must remain distinct. UAM may prepare isolated work when explicitly configured, but side chats and conversation branches do not automatically request a new worktree. Source files and user-owned branches survive failed preparation, switching and cleanup.

A managed worktree must stay on its own branch. When it was switched to another branch or a detached HEAD, for example by the agent, opening or resuming the chat stops with an error that names the folder, the current and the expected branch, and the `git switch` that restores it. UAM does not adopt the other branch, because worktree cleanup removes the worktree's own branch. **Implementation: Partial.** SSH worktrees report this, with a native test against a stand-in SSH helper on macOS and Linux; local worktrees and a live SSH host are not checked. Stop always works: a turn restored after a restart whose worktree fails this check is stopped on the host without validating the workspace, which a native test covers; the next send reports the branch error until it is restored. Until it is stopped or the branch is restored, the restored turn keeps its recovery.

[New-chat workspace and starting branch](../chat/new-chat.md) · [Sidebar grouping](../sidebar/all-chats/workspace-folders.md) · [Version Control](../settings/version-control/version-control.md) · [Workspace actions](../chat/composer/workspace-actions.md) · [Engineering reference](../reference/architecture/chats-imports-workspaces.md).

[Checkpoints, rollback and workspace recovery](checkpoints.md).

[Rebuild workspace folders](folder-recovery.md) restores catalogue grouping independently of Git rollback.

[Native history import](../settings/chat-data.md#native-provider-histories) can stage an unregistered local source workspace for explicit review. Assignment names its original path without relocating history or creating directories; only selected import publishes the UAM workspace definition through the guarded folder-save path.
