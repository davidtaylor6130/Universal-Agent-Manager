# Chat imports and exports

UAM distinguishes native provider-session import from portable chat bundles. Both preserve supported history and identity without overwriting unrelated saved work; neither discovery nor opening a transcript authorizes execution. Native imports retain provider ownership and deletion rules, while bundle imports create passive UAM copies with collision handling. Failed or incomplete reads must remain visible rather than being accepted as complete or destroying existing history.

[Native session import](native-imports.md) owns eligibility, discovery and refresh. [Chat Data settings](../settings/chat-data.md) owns portable export/import. [Workspaces](../workspaces/workspaces.md) · [Import reference](../reference/architecture/chats-imports-workspaces.md).
