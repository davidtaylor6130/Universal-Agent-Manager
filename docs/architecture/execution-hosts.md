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

## Repository matches and launch checks

Repository matching reads VCS identity on background workers for saved workspace folders. Git matches normalized remote identity and relative workspace location; SVN matches the normalized repository root URL and checkout path. Automatic matching requires exactly one destination on a host; ambiguous matches need a manual mapping. Configured manual mappings remain available when discovery cannot establish a match. Inventory snapshots belong to the data root, folders and host connection identities, expire after 30 seconds, and are discarded when those inputs change.

An automatic assignment is rechecked against fresh source and destination identity before launch. The check belongs to the chat incarnation and assigned connection; a changed or failed match blocks launch rather than silently using another repository. A successful launch check is reusable for two seconds, with at most four concurrent checks.

## Runner startup observation

Saved `startupEnabled` expresses the desired setting; transient `runnerStartupApplied` separately reports `pending`, `enabled`, `disabled` or `failed`, with a bounded `runnerStartupError` where available. Missing applied state means unknown. Loading settings or changing runner installation identity clears confirmation.

`startup.observe` reads registration without modifying it. Linux checks the managed user unit, enablement and fragment path, including a missing unit; Windows checks the helper's exact per-user Run value. macOS advertises no runner startup support. Applying startup requires both `runnerStartup` and `runnerStartupObservation` capabilities and a matching confirmed result, so older runners cannot establish applied state by echoing the desired flag. Registration observation does not verify a real login session.

## Windows installation isolation

Windows service pipes, mutexes and shutdown events include the physical installation root's volume and file identity alongside the user, runner version and protocol. Separate managed roots can run concurrently; case, short-path and junction aliases of one root share its endpoint. Missing user or directory identity fails closed. An explicit `UAM_RUNNER_SERVICE_NAMESPACE` retains the prior namespace-based identity for isolated acceptance services.

When updating alongside a service that still owns provider processes, install into a new managed root and verify it before changing the host's `runnerDirectory`. Preserve the older service and its processes; changing the configured directory does not migrate them. Startup registration must point to the verified new executable, with any previous owned registration backed up before replacement.

Windows activation and rollback cleanup treats absent optional receipt files as already removed, while removal errors for existing files remain failures. A first installation has no rollback executable; that absence must not turn a successfully verified activation into a reported failure.

## Central configuration launch scope

Remote `process.start` binds the provider setup directory to the exact lexical workspace. A trailing directory separator or normalized dot component identifies the same workspace; unrelated directories and symbolic-link resources remain rejected. Configuration rejection alone does not prove a provider is absent: a successful start reply may have been lost. Before reporting a definitive setup failure, the proxy requires a matching, token-scoped `process.poll` reply of `session_not_found`. The probe requests acknowledged output without consuming existing spool cursors; a surviving process, foreign ownership or failed probe retains ambiguous reconnect handling.
