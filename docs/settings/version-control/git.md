# Git

Git integration supports explicitly configured workspace isolation, branches, checkpoints and supported commit or port-to-source actions. Preserve user-owned branches, ignored files and unrelated changes; changing a conversation branch does not itself request a new worktree. Git enablement is separate from SVN tracking and local snapshot choices, and remote operations must execute against the intended remote workspace.

[Local snapshots](local-snapshots.md) · [Workspaces](../../workspaces/workspaces.md).

Automatic isolation prepares a separate worktree for an eligible new chat. Default base branch is a local existing ref (empty means HEAD); Pull before first chat is a conditional fast-forward on a clean upstream-tracked source, not an unconditional fetch/reset. Port methods Merge and Squash stage changes for explicit later commit; Patch applies the exported patch without pretending an upstream publication. Failed preparation/porting preserves unrelated work and reports the exact unresolved step.
