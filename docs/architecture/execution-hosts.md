# Execution environments and helpers

The controller selects local execution or a configured SSH environment and passes provider, workspace and resource context to that computer. The remote helper owns remote processes, output and reconnection, while the controller remains the catalogue owner. Authentication, helper identity, cancellation and reconnect must not silently switch environment or replay delivered work.

[Execution environments](../remote-environments/remote-environments.md) · [Routing](../remote-environments/routing.md).

[Engineering reference](../reference/architecture/execution-hosts.md).
