# Side chats

A side chat is a separate conversation for related work that retains its relationship to the originating chat. It does not automatically request a Git worktree or modify the parent conversation; workspace isolation is a separate explicit choice. UAM preserves its own side-chat relationship across providers. Provider-spawned subagents are a different mechanism and must not be confused with user-created side chats.

[Subagent history](agent-history.md) · [Workspaces](../workspaces/workspaces.md).

Return to main chat preserves the side conversation, Resume side chat reopens it, and Dismiss side chat is a distinct explicit close/discard action. The main conversation is reference context rather than silently shared execution history; these navigation choices must not automatically port workspace changes.

Side-chat reference context excludes quarantined partial compaction summaries while retaining completed summaries and ordinary conversation in proven saved order. If the selected parent history cannot distinguish those bytes safely, creation holds without changing the parent or creating a side conversation; loaded and paged history follow the same boundary.
