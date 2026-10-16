# Checkpoints and workspace recovery

An isolated Git checkpoint records workspace state associated with a completed turn; it is distinct from branching a conversation, committing selected files or publishing upstream. Roll back turn requires an explicit supported action against the correct owned checkpoint and must describe affected files before mutation. Port-to-source and discard-worktree actions preserve unrelated files, existing branch ownership and other chats' references; conflicts, active work and incomplete cleanup must block or report recovery rather than claim success. A failed provider turn is not permission to reset a workspace automatically.

[Workspaces](workspaces.md) · [Review changes](../changes/changes.md) · [Local snapshots](../settings/version-control/local-snapshots.md) · [VCS boundaries](../architecture/vcs-boundaries.md).
