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

Hidden ephemeral model-discovery processes use the runner's token-owned 60-second transient lease on protocol 3. Ordinary owned polling renews it; an app crash or transport outage beyond the lease lets the runner drainer terminate the process tree. Confirmed exit or expiry is retired lazily on the next busy/request check, and failed retirement keeps the busy guard. If final output acknowledgement is withheld beyond the lease, discovery may expire and require a catalogue retry; do not replay or attach to the expired process. Protocol 2 cannot safely launch hidden ephemeral discovery and is rejected for that route. Visible discovery and user or goal processes remain durable. This lease does not journal cleanup authority or recover tokens from older orphaned processes.

## Repository matches and launch checks

Repository matching reads VCS identity on background workers for saved workspace folders. Git matches normalized remote identity and relative workspace location; SVN matches the normalized repository root URL and checkout path. Automatic matching requires exactly one destination on a host; ambiguous matches need a manual mapping. Configured manual mappings remain available when discovery cannot establish a match. Inventory snapshots belong to the data root, folders and host connection identities, expire after 30 seconds, and are discarded when those inputs change.

An automatic assignment is rechecked against fresh source and destination identity before launch. The check belongs to the chat incarnation and assigned connection; a changed or failed match blocks launch rather than silently using another repository. A successful launch check is reusable for two seconds, with at most four concurrent checks.

## Cached health and connection attempts

A configured SSH runner with cached `ready`, `error` or `offline` status may attempt a fresh connection when its alias, platform, installation directory, version and protocol form a valid existing bridge command. Cached health is an observation, not permanent proof that the runner is unavailable. Unsupported protocols, unsafe or missing connection fields, and uninstalled, installing or disabled runners remain rejected.

`remote::CanAttemptRunnerConnection` applies this admission rule to explicit provider, terminal, worker, history and CLI operations; automatic routing still uses its health selection. The actual runner connection must independently pass nonce, version, protocol and capability checks. This does not install a runner, switch its configured directory, migrate existing processes or perform an additional SSH probe on the UI thread.

## Runner startup observation

Saved `startupEnabled` expresses the desired setting; transient `runnerStartupApplied` separately reports `pending`, `enabled`, `disabled` or `failed`, with a bounded `runnerStartupError` where available. Missing applied state means unknown. Loading settings or changing runner installation identity clears confirmation.

`startup.observe` reads registration without modifying it. Linux checks the managed user unit, enablement and fragment path, including a missing unit; Windows checks the helper's exact per-user Run value. macOS advertises no runner startup support. Applying startup requires both `runnerStartup` and `runnerStartupObservation` capabilities and a matching confirmed result, so older runners cannot establish applied state by echoing the desired flag. Registration observation does not verify a real login session.

## Windows installation isolation

Windows service pipes, mutexes and shutdown events include the physical installation root's volume and file identity alongside the user, runner version and protocol. Separate managed roots can run concurrently; case, short-path and junction aliases of one root share its endpoint. Missing user or directory identity fails closed. An explicit `UAM_RUNNER_SERVICE_NAMESPACE` retains the prior namespace-based identity for isolated acceptance services.

When updating alongside a service that still owns provider processes, install into a new managed root and verify it before changing the host's `runnerDirectory`. Preserve the older service and its processes; changing the configured directory does not migrate them. Startup registration must point to the verified new executable, with any previous owned registration backed up before replacement.

Windows activation and rollback cleanup treats absent optional receipt files as already removed, while removal errors for existing files remain failures. A first installation has no rollback executable; that absence must not turn a successfully verified activation into a reported failure.

## Central configuration launch scope

Remote `process.start` binds the provider setup directory to the exact lexical workspace. A trailing directory separator or normalized dot component identifies the same workspace; unrelated directories and symbolic-link resources remain rejected. Configuration rejection alone does not prove a provider is absent: a successful start reply may have been lost. Before reporting a definitive setup failure, the proxy requires a matching, token-scoped `process.poll` reply of `session_not_found`. The probe requests acknowledged output without consuming existing spool cursors; a surviving process, foreign ownership or failed probe retains ambiguous reconnect handling.

## Instruction-file CEF lane

The configured instruction editor previously showed Loading when directory enumeration blocked the CEF user-blocking lane: `AppSeedMarkdownTask→ListEntries→opendir` blocked all 491 samples in a one-second capture. Alpha-102 now passes a real CEF test with this shared queue blocked. A separate repeat Documents prompt follows the alpha-102 ad-hoc signature; TCC logs show a code-requirement mismatch, not a filesystem defect. Stable local signing is tracked separately.

## Alpha-102 installer admission checkpoint

The alpha-102 per-host scan correction passed independent review. Alpha-102 Windows helper activation failed on a loaded executable and rollback Copy, so alpha-100 was restored. Alpha-103 then installed and both official Homelab and Windows updates reported Ready. Read-only resident verification confirms Linux PID 3021205/start 5332272 maps installed helper SHA `74c34fc40ecf947a36f35d287c3ffae6b1df94fded858fdc69e53f684235a1c5`; Windows fresh PID 5236 was created after the alpha-103 binary and transaction writes and passed protocol-3 nonce hello and safe-path checks, without an observed mapped-image hash. Both used source fingerprint `3c2fa6c14497e036c320921a659751dec77d5f30950ee55d2da196c1ac930601`. The sidecar metadata still shows previously known fingerprints by deliberate deferral until the next-wave helper contract; no resident failure was observed. Preserve the strict busy guard and runtime authority; the helper verification performed no service restart, provider input, remote file write or InferDeck action.

## Remote isolated worktrees and installer quiescence

Remote isolated worktree creation uses host-bound asynchronous preparation. A saved local receipt binds the chat incarnation, source, selected host connection identity, remote Git common directory, destination, branch, immutable base commit and operation identity. Remote Git work uses bounded protocol-3 runner operations with acknowledged output; controller-local path, symlink and canonicalization assumptions do not apply to remote paths. The alpha-101 final CTest passed 2,242 framework cases, 18 auxiliary cases and 19/19 gates, including all 20 remote-worktree cases and reviewed pre-start/cleanup fixes. Alpha-103 is installed/reopened; its canonical full CTest passed 2,247 framework cases, zero failures, all 18 auxiliary tests and all 19 gates. Both official helper updates reported Ready. Linux PID 3021205/start 5332272 maps the installed helper hash; Windows fresh PID 5236 passed protocol-3 nonce and safe-path checks, with no mapped-image hash claim. Existing helper sidecar metadata remains at previously known fingerprints by deliberate deferral to the next-wave contract. No busy guard or runtime authority was bypassed. No provider prompt or InferDeck startup occurred. Existing foreign local-worktree and retention ownership remains unchanged.

Recovery is deliberately evidence-based: mark creation complete only after exit code zero and fresh Git proof that the target is unlocked and non-prunable. A pending operation must poll the exact old runner and token; a missing process alone is not success. A definite failure archives the receipt as `*.json.failed`, keeps remote artifacts and branch intact, and allows a later fresh attempt. If the controller crashes after the remote operation settles but before Remove is recorded, an exited helper process record can remain until cleanup or restart; recovery inspects the original operation and never replays Git creation. The final alpha-101 CTest receipt covers the post-review ACP and remote-worktree fixes; user/platform acceptance and helper update remain separate. See `.UAM/AINotes/ssh-worktree-plan.md`, `.UAM/AINotes/targeted-integration-prestart-gates.json`, `.UAM/AINotes/goal-neighbors-fourth-gates.json`, `.UAM/AINotes/settlement-claude-review.json` and `.UAM/AINotes/alpha101-final-cross-helper-artifacts.json`.

Runner installation quiescence is also in progress. It may stop only owned hidden ephemeral discovery after confirming there is no visible, user or goal runtime and no selected import activity. Preserve stop tokens until exit is confirmed, wait through a bounded continuation, then recheck the complete host identity before bootstrap. A visible runtime, unknown owner, selected import, unconfirmed stop or stale reservation keeps installation blocked. Its earlier 15-case slice is covered by the final alpha-101 CTest. Alpha-102’s per-host scan change has independent review with no blocker; helper replacement remains stopped by strict runtime-busy admission before upload. This does not authorize standalone installation or bypassing the helper's busy guard.


## Alpha-106 helper rollout checkpoint

Alpha-106 is installed and open with Backend connected. The full native CTest passed 2,258 core cases, zero failures, all 18 auxiliary tests and all 19 gates. The Windows official GUI update reports Ready; the native candidate still awaits resident verification. Linux remains on alpha-103 with zero children and bridges and no active summary flags. Its helper contract update is pending because the controller busy guard and dynamic startup chooser blocked the GUI retry. The CLI bootstrap is being prepared without a reset or changes to runtime flags. Preserve the busy guard and current ownership; do not infer helper activation from the GUI Ready state.
