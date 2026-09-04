# Issues fixed

## Large tool output stalled the runtime and required manual chunk replacement

- Severity: P1.
- Root cause: every Codex tool-output delta synchronously serialized and rewrote the complete chat, making a multi-megabyte result effectively quadratic. The deferred-output modal also replaced its only 128 KiB page, opened live tools at byte zero, and required manual refresh.
- Data flow reviewed: `Codex output delta -> persisted tool state -> chat save scheduler -> deferred CEF page -> tool modal`.
- Fix: output deltas now use the existing 500 ms coalesced save path. The modal retains loaded pages in order, opens running tools at the current tail, follows live output automatically, and lets the user pause/resume tail following without discarding loaded text.
- Regression: Codex output deltas schedule a deferred save; adjacent output pages remain together; a running deferred tool requests the latest page on open.
- Validation: 644/644 frontend tests and all 6 native CTest targets pass.
- Residual risk: deliberately loading an entire very large completed output retains that requested text in the open modal until it closes.

## SSH reload could split, reorder, remount, or truncate tool turns

- Severity: P1.
- Root cause: remote Codex history created one assistant message per text item and discarded tool items; reconnect restored ordered blocks but not the runtime tool records; replay suppression rebuilt field groups instead of using persisted block order; timestamp-less remote messages received a new `Date.now()` identity on every reconciliation.
- Data flow reviewed: `remote thread/read -> transcript parser -> persisted blocks/tools -> reconnect/replay -> CEF reconciliation -> React rows`.
- Fix: remote history now reconstructs one ordered assistant timeline per turn, reconnect restores complete tool metadata and accumulated output, replay follows persisted blocks, and missing timestamps use a deterministic identity.
- Regression: text/tool/text remote history remains one assistant message; reconnect retains title, arguments, status, and prior output; replay expectations preserve interleaving; repeated timestamp-less hydration keeps the same row identity.
- Validation: focused frontend tests pass 25/25; all 6 native CTest targets pass.
- Residual risk: uncommon Codex history item types other than user text, assistant text, command execution, and file changes remain intentionally ignored until Codex exposes a stable persisted schema for them.

## Active compact work could not be collapsed

- Severity: P2.
- Root cause: active compact turns bypassed the working summary, while the shared summary forced itself open whenever marked active.
- Data flow reviewed: `live turn events -> compact event selection -> working summary state -> transcript layout`.
- Fix: active compact events use the same working summary as completed work, and a manual collapse remains respected while the turn continues.
- Regression: an active compact tool/thought timeline starts expanded and closes on the first click without losing its events.
- Validation: MessageBlocks and ChatView regressions pass; full frontend suite passes 644/644.

## SSH helper-owned chats could stop or lose recovery state after transport failures

- Severity: P1.
- Root cause: several stdout, stderr, acknowledgement, setup, prompt-replay, and asynchronous stdin write failures used fatal ACP cleanup intended for invalid protocol data. That cleanup could finalise an active turn, fail its tools, clear durable ownership, or send the helper an explicit stop even though only the SSH transport had failed. Crash restoration could also leave completed helper processes awaiting an acknowledgement indefinitely, and idle cleanup could be blocked by stale runner health or stop state.
- Files changed: `src/common/runtime/acp/acp_polling.cpp`, `src/common/runtime/acp/acp_session_internal.h`, `src/common/runtime/acp/acp_session_lifecycle.cpp`, `src/common/runtime/acp/acp_session_runtime.cpp`, `src/common/runtime/acp/acp_response_handlers.cpp`, and `src/remote/runner_proxy.cpp`.
- Fix: recoverable remote transport failures now stop only the local proxy, preserve the helper-owned process, active turn, pending tools, queued prompts, delivery identifiers, and durable reconnect markers, then retry attachment without a fixed attempt limit for active turns. Fatal invalid JSON, protocol violations, and oversized output remain fail-closed to prevent replay loops. Persisted source-exit receipts reconnect long enough to acknowledge helper cleanup. Recovery attachment accepts stale runner health and pending idle cleanup state. Startup control and session-setup write races retain undelivered prompts. Closing proxy stdin no longer stops the remote provider; the proxy calls `process.stop` only after the explicit authenticated stop control line.
- Regression tests: `AcpRemoteTransportFailurePreservesHelperOwnedTurnAndSchedulesReconnect`, `AcpPersistedRemoteSourceExitReconnectsToAcknowledgeHelperCleanup`, `AcpIdleRemoteCleanupCanAttachDespiteStaleRunnerHealth`, `AcpStartupControlWriteFailureKeepsUndeliveredPromptForRecovery`, and `CodexInitializeErrorStopsTheStaleTransportAndRetriesAnUndeliveredPrompt`. Existing coverage also verifies unbounded active-turn reconnects, silent remote-turn preservation, idempotent prompt delivery, durable output replay, explicit-only provider stopping, concurrent chats, and bridge reconnection.
- Verification: all 637 UI tests and all 6 native CTest targets pass, including real Unix-socket runner integration. The signed `4.9.0-alpha-9` macOS bundle is valid. Matching protocol-3 helpers were built for macOS, statically linked Linux x86_64 and ARM64, and Windows x86_64; their recorded SHA-256 values match the packaged binaries. Windows was cross-built on macOS but not runtime-tested on a Windows host.
- Safety boundary: the installed application, user data, and running SSH helper were not modified or launched during this work.

## Remote goal actions could stall, mutate early, or lose recovery state

- Severity: P1.
- Root cause: goal cancellation did not consistently include pending or unconfirmed remote stops, provider cancellation could partially pause the goal before cleanup was confirmed, Claude's empty cancel message lost the stop error, failed fresh-session cleanup stranded deferred prompts, and restart normalization retained stale goal identifiers.
- Files changed: `src/app/goal_service.cpp`, `src/app/goal_service.h`, `src/common/runtime/acp/acp_session_lifecycle.cpp`, `src/common/runtime/acp/acp_session_runtime.cpp`, `src/common/runtime/acp/acp_session_runtime.h`, `src/common/runtime/acp/acp_polling.cpp`, `UI-V2/src/ipc/cefBridge.ts`, `UI-V2/src/store/slices/goalsSlice.ts`, `UI-V2/src/components/shared/GoalBanner.tsx`.
- Regression tests: pending and unconfirmed deletion stays fail-closed and restarts cleanup; Claude deletion reports the pending stop; an uncorrelated alpha-2 child is stopped before its current owner goal mutates; failed fresh-session cleanup remains recoverable; stale active goal IDs are persisted away; legacy alpha-2 process-exit blockers show connection recovery; UI retries one action only while a confirmed stop is pending and times out after a fixed bound without flooding diagnostics.
- Verification: 634/634 UI tests and all 6 native CTest targets pass. The isolated alpha-5 bundle builds and passes strict deep signature verification.

## An inactive grid chat sometimes required two close clicks

- Severity: P2.
- Root cause: the close handler used a render-time active-session snapshot, so its first state update could target the wrong pane when closing an inactive split/grid chat.
- Files changed: `UI-V2/src/components/layout/MainPanel.tsx`.
- Regression test: closing an inactive grid chat clears the assigned pane on the first click.
- Verification: the focused regression and complete UI suite pass.

## Completed goals could not be removed

- Root cause: completed goals were blocked independently in the banner, Zustand action, and native service; after those guards were removed, authoritative chat patches still preserved the stale goal when native serialization omitted an empty `goals` array.
- Files changed: `UI-V2/src/components/shared/GoalBanner.tsx`, `UI-V2/src/store/slices/goalsSlice.ts`, `UI-V2/src/store/useAppStore.ts`, `src/app/goal_service.cpp`.
- Regression test: completed goals expose Delete; native completed-goal removal succeeds; an authoritative chat patch without goals clears the previous list.
- Verification: focused UI and native regressions passed, and an isolated package removed the seeded completed goal from both the GUI and isolated chat JSON. The final package builds and passes strict deep signature verification.

## A trailing stream placeholder split one assistant turn into two GUI messages

- Root cause: the renderer treated a trailing streaming placeholder as a second assistant; stale state could discard buffered text; remote recovery discarded the durable assistant anchor; suffix-only reconnect events could overwrite the prefix.
- Files changed: `UI-V2/src/components/views/ChatView.tsx`, `UI-V2/src/store/useAppStore.ts`, `src/common/runtime/acp/acp_message_sync.cpp`, `src/common/runtime/acp/acp_session_lifecycle.cpp`, `src/common/runtime/acp/acp_session_runtime.cpp`.
- Regression test: durable prefix + tool + suffix renders and persists as one assistant, including suffix-only event recovery and rejected stale state payloads.
- Verification: all 605 frontend tests and six focused native lifecycle regressions passed. Native source and the final package compile successfully, and the bundle passes strict deep signature verification.

## Failed turns left completed tool activity spinning

- Root cause: terminal failure paths persisted active tool statuses before settling the turn, so reloads still showed `pending`, `running`, or `in_progress`.
- Files changed: ACP response, Claude, Codex, polling, and shared message-sync paths.
- Regression test: a failed Codex turn converts an active tool to `failed` in both runtime and durable assistant state.
- Verification: the full native source and signed package compiled; focused failed-turn and durable-tool-state regressions passed.

## Typing and closed-chat renderer memory overhead

- Root cause: every caret move, draft write, completed-turn concatenation, slash-option allocation, and retained closed transcript added avoidable renderer work or memory.
- Files changed: `UI-V2/src/components/views/ChatView.tsx`, `UI-V2/src/components/layout/MainPanel.tsx`, and session store types/slice.
- Regression test: IME input, page-hide draft flushing, stable slash options, idle transcript eviction, and deferred eviction after a closed active turn.
- Verification: the `4.9.0-alpha-3` isolated package with 200 rendered messages measured 100 inputs at 9.2 ms p95 idle and 8.9 ms p95 with eight synthetic stream chunks per input; both runs had zero frames over 16.7 ms and zero long tasks. Under stream pressure, renderer JS heap was 23.0 MiB; total app-process RSS was 368.2 MiB and snapshot CPU was 0.0%.
