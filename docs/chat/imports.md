# Chat imports and exports

UAM distinguishes native provider-session import from portable chat bundles. Both preserve supported history and identity without overwriting unrelated saved work; neither discovery nor opening a transcript authorizes execution. Native imports retain provider ownership and deletion rules, while bundle imports create passive UAM copies with collision handling. Failed or incomplete reads must remain visible rather than being accepted as complete or destroying existing history.

[Native session import](native-imports.md) owns eligibility, discovery and refresh. [Chat Data settings](../settings/chat-data.md) owns portable export/import. [Workspaces](../workspaces/workspaces.md) · [Import reference](../reference/architecture/chats-imports-workspaces.md).

Portable exports omit private runner and provider-companion credentials; exporting does not change the original chat or its execution authority.

Portable bundles carrying UAM-owned native media must include validated original bytes and preserve each message and archival queued-input association. Import binds those originals to the final imported chat identity, including collision-renamed chats, while keeping queued content passive. Renaming, pinning and other metadata saves must preserve the archived associations for later export. Validate the complete incoming original set before publishing any chat; missing or corrupt originals must not become a successful metadata-only import. Opaque provider URIs remain references and do not authorize fetching bytes.
