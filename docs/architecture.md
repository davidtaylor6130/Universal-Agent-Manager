# Architecture

UAM is a desktop application with a React interface inside CEF, native application services, and provider runtimes running as separate CLI processes. Structured protocols are normalized into UAM chats; xterm.js exposes native terminal sessions. Shared services own chat organisation, branching, configuration, memory, usage and UAM tools so that provider differences do not redefine the product. SSH moves execution to a helper on another computer while the controller retains its chat catalogue; optional phone access publishes a browser interface from the controller.

- [App state and persistence](architecture/app-state-persistence.md)
- [React and CEF](architecture/react-cef-transport.md)
- [Provider sessions](architecture/provider-sessions.md)
- [Chats and workspaces](architecture/chats-imports-workspaces.md)
- [Memory](architecture/durable-memory.md)
- [Permissions](architecture/permissions-notifications.md)
- [Usage](architecture/usage-history.md)
- [Execution environments](architecture/execution-hosts.md)
- [Build and release](architecture/build-test-release.md)
- [Recorded decisions](architecture/decisions.md)
- [System flow](architecture/architecture_1.mmd) and [workspace flow](architecture/architecture_2.mmd)

The diagrams describe broad connections, not proof that every route is implemented. Protocol constraints and source locations are in [engineering reference](reference/README.md).
