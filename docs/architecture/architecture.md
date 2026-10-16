# Architecture

UAM is a desktop application with a React interface inside CEF, native application services and CLI provider processes. Shared services own conversation identity, organisation, configuration, memory, usage and UAM tools; provider adapters translate native protocols into those shared meanings. Platform services own OS integration, VCS services own repository operations, and SSH helpers own remote execution. The controller retains its catalogue; optional phone access publishes a restricted browser interface.

| System agreement | Responsibility |
|---|---|
| [Code structure](code-structure.md) | Layer responsibilities and dependencies |
| [Source of truth](source-of-truth.md) | Durable records, overlays and derived state |
| [Persistence](app-state-persistence.md) | Saving, recovery and data-root selection |
| [React and CEF](react-cef-transport.md) | Privileged bridge and UI projections |
| [Provider sessions](provider-sessions.md) | Structured/terminal runtime relationships |
| [Chats and workspaces](chats-imports-workspaces.md) | Conversation and directory ownership |
| [Memory](durable-memory.md) | Recall, extraction and resource ownership |
| [Permissions](permissions-notifications.md) | Native and UAM decisions |
| [Usage](usage-history.md) | History accounting and account allowances |
| [Execution environments](execution-hosts.md) | Controller/helper responsibilities |
| [Platform boundaries](platform-boundaries.md) | OS-specific implementations |
| [Provider boundaries](provider-boundaries.md) | Provider-specific implementations |
| [VCS boundaries](vcs-boundaries.md) | Git/SVN-specific implementations |
| [Build and release](build-test-release.md) | Inputs, packaging and verification |
| [Recorded decisions](decisions.md) | Deliberate product choices and unresolved scope |

[System flow](architecture_1.mmd) and [workspace flow](architecture_2.mmd) show broad connections, not proof of every implementation. [Engineering rules](../engineering/engineering.md) define testing and logging; [reference](../reference/README.md) retains source detail. [Feature guide](../README.md).

The [previous architecture entry point](../architecture.md) remains as a compatibility link.
