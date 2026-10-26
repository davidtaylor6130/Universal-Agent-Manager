# Companion protocol, version 1

Technical reference for the restricted transport shared by the browser companion and native clients. The main UAM process owns chats, provider processes, downstream SSH and persistence. This extends the existing HTTP protocol; it does not add a provider runtime, standalone service or WebSocket. Product agreements remain in [Phone access](../phone-access.md), [Phone browser sections](../phone-browser.md) and [Phone settings](../settings/phone-access.md). Native-client product wording and acceptance are maintained by the parent implementation task.

## Connection and authorization

Send `POST /api` with `Authorization: Bearer <token>` and UTF-8 JSON `{"action":"actionName","payload":{}}`. Payload may be omitted or null for actions with no parameters; otherwise it must be an object. Replies are the existing action's JSON value, or `{"error":"message"}` with an HTTP error status. Binary bridge callbacks are unavailable. Every capabilities, settings, upload, commit and response-chunk request passes the same bearer authentication and origin check before body parsing or transfer lookup.

The listener remains `127.0.0.1`, with the existing opt-in HTTPS proxy and configured HTTPS origin. Browser `Origin` must exactly match the loopback origin or configured HTTPS origin. A native HTTP client may omit `Origin`; it still requires the bearer token and the published HTTPS route. Do not disable certificate verification. Token issuance, pairing, publication configuration, system permissions and TLS settings remain desktop-owned. No authentication headers or request bodies are logged or persisted by the transfer implementation.

Wire requests remain capped at **65,536 bytes** including JSON syntax. Clients send ordinary JSON below **61,440 bytes** and upload the entire JSON request envelope for larger requests. Existing clients remain compatible with ordinary requests, gzip responses, revision polling and response chunks.

## Capabilities and host identity

`getCompanionCapabilities {}` returns:

```json
{
  "protocolVersion": 1,
  "mainHost": {
    "id": "local",
    "label": "This computer",
    "transport": "local",
    "platform": "",
    "architecture": "",
    "instanceId": "opaque-main-process-boot-id"
  },
  "readOnly": false,
  "actions": [
    {"action": "getInitialState", "chatScoped": false, "readOnly": true},
    {"action": "sendAcpPrompt", "chatScoped": true, "readOnly": false}
  ],
  "features": {
    "revisionPolling": true,
    "responseChunks": true,
    "requestChunks": true,
    "byteAttachmentStaging": true,
    "chatRename": true,
    "chatDelete": true,
    "chatBranch": true,
    "settingsReadOnly": false
  },
  "restrictions": {
    "settingsReadOnly": false,
    "importedChatsReadOnly": true,
    "desktopSelectionReadOnly": true,
    "desktopTokensUnavailable": true,
    "globalConfigurationReadOnly": false,
    "configurationDefinitionsReadOnly": true,
    "connectionAdministrationReadOnly": true,
    "attachmentPathsRequireStaging": true,
    "automaticReplay": false
  },
  "writableSettingsActions": ["setProviderChatDefaults", "setMemorySettings", "setUamAgentPreferences", "setStorageSettings", "setVcsSettings", "setProviderMcpConfiguration", "setProviderAvailability"],
  "unavailableActions": {
    "deleteSessions": "Bulk deletion requires desktop ownership."
  },
  "limits": {
    "wireRequestBytes": 65536,
    "directRequestBytes": 61440,
    "maxRequestBytes": 33554432,
    "requestChunkBytes": 32768,
    "maxRequestTransfers": 4,
    "requestTransferTotalBytes": 33554432,
    "requestTransferTtlSeconds": 120,
    "maxInFlightRequests": 8,
    "inFlightRequestTotalBytes": 33554432,
    "maxResponseBytes": 33554432,
    "maxResponseTransfers": 16,
    "responseTransferTotalBytes": 33554432,
    "responseTransferTtlSeconds": 120
  }
}
```

The example abbreviates `actions`; the actual response enumerates every permitted action below, including the three upload operations. `readOnly` on an action describes its product operation, not whether caching, model discovery, allowance refresh or paging may update internal state. Terminal attach/read/write share one mutating action. `mainHost.id` is the controller's existing local execution-host ID, distinct from an SSH execution-host ID and a chat ID. `instanceId` is the existing main-process boot ID, stable during that process lifetime and changed on restart; it is not a durable hardware identity. Label/platform/architecture come from the existing host record and may be generic or empty. Platform build exclusions and existing provider/runtime validation can still reject a listed shared action.

## Settings projection and selective writes

`getCompanionSettings {}` returns `{"readOnly":false,"settings":{...}}`; this is the safe product-settings projection, not the desktop publication/token configuration action of the same name. The identical projection appears in companion `getInitialState`, preserving its envelope and revision polling. Capabilities lists the exact permitted mutators in `writableSettingsActions`. Connection bind/TLS/publication configuration, tokens, provider commands and agent/skill/MCP definitions remain desktop-owned. No cloud or account service is added.

The projection retains appearance/sidebar values, active/default provider IDs, provider availability, favorite UAM agent IDs and cycle shortcut; app memory level/enablement, idle delay, recall budget and worker provider/model bindings; permission-reviewer provider/model and goal/output/setup limits; managed-worktree cleanup enablement/retention and backup-log/HTTP-cache retention. `providerChatDefaults[providerId]` keeps only `modelId`, `reviewerModelId`, `approvalMode`, `commandSafetyTier`, `memoryEnabled`, `memoryLevel`, `reasoningEffort`, `serviceTier`, `smallModelMode`, `featurePreference`, `hiddenModelIds` and `hiddenProviderIds`. `hostScopedSettingsValues[sshHostId]` retains the effective `defaultNewChatProviderId` and the same safe provider-default fields, without copying secret-bearing definitions. Worker bindings retain only `workerProviderId` and `workerModelId`; raw extraction diagnostics are omitted. Initial state also projects `memoryActivity` to counts, timestamps, worker IDs, timeout/cancellation/exit facts and a fixed `status` enum (`running`, `timedOut`, `cancelled`, `completed`, `failed` or `unknown`), without raw last-status, worker-status, output or error strings.

`executionHosts[]` retains only `id`, `label`, `transport`, `runnerStatus`, `platform`, `architecture`, `lastSeenAt`, `customIcon` and `customIconAsset` when present. SSH aliases, runner paths, instruction bodies, MCP definitions, central provider configuration, voice-server configuration and unknown future fields are omitted, including unknown fields nested inside provider defaults or worker bindings. Initial state also omits desktop CLI diagnostics/version-management details and shell-action configuration, and restricts provider records to existing display/protocol fields. Ordinary authenticated transcript/tool output, chat workspace identity and permission requests remain available. Desktop CEF serialization is unchanged.

Every mutation below validates a nonempty partial payload and rejects unknown fields before either direct or uploaded dispatch. Omitted fields preserve their current values through the existing handler. Required companion host IDs are explicit configured IDs; no setting action creates a host. Provider IDs are the six canonical CLI IDs. Settings changes use existing persistence acknowledgements and failure rollback; clients retain failed/uncertain drafts and refresh deliberately, with no automatic replay. A saved default does not rewrite existing explicit chat choices.

| Existing action | Companion payload and result |
|---|---|
| `setProviderChatDefaults` | `{scopeExecutionHostId?,defaultProviderId?,defaults?:{providerId:{partial default fields}}}` → `{}`; provide a default provider or at least one provider-default field |
| `setMemorySettings` | `{enabledDefault?,levelDefault?,idleDelaySeconds?,recallBudgetBytes?,workerBindings?:{chatProviderId:{workerProviderId?,workerModelId?}},permissionReviewerProviderId?,permissionReviewerModelId?,goalMaxLoopIterations?,acpTurnOutputLimitMiB?,acpSetupInactivityTimeoutSeconds?}` → `{}` |
| `setUamAgentPreferences` | `{favoriteUamAgentIds?,uamAgentCycleShortcut?}` → saved `{favoriteUamAgentIds,uamAgentCycleShortcut}` |
| `setStorageSettings` | `{backupLogRetentionDays?,httpCacheClearIntervalDays?}` → `{}`; resolve omitted values after pending storage publication is adopted |
| `setVcsSettings` | `{worktreeCleanupEnabled?,worktreeRetentionDays?}` → `{}`; companion cannot change Git/SVN enablement or other VCS configuration |
| `setProviderAvailability` | `{executionHostId,providerId,enabled}` → `{}`; existing explicit per-provider/environment availability switch |
| `getProviderMcpStatus` | `{executionHostId}` → sanitized status below; no caller-supplied paths or definitions |
| `setProviderMcpConfiguration` | `{executionHostId,providerId,serverId,enabled}` → sanitized status below; toggle an existing configured server or protected `uam-mcp`/`shared-setup` control only |

Provider-default scope may be omitted/empty/`*` for All environments, or a configured SSH host ID for its override. It cannot create a distinct local override. Provider fields use the existing model-ID, reasoning, speed-tier, approval and safety rules. Optional hidden-model/provider lists contain at most 4,096 bounded IDs. Provider `memoryLevel` is `default`, `off`, `readOnly`, `strict`, `balanced` or `open`; app `levelDefault` has the same concrete levels without `default`. A supplied level also carries the corresponding enablement so switching from Off works. A supplied enablement preserves the applicable concrete level; enabling Off restores provider inheritance or the app's Strict baseline. Contradictory supplied enablement/level pairs fail 400. Other omitted defaults remain untouched.

Memory idle delay is 30–3,600 seconds, recall budget 512–8,192 bytes, goal loop limit 1–200, turn-output limit 256–4,096 MiB and ACP setup inactivity timeout 60–3,600 seconds. Model-only worker edits require an existing binding and preserve its provider. Changing worker provider clears an omitted incompatible model; retaining the same provider preserves an omitted model. Reviewer provider/model may be empty to select the existing fallback. Favorite agent IDs use the existing identifier rules with at most 64 entries; the unchanged native handler normalizes duplicates/built-ins. Shortcut is `shift+tab`, `control+shift+tab`, `alt+shift+tab`, `meta+shift+tab` or `disabled`. Log/cache retention is 0–3,650 integer days, with 0 disabling age expiry/periodic clearing; worktree retention is 1–3,650 days. Worktree edits fill the native handler's required Git/SVN flags from current state on the UI thread, preserving them.

MCP status preserves observed connection state separately from next-launch enablement:

```json
{
  "executionHostId": "local",
  "computerUseBuiltInAvailable": true,
  "repairMode": "next-session-launch",
  "connectedProofAvailable": true,
  "handshakeProofAvailable": true,
  "providers": [{
    "providerId": "codex-cli",
    "uamControlEnabled": true,
    "sharedSetupEnabled": true,
    "status": "configured-unverified",
    "detail": "Configuration is available; a live connection has not been confirmed.",
    "activeSessionCount": 0,
    "configuredSessionCount": 0,
    "connectedSessionCount": 0,
    "additionalServerCount": 1,
    "providerSessionFailed": false,
    "mcpServers": [{"id":"configured-server-id","name":"Tools","enabled":true}]
  }]
}
```

The real status lists all six providers. Status can be `disabled`, `missing`, `configured-unverified`, `failed`, `handshake-confirmed` or `connected`, with `unknown` for an unrecognized future state. The companion creates bounded fixed explanatory text instead of forwarding raw provider diagnostic text. Server rows contain only ID, display name and enabled state; commands, arguments, paths, URLs, headers, environment references/values, capability IDs and unknown fields are omitted. Failures retain HTTP status but use generic settings/MCP messages rather than forwarding raw save/provider errors. A configured or enabled switch is never a live-connection claim.

## Safe action surface

All actions marked chat-scoped in capabilities require an explicit nonblank `chatId` of at most 256 bytes; they never default to desktop selection. Existing handler schemas, optimistic revision checks, persistence acknowledgement, runtime/host checks and provider permission controls continue to apply. Mutating chat actions are checked against the current summary on the CEF UI thread and reject `importedReadOnly` chats with 403. Ordinary read-only chat access remains available. Managed/internal chats absent from that visible summary are not mutation targets.

The following are read operations. Optional fields are indicated with `?`; braces show payload keys, not a different envelope.

| Action | Payload / result |
|---|---|
| `getCompanionCapabilities` | `{}` → response above |
| `getCompanionSettings` | `{}` → selective settings projection above |
| `getInitialState` | `{knownBootId?,knownStateRevision?}` → existing summary state or `{unchanged:true,bootId,stateRevision}`; `summaryOnly:true` is enforced |
| `getChatMessages` | `{chatId,messagesDigest?,deferToolCallContent?,refreshNative?,limit?,before?}` → existing transcript/digest/page result; limit 1–200 |
| `getChatFileChangeReceipts` | `{chatId}` → existing recorded provider edits |
| `listManagedSubagents` | `{chatId,runId?,offset?:0..10000,limit?:1..100}` → bounded managed-run listing and current control scope |
| `getManagedAgentTranscript` | `{chatId,transcriptChatId}` → the parent-scoped stored child conversation; no execution authority |
| `getToolCallContent` | `{chatId,toolCallId,messageIndex?,offset?}` → existing lazy tool content result |
| `getAttachmentImagePreview` | `{chatId,messageIndex,attachmentId}` → existing owned message image preview; never accepts a path |
| `listUamAgents` | `{chatId}` → `{agents:[{id,description,builtIn}],errors:[...]}` |
| `discoverProviderModels` | `{chatId}` → existing chat/workspace/provider model catalogue; global implicit scope is unavailable |
| `getProviderAllowances` | `{executionHostId,refresh?:false}` → existing account-allowance service result; no CLI command or credential input |
| `getUsageHistory` | `{executionHostId?,refreshId,startMs,endMs,windowDays,priceOverrides?,snapshotId?,offset?}` → existing usage JSON or usage page |
| `listRemoteDirectories` | `{executionHostId,directory}` → existing configured SSH-host directory listing; no SSH setup or host edits |
| `getCompanionResponseChunk` | `{id,offset}` → response-transfer chunk below |

Usage `windowDays` is 0, 1, 7, 30 or 90; All Time uses 0 and `startMs:0`. `executionHostId` is a configured host ID (`local` for the main device, `*` for usage across machines). A usage continuation repeats the original request fields with `snapshotId` and unsigned `offset`. `priceOverrides` is the existing array of `{providerId,modelId,speed,inputPerMillion?,cachedInputPerMillion?,cacheWritePerMillion?,outputPerMillion?}`; speed is `standard`, `fast` or `ultrafast`. Usage pages have `{usagePage:1,snapshotId,refreshId,offset,nextOffset,totalBytes,chunk}`; concatenate their UTF-8 JSON strings, using `nextOffset` until null. Usage snapshots retain existing desktop browser ownership, so simultaneous desktop/mobile large usage refreshes can invalidate one another; surface 409 and require deliberate refresh. There is no silent continuation restart.

The following mutate only the requested chat or create a chat. Their responses remain the existing handlers' responses, usually `{}`, unless noted.

| Action | Payload |
|---|---|
| `createSession` | `{title?,folderId?,providerId?,executionHostId?,routingMode?,workspaceDirectory?,defaults?,gitBaseBranch?}`; force `selectChat:false`; returns `{chatId}`; sharing another chat's worktree is unavailable |
| `openNativeSessionChat` | `{chatId,nativeSessionId,refreshHistory?}`; force `selectChat:false` |
| `renameSession` | `{chatId,title}` |
| `deleteSession` | `{chatId}` only; ordinary related-chat/runtime/workspace deletion safeguards apply |
| `branchFromMessage` | `{chatId,messageIndex,content?,operationId?,retainedAttachmentIds?,retainedMarkdownStoreFiles?,regenerate?:true}`; force `selectChat:false`; returns `{chatId,warning?}` |
| `setChatPinned` | `{chatId,pinned,expectedPinned?}` |
| `setChatSnoozedUntil` | `{chatId,untilMs}`; unsigned/zero wake time, 0 clears snooze |
| `setChatSettled` | `{chatId,settled?:true}` |
| `acknowledgeChatAttention` | `{chatId,attentionRevision}` |
| `recordRuntimeActivity` | `{chatId}` |
| `setChatModel` | `{chatId,modelId,modelRole?:"worker"}`; optional reviewer role uses existing validation |
| `setChatProvider` | `{chatId,providerId}` |
| `setChatCodexOptions` | `{chatId,reasoningEffort,serviceTier?,serviceTierExplicit?}` |
| `setChatApprovalMode` | `{chatId,modeId}` |
| `setNativeModelOption` | `{chatId,nativeSessionId,executionHostId,lifetime,optionId,value:string|null}`; exact current native session and advertised choice required |
| `refreshNativeModelOptions` | `{chatId,nativeSessionId,executionHostId,lifetime}`; current native ownership required; imported read-only chats denied |
| `cancelManagedSubagent` | `{chatId,runId,controlScopeId}`; current parent listing and runtime ownership required |
| `submitManagedSubagentFollowup` | `{chatId,runId,controlScopeId,afterTurnId,clientRequestId,text}`; text at most32KiB, exact current turn and same-request retry rules retained |
| `setChatCommandSafetyTier` | `{chatId,commandSafetyTier}`; existing `off`, `acceptEdits`, `aiReview`, `yolo` controls |
| `setChatUamControlEnabled` | `{chatId,enabled,applyOnNextLaunch?}` |
| `setChatUamAgent` | `{chatId,agentId}` |
| `setChatMemoryEnabled` | `{chatId,enabled,memoryLevel?}` |
| `setChatSmallModelMode` | `{chatId,enabled}` |
| `setAcpConfigOption` | `{chatId,configId,value}`; existing provider session option validation |
| `compactAcpSession` | `{chatId}` |
| `manageQueuedAcpPrompt` | `{chatId,operation,...existing revision-bound queue fields}`; `remove`/`steer` require `{index,expected:{id,revision}}`; other operations `edit`, `reorder`, `discardUnsent` retain their existing schemas |
| `stageChatAttachments` | Byte staging schema below |
| `sendAcpPrompt` | `{chatId,text,attachments?,goalMode?,goalId?,computerUseMode?,steerNow?}`; attachments must be unchanged staging results; host `markdownStoreFiles` are unavailable |
| `resolveAcpPermission` | `{chatId,requestId,optionId,cancelled?}` |
| `resolveAcpUserInput` | `{chatId,requestId,answers:{questionId:[answer,...]}}` |
| `cancelAcpTurn` | `{chatId}` |
| `stopAcpSession` | `{chatId,purpose?:"interrupt"}`; existing scoped stop, not global process control |
| `companionCliTerminal` | `{chatId,terminalId,operation?:"attach",startupTime?,cursor?,data?}`; read/write require exact startup identity; read requires unsigned cursor; writes remain capped at 64 KiB |

Native model/reasoning editors retain the original phone and chat execution context locally, including host, workspace, project and root incarnation, and reject an older selection after that context changes. They preserve exact-case identifiers and send only the existing payload fields above; these client checks add no server authorization or atomic ownership fields.

The terminal action only attaches/reads/writes a matching existing live terminal. It cannot start, resize, select or destroy the desktop terminal. Structured and terminal permission decisions remain owned by their existing runtime routes.

Queue mutations are authorized against the **current** UI-thread `acpSession.queuedPrompts` projection immediately before native dispatch. `remove` and `steer` require a nonblank stable ID and positive native-range revision; index-only companion requests fail 400. A stable ID may move to another displayed index without invalidating the action. A missing, duplicate, prepared or revised row fails 409, including a former queued row that moved into `pendingDirectPrompts` or `unsentDirectPrompts` without changing ID/revision. A held queue rejects `steer` with 409 but still permits removal. Existing `edit` uses `{promptId,expectedRevision,text,retainedAttachmentIds?}` and requires that same current visible identity; `reorder` uses `{expected:[{id,revision}],orderedIds:[id]}` and requires the complete current visible snapshot in its original order. Native text, attachment, order and persistence safeguards still apply. `discardUnsent` retains its separate `{messageIndex,expectedMessagesDigest}` transcript guard. Unknown queue operations fail 400. These restrictions apply equally to direct and uploaded requests; clients refresh after a conflict rather than replaying the mutation.

`deleteSessions`, `retryFailedMessage`, `selectSession`, `applyOrganizationAction`, desktop transcript ownership, unlisted global settings, token/connection-configuration access, agent-definition editing, arbitrary shell commands, clipboard and host installation are not exposed. Unknown actions fail 403, including when uploaded. `createSession` rejects `sourceChatId`, `expectedWorkspaceDirectory` and `expectedExecutionHostId`, whose asynchronous shared-worktree path could restore stale desktop selection on rollback. Ordinary creation stays synchronous and preserves selection, and native-session open captures/restores the current selection and composer during its completion, including index changes caused by sorting and failure rollback. Connected desktop-selection verification remains a parent acceptance gate.

## Chat deletion and message branches

`deleteSession` accepts only an explicit `chatId`, never bulk targets or an implicit desktop selection. It uses the ordinary durable deletion transaction, related-chat expansion, provider-native ownership, running-runtime and workspace safeguards. The commit checks the **current** desktop selection after asynchronous staging. Deleting a different chat preserves that selection and its composer even when removing records shifts indices, including a selection and draft made while staging awaited. Deleting the currently selected desktop chat uses ordinary desktop deletion semantics: choose the nearest surviving chat and clear the deleted chat's composer. Publication and cleanup progress remain visible in shared state; a successful deletion reply is `{}`. Disk cleanup can still remain pending for safe restart recovery under the existing handler contract.

`branchFromMessage` requires a nonnegative integer `messageIndex` within the native handler's integer range and an existing user message. `content` optionally replaces that message's text; omission copies it. Omit `retainedAttachmentIds` to preserve all original attachments, or supply up to 128 unique source-message IDs to retain a subset; the owning domain validates membership and rejects duplicates. The source history, native transcript identity, media and provider session remain untouched; the child copies history through the selected message and gets its own UAM identity without reusing the source's provider session ID. The ordinary branch service retains host/workspace configuration, goal snapshots, worktree isolation and cold-history identity guards.

Companion policy forces `selectChat:false` before dispatch, including uploaded requests and callers that submit true. That value propagates through history loading, child creation, operation-ID reuse and regeneration failure/rollback; none selects a desktop chat or clears its draft. The desktop bridge keeps its existing `selectChat:true` default. `regenerate` defaults true and retries the copied final user message through the existing runtime; false only creates and returns the child. Optional `operationId` is a nonblank string at most 128 bytes and uses the existing branch identity/collision safeguards. A regeneration failure with a retained idempotent branch can return `{chatId,warning}`; clients must surface the warning and refresh state rather than automatically creating or retrying another branch. These actions share the same imported-chat read-only authorization as other mutations. Provider-specific/live-runtime, simultaneous desktop navigation and native-device acceptance remain parent gates.

## Byte attachment staging

Call `stageChatAttachments` with:

```json
{"chatId":"chat-id","items":[{"name":"photo.png","kind":"image","mimeType":"image/png","dataBase64":"..."}]}
```

Supply 1–16 items. Allowed item keys are exactly `name`, `kind`, `mimeType` and `dataBase64`; kind is `file` or `image`, name is nonblank and at most 256 bytes, optional MIME type is at most 256 bytes, and base64 is canonical RFC 4648 with padding, no whitespace and zero unused bits. A decoded item is capped at 25 MiB, but the 32 MiB complete request-envelope cap includes base64 expansion and JSON, so the effective payload maximum is smaller. `chatId` must also be a portable identifier of at most 64 ASCII letters/digits/underscores/hyphens. Paths, directories, caller IDs and preview-origin paths are rejected before dispatch.

The existing staging handler writes bytes under the chat workspace's `.UAM/attachments/<chatId>/` data directory and stores preview bytes through the existing application-data preview service. SSH chat staging uses the main host's existing runner path. It returns `{"attachments":[{"id","name","kind","type","path","size","copied","previewKey","previewOriginPath"}]}`; the displayed braces here list returned keys, not literal JSON values. Local paths and remote paths are created by that handler, never chosen by the phone. Its existing async completion guard rejects a changed workspace/host/read-only chat.

Pass complete returned objects, unchanged, as `sendAcpPrompt.payload.attachments`. The companion keeps only these server-issued descriptors, bound to the chat ID and its observed workspace/host identity; it rejects invented, modified, cross-chat, changed-workspace or expired descriptors before forwarding the send. Descriptor lifetime is one hour, with at most 256 descriptors, at most 64 KiB of descriptor metadata per staging result and 1 MiB aggregate metadata. Restage deliberately after expiry or server restart. Do not pass `dataBase64` directly to `sendAcpPrompt`: that existing handler consumes staged paths, not upload bytes. A lost staging reply may have created files; do not silently repeat staging or sending. Staging runs on the user-visible file worker; non-image bytes bypass platform image decoding before taking the image-cache lock.

## Chunked request envelopes

Upload **the complete UTF-8 JSON request envelope**, not a raw attachment and not just its payload. The enclosed request goes through the existing bridge parser and the same final companion policy as ordinary requests. All operations below are separate authenticated `POST /api` requests. Upload IDs are opaque server UUIDs and cannot be supplied at begin. They are scoped to this server/token authority, not a claimed per-phone identity.

| Action | Exact payload | Success result |
|---|---|---|
| `beginCompanionRequestTransfer` | `{totalBytes}` | `{id,totalBytes,chunkBytes:32768,nextOffset:0,expiresInSeconds:120}` |
| `appendCompanionRequestChunk` | `{id,offset,base64}` | `{id,nextOffset}` |
| `commitCompanionRequestTransfer` | `{id}` | The enclosed action's normal result and status |

`totalBytes` is a positive JSON integer at most 33,554,432. `offset` is an unsigned JSON integer and must equal the exact decoded byte count already accepted. Each nonempty chunk uses strict canonical base64 and decodes to at most 32,768 bytes. Offset is in bytes, including for UTF-8 envelopes whose characters cross chunk boundaries. The client advances only to the returned `nextOffset`. Unknown keys in these three transport payloads are rejected. No inbound request compression or wire-limit increase is introduced.

At most four incomplete uploads reserve at most 32 MiB in aggregate, using declared sizes rather than bytes received. The absolute lifetime is 120 seconds from begin; successful appends do not extend it. Expired IDs are inaccessible immediately and their retained bytes are pruned on the next operation or the five-second server sweep; shutdown clears the store. Request bodies exist only in bounded transient upload memory until commit/expiry; no request replay ledger or credentials are retained in that store.

After parsing and policy validation, UI-dispatched actions additionally require an in-flight admission lease: at most **eight actions** and **32 MiB of serialized request envelopes in aggregate**, including committed uploads. The lease survives queued dispatch and asynchronous completion and releases on reply or callback destruction. Request-transfer controls and response-chunk reads stay on the transport thread rather than consuming UI admission. A full in-flight budget returns 503 without dispatching the action. Incomplete-upload reservations and admitted actions are separate 32 MiB budgets; these are retained serialized-payload bounds, not a promise that JSON objects and transient parsing copies use exactly that much heap memory.

Commit consumes the ID **before** parsing, policy checks or action dispatch. Incomplete, malformed, disallowed and successful commits are all single-use. Enclosed transfer-control actions fail 403. A repeated commit receives 404; it cannot determine whether a lost prior response represented a successful provider send. On any uncertain commit/send result, show an unknown outcome and refresh/reconcile visible state. **Never automatically begin another upload, resend a prompt or retry commit to recover a result.** User-authorized deliberate retries remain separate actions.

| Status | Meaning |
|---|---|
| 400 | Invalid envelope, fields, offset type or strict base64 |
| 401 | Missing/incorrect bearer or rejected origin |
| 403 | Disallowed action, nested transfer, read-only chat or unowned attachment descriptor |
| 404 | Unknown, expired or consumed transfer ID / missing chat |
| 409 | Noncontiguous offset, incomplete consumed commit, or existing handler revision/runtime conflict |
| 413 | Wire body over 64 KiB, declared total/chunk/attachment too large or chunk overruns total |
| 503 | Upload count/aggregate capacity, UUID generation/collision, attachment descriptor capacity or shutdown |

Existing handler errors keep their existing statuses. Failed append validation leaves the accepted prefix intact; it does not acknowledge uncertain network delivery. Upload commit does not create a durable exactly-once guarantee across process crashes.

## Existing response transport and polling

JSON replies retain gzip negotiation and the 900 KiB wire-body ceiling. Oversized replies return `{"uamTransfer":{"id","totalBytes","chunkBytes":131072}}`; call `getCompanionResponseChunk {id,offset}` with bearer authentication for `{"base64","nextOffset","done"}` until done, then decode the accumulated UTF-8 JSON. These response-transfer schemas remain distinct from request uploads. Raw API replies are capped at **32 MiB**, with larger replies rejected as 413 before gzip or transfer storage; request a smaller transcript/tool page. The thread-safe response store retains at most **16 entries and 32 MiB total**, with a **120-second inactivity expiry**. Its five-second sweep removes abandoned entries even without a later store/read; successful final chunks consume the ID and release its bytes. Oversized replies enter bounded storage before a wire reply is queued, so the server task queue holds the small descriptor rather than an additional complete raw body. Entry/byte capacity or response-ID failure returns 503. Stored-response, incomplete-upload and admitted-request budgets are separate; JSON serialization and temporary compression/chunk buffers remain additional bounded work.

Live chat state continues to use `getInitialState` with known boot ID/state revision and `getChatMessages` with message digests/pages. No WebSocket subscription, provider-message replay or new native runtime is required. Native clients should serialize user mutations and surface stale/disconnected state, retaining authored drafts until the result is known. A request-upload completion is only transport completion; ordinary provider/persistence acknowledgement still determines the action outcome.

## Source and verification entry points

[Server](../../src/cef/companion_server.cpp), [pure policy and staged-descriptor guard](../../src/common/companion/protocol.h), [selective settings policy](../../src/common/companion/settings_protocol.h), [pure request transfer store](../../src/common/companion/request_transfer.h), [focused protocol cases](../../tests/companion_protocol_tests.cpp), [native lifecycle cases](../../tests/companion_lifecycle_tests.cpp) and [opt-in HTTP smoke test](../../tests/companion_protocol_http.py). Build `uam_companion_protocol_tests` in the existing `Builds/agent` configuration; CTest names start `uam_companion_`. The pure target uses the existing bridge parser without CEF/Foundation linkage. Full app build, live HTTPS/browser/native-device checks, simultaneous desktop-selection/usage acceptance, Claude review where applicable and David's approval remain separate parent-task gates.

Provider model-option and managed-child actions use the same central action policy, explicit parent chat identity and final transferred-envelope authorization as other companion requests. Unknown fields, malformed ownership and read-only mutation targets are rejected before native dispatch; successful admission does not replace native lifetime, listing-scope, permission or save-before-delivery checks. These API routes do not establish that the currently installed native iPhone UI exposes the new provider controls.

The native client's source Auto routing control projects only the public `autoRouting` fields and four ownership keys. Tier IDs remain opaque exact strings; `setNativeModelOption` supplies `optionId:"autoRouting"` and a string choice or JSON `null` for Default. `refreshNativeModelOptions` supplies ownership only. Missing capability advertisement never falls back to these actions; the existing desktop allowlist is unchanged. The control displays native effective/pending/activating values without treating the mutation's `pending:true` acknowledgement as an effective preference. The simulator test host and injected application checks are verified; live-provider, rendered interaction and physical-device acceptance remain pending.

On message branching, optional `retainedMarkdownStoreFiles` selects at most128 existing captured skill identities (nonempty, no NUL, at most16384bytes each); omission retains the source selection while `[]` removes all target skills. The native branch service still proves exact source membership, uniqueness and captured prompt availability before publication. This field cannot supply new host-skill attachments or fabricated prompt text, and ordinary companion sends retain their host-path prohibition.


Native managed-child readers use bounded `listManagedSubagents` pages and an exact-run listing before opening `getManagedAgentTranscript`; only that filtered listing advertises follow-up capability. The client validates root, run, transcript/provider association, listing pagination and control scope, and retires reads after navigation, connection/root identity changes or backgrounding. Historical and failed reads keep content without live control authority. Follow-up retains a bounded app-lifetime draft keyed by connection/root identity/run/transcript and a six-field original request across uncertainty; retries replace only the current control scope. Matching root/run/transcript/client ID, next turn number and queued-admission receipt are required to settle input. Raw `{ok:true,runId:...}` Stop receipts retain their identity through client envelope decoding. A validated current read retires only acknowledged Stop state, so a later explicit Stop follows its fresh capability without replaying an old request. Pending and uncertain state is retained. Confirmed empty drafts can release protected capacity without discarding authored input or actionable validation feedback. No desktop-only actions are added to the allowlist.
