# Execution hosts and remote runner

Execution routing selects where a provider or worker command runs. A local host uses native process services; configured remote hosts communicate with a runner over the remote protocol. Routing is explicit app configuration, not a change to provider identity.

## Flow and invariants

`ExecutionRoutingService` resolves an operation's host, and the integration layer passes host context into launch and history services. The runner client and protocol exchange typed requests with a remote runner. The runner owns remote process execution, spooling and host-local usage collection. Preserve output framing, cancellation and exit status across the transport. Do not expose remote host credentials in logs or frontend state.

Terminal instruction preparation uses the effective provider and host `shared-setup` override, including when global shared setup is disabled. Routing confirmation fingerprints include configured MCP servers; changing a server command, environment or enabled state requires fresh confirmation before automatic admission.

Remote memory publication and context policy are separate from local memory storage. Any change to host selection must cover local defaulting, unavailable hosts and multiple-host history.

## Ownership and checks

- Routing and host configuration: `src/app/execution_routing_service.*`, `src/app/execution_routing_integration.*`, `src/common/config/execution_host_config.h`.
- Remote protocol/client/runner: `src/remote/runner_protocol.*`, `src/remote/runner_client.*`, `src/remote/runner_state.*`, `src/remote/runner_main.cpp`.
- Frontend routing: `UI-V2/src/store/slices/executionRoutingSlice.ts`, `UI-V2/src/components/modals/ExecutionRoutingDialog.tsx`.
- Focused checks: `tests/execution_routing_tests.cpp`, `tests/remote_runner_tests.cpp`, `tests/remote_delivery_exit_tests.cpp`, `tests/remote_context_policy_tests.cpp`, `tests/multi_host_history_tests.cpp`.

Spool caps and process timeouts are defined by the runner and operation; this page sets no additional budget. Preserve bounded buffering and report the actual remote exit result.
