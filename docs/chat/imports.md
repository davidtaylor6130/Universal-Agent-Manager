# Chat imports and exports

Native-history import preserves provider identity and complete supported conversation content while avoiding duplicate imports, known-empty sessions and resurrection of deleted chats. Unknown or failed exports remain retryable rather than being accepted as complete. Portable chat bundles open as passive read-only transcripts; running them requires explicit reconnection of workspace, provider and permissions. Existing history must remain intact when an import or refresh fails.

[Workspaces](../workspaces/workspaces.md) · [Import reference](../reference/architecture/chats-imports-workspaces.md).
