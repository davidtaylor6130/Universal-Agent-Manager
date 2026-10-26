# Branching

Branching creates a separate conversation from a chosen point without changing the original. UAM owns the branch relationship and supplies the required context when native branching is unavailable; compacted context must stay compacted rather than replaying superseded history. Conversation branching and Git branch/worktree isolation are separate choices. An empty chat should create a new chat instead of a historical branch; native branching refuses any point that is not a user message. A Git branch chosen in the composer before the first user message is kept pending and created when that message is sent.

Retained originals must stay readable in the new conversation. If a required original cannot be verified during preparation, creation refuses without changing the source chat. This retention rule does not establish native-original input delivery or provider continuation support.

[Workspaces](../workspaces/workspaces.md) · [Current branching evidence](../reference/architecture/chats-imports-workspaces.md).
