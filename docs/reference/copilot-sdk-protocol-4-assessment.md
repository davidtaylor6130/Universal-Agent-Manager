# Copilot SDK protocol 4 assessment

Source assessment, 2026-11-23. Nothing here comes from a live provider run or an authenticated account, and it does not establish acceptance. UAM still admits only [verified protocols](../providers/copilot/copilot.md); this page records why protocol 4 stays refused and what porting it needs.

## Sources

- `@github/copilot-sdk@1.0.34`, the newest published SDK on npm: `SDK_PROTOCOL_VERSION = 3`, accepting 3 only, paired with CLI 1.0.112 (`copilotCliVersion`).
- `@github/copilot-linux-x64@1.0.113` and `@1.0.114` from npm, inspected without running them. The Node single-executable blob embeds a `copilot.tgz` holding `cli-main.js`, a bundled `copilot-sdk/` (`SDK_PROTOCOL_VERSION = 4`, accepting 4 only) and the native `prebuilds/<platform>/runtime.node`.
- The bundled `changelog.json` has no protocol or SDK entry for 1.0.113 or 1.0.114.

CLI 1.0.112 reports protocol 3. 1.0.113 (published 2026-11-17) is the first stable release that reports 4; its prereleases were not inspected. The server answers `ping`, `connect` and `status.get` with its one protocol number and has no negotiation. A client does not send its protocol version.

## What changed for UAM's requests

Method names are identical in both SDKs. These are unchanged: `ping`, `status.get`, `models.list` (apart from two deprecated price fields UAM does not read), `account.getQuota`, `session.send` (including `mode: immediate`), `session.abort`, `session.model.*`, `session.history.compact`, `session.permissions.*`, `session.ui.handlePendingExitPlanMode`, `session.managedSettings.get`, `session.metadata.snapshot` and `session.tools.*`. The permission, user-input and exit-plan-mode request data, and the SDK code that answers them, are also unchanged.

`session.create` and `session.resume` parameters moved from flat keys to groups. The keys UAM sends move as follows:

| Protocol 3 | Protocol 4 |
|---|---|
| `requestPermission` | `permissions.request` |
| `permissionMode`, `sandboxConfig` | `permissions.mode`, `permissions.sandbox` |
| `requestUserInput`, `requestExitPlanMode` | `interaction.requestUserInput`, `interaction.requestExitPlanMode` |
| `streaming`, `includeSubAgentStreamingEvents` | `events.streaming`, `events.includeSubAgentStreamingEvents` |
| `mcpServers`, `envValueMode` | `mcp.servers`, `mcp.envValueMode` |
| `availableTools`, `excludedTools` | `tools.availableTools`, `tools.excludedTools` |
| `reasoningEffort` | `inference.reasoningEffort` |
| `managedSettings`, `excludedManagedSettings`, `enforceManagedModelDefaults` | `policy.*` |
| `configDir`, `disableResume` | `configDirectory`, `suppressResumeEvent` |

The 1.0.114 server reads only the grouped keys. For example, it sets `enablePermissionCallback` from `permissions?.request ?? false` and `enableUserInputCallback` from `interaction?.requestUserInput ?? false`. The native runtime in both versions contains the error "Session option '…' has moved to '…' in protocol 4; update the SDK or request payload". 1.0.114 also contains "Legacy SDK options are deprecated; use grouped options".

## What changed in UAM's events

In the protocol-4 event schema, `data.parentToolCallId` is gone from `assistant.message`, `assistant.message_delta`, `assistant.usage`, `tool.execution_start` and `tool.execution_complete`; only the envelope `agentId` remains. UAM uses `parentToolCallId` to attach subagent tool rows and history to their parent task tool, and in managed continuation's tool checks.

`tool.execution_partial_result`, which UAM reads for streamed `partialOutput`, is replaced by `tool.shell_output`. The `terminal` content type in `tool.execution_complete` is replaced by `shellExit`. `providerRouting` is added to session start, resume and model-change data, which is additive.

## Why protocol 4 stays refused

UAM's current setup request sends `requestPermission` and `requestUserInput` flat. Under protocol 4, a server either rejects that request or, if it accepts the flat shape, leaves the permission and user-input callbacks off. In the second case UAM would no longer see Copilot's permission requests, and its approval ceilings would not apply. That is not safe to infer. Subagent attribution and partial shell output also need new mappings.

Porting needs the grouped setup shape on create, resume and managed continuation, `agentId`-based child attribution, and `tool.shell_output` handling. Each needs fixture coverage of the protocol-4 wire messages before 4 joins the verified list.

## Unknown without a live run

- Whether 1.0.113 and 1.0.114 reject flat keys or migrate them with a warning, and which key is reported first.
- What the server does with tool permission requests when `permissions.request` is absent: deny, auto-approve or apply its own default.
- Whether managed continuation's `toolFilterPrecedence` is accepted on create as `tools.filterPrecedence`.
- The exact `tool.shell_output` payload the server emits, and whether `agentId` reliably maps to the `subagent.started` tool call.
- Whether the 1.0.113 prereleases report 3 or 4.
