# MCP Servers

MCP Servers is the shared place to add, edit, enable and remove tool-server connections, with transport, execution environment, workspace scope and secret references. The guided setup validates the connection definition; sensitive endpoints, commands and arguments remain in the editor instead of cluttering collapsed summaries. Apply provider setup on the executing computer, resolve that computer’s environment references, and report actual observed connection state separately from an enabled switch.

[Provider MCP contract](../providers/mcp-connections.md) · [Central Setup](central-setup/central-setup.md) · [Environment scope](environment-scope.md).

Form/guided and JSON views edit the same draft server definitions. Support stdio commands/ordered arguments or HTTP/SSE endpoints with appropriate headers/environment-value references, plus import review and explicit invalid-field feedback. Preserve unsaved changes on view/navigation transitions. Built-in UAM services have separate protected controls and are not ordinary deletable user servers. [UAM connection](../providers/uam-tools.md).

Connection examples and placeholders use reserved example domains and synthetic paths rather than an operator’s actual host or account details.
