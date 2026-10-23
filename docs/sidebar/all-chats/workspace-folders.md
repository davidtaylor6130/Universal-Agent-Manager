# Workspace folders

Workspace folders group chats around user-selected working directories, with one level of grouping in this release. Display organisation is separate from the actual provider working directory, native session identity and managed Git worktree. Imported linked worktrees should remain associated with their canonical workspace without rewriting the directory in which their provider actually runs. Moving a chat must wait for any pending final terminal history under its existing workspace; a blocked move retains grouping and settings and provides an explicit retry. See [terminal continuity](../../providers/terminal-view/session-continuity.md).

[Workspaces](../../workspaces/workspaces.md).
