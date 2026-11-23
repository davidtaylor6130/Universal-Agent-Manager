# Workspace-folder recovery

Rebuild workspace folders reviews saved chats and their known workspace/host identities to reconstruct missing catalogue grouping without inventing directories or changing provider working paths. Preview ready groups separately from chats needing attention, and allow deliberate recovery only against current saved state. Missing/unavailable workspaces remain visible as problems; deleting unavailable chats is a separate explicit destructive choice, not a side effect of recovery. Removing a workspace entry and its chats must describe that scope while leaving the user's actual directory on disk unless a different supported owned-worktree action was explicitly requested.

If `folders.txt` could not be read or validated at startup, workspace changes, including rebuilding folders and deleting workspaces, are refused until the file is repaired and UAM restarts; see the [store recovery rules](../architecture/source-of-truth.md#store-recovery-rules).

[Workspace folders](../sidebar/all-chats/workspace-folders.md) · [Source of truth](../architecture/source-of-truth.md) · [Deletion](../chat/lifecycle.md).
