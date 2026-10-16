# Chat imports and exports

Native-history import preserves provider identity and complete supported conversation content while avoiding duplicate imports, sessions without conversation content and resurrection of deleted chats. Authored text, attachments, reasoning, plans and tools qualify; whitespace, setup-only messages, empty interruption markers and compaction markers alone do not. Confirmed empty sessions remain eligible for later import after receiving content, without a deletion tombstone. Unknown or failed exports remain retryable rather than being accepted as complete. Portable chat bundles open as passive read-only transcripts; running them requires explicit reconnection of workspace, provider and permissions. Existing history must remain intact when an import or refresh fails.

[Workspaces](../workspaces/workspaces.md) · [Import reference](../reference/architecture/chats-imports-workspaces.md).
