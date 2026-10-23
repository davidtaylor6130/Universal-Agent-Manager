# Issues fixed

## Observed memory fallback — 2026-10-23

- When a memory save is not sent because its preliminary read fails, the UI now restores the latest accepted native display value instead of an older captured value. Explicit supplied fields are observed separately from the merged optimistic view; unconfirmed and newer drafts retain ownership, and a push never confirms a write.
- Verification: the real store/fake-CEF baseline reproduced three intended failures with three safeguards passing. The correction passes app typecheck,438 focused tests, full2982 with zero skips,23 unchanged performance checks, isolated UI build and docs integrity. Claude found no scoped P0–P2. The baseline result-counting stop is qualified from its unchanged report without a test rerun.
- Limits: unversioned pushes can arrive out of order across channels; partial level-only display pairing is an existing edge, and other parent P3s plus real-Mac/Windows/host acceptance remain. No Complete, installed result or user sign-off is claimed. Exact integration and receipts (private evidence retained locally).

## Managed deadline retirement — 2026-10-23

- Expired ordinary managed runs now request Timeout Stop and check the existing pending-stop predicate before terminal publication and runtime removal. Transcript-save or final-ledger refusal retains the context, deadline and recorded output for retry.
- Verification: the original inert disconnected control failed its first post-Poll running-status assertion; the unchanged control plus five additive DATA cases and three timing controls now pass, followed by three existing cancellation/terminal DATA controls. The source review checked both actual Stop producer contracts; documentation integrity is checked separately.
- Limits: these inert/file tests do not prove OS process termination, remote ownership or full native behavior. The unsafe original remote fixture and R37/lifetime holds remain, managed continuations are outside this increment, and real-Mac/Windows/David acceptance is pending. No Complete or release claim. Exact integration and receipts (private evidence retained locally).

## Memory settings save outcomes — 2026-10-23

- Memory-setting saves now consume scoped native publication outcomes, serialize explicit field intents and retain newer drafts; unconfirmed displayed choices have a persistent field-labelled Settings warning and are never automatically resent.
- Verification: coherent native inventory3440 with producer16/coordinator9/timing3 PASS, connected CEF-handler10 PASS and separate companion30 PASS; focused UI427, full UI2971 with zero skips, performance23, app typecheck, isolated UI build and docs integrity PASS. Claude rounds1–4 closed the scoped P2 findings; original two-failure/six-case evidence and typing prerequisites remain immutable.
- Limits: repository and fake-CEF tests do not establish installed-app or Windows behavior; external-write/failed-read, conservative plain-refusal, ordering/host-scope and notice UX P3s remain. No Complete or David acceptance is claimed. Integration and exact receipts (private evidence retained locally).

## Input-write test imported a private provider handler — 2026-10-22

- Subsequent qualification: actual corrected full52 noncore gates pass52/52, exit0 in41.873s, with all owned kernel groups absent (receipt705d266734cbb81923b73117d8264c0f6edc0868b270e23003a32a510cf1f7d3). Original51P1F/HOLD remains immutable. Same signed142 app/helpers continuation is running at handoff without rebuild/version allocation; freeze, independent verification and install remain pending, with no readiness or acceptance claim.

- Test-boundary cause and correction: the new local input-write observation test included the private Claude handler, causing the sole provider-terminal-boundary failure among52 noncore gates (51PASS1FAIL). The test now uses the real public ProviderRuntimeRegistry adapter and OnAcpHandleMessage; original initialization/write assertions and timing remain, with a stronger true-return assertion.
- Verification: test-only patch26a522bc builds in9.634s, exact Claude initialization passes0.257s,99 connected native cases pass21.364s and the exact boundary gate passes0.239s on corea5fd84b6/inventory3385 with no drift or owned groups. The bounded test/source/result join (private evidence retained locally) preserves original51P1F and the package driver HOLD; full52 continuation is pending.
- Bounds: no production appbundle/UI/helper-contract behavior change; authentic142 compiled/signed and four genuine helpers do not establish an artifact freeze, readiness or installation. Existing public-interface agreement already requires this route, so no feature-doc change is needed. Ten reviewed UI pins remain unchanged; broader acceptance gates and unknown guard transaction HOLD remain, with no Complete or user approval.

## Managed receipts and discard could leave duplicate outgoing aliases — 2026-10-22

- Subsequent final qualification: full2627 frontend tests across152 files/two disjoint shards,23 performance tests, final typecheck and productionUI build pass; one performance mock-arity-only expectation correction preserves100ms/count/payload guards and historical22PASS1FAIL. Fresh Claude R2 source-clear on ten pins resolves R1 and precommit findings within reviewed scope; original failures remain. Native99 unique scoped passes are not fullnative. Package preparation is active only, with no artifact/install/user approval; a separate read-only guard mismatch keeps the future Quit/install transaction on HOLD. See the final qualification (private evidence retained locally).

- Cause and selected corrections: managed saved-parent receipts lacked ordinary strong delivery identity, leaving a duplicate waiting attempt; successful cloned-row discard and hidden discarded aliases could reappear after trim. Exact validated saved-parent observation now binds one owned row without provider-delivery inference, while guarded discard transitions retire only the proven alias. A later callback-to-React-commit replacement race is corrected by associating at the actual guarded object observation before rendering; unique-origin and scope guards remain.
- Verification: corrected62baseline56PASS6genuine DOM failures retains the initial four mount-seeding prerequisite failures; original assertions remain. Expanded68baseline58PASS10FAIL becomes68PASS4.684s plus687connectedPASS18.173s and app tsc0/6.999s after the four-source patch. The later exact precommit case genuinely fails in2.885s, then the oneboolean correction gives69isolatedPASS4.775s. The source/doc/result join (private evidence retained locally) distinguishes those source states;687/tsc precede the final boolean.
- Bounds: original Claude R1 NOT CLEAR remains historical; implementation corrections await fresh running Claude R2 review. Serialized full-unit shards/final typecheck/performance are active, with no final full/performance PASS. Root clarified saved-parent receipt versus provider/child authority; no fullnative, installed141/live or David acceptance claim. Claude Review, Un-Reviewed and Not User Tested remain; no Complete.

## Authored carrier edits retained an obsolete input-write fact — 2026-10-22

- Cause and selected fix: accepted unsent/held-input edits and changed branch replacement copied the previous historical input_write_completed boolean onto changed text, files or skills. Two C++ files now clear only that old fact when carriers change; no-op and refused edits, unchanged rows, recovery policy and input identity remain conserved.
- Verification: six genuine !input_write_completed assertion REDs and two no-op/stale-refusal PASS precede the exact patchfe28f42d, independently source-reviewed within its native scope. The unchanged eight controls pass in0.533s, then80 connected native cases pass in3.495s after build exit0/26.709s on binary5043afbf/inventory3385 with no drift or owned groups. The exact source/doc/result join (private evidence retained locally) retains original failures.
- Bounds: root added the narrow historical-fact invalidation agreement; this does not grant delivery or recovery authority. Fresh Claude outgoing P1 managed-recorded duplication and plausible P2 discard-ghost findings remain unresolved. No fullUI/fullnative, installed141/live-provider or David acceptance claim; Un-Reviewed and Not User Tested remain, with no Complete.

## Native refresh could lose protected input provenance — 2026-10-21

- Cause and selected correction: shifted or regrouped native history could fail to preserve newly added input identity/origin/write-fact metadata, while incoming protected claims required global refusal. R2 rejects new unowned protected facts and retains canonical DATA when protected-prefix regrouping cannot safely preserve identity; legacy regrouping remains covered.
- Verification: the genuine original ambiguous-regrouping refusal RED at line110 now passes unchanged, followed by63/63 connected native cases in1.835s after a22.609s build exit0 on binary77448451/inventory3377, with no source drift or owned survivors. The qualified source/test/doc join (private evidence retained locally) retains the original R1 global-incoming-claim P2, native RED and prior39PASS1backup-fixtureFAIL qualification; bounded independent R2 source review is clear.
- Bounds: scoped native/source and fixture verification, not full-native or primary-save/import/remote-open runtime composition. Root updated shared delivery, six Partial capability qualifications and historical-fact/data-authority agreements; this does not infer runtime support. The UI consumer remains ignored/unrun with its qualified15PASS9FAIL baseline; earlier621 frontend PASS predates that consumer. Installed141, Claude Review, Un-Reviewed, Not User Tested and David acceptance remain unchanged; no Complete or stable-release claim.

## OpenCode Homebrew completion could report false update success — 2026-10-21

- Cause and selected fix: a completed Homebrew probe could report success while the detected version remained1.19.3 below the offered2.1.2. The selected correction requires the achieved numeric version to meet the offered minimum floor; newer-than-offer remains eligible, while npm exact-target mismatch remains failed.
- Verification: the original injected three-case boundary run retains one intended no-op RED and two controls PASS. The corrected canonical build exits zero in827.602s, then all four focused cases—including unchanged, below-floor, newer-than-offer and npm mismatch—pass in0.089s on binary2225d7e1 with four exact frozen pins and no drift. The joined source and native evidence (private evidence retained locally) preserves original failures and owning-doc scope.
- Bounds: process-free injected completed-probe verification only, not real installer success, screenshot cause, installed2.1.2 or latest/channel proof. Comparison uses three numeric components rather than full semver/prerelease rules. Installed alpha141 remains unchanged; bounded source review is recorded separately and Claude Review, Un-Reviewed, Not User Tested, full product gates and David acceptance remain pending. No Complete or full-native-clean claim.

## Optional pull refused verified local-only branches — 2026-10-21

- Cause and authorized policy repair: pull-before-chat required an upstream and collapsed failed tracking lookup into the same refusal. David explicitly chose automatic local continuation when a local branch has no upstream. The selected source requires a successful exact-ref tracking lookup and expected row, skips only the optional pull for a verified empty upstream, and preserves lookup errors and configured tracked-branch clean/base/operation/fast-forward guards.
- Verification: the two new local-no-tracking/no-remotes cases genuinely fail operation.ok/merged before the repair (0/2); the corrected build exits zero in14.248s and all nine connected native cases pass in18.605s on binary74b8ebae, with changedDuring empty. Only the upstream mode was intentionally removed from the old unsafe-source aggregate under this explicit policy change; all other arms remain. The joined policy/source/test evidence (private evidence retained locally) preserves the original RED and source/doc pins.
- Bounds: fixed locally, not installed or live verified; independent native source review records no supported P1/P2 on the exact selected three-path pins, without rerunning tests. No injected tracking-lookup failure control or Windows execution is claimed. The owning Git agreement was updated by root for the changed policy; provider capabilities and architecture are unchanged. This does not establish the actual repository trigger or resolve duplicate outgoing presentation; one queued intent and no saved messages is not proof of no input execution. Existing ownership, Un-Reviewed, Not User Tested and David acceptance gates remain; no Complete, full-native or stable-release claim.

## Blank assistant text rendered an empty history row

- Status: Locally validated by374 affected cases, full2538/2538 unit tests,23/23 performance tests and production UI build, with bounded Claude source review recorded. The original2537PASS/1FAIL remains historical: a preserved blank assistant-text block rendered an extra empty row after launch-group barriers were retained. This is not Complete or David approval.
- Selected correction: MessageBlocks now returns null for blank trimmed text only at rendering; stored blocks, coalescing/grouping barriers and original test oracles are unchanged. The bounded evidence/review join (private evidence retained locally) pins sourceeb44119b/patchcfae041 and fresh narrow Claude source-clear, which is not test validation.
- Bounds: the proper project-harness receipt (private evidence retained locally) records focused/full/performance/UI-build PASS on an unchanged378-file closure. Earlier wrong-root npmexec errors/owned SIGINT remain invalid-harness evidence, not product failure. Native-app/live/assistive-technology and user acceptance remain pending; no stable-release claim.

## Bulk delete confirmation could overlap and lose failure feedback

- Confirmed UI cause and selected fix: FolderTree allowed repeated confirmation/cancel while deletion was unresolved and lacked guarded asynchronous failure cleanup; the shared deleteSessions negative-ACK path did not report through shared notifications. A busy/ref guard captures selected IDs, blocks overlap/cancel until settlement and resets on failure; negative ACK and thrown failures now use shared feedback.
- Verification: portal-corrected original three cases record2FAIL1PASS, expanded five4FAIL1PASS, then the minimal two-file change passes5private cases. Five copied permanent regressions and connected neighbors pass351/351 across4files, app typecheck exits zero, and source closure is unchanged during verification. The exact receipt/review join (private evidence retained locally) retains earlier fixture/failure states.
- Bounds: fake callbacks only; no real deletion, native user-report root cause or all-bulk fix claim. Native ACK/deleted-ID projection, selection revision, message bytes and explicit retry remain unchanged. Prior full2523/23/UI-build results predate this fix; final batch gates remain pending, while the bulk-specific Claude source review is now recorded with no supported P0/P1/P2 for the guarded source only. Existing confirmation/shared-error agreements are restored without capability/design expansion; visual/live checks and David acceptance remain open.

## Unchanged result refusal repeatedly rewrote its ledger

- Cause and selected fix: stale-history/revision refusal saved equal-byte ordinary v1 ledger content and reported a change on every poll. The tiny selected guard change now returns false without publication; real mutations, errors and delivery-attempt semantics are unchanged.
- Verification: the genuine physical-file witness RED at line228 on scheduler5908 now passes on bb7cf6cd with unchanged test0c034, assertions and timings. New physical-readonly, prior three result cases and two connected controls give six unique PASS in 0.25s; the selected build exits zero in15.322s, binary1e070001/inventory3317. The six-receipt join (private evidence retained locally) retains original RED and prior qualified fixture history.
- Bounds: existing no-unowned-mutation/data authority is restored without an expectation or capability change. The old bounded-result control does not resolve the newer unconfirmed-deadline negative, and private cold-shutdown queue conservation remains RED. Full native/app/helper gates, install/live checks and David acceptance remain pending; no stable/release approval.

## Empty OpenCode picker discovery omitted the selected host

- Cause and selected fix: empty-chat model-picker discovery omitted its fourth execution-host argument, allowing native discovery to fall back to local. The one-line call now forwards the selected host or explicit `local`.
- Verification: four private call-contract REDs precede six private focused GREEN cases; canonical verification passes six cases in 3.084s and app typecheck exits zero in 6.545s with no source changes during the run. The integration/evidence join (private evidence retained locally) pins that scope; fake transport is not native remote-backend or provider verification.
- Bounds: the existing chosen-environment model agreement is restored without capability expansion. Earlier full2519/23/UI-build evidence predates this increment; the final combined full gate remains pending. Claude review, visual/live checks and David acceptance are outstanding; no2523/full-suite or release claim.

## Managed results could replace newer saved parent history

- Cause and selected fix: result delivery saved a loaded parent without proving its saved history was current. The result-only change checks saved history/metadata under the captured revision, publishes through prepared-save CAS and leaves delivery attempts unchanged while an authored save is pending.
- Qualified verification: all three focused current-append, stale-protection/explicit-reload and pending-authored-save cases pass on unchanged scheduler5908c797 and final binarya8b204c1. The first post-fix run’s 1 PASS/2 FAIL and raw diagnostic remain recorded: attempts0/messages_equal1 differed only at `/branch_root_chat_id`. Fixture setup now supplies the actual domain creation invariant before first SaveChat; original assertions/timings and strict metadata/CAS are unchanged. This is fixture-corrected GREEN, not identical-original-fixture GREEN; the receipt join (private evidence retained locally) preserves that distinction.
- Bounds: existing canonical-history authority is restored without expanding a feature or provider capability. Deadline/FinishRun changes were excluded; timeout RED and blocked private ColdA remain unresolved. Full native/app/helper gates, installation/live verification and David acceptance remain pending; no stable/release approval.

## Oversized managed-ledger primary could be replaced by a legacy backup

- Cause and fix: `ReadPath` left output text empty when a primary was unreadable or exceeded its limit, and mutable recovery mistook that uninspected state for permission to adopt a backup. Recovery now starts ineligible and opts in only for a safely absent or empty primary, or fully inspected bounded bytes; recognizable managed records remain protected.
- Verification: the genuine oversized-primary RED now passes with the ordinary legacy-recovery positive and seven connected cases, nine unique GREEN in total. The selected build exited zero in 13.249s; original assertions and timings are unchanged and the earlier RED remains recorded in the four-receipt verification join (private evidence retained locally).
- Bounds: this restores the existing validated-backup/data-authority agreement without a feature or capability change. Scheduler source is unchanged; timeout/stale-parent REDs and the blocked private ColdA candidate remain unresolved. Full native/app/helper gates, installation/live verification and David acceptance are pending; no stable/release approval.

## Recovery receipt bytes and attachment-only input

- Cause and selected fix: the shared queued-prompt sanitizer trimmed authored edge whitespace and rejected trimmed-empty text before sanitizing attached files. Canonical source now retains exact text bytes and valid attachment-only pending, failed-direct and future queued projections while preserving malformed-input/context rejection, identity indices and native recovery authority.
- Verification: the sealed preimage records 11 FAIL/6 PASS and the corrected sanitizer 17/17 GREEN; historical connected private candidate650 passes 650/650. These are scoped proofs, separate from new toast33/33 checks and the root combined frontend run still in progress.
- Bounds: the canonical integration/evidence join (private evidence retained locally) pins selected source and owning docs. Final Claude review was not performed because the new Sonnet5.5 round hit API rate limit; combined/full gates, packaged/live checks and David acceptance remain pending. No complete/full-fix, app/install or release claim.

## Enabled MCP startup was classified as a missing connection

- Cause and fix: shared native status treated an active session without a capability as missing before binding. Only a matching running `starting`, not-ready, empty-capability, error-free session now counts as setup; all active sessions must be setting up for that fallback. Ready/error, stale binding, opt-out and mixed missing cases remain actionable, without inventing configuration, handshake or heartbeat proof.
- Verification: old header fails both desired regressions (0/2); the narrow postimage passes all 11 native status cases (9 existing plus 2) in 0.07s. Both builds exited zero and the actual heavy lane was handed back. The source/review qualification (private evidence retained locally) pins receipts and records independent native source review with no supported P1/P2.
- Bounds: the owning UAM tool agreement is selectively integrated; shared UAM delivery and all six provider capability labels remain unchanged, with no architecture relationship change. UI650’s 650/650 private tests are not canonical UI integration; final Claude review, visual/live acceptance, full gates and David’s approval remain pending. No app build/install or stable-release claim.

## Host-scoped memory defaults ignored the execution host

- Cause and fix: inherited memory read only global provider defaults. The shared resolver now uses the existing environment-scoped provider-record selector, preserving explicit chat choices and selected-record Default → app-default behavior. Chat Defaults documents that rule; provider delivery labels and architecture relationships are unchanged.
- Verification: genuine old-resolver regression was 6 FAIL/1 PASS; final targeted run passes all 11 original plus 7 new cases (18/18). The first post-fix 17/18 fixture failure remains recorded: roundtrip setup now sends actual UI `memoryLevel` plus `memoryEnabled`, with expected assertions unchanged; the unbuilt intermediate is retained.
- Bounds: independent final native source review is clear as recorded in the qualification (private evidence retained locally). The first package-ON build stopped at the stale-helper guard; native iteration used the same build folder with packaging OFF and all six providers/SSH/mobile/computer-use/tests ON. Genuine helper refresh, package-ON/full gates, a new installed app, live memory verification and David’s approval remain pending; installed app/userdata were unchanged.

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


## Usage limits — snapshot presentation and cost coverage

- Status: Implemented and focused-validated; required Claude review, full native gate and David acceptance remain pending.
- Found from: Usage and Limits task group and independent data-flow review.
- Symptom: Missing periods looked like five-hour/weekly allowances, future resets announced that they had passed, quiet pages retained stale freshness, and mixed cost totals concealed unpriced tokens.
- Root cause: Presentation inferred periods, included unconditional accessible reset text, lacked a mounted clock, exposed unknown values as numeric meters, and gated missing-price coverage on reported charges.
- Data flow reviewed: `host-scoped session snapshots → allowance cards/composer indicator`; `historical accounting → cost totals/status/chart`.
- Fix: Neutral missing periods, decorative unknown tracks, one shared display clock, accurate reset announcements including individual spend expiry, matching-provider composer allowances and visible partial-cost coverage.
- Regression: New UsageLimits and UsagePage cases plus composer allowance checks reproduce the original failures and pass after correction.
- Validation: Full frontend 1835 unit +23 timing cases pass; TypeScript/Vite production build and Release native build pass; browser fixture screenshots inspected.
- Residual risk: An isolated packaged Usage/Limits smoke check passes; authenticated-provider and full visual acceptance remain pending, and Claude review is blocked by its recorded session limit.

## Usage limits — Claude declared allowance events

- Status: Implemented; 118/118 related native cases pass; full native gate remains failing outside the changed usage paths.
- Found from: Provider contract and session-identity review.
- Symptom: Verified Claude numeric rate-limit events did not reach allowance views.
- Root cause: Only continuation status consumed those events.
- Data flow reviewed: `Claude rate_limit_event → native session usage → existing serialized allowance → Limits and composer`.
- Fix: Project confirmed fractional utilization only for owning-session generic five-hour/weekly windows, preserve unknown values and quota continuation, conservatively floor percentages, retain per-window freshness, and clear old allowances on native-session change.
- Regression: Seven allowance cases cover units, malformed and absent values, identity, rejected continuation, reset, rounding and partial-update age; existing resumed-usage assertion now requires cleared old-session allowance state.
- Validation: Shipped Anthropic CLI unit semantics verified; linked RED regressions followed by passing native cases and Release build.
- Residual risk: No live authenticated Claude event test; Codex read-only allowance acquisition succeeds, a bounded all-provider pass also acquires Copilot SDK quotas but its ACP method is unavailable; Gemini sign-in and Antigravity trust gates remain, with further adapters deferred per David.

## Usage accounting — retained peer projections

- Status: Implemented and focused-validated; installed CPU improvement and David acceptance remain unverified.
- Found from: Cached accounting audit and read-only installed-process sample.
- Symptom: Cached individual hosts advertised complete collection and metadata-only refreshes repeatedly imported unchanged receipts.
- Root cause: Completeness ignored cache state and temporary accounting stores were rebuilt for every projection using whole-header identities.
- Data flow reviewed: `immutable local/peer receipts → ordered scoped accounting cache → lifetime ledger → host and combined views`.
- Fix: Preserve cached totals with incomplete coverage; retain disk-backed stores and same-interval immutable ledgers using facts-only identity, invalidating changed facts, sources and narrowed receipt scopes.
- Regression: Legacy and streamed import counters remain unchanged during metadata refresh; token/cost changes and regressed/rolling bounds force revalidation.
- Validation: 118/118 related native cases pass, including existing paging and accounting neighbors; independent cache source review found no remaining concrete findings; Release build passes.
- Residual risk: Periodic lifetime scans and collector export rebuilding remain; process sampling is not a before/after benchmark. The broad native run reports 42 failures, one corrected neighboring Claude assertion subsequently passes and 41 separate runtime/queue/provider assertions remain unresolved.

## Usage verification — stale frontend fixture contracts

- Status: Test-only corrections validated; product settings and queue behavior unchanged.
- Found from: Full frontend verification; both failures reproduced against the untouched starting revision archive.
- Root cause: An unconfigured SSH default fixture expected inherited local defaults and a legacy queue callback expected omission of an explicit optional undefined argument.
- Fix: Assert existing host-scoped empty defaults and exact callback arguments while retaining configured-host, duplicate-removal and failed-save protections.
- Validation: 26 neighboring cases and the full 1858-case frontend run pass.


## Usage limits — account reads without chats

- Status: Four independent account feeds are implemented and live-verified; full native acceptance remains incomplete because the broad inventory fails outside these account paths.
- Found from: David rejected the session-only Limits page because allowances must be available without running a chat.
- Root cause: The page selected allowance snapshots from active chat bindings rather than acquiring account allowances independently.
- Data flow reviewed: Existing provider login → bounded account-only reader → scoped native cache and persisted presentation fields → immediate CEF query → account cards, independently of history collection and session state.
- Change: Startup account workers, immediate persisted snapshots, independent refresh, honest unknown values, per-model windows, auth-failure invalidation, command validation, local/remote isolation and bounded owned processes.
- Native acceptance evidence: An isolated rebuilt desktop data root contains zero chat files and displays real Codex, Claude, Copilot and Antigravity values in `<private working evidence>`.
- Credential wait correction: A real macOS Keychain read took 20.9 seconds before HTTPS; a cancellable no-prompt system helper now has a 120-second account-access budget separate from the 10-second network budget, without changing permissions or initiating login.
- Review: Actual Claude Sonnet/high broad and follow-up source/visual reviews pass with all eight findings resolved; the remaining original event, merge, paging and accounting contracts also receive a no-material-findings source verdict.
- Verification: All 1,861 frontend unit cases, 23 serialized performance cases, 42 account-native cases and 20 auxiliary CTests pass; full Release build and strict deep signing pass; broad native inventory is 2,389/2,432 with 43 failures, including two Git fixture cases that pass unchanged isolated rechecks.
- Remaining limits: Gemini returns an unsupported-client entitlement response, which David accepts as unavailable to him; the current OpenCode local/self-hosted setup has no hosted subscription identity to query (OpenCode Go itself has limits), and native Windows acceptance is unverified.

## Phone connection and guided pairing recovery

- **PHONE-01 (P1)**: Saved enablement did not start or stop the serving runtime; one owned lifecycle now applies and observes it.
- **PHONE-02 (P1)**: Incomplete mutation replies cleared proxy errors and enabled an unusable QR; responses now retain observed state and pairing requires readiness.
- **PHONE-03 (P1)**: A stale LAN address and absent secure listener caused timeout; address repair replaces both server origin and proxy before publication.
- **PAIR-01 (P2)**: Pairing feedback was buried beneath the form; entry now advances to explicit checking and persistent result screens.
- **PAIR-02 (P2)**: Pairing reused uncertain chat-mutation wording; setup now maps sanitized connection failures to relevant recovery.
- **PAIR-03 (P1)**: A validated snapshot was discarded before a swallowed second refresh; pairing now activates the verified client and snapshot once.
- **PAIR-04 (P1)**: Concurrent submission and late cancellation could persist an obsolete pairing; captured attempts and pre-persistence cancellation/generation guards prevent this.
- **PAIR-05 (P1)**: The private-CA desktop QR supplied no native trust path; optional public-root pairing now validates only the exact saved HTTPS origin.
- **PHONE-04 (P2)**: Restart-required status had no inline action; desktop recovery can request normal application shutdown and supervised relaunch.

Observed RED handler regressions, focused runtime/native/React checks and strict HTTPS fixtures are recorded in `<private working evidence>`. GLM review follow-ups, final packaged installation, the full native gate and David’s device acceptance remain distinct; this entry does not claim user sign-off.

Phone recovery verification follow-up: actual CEF/Caddy toggle and origin replacement pass; final lifecycle/configuration 10/10, connected handlers 5/5, frontend 1886/1886 plus performance 23/23, iOS 78/78 plus two pairing UI journeys, all-target build and 49 auxiliary CTests pass. The full core gate remains failing at 2410/2451, with no additional exact failures relative to the earlier 42-case baseline. GLM backend/native and Claude critical GUI findings prompted configuration authority, trust migration, scanner fallback, poll-readiness, copy-cleanup and dismissal fixes. The failed native touch check was blocked by iOS's separate Save Password dialog, which the revised test explicitly declines without weakening application assertions. Final contrast and signed IPA export are verified; merge/install/production LAN and David acceptance remain separately tracked in the V5 log; nothing here claims a fully green repository or physical acceptance.

## Native Active and navigation follow-up

- **MOBILE-ACTIVE-01 — Fixed locally and simulator verified; physical acceptance pending:** native Active used unread/runtime flags without root settlement, recency or family snooze; full-catalogue family projection now preserves unread identity while matching the desktop policy and keeps a matching child’s available root during search. The actual sanitized1,277-chat catalogue yields exactly the two expected families and excludes pinned Cuda, with no provider/chat mutations. Root-less catalogue rows retain the established desktop authority policy.
- **MOBILE-PERF-01 — Fixed locally, signed and installed; physical acceptance pending:** Home repeatedly sorted chats by a computed date that created ISO8601 formatters inside every comparator, reproducing31.2seconds of CPU time with no network operation. Dates and family sort keys are cached and one projection is reused; warm actual-catalogue projection is30–40ms, with initial construction418ms. These are host measurements, while a real1,277-chat simulator journey also passes repeated open/back interaction budgets.
- **MOBILE-READ-01 — Fixed locally and simulator verified; physical acceptance pending:** catalogue read failures polluted mutation-error state and survived successful recovery; scoped connection feedback now clears independently, optional capabilities cannot disconnect a valid snapshot, and credential/trust/address recovery offers touch-sized Retry and inline Pair again while retaining local drafts.
- **MOBILE-SHEET-01 — Fixed locally, signed and installed; physical acceptance pending:** competing root boolean sheet presenters could leave subsequent Settings taps blocked after nested pairing results. Corrected system-URL baseline reproduces two failed subsequent-touch journeys; one enum sheet owner with actual-dismissal latch passes all three corrected journeys. Earlier XCUIApplication.open runs reset the app and are explicitly excluded as navigation evidence.

Final100/100iOSunit cases and nine focused UI journeys pass, the signed Release is installed, and bounded Claude r4/r5 findings are resolved; receipts are recorded in `<private working evidence>`. Existing broad native failures and David-specific acceptance remain separate; no Complete or fully green repository claim is made.

The final startup repair also covers missing Keychain credentials and invalid saved endpoints/certificates before a client exists; dismissing an unrelated error cannot erase persistent repair. An unloaded catalogue now says Connecting or Connection unavailable rather than All quiet. A reused fake-host Stop assertion failed after a prior test cancelled its child; restarting only that task fixture restored the original state and the unchanged Stop/draft journey passes. No production chat was cancelled by these tests.


## Provider parity — native protocol and recovery

- Status: Implemented and source-reviewed; user/platform acceptance is pending and the full native gate retains 41 failures.
- Found from: Provider audit, current native CLI/SDK sources, actual no-model native probes and independent implementation/Claude reviews.
- Confirmed defects: Narrow Copilot ACP transport, incomplete executing-host image delivery, maintenance/reply persistence gaps, missing exact SSH history projection, OpenCode native child permission/activity observation, private authority in exports, unsupported model-control fallbacks and incomplete configurator provider selection.
- Change: Native Copilot SDK protocol3 and framed local/SSH transport; exact permissions, Plan confirmation, steering, maintenance and recovery; bounded native attachments and immutable history; authenticated same-process OpenCode child approvals and terminal status; native Claude/Antigravity options; exact Codex remote rich history and native child links; private authority redaction and six-provider configurator choices.
- Review fixes: Keep Plan until delivered exact native confirmation saves; honour Claude Inherit and unspecified Default; avoid unrelated-chat defaults and unsupported Antigravity effort lists; retry transient native observations; preserve transport errors; retain shutdown state and plain CLI on occupied observer ports; protect settled Claude maintenance across save/ACK/restart replay.
- Verification: All199related native cases,1866frontend unit and23serialized performance cases pass; configurator12/12Python, documentation138pages/593links/six24-capability rows,20/20auxiliary CTests, final Release app/helper/native build and deepmacOSsignature pass.
- Full inventory: 2527/2568pass,41fail; every remaining failure name is also in the independently executed starting V5 binary (2394/2440pass,46fail), without claiming an archived-source baseline or identical causes.
- Review: Actual Claude Sonnet/high initial sixP2 findings are resolved; follow-up, source adjudication and final native follow-up find no concrete introduced P1/P2 or remaining replay/durability defect in supplied scopes. This is source-only review, not David acceptance.
- Remaining decisions: Current Antigravity/Gemini protocol limits, validated conditional effort/speed, Claude expert-file Fast, OpenCode read-only delegation, multi-child selection UI, existing owned V5 failures and authenticated SSH/Windows/user acceptance stay explicit in docs/documentation-review.md and the V5 log.


## Provider parity follow-up — child state and pane identity

- Status: Source implemented; connected frontend verification passes; Claude follow-up and native integration remain pending.
- Found from: David’s T3 alpha 0.1.0 reference and actual Claude review of the grouped child view.
- Symptom: A completed spawn could stop a running child preview from refreshing, failed provider events lost their visible status/activity, and selecting a native child could replace its parent in another pane.
- Root cause: Parent tool state was reused for child activity and native parent identity was interpreted as a UAM branch-family identity.
- Data flow reviewed: Native receiver state → normalized child projection → grouped rows/preview polling → child open receipt → MainPanel layout.
- Change: Retain validated children and actual states, show task failure/activity, use child state for refresh, and keep successful subagent navigation distinct from branch selection.
- Regression: Actual-state polling cases, failed id-less event presentation, latest unknown-state handling, and a two-pane parent-preservation case reproduced RED before the layout fix.
- Validation: New 52 frontend child-state cases and the exact pane case pass; a fresh connected suite and required broader gates are pending.
- Residual risk: Full T3 lineage, individual timing, managed follow-up, native cancellation availability, authenticated providers and SSH/Windows acceptance remain separate incomplete work.


## Provider parity — partial native import ownership

- Status: Direct-import corrections pass80/80connected history cases; downstream terminal-consumer findings remain in progress, so the full change is not complete.
- Found from: Independent Gemini JSONL R4/R5 data-flow review and actual native regressions.
- Symptom: A related-workspace lookup could relabel native B as A, and partial import could discard unrelated catalogue rows or infer draft links.
- Root cause: The caller overwrote the loader-attested owner and used full-catalogue reconciliation for a partial source.
- Data flow reviewed: Attested native discovery/file → loader → scoped partial overlay → native key/folder preparation → primary save → terminal consumer/sidebar reload.
- Change: Preserve attested workspace ownership, use read-only tombstone/local overlays without inferred-link writes, and retain unsaved live work at direct import publication.
- Regression: Corrected targeted/bulk RED reproduced unrelated native A removal; five retained/new imports and75remaining connected cases now pass (37journal/80total).
- Residual risk: R5 final-terminal sync and reload preservation fail their intended regressions and are being corrected; binary parts, full native46failures, live/platform and user acceptance remain pending.

## Provider parity — stable terminal sync draft

- Status: Source corrected; retained control assertions now pass far enough to expose the two separate terminal history failures; connected/final validation pending.
- Found from: Actual R5 terminal consumer regression execution.
- Symptom: Reloading/reordering the sidebar cleared the selected parent draft despite returning to the same stable chat ID.
- Root cause: SelectChatById compared the new row at the stale index and cleared composer_text before the final stable-ID comparison.
- Data flow reviewed: Targeted import → sidebar reorder → selection restoration → native composer projection.
- Change: FinalizeChatSyncSelection retains the captured draft when the final selected stable ID matches its prior ID and clears it when navigation changes the chat.
- Regression: Both new consumer cases first failed their unchanged draft assertion; after correction both reach the later intended final-answer/unsaved-tail failures.
- Residual risk: Final source review, full inventory and platform/user acceptance are outstanding.

## Provider parity — Companion viewport retention

- Status: Fixed locally; full unit verified, performance/build/Claude/platform acceptance pending.
- Found from: Full frontend verification after nested navigation.
- Symptom: A desktop200-message window restored in companion mode instead of its50-message initial window.
- Root cause: Parent-return geometry cache lacked client context and admitted standalone mounts without an explicit pane key.
- Data flow reviewed: ChatView unmount cache → same chat/pane identity → companion initial window.
- Fix: Retain viewport only for explicit pane keys scoped to desktop or companion.
- Regression: Existing ordered ChatView cases reproduce RED, plus actual MainPanel desktop-to-companion regression; unchanged258 ChatView cases and21navigation pass.
- Validation: Root full frontend1970/1970 passed; pending gates remain in V5 log.


## Provider parity — Nested catalogue recovery

- Status: Source implemented; focused and full frontend gates pass; current packaged grandchild, Claude review and David acceptance remain pending.
- Found from: Independent navigation r2/r3 reviews and unchanged sidebar performance regression.
- Symptom: Same-parent clicks accepted late child opens, catalogue return activated the wrong same-parent pane, branch/native-descendant children disappeared from discovery, passive Refresh reread a stale snapshot, and unrelated sidebar rows rerendered on selection.
- Root cause: Sidebar selection bypassed store intent, pane activation ignored the active match, catalogue discovery stopped at the displayed root, Refresh bypassed attested native import, and row subscriptions used the global active ID.
- Data flow reviewed: Exact parent/child lineage → catalogue disclosure/search → store selection intent/import → pane route → parent context restoration.
- Fix: Publish every explicit selection intent, prefer the active matching leaf, recursively discover validated actual-parent paths lazily, re-import exact passive references before rereading, and subscribe rows to stable local selection booleans.
- Regression: Four initial navigation findings and one descendant finding reproduce before correction; unchanged 1,000-row timing and actual-row Profiler reproduce excess work before correction.
- Validation: Current r4 focused839/839, full1985/1985 unit,23/23 serialized performance and TypeScript/Vite build pass; all258 original ChatView cases remain unchanged. Prior r3 packaged synthetic CEF verifies managed/native child-only views, no separate Active children, fixed and catalogue parent returns retaining a draft, and rich30-message paging; saved screenshots were reopened and inspected.
- Residual risk: r4 descendant source is newer than that packaged binary; live provider controls, two-pane viewport/relaunch recovery, platform checks, mandatory Claude review and David approval remain open.


## Provider parity — original receipt durability

- Status: Source corrected and exact regression verified; full connected/native gates and platform acceptance pending.
- Found from: Independent asset foundation review NA-R1-01.
- Symptom: Matching receipt bytes could be accepted after a failed receipt file sync.
- Root cause: Readability plus directory sync was treated as proof of file durability.
- Data flow reviewed: Future admitted original batch → receipt writer/native sync → primary publisher → restart owned-original reader.
- Fix: Keep per-batch successful-file-sync witnesses for attempted and published markers; reject unknown matching markers conservatively, retaining originals.
- Regression: Two constructed complete-readable-marker states fail0/2 before correction and pass2/2 after; these do not inject an OS fsync failure or prove crash recovery.
- Validation: Original18 foundation cases passed before correction; corrected receipts2/2pass, independent R2 source closes finding without new P0–P2.
- Residual risk: No production parser/publication callers yet; full media integration, final connected gates, Windows/native fault and crash behavior remain unverified.


## Provider parity — loaded history publication

- Status: Guarded source and254connected native cases pass; full gates/live/platform acceptance pending.
- Found from: Independent Gemini JSONL R7 loaded-consumer review.
- Symptom: Partial loaded native snapshots and blanket catalogue reload lost selected or unrelated unsaved live tails.
- Root cause: The loaded consumer bypassed partial-import ownership/preservation and receipt-scoped adoption.
- Data flow reviewed: Completed native loader snapshot → exact loaded import admission → local overlay/save receipt → selective sidebar adoption → digest acknowledgement.
- Fix: Reuse exact scoped ownership, tombstones and unchanged authored-work guard without a final-terminal bypass; adopt only published IDs and retain retry eligibility on deferral/save failure.
- Regression: Three product regressions fail before correction; all8retained/new R7 cases pass, including active-owner deferral, scope/tombstone/no-inference, save retry and actual512-bytePTYEOF control.
- Validation: Current54journal/17lineage/20foundation within254unique connectedPASS; two older fixture contracts resolved explicitly, including actualSIGSEGV from unchecked front() on lazy unselected row, now hydrated/validated. Independent R8 no new confirmedP0–P2.
- Residual risk: NoCEF output-delivery claim from browser-nullPTYfixture; real multi-slice exitedPTY trigger remains unproven; media4parserRED, full/core platform and David acceptance remain open.


## Provider parity — deferred subagent card boundary

- Status: Reproduced in packaged r19; corrected in source and verified by actual packaged r20 retry; full native and fresh Claude acceptance remain pending.
- Found from: Actual isolated packaged CEF visual journey after 2010 unit and 23 performance cases passed.
- Symptom: Opening a saved result group replaced the app with interface recovery; raw canonical results also lost card metadata through the deferred details envelope.
- Root cause: Saved message pages omit deferred content while the new presenter assumed a string, and raw JSON decoding assumed a different transport shape.
- Data flow reviewed: Saved tool → deferred StateSerializer page → buildMessageFromCpp → MessageBlocks → SubagentGroup and canonical result helper.
- Fix: Separate exact-run bounded output-only metadata projection for saved/live tools; frontend shared page normalization and defensive presentation correction retain explicit missing-data presentation.
- Regression: Three native serialization cases fail0/3 before production then pass3/3; seven frontend cases fail before correction then pass, and actual packaged retry succeeds.
- Validation: Current265/265 connected native pass10.28s, full2018unit and23performance pass; signed r20 app opens canonical and legacy groups, preserves exact parent draft on Back; failure and successful screenshots saved and reopened.
- Residual risk: Fresh Claude R2 source review, wider platform/user acceptance, four binary-history parser RED cases and broader native full gate remain open.

## Native attachment sources disappeared during persistence

- Status: Source corrected; focused validation passed; full integration and acceptance pending.
- Found from: Provider parity data-flow audit and independent persistence review.
- Symptom: Pathless native references were omitted, pending originals were acknowledged without bytes, and unloaded saves could reuse shape-only ownership checks.
- Root cause: Attachment serializers, page digests and generic save admission assumed every attachment had a filesystem path; new-source decode failure could be normalized into absence.
- Data flow reviewed: `Gemini native Part -> typed source metadata -> repository/index -> saved or detached message -> frontend projection`.
- Fix: Shared source validation retains durable pathless metadata, rejects unprepared or mixed authority, binds derived page indexes to validated owners and fails explicitly for invalid new-source records.
- Regression: Six original repository/digest cases and four ownership, queue and projection cases failed before their respective corrections and now pass.
- Validation: Connected native 86/86 passed; frontend 2049/2049 unit and23/23 performance passed; production UI build passed.
- Residual risk: Original staging/publication, rich mutation integration, owned preview/delivery, portable bundles, full native gates, fresh Claude media review and user acceptance remain pending.


## Provider parity — equivalent refresh media preservation

- Status: Correction passes focused and packaged verification; overall native gate failed, so work is not complete.
- Found from: Native media integration tracing.
- Symptom: Equivalent text/time rows retained local details while discarding newly imported native media associations.
- Root cause: Whole-row reuse discarded incoming nonlegacy attachments.
- Data flow reviewed: Native parse → shared transcript candidate merge → guarded full-primary publication → saved message/page/frontend.
- Change: Preserve local path attachments while replacing the incoming native projection, retain recorded part associations and avoid repeated-refresh duplication.
- Regression: Actual new case failed0/1 before correction, then six connected refresh cases passed; included in112/112 current native selection.
- Validation: All14 handler cases and112 connected native pass; signed app real CEF synthetic refresh saves a byte-exact PNG, and reopen without provider source restores its thumbnail.
- Residual risk: Other native mutation callers, delivery, bundles and live platform/account acceptance remain partial; full native2759/2800 has41 failures.

## Provider parity — stale refresh and preview ownership

- Status: Source corrections reproduced and focused-verified; full native gate failed and user acceptance remains pending.
- Found from: Independent caller review and actual connected/handler execution.
- Symptom: Refresh could restore stale transient Computer Use state or managed revision; preview rejected valid long native IDs or accepted a late changed owner/cold saved association.
- Root cause: Durable metadata excludes runtime-only state, the preview ID bound assumed legacy identities, and summary recency/count was too coarse for cold association ownership.
- Data flow reviewed: Admitted refresh/preview identity → worker preparation/read → final owner check → primary adoption or CEF response.
- Change: Compare the full transient Computer Use tuple and app consent, carry the newest managed revision, admit bounded16KiB recorded IDs and recheck root/incarnation/birth/storage witnesses. Cold snapshots capture the repository storage revision explicitly.
- Regression: Four actual handler failures10/14 became14/14; connected cold association failure111/112 became112/112 with the original assertion retained.
- Validation: Fresh independent R2 closes both caller P2 findings and supports these preview corrections; full UI2053/2053, performance23/23, all-target signed build and isolated packaged refresh/reload pass. Worker helper ownership/retirement tests also pass after explicit test capture release and actual permit-drain synchronization.
- Residual risk: Repository storage witness remains size/mtime, not a cryptographic revision. Cooperative worker/CEF task release and abrupt-shutdown original retention remain accepted limits. Full native2759/2800 has41 failures; remaining imports/SSH/delivery/bundles and David acceptance stay pending.


## Provider parity — completed retry policy retirement

- Status: Fixed locally and validated; required Claude/user/platform acceptance remains pending.
- Found from: Public retry-capacity, failed-save and deferred-turn regressions.
- Symptom: Successfully completed explicit retries retained bounded recovery policy records and could prevent later failed input from being retained.
- Root cause: Native completion was not associated with the exact dispatched capture through the full-save acknowledgement barrier.
- Data flow reviewed: Captured authored retry → actual dispatch → owned native completion → staged full save → durable retirement.
- Fix: Preserve bounded exact owner/turn proof across runtime reset and callback batching, retire only on actual full-primary commit, and retain cancelled/partial/failed/foreign/uncertain records without replay.
- Regression: Three intended public REDs now pass, along with sixteen neighbors and three additional native protocol cases covering twelve arms.
- Validation: Complete native R36 2915/2915 PASS; all20 auxiliary gates and application/UI builds PASS; independent R31 source review found no newP0-P2.
- Residual risk: Actual Claude R37 rate-limited; live provider/host/platform and David acceptance remain separate.

## Provider parity — discovered inline media preparation

- Status: Fixed locally and validated within controller-local Gemini batch scope; broader media work remains Partial.
- Found from: Real parser → public discovered batch → primary reload regressions.
- Symptom: Inline-only and mixed native originals were rejected by generic save admission instead of becoming a valid saved import.
- Root cause: The batch did not route pending originals through admitted owner/revision-bound media preparation.
- Data flow reviewed: Detached parsed payload → bounded worker owner → final import identity/revision → prepared original and primary → actual publication acknowledgement.
- Fix: Reuse PreparedNativeMediaChatSave while retaining consumed payloads, rejected candidates and old transcripts through bounded worker cleanup; acknowledge originals only after actual primary publication.
- Regression: Initial inline/mixed byte-equality cases and guarded tombstone/retry controls pass without relaxing generic Save.
- Validation: Nine public media and129connected cases PASS, complete native R36 2915/2915 PASS, all20 auxiliary and app/UI builds PASS; independent applied-source review no newP0-P2.
- Residual risk: Pre-batch discovery lifetime, generic/synchronous/SSH callers, native-original delivery and live/platform/user acceptance remain incomplete; no live Gemini used.

## Provider parity — same-count native media refresh

- Status: Fixed locally and validated for admitted controller-local Gemini batches.
- Found from: Independent reconciliation trace and four public inline/URI/owned-byte-health REDs.
- Symptom: Same-count media changes were skipped, and fresh unchanged source bytes could not repair a missing or corrupt saved original.
- Root cause: Existing text/tool equality omitted source attachments and storage metadata equality did not prove original byte health.
- Data flow reviewed: Fresh native candidate → existing local-detail preservation → dedicated batch comparison → guarded preparation → exact primary/original reload.
- Fix: Compare exact source/provenance and opaque URI while treating Pending/Owned/key/preview representation alone as equivalent; bounded scoped original reads establish healthy no-op reuse, and unavailable originals reach existing repair staging.
- Regression: Inline addition, URI change, missing-original and corrupt-original cases fail before correction then pass; healthy Pending/Owned and unchanged URI keep identical primary/key.
- Validation: All9unchanged public media cases PASS,129connected PASS, full native R36 2915/2915 PASS,20auxiliary and app/UI builds PASS; independent R35 source review found no newP0-P2.
- Residual risk: Generic/synchronous/unrelated provider comparison and absent-native removal semantics unchanged; inherited originals without fresh bytes, wider platform and user acceptance remain separate.

## Native verification — complete receipt observation

- Status: Test fixture corrected and validated; no production behavior changed.
- Found from: R35 full-run Stop receipt assertion failure followed by exact and same-batch isolation PASS.
- Symptom: A receipt file could exist before its complete expected Stop bytes were written.
- Root cause: The fixture treated file existence as publication readiness and read once.
- Data flow reviewed: Actual runtime Stop write → inert shim captured-line proof → file creation → gated content publication → observer and cleanup assertions.
- Fix: Poll and cache exact complete expected contents within the same original three-second deadline, retaining final exact equality and all ownership/cleanup assertions.
- Regression: Deterministic gated-publication case reaches intended RED after proving exact actual runtime bytes, then passes with original confirmed/failed Stop neighbors3/3 and original32casebatch.
- Validation: Independent R36 source review confirms no assertion or bound weakening; complete native R36 2915/2915 PASS; unchanged auxiliary production inputs retain20/20PASS.
- Residual risk: R35's actual failed bytes were unrecorded, so this does not retrospectively prove its particular cause or imply live SSH/platform acceptance.

## BUG-005 — Unchecked managed ledger overwrites

- Status: Fixed locally and validated; review and user acceptance remain pending.
- Found from: Three existing-public-API overwrite REDs and shared-writer source trace.
- Symptom: A stale managed record or ordinary legacy save could overwrite a newer or recognizable invalid managed primary.
- Root cause: Generic Save validated only the candidate, without inspecting existing managed authority.
- Data flow reviewed: Scheduler/control Save → primary inspection → shared writer coordinator → backup publication; mutable startup recovery shares the same boundary.
- Fix: Permit identical inert managed roundtrip without writes, reject unchecked managed replacement/downgrade, and provide separate single-use prepared publication with exact predecessor, ownership, immutable-transition and detailed uncertainty checks.
- Regression: Three original overwrite cases reach intended RED, then pass unchanged; six additional conditional cases cover winner/staleness, cleanup/owner loss, authored-policy guards and pre/post replacement outcomes.
- Validation: Full native R41 2926/2926 PASS, all20 auxiliary PASS, all-target signed app build PASS; independent applied-source review no newP0-P2.
- Residual risk: Same-child admission/runtime/birth integration remains unavailable; actual Claude R41 rate-limited, with platform/live/user acceptance pending.

## BUG-006 — Nonregular ledger inspection blocks

- Status: Fixed locally and validated; no broader filesystem-security claim.
- Found from: Independent review of the new bounded inspection followed by controlled public FIFO RED.
- Symptom: Save could wait for named-pipe input while holding the shared ledger writer mutex.
- Root cause: A byte-bounded stream read did not first require a regular file.
- Data flow reviewed: Existing-primary type → bounded inspection under writer coordination → managed recognition → rejection without publication.
- Fix: Reject nonregular primaries before opening the inspection stream.
- Regression: A private bounded child and nonblocking writer prove real FIFO reader pairing before release, then the unchanged case passes with the regular-file guard; regular empty/malformed legacy and oversized-preservation controls pass.
- Validation: Five writer cases PASS, full native R41 2926/2926 and20 auxiliary PASS; independent narrow source closure.
- Residual risk: POSIX FIFO execution and macOS builds do not establish Windows execution or arbitrary hostile pathname-race immunity; pending acceptance retained.

## BUG-007 — Discovery retirement observation

- Status: Fixed locally and validated; test-only correction, acceptance pending.
- Found from: Full native R45 failure and the preexisting Discovery Lease Expiry Timing ledger entry.
- Symptom: The lease fixture requires session removal while nonblocking cleanup can legitimately retain ownership.
- Root cause: Aggregate HasManagedProcesses is not a cleanup-completion fence; a diagnostic reproduction returned refused process_retiring. The initial R45 reply was unprinted and remains unattributed.
- Data flow reviewed: `lease expiry → retiring ownership → nonblocking cleanup → owned poll response`.
- Fix: Observe only refused process_retiring for up to three seconds, rejecting successful expired access or any other error immediately. Original lease timing, final not-found, durable survivor and complete-cleanup assertions remain. Production behavior is unchanged.
- Regression: Original exact case failed; diagnostic attempt captured process_retiring before correction. Corrected3/3 focus and original32/32 batch pass.
- Validation: Full R47 native2932/2932 in422.97s; auxiliary20/20 in42.85s; complete build, docs and diff checks pass. Independent source review finds no expiry weakening.
- Residual risk: Platform and David acceptance remain pending; remote/live-provider behavior is not newly certified.


## BUG-008 — Changed private spool deletion

- Status: Fixed locally and validated; platform/review/user acceptance remains pending.
- Found from: Independent R51 source review and actual public R53/R54 regressions.
- Symptom: The new opt-in Builder could delete replacement spool bytes after Finish or continue appending after an unfinished spool was replaced.
- Root cause: Initial creation ownership was treated as permanent pathname cleanup authority.
- Data flow reviewed: `exclusive creation → Add/CopyRecords → Finish/seal → destruction`.
- Fix: Capture authored SHA/count and own-write metadata only for the opt-in spool, refuse changed or failed states, and remove only unchanged witnessed bytes; legacy buffering and cleanup remain unchanged.
- Regression: Finished replacement disappears at the regular-file guard in actual R53; unfinished replacement incorrectly accepts next Add in actual R54; both unchanged cases pass after correction.
- Validation: All17 publication and33 legacy neighbors PASS; full R55 native2949/2949 and auxiliary20/20 PASS; signed all-target build and fresh independent source review pass.
- Residual risk: Cooperative app-writer witness does not provide adversarial native file identity; actual Windows execution and David acceptance remain pending. The storage primitive has no runtime caller.

## BUG-009 — Conditional root spelling rejection

- Status: Fixed locally and validated; Windows/review/user acceptance remains pending.
- Found from: Independent R51 source review and actual legacy-controlled public R53 regression.
- Symptom: New conditional CreateAbsent and UpdateExisting refuse a valid data root ending in a separator.
- Root cause: A lexical direct-parent comparison retained the root's terminal empty component.
- Data flow reviewed: `explicit data root → directory admission → private preparation → guarded publication`.
- Fix: Remove terminal empty spelling components once and consistently reuse the result, preserving filesystem roots, non-link checks, absence and revision/content guards.
- Regression: Actual R53 null preparation follows successful legacy-root control and independent candidate encoding; unchanged create/update exact-byte case passes after correction.
- Validation: All17 publication and33 legacy neighbors PASS; full R55 native2949/2949 and auxiliary20/20 PASS; signed all-target build and fresh independent source review pass.
- Residual risk: Windows path execution remains unverified; no canonicalization, routing change, runtime consumer or user acceptance is implied.

## BUG-010 — Branch originals kept the source owner

- Status: Fixed locally and automated gates verified; live provider, Windows and David acceptance remain pending.
- Found from: Provider parity source audit and actual existing-public domain/CEF REDs.
- Symptom: Branching history with an owned native original fails the foreign-owner save guard.
- Root cause: Copied history retained source-owned asset keys in a different chat.
- Data flow reviewed: `branch selection → worker original preparation → final-owner candidate → guarded SaveHistory → worker original ACK`.
- Fix: Prepare exact-byte originals for the final admitted owner, bind source/request positions, preserve operation reuse and acknowledge only after successful primary save; keep the generic foreign-owner rejection.
- Regression: Six provider branch paths, distinct MIME declarations sharing bytes, loaded/cold/replay CEF, cancellation, stale incarnation, real queue-journal save rejection and real published-receipt failure.
- Validation: R65 native2980/2980, auxiliary20/20, UI2134+23, connectedCEF38/38, signed all-target build and actual Claude source review pass.
- Residual risk: Native-original input delivery is separate; post-refusal/throw and post-replacement save faults have no branch-specific injection coverage; real platform/provider/user acceptance remains open.

## BUG-011 — Unsent editing created a branch

- Status: Implemented and full automated gates verified; visual/focus timing, review and user/platform acceptance remain pending.
- Found from: Actual ordinary-unsent Edit/Enter DOM regression.
- Symptom: Editing a definitely-unsent input took the delivered-message branch route.
- Root cause: The recovery editor used a shared branch callback without a guarded local mutation.
- Data flow reviewed: `ordinary Unsent editor → anchored one-row probe → strict CEF editUnsent → known-unsent/captured-policy validation → candidate SaveChat → committed-row reconciliation`.
- Fix: Save the exact local row with independent native-file and captured-skill selections, fresh certainty/digest/receipt guards, unchanged policy/admission identity and no Send/Retry/branch; restore owning editor/composer focus with generation/chat/mount checks.
- Regression: Actual wrong-operation/stub/missing-bridge REDs, stored context and reload, stale/uncertain/delivered/receipt/queue/child/pending-save/malformed cases, commit-push ordering, double Enter, focus and stale pane/unmount controls.
- Validation: R65 native2980/2980, auxiliary20/20, UI2134+23, connectedCEF38/38 and signed all-target build pass; Claude R64 supports no P0–P2.
- Residual risk: Browser preview and native desktop automation could not provide a real visual journey; optional RAF/React commit-order focus risk is unverified; the separate delivered-message skill gap is resolved locally under BUG-012/R68.

## BUG-012 — Delivered skills mixed with file IDs

- Status: Fixed locally and full automated gates verified; review coverage, real visual, Windows and David acceptance remain pending.
- Found from: Actual public delivered-edit DOM and CEF regressions.
- Symptom: Editing a delivered input sent skill paths as native file IDs and ignored independent skill removal.
- Root cause: The shared editor and branch API carried one resource selection while persisted skills used separate file and captured-text vectors.
- Data flow reviewed: `delivered editor → immutable store request → CEF → domain/lifecycle → prepared original witness → saved branch/retry`.
- Fix: Carry independent optional selections, preserve original captured pairs/order, distinguish omission from explicit removal, bind prefix/request/source/candidate context and compare effective operation reuse before new assets or retry. Allow removing all unavailable target snapshots while retaining strict nonempty validation; restore prior preview blank-source eligibility.
- Regression: Actual native-ID and wrong-retained-context REDs; actual saved/reloaded empty-removal and preview REDs; native subset/removal/omission/blank/invalid/reuse/stale controls, cold CEF and held worker contexts, category collisions and immutable retry payload.
- Validation: R68 native2992/2992, auxiliary20/20, UI2166 plus23performance, connectedCEF46/46, explicit project typechecks, required production frontend/all-target build and strict signature PASS; actual Claude R68 no unresolved P0–P2.
- Residual risk: Optional empty-token negative/cold-wire replay controls passed in R69; preexisting omitted-blank direct-native/preview policy difference and legacy empty-block serialization are tracked separately; native-original input delivery, provider continuation and real platform/user acceptance remain open.

## BUG-013 — Expired reservations blocked launch and compaction

- Status: Fixed locally and full automated gates verified; user and Windows acceptance remain pending.
- Found from: Shared-runtime source audit and three actual public regressions.
- Symptom: An abandoned branch preparation leaves a weak-map entry that rejects a later launch or compaction and can clear pending intent.
- Root cause: Three guards tested map membership while sibling admission paths tested whether the weak owner was live.
- Data flow reviewed: `BeginBranchPreparation owner release → weak reservation → public Send/Poll/Compact → pending launch or maintenance`.
- Fix: Use existing find-and-not-expired semantics in those three checks, retaining live-owner behavior without erasure or ownership changes.
- Regression: Absence/live controls precede genuine expired-owner public cases; unfinished version tasks remain unfinished and no provider is launched.
- Validation: Actual3/3 RED then3/3 GREEN, connected37/37, real CEF47/47, full R69 native2995/2995 in442.04s, auxiliary20/20, signed all-target build and independent applied-source review PASS.
- Residual risk: Windows and David acceptance remain open; no live Gemini, native-original delivery or managed v2 consumer is enabled.

## BUG-014 — Saved original images failed Retry

- Status: Fixed locally and full automated gates verified; live-provider, Windows and David acceptance remain pending.
- Found from: Provider parity source audit and actual public saved-history regressions.
- Symptom: Retry of a saved branch-owned image refused its retained original, and an internal idle-provider Stop could discard the permission while workspace preparation remained pending.
- Root cause: Strict generic path validation had no selected saved-row byte-delivery route, and the internal Stop revoked invocation-only permission before an asynchronous workspace return.
- Data flow reviewed: `persisted original → final branch owner → explicit selected-row Retry → queue/turn/image worker → durable packet → final native write`.
- Fix: Capture and bind the complete selected row, read only validated owner-scoped originals on the bounded cancellable worker, retain exact completed-worker and packet witnesses, and restore the same validated unbound permission across the internal workspace handoff. Direct/raw input, generic validators, explicit Stop and changed-source revocation remain strict.
- Regression: Actual missing-delivery RED and actual Stop-to-Pending permission RED precede corrections; eight public cases prove exact decoded PNG delivery, unchanged original history/assets, direct/mixed/foreign/opaque/corrupt/missing refusal, Stop/root/incarnation/declaration revocation, pending-packet drift and genuine workspace completion without recapture.
- Validation: Focused8/8 in2.14s, connected177/177 in104.96s, R74 full native3003/3003 in447.22s, auxiliary20/20 in41.16s, real CEF47/47, signed all-target build and actual Claude R74 source review with no supported P0–P2. All997 frozen inputs matched; frontend sources remain at the separately verified R68 checkpoint.
- Residual risk: This route is controller-local Claude image Retry only; other providers/hosts/recorded sources retain explicit limits. Windows, live Claude and David acceptance remain open. No live Gemini, installed-data management, commits or pushes were performed.

## BUG-015 — Saved Codex images failed Retry

- Status: Fixed locally and full automated gates verified; live-provider, Windows and David acceptance remain pending.
- Found from: Provider parity audit, public native-frame regression and independent binding review.
- Symptom: Codex refused retained branch-owned images, and clearing the saved native thread during preparation could still deliver to the old thread.
- Root cause: Saved Retry lacked a Codex native byte route, and the preparation witness did not retain the saved chat native ID.
- Data flow reviewed: `published original → branch owner/reload → explicit Retry → bounded worker → native encoder → durable sealed packet → final write`.
- Fix: Encode validated worker images as native image URLs with captured names/order, bind effective native model/policy and saved thread before preparation, and retain a separate exact native request/method/allocated-ID witness through committed delivery. Raw/default validation and ordinary native path input remain strict.
- Regression: Actual supported-provider RED followed by six passing public cases; a separate public Poll/save regression observed old-thread delivery after clearing only the saved ID, then passed after the narrow metadata correction. Controls cover exact PNG/GIF bytes, complete-source refusal, direct input, Stop/cancel, worker/packet drift and no recapture.
- Validation: R76 focused7/7 in8.94s, connected221/221 in108.14s, full native3010/3010 in454.81s, auxiliary20/20 in42.38s, real CEF47/47, signed all-target build and actual Claude R76 source review with no supported P0–P2. All997 frozen inputs matched; frontend sources remain at the separately verified R68 checkpoint.
- Residual risk: New Codex tests exercise a genuinely ready private native thread; cold async setup is source-traced rather than directly exercised by these new cases. Live native/model, Windows and user acceptance remain open. No live Gemini, installed-data management, commits or pushes were performed.

## BUG-016 — Saved Copilot images failed Retry

- Status: Fixed locally and full automated gates verified; live-provider, Windows and David acceptance remain pending.
- Found from: Provider parity audit, pinned native SDK contract and public private-peer regressions.
- Symptom: Copilot refused retained originals, and a later owned Auto model observation could leave an in-progress image qualification valid for the wrong model.
- Root cause: No selected saved-row native blob route existed, and model invalidation covered Ready evidence without covering its query phase.
- Data flow reviewed: `published original → branch/reload → explicit Retry → native evidence → bounded image worker → sealed durable packet → native write`.
- Fix: Require three exact owned, deadline-bound native responses for running version, selected vision capability and current model; bind pure evidence through existing worker/packet/final-write checks; encode actual prepared bytes with captured names/order; invalidate querying and ready evidence on conflicting owned native model observations.
- Regression: Actual unsupported-provider and isolated native query-order REDs precede corrections; seven cases cover warm/cold resume, original PNG/GIF plus Legacy JPEG order, complete-source refusal, real30second expiry, late replies, Stop, finished workers and actual durable-packet drift.
- Validation: R79 focused seven cases cumulative PASS with documented fixture-only rerun,269/269 connected in116.37s,3017/3017 full native in476.57s,20/20 auxiliary in41.24s,47/47 real CEF, signed all-target build and strict signature; actual Claude R79 no supported P0–P2 and997frozen inputs unchanged.
- Residual risk: Native configured vision does not prove backend visual understanding or organization policy; WebP/reencode/workspacePending/SSH/native-send-failure arms are not forced by these seven cases. Live Copilot, Windows and user acceptance remain pending. The setup-only installed-provider fixture incident is retained in BUG-018. No live Gemini, app installation, commits or pushes.

## BUG-017 — Saved Retry lost its row

- Status: Fixed locally and full automated gates verified; platform and David acceptance remain pending.
- Found from: Actual private SDK startup regression after native-agent restart.
- Symptom: A genuine saved-image Retry failed with “could not bind its exact turn” after internal launch reset.
- Root cause: StartAcpUserPromptBatch selected the existing row from reset session state despite retaining the validated saved permission.
- Data flow reviewed: `public selected-row Retry → saved permission → internal Stop/launch reset → authored-row selection → final turn binding`.
- Fix: Select the row from the same validated saved permission when present; retain explicit row overrides and all final pointer/source/row/serial guards.
- Regression: Actual0/1 private-peer startup RED in0.46s precedes the one-initializer correction; warm and cold Copilot arms then pass with unchanged saved source and exact native byte delivery.
- Validation: Seven cumulative focused cases and269connected cases PASS; R79 full native3017/3017,auxiliary20/20,real CEF47/47,signed all-target build and actual Claude R79 source closure. Fresh bounded owner ACKs and exact boundary review are recorded.
- Residual risk: This restores existing selected-row authority and mints none; live native/provider, Windows and David acceptance remain pending.

## BUG-018 — Private SDK fixture lost isolation

- Status: Test correction validated; the historical external setup action remains recorded.
- Found from: Fixture prerequisite failure during the Copilot query-order regression.
- Symptom: The private peer stopped being executable, allowing normal executable resolution to fall back to installed Copilot.
- Root cause: Atomic WriteTextFile replaced the script inode and execute mode; private PATH alone did not prevent fallback.
- Data flow reviewed: `test script rewrite → executable resolution → native setup → private peer trace prerequisite`.
- Fix: Restore and assert owner execute permission after both rewrites and before every fixture launch/Poll, retaining the private peer/PATH through AppState teardown; production fallback behavior is unchanged.
- Regression: Original run53032 is a setup failure, not feature RED; complete log has ping/models.list/failed session.resume and zero session.send. Corrected run11879 reaches the intended isolated query-order RED before production correction.
- Validation: Corrected seven-case focused cumulative PASS,269connected PASS,R79 full native3017/3017,auxiliary20/20,real CEF47/47,signed build and actual Claude R79 source isolation review.
- Residual risk: Installed CLI startup may have accessed its usual configuration/logs; no prompt or image was submitted, and no claim of untouched installed data is made. The user was informed immediately; installed data was not inspected or deleted to undo the run.

## BUG-019 — OpenCode dropped valid responses

- Status: Fixed locally and full automated gates verified; live-provider, Windows and David acceptance remain pending.
- Found from: Actual native permission-bootstrap failure, public delayed-response regression and independent drain review.
- Symptom: The owned OpenCode permission server failed bootstrap before its HTTP response arrived, and final bytes could be lost when the process exited between read and poll.
- Root cause: Query treated the platform -2 would-block sentinel as fatal and broke on process exit without draining to actual EOF.
- Data flow reviewed: `owned companion/terminal/runner request → typed loopback HTTP → process stdout/exit → bounded JSON response → native permission/activity result`.
- Fix: Wait through would-block and drain positive final bytes until confirmed exit plus EOF, preserving deadline, output bounds, cancellation, exact status/exit validation and cleanup.
- Regression: Actual private-loopback delayed Query RED precedes the sentinel fix; separate deterministic final-drain RED precedes the EOF fix, while seven strict failure/cancellation arms remain passing.
- Validation: Reader/permission/terminal/runner38/38, connected313/313, R81 full native3026/3026 in477.92s, auxiliary20/20, explicitly rebuilt real CEF47/47, signed all-target and strict signature; actual Claude R81 closes both findings. All997 frozen inputs match.
- Residual risk: Deterministic read/exit injection supplements actual macOS process/HTTP execution; Windows and live native/provider behavior remain unverified. No deadline, ownership or permission policy was relaxed.

## BUG-020 — Saved OpenCode images failed Retry

- Status: Fixed locally and full automated gates verified; live-provider, Windows and David acceptance remain pending.
- Found from: Provider parity audit, pinned native producer contract and actual public branch/reload/Retry failure.
- Symptom: An explicit local saved-image Retry refused its retained originals despite a usable native ACP image route.
- Root cause: OpenCode lacked selected-model image admission and a saved-original ACP encoder bound to the existing worker and durable packet.
- Data flow reviewed: `original publication → final branch owner/reload → explicit Retry → owned server/model evidence → bounded image worker → sealed durable packet → native ACP write`.
- Fix: Use separate bounded typed image evidence from the same native process/workspace, retain exact invocation/model/settings authority through final delivery, and encode validated prepared bytes with immutable names/order and the existing context prefix. Raw input, strict Legacy remainder and permission decisions remain unchanged.
- Regression: Actual unsupported-provider RED follows genuine native bootstrap; warm and cold resume deliver exact PNG bytes, while four supplemental public groups cover invalid evidence, cancellation/ownership loss, real native configuration drift after worker/packet, ordinary text and recorded-source refusal.
- Validation: Focused5/5 plus supplemental4/4 separately, connected313/313 in133.18s, R81 full native3026/3026 in477.92s, auxiliary20/20, real rebuilt CEF47/47, signed all-target and strict signature; actual Claude R81 reports no supported P0-P2. All997 frozen inputs match.
- Residual risk: Native configured support does not prove backend vision or atomic external configuration. Large catalogs may exceed256KiB; commands, remote originals and unadmitted companions remain explicit gaps. Exact new wire positives force PNG, while other supported formats/reencode and live/platform/user acceptance remain separate. No live Gemini, installation, commits or pushes.


## BUG-021 — Handler tests started account readers

- Status: Isolated locally with focused and connected tests passing; broader current gates and exact historical prompt attribution remain pending.
- Found from: David's repeated Claude Code Keychain prompt report and deterministic constructor trace.
- Symptom: A private provider fixture could start unrelated account, native-history and pricing readers before its test action.
- Root cause: Real UamQueryHandler construction unconditionally initialized production background services, independent of the configured private provider command.
- Data flow reviewed: `handler construction → account/history service startup → default reader → system credential helper`.
- Fix: Preserve the public production constructor and use a private test-only construction seam that omits both services before any worker exists; replace all 56 unrelated fixture constructors without changing provider profiles or prior assertions.
- Regression: HandlerFixtureSkipsUnrelatedBackgroundReaders checks both service pointers and unchanged profiles; independent in-memory reversal recovered exact previous hashes for all five affected test files.
- Validation: R97 build and isolated no-reader regression pass; current managed-handler connected inventory 64/64 passes. No installed Claude review was launched after the pause.
- Residual risk: Screenshot PID/requester and prior successful credential/history/network outcomes remain unknown. No real credential, cache, Keychain or ACL was inspected or changed. Product account behavior is unchanged; broader gates and Windows remain unverified.

## BUG-022 — Mixed managed work exceeded capacity

- Status: Corrected locally with focused and connected tests passing; broader current gates remain pending.
- Found from: Actual mixed legacy/witnessed four-slot admission regression after reader isolation.
- Symptom: A legacy run could be admitted while a witnessed Running transition already reserved the last slot but had not published it.
- Root cause: Legacy admission counted only published Running records, while witnessed admission also counted its pending reservation.
- Data flow reviewed: `pending Running transition → shared slot count → legacy/witnessed admission → actual native launch`.
- Fix: Both routes use the same count of distinct published, pending and retained uncertain Running reservations; ownership and cancellation policy are unchanged.
- Regression: Actual saved_running <= 4 failure preceded the correction; pure legacy and mixed controls retain their real no-invocation, version-task and persisted-state assertions.
- Validation: Direct focused 3/3 and R97 unfiltered managed handler 64/64 pass; independent source review covers uncertain lifetime. The generic wrapper's custom-footer parsing failure is recorded separately from direct binary success.
- Residual risk: Uncertain publication lifetime is source-reviewed rather than newly fault-injected; full current core/other handlers/package, Windows and David acceptance remain pending.

## BUG-023 — Antigravity live usage stayed unknown

- Status: Fixed locally with focused native and current frontend gates passing; broader native/package and acceptance remain pending.
- Found from: Public serialized usage RED and independent malformed-step deduplication review.
- Symptom: Saved messages had native token counts while the live panel had no usage; an invalid step ID could also consume the real step-zero dedup slot.
- Root cause: DONE counters never populated provider_usage, and malformed IDs were coerced to zero before shared deduplication.
- Fix: Project only validated exact observed counters, preserve unknown versus genuine zero and incomplete last-turn status, and keep invalid identities out of both deduplication and counters while continuing noncounter content.
- Regression: Actual absent projection RED and separate existing saved-counter RED precede the corrections; native cumulative results remain excluded and valid duplicate steps count once.
- Validation: R100 focused native7/7 includes all four new usage cases, the old cumulative/dedup control and two compaction cases; UI19/19 usage, current full frontend2216+23 and independent source review pass.
- Residual risk: Totals cover events observed while attached, not native all-history or context-window occupancy; account behavior is unchanged. Full native/package, Claude Review, Windows and David acceptance remain pending.

## BUG-024 — Unsupported compact command stayed available

- Status: Fixed locally with focused native and current frontend gates passing; broader native/package and acceptance remain pending.
- Found from: Unconditional Chat command source audit and actual public DOM/native serializer REDs.
- Symptom: Unsupported or stale runtime choices could still offer /compact and send a request; palette selection could erase the command before availability was rechecked.
- Fix: Publish advisory availability from actual ready owned adapter support, sanitize exact provider/model/native anchors, and recheck the current store before palette clearing or dispatch with scoped ACK feedback.
- Regression: Three actual unsupported DOM REDs and a genuine private native Codex false-stub RED precede correction; matching positive routes, native identity drift, busy/Stop and projection no-side-effect controls remain explicit.
- Validation: Native connected7/7 includes both compaction cases; focused compact DOM34/34, reconcile42/42, TypeScript, full frontend2216+23 and independent source review pass.
- Residual risk: Backend permission remains authoritative and stopped/unknown sessions are unavailable; no unsupported provider command is invented. Full native/package, Claude Review, Windows and David acceptance remain pending.

## BUG-025 — Managed retirement rewrote canonical proof

- Status: Fixed locally with connected validation; full current native/package gates pending.
- Found from: Genuine native-resume refusal and running-cancellation regression failures.
- Symptom: A stopped child kept a pending terminal save and never retired despite unchanged conversation content.
- Root cause: Natural-exit and cancellation retry paths repeatedly finalized an already stopped original session, advancing the canonical revision during detached terminal preparation.
- Data flow reviewed: `provider exit or explicit Stop → owned stopped lease → scheduler cancellation retry → canonical history save → terminal ledger CAS → parent result/reap`.
- Fix: Reuse exact original stopped ownership for read-only retirement and skip redundant Stop only on a proven stopped resumed owner; preserve input prohibition, real Stop/reap evidence, canonical revision checks and uncertainty handling.
- Regression: Actual unchanged refusal and running-cancellation tests failed before correction; cancellation diagnostics confirmed the retry after terminal preparation.
- Validation: Affected native builds pass; exact cancellation1/1 and complete managed74/74 pass, including foreign native events, initial/legacy controls, replay, callback failure and saved history.
- Residual risk: Private protocol peers establish the tested runtime behavior; live provider, Windows, full current core/package and user acceptance remain pending.

## BUG-026 — Native history fixtures used installed paths

- Status: Corrected locally; exact regression3/3, signed all-target, strict signature, auxiliary20/20, rebuilt CEF47/47 and goal3/3 pass; full native repeat is incomplete after actual disk exhaustion.
- Found from: Full native verification under the known-reader sandbox after the user reported credential prompts.
- Symptom: Three import/history fixtures fail when unrelated installed Claude history is unavailable; its appended error also replaces the expected final Copilot error.
- Root cause: Cross-provider discovery with an empty provider filter scans Claude regardless of profile removal, and these fixtures omitted its private CLAUDE_CONFIG_DIR binding. Two also omitted a private Gemini history root.
- Data flow reviewed: fixture environment -> folder discovery -> provider history readers -> ImportResult/errors/ownership assertions.
- Fix: Ten setup lines create empty private history directories and retain existing ScopedEnvVar guards for the reader lifetime; production behavior and every existing action/assertion remain unchanged.
- Regression: R107 actual3029/3032 records all three original failures; after the independently reviewed fixture-only patch, the same three exact cases pass3/3 in0.22s with the unchanged sandbox.
- Validation: Affected core build and signed all-target/package build pass; source patch/result hashes are retained in provider-parity-private-history-fixture-r108.json.
- Residual risk: Full3032 repeat remains incomplete after actual disk exhaustion in both parallel and serial runs; the sandbox is a known-path guard, not universal isolation, and earlier credential prompt attribution/read outcomes remain unknown.

R109 current verification checkpoint for BUG-025/026 and the integrated provider slice: current managed80/80, full frontend2230/2230 plus23/23 performance, signed all-target/strict signature, auxiliary20/20, rebuilt actual CEF47/47 and goal3/3 pass;1016 chosen source/test inputs match their frozen R108 hashes. Current full native clearance is NOT established: R1073029/3032 had the three subsequently corrected private-history fixture failures; parallel R108 and serial R109 repeats were stopped after actual ENOSPC. Existing user/Claude/Windows acceptance flags remain; no installed main-app or live Gemini validation.

## BUG-027 — Initial setup errors retained running owners

- Status: Scoped correction passes current full core3032/3032 and managed88/88; signed package and standard auxiliary gates pass, with live/platform/user acceptance pending.
- Found from: Actual public Codex contradictory-model arm, which timed out after a genuine private setup reply.
- Symptom: A first managed child retained Running status and capacity with a live errored process and zero input serial.
- Root cause: The scheduler retired definite asynchronous setup/admission errors only for resumed owners; its separate exit path required the process to have stopped.
- Data flow reviewed: Raw setup model → queued model refusal → owned lifecycle Error → scheduler stop/save/terminal publication/result delivery/reap.
- Correction: Apply the existing exact-session unsettled-error retirement path to first-attempt owners too, retaining model refusal and all normal publication and process guards.
- Regression: Original four-arm public case failed at its unchanged30-second settlement wait; temporary count/boolean diagnostics confirmed the live error and were removed byte-exactly before verification.
- Validation: Serial handler build and unchanged negative four-arm plus Claude binding/native-resume refusal controls pass3/3; subsequent full86/86 managed R121 and current canonical core3032/3032 R133 pass; current signed package and standard auxiliary20/20 also pass; live/platform/user acceptance remains pending.
- Residual risk: Private peers establish protocol/lifecycle behavior, not live inference or provider sandbox enforcement; Windows, required review and David acceptance remain pending. No installed Claude or Gemini was launched.


## BUG-028 — Malformed native settings retained binding

- Status: Scoped correction applied; focused5/5 and current full managed83/83 pass; integrated verification pending.
- Found from: Independent Codex initial-binding source review, then actual public private-peer regression.
- Symptom: A present collaborationMode settings object without its required model retained the original execution binding after owned completion.
- Root cause: The metadata parser treated an absent required nested model like an entirely absent optional metadata observation.
- Data flow reviewed: Native thread/settings/updated → bounded model validation → private execution proof invalidation → completed saved/reaped binding.
- Correction: Require a nonempty validated model inside a present valid collaborationMode settings object; retain absent optional metadata behavior and all existing ownership/publication guards.
- Regression: Healthy matching-model arm passed, followed by the malformed missing-model arm failing at the intended binding-preservation assertion before correction.
- Validation: Affected handler build passes; new regression plus original Codex positive/four-negative controls and two Claude binding/resume neighbors pass5/5.
- Residual risk: Codex same-child follow-up now passes full86/86 managed R121 and current core3032/3032 R133; current signed package and standard auxiliary20/20 also pass; Windows, live provider and David acceptance remain pending. No installed Claude or live Gemini launched.


## BUG-029 — Private peer hid premature input

- Status: Test-harness correction verified with actual isolated peer scripts; rebuilt full86/86 managed cases pass.
- Found from: Read-only review of the new delayed-mode continuation fixture.
- Symptom: A premature request buffered in Python could escape the pipe readiness guard until authoritative mode release.
- Root cause: Buffered stdin iteration and FD-only select observed different input buffers.
- Data flow reviewed: Batched native protocol input → Python read-ahead → mode-delay guard → later input counter.
- Correction: Use one os.read-based line reader for outer dispatch and the FD guard, including a final pre-publication check.
- Regression: Both actual peer scripts receive resume plus premature turn/start in one pipe write with stdin open; old peer has no pre-release error, corrected peer rejects turn/start before release and records no input witness.
- Validation: The exact standalone peer probe passes; source R112 test bodies are unchanged, production is untouched; subsequent full continuation runtime86/86 R121 and current core3032/3032 R133 pass, with current signed package and standard auxiliary20/20 also passing; external acceptance remains pending.
- Residual risk: This verifies the private test oracle, not product delivery prevention or live provider behavior.


## BUG-030 — Refused Codex resume lost retirement

- Status: Scoped correction passes all7 focused cases, all9 native refusal arms and full86/86 managed cases; current core3032/3032, signed package and standard auxiliary20/20 also pass; broader acceptance pending.
- Found from: Actual public same-child Codex refusal regression after initial completed/save/result/reap prerequisites.
- Symptom: A refused second setup sent no task input, yet its durable managed run remained Running after its process stopped.
- Root cause: Generic transport invalidation reset the original turn/process lease through an unobserved restart-stop before managed terminal publication.
- Data flow reviewed: Exact native resume error → generic restart-stop/reset → missing original stop lease → managed terminal publication cannot settle.
- Correction: Intercept the owned Codex resume error before generic invalidation and invalid-ID fallback; revoke input and retain the original lease for the existing tagged managed Stop/save/CAS/reap path.
- Regression: The unchanged30-second terminal wait failed; bounded diagnostics proved ledger Running, one owner, one stopped session, one total input, empty process ID and serial0. Diagnostics were removed byte-exactly before GREEN.
- Validation: Serial/parallel affected target builds pass; 7/7 focused includes normal/replayed continuation, delayed mode, all9 refusal/model/policy/identity arms, initial binding and preserved Claude refusal controls.
- Residual risk: Private protocol peers establish the tested behavior; current core3032/3032 R133 passes; current signed package and standard auxiliary20/20 also pass; live providers, Windows, required review and David acceptance remain pending. Generic reset/ownership/stop guards were preserved.


## BUG-031 — Native verification sandbox blocked sockets

- Status: Harness correction applied; actual private-socket/network canaries and exact31/31 failed-case rerun pass; canonical full R133 passes3032/3032 with zero failures or ENOSPC.
- Found from: Actual canonical R130 full native run, 3001/3032 passing with31 failures and zero ENOSPC.
- Symptom: Private runner fixtures could not create their temporary Unix sockets under the newly restricted verification sandbox.
- Root cause: The harness allowed localhost TCP but omitted temporary Unix-domain endpoints required by the real local runner fixtures.
- Data flow reviewed: Native fixture temporary socket → actual runner serve/bind → proxy or terminal attach → public native test assertion.
- Correction: Add directional temporary-root Unix socket permissions to the verification profile, retaining known-reader/process restrictions and external-network denial; product code and case bodies remain unchanged.
- Regression: Original full run includes explicit socket-bind EPERM and missing-socket failures; corrected profile proves both lexical /tmp and resolved /private/tmp socket bind/connect/echo, TCP loopback and external TEST-NET denial.
- Validation: All31 originally failed cases pass under the corrected profile in32.32seconds with one process; two setup-receipt cases also pass, but their original causes are not inferred solely from isolated success. Full canonical R133 passes3032/3032 with two processes in471.44seconds against the same binary hash, with zero failures/ENOSPC.
- Residual risk: R146 additionally admits only the OS-resolved temporary UNIX sockets needed by Chromium; old/new actual canaries demonstrate the boundary and the same CEF binary passes47/47 R147; external network remains denied. Current signed package and normal auxiliary20/20 pass. The separate restricted picker timeout remains unexplained under its existing owner; no live-provider, platform, review or David acceptance is supplied.


## BUG-032 — Child reopening lost reading position

- Status: Correction automated-verified;129/129 connected cases,2237/2237 full UI,23/23 performance, production frontend and signed app build pass; required Claude Review and acceptance remain pending.
- Found from: Takeover audit of the existing T3 subagent experience agreement.
- Symptom: Back/reopen discarded an earlier selected child message page and its scroll position in both managed and passive native conversations.
- Root cause: Child-only navigation unmounted components whose page and geometry lived only in component state, while the executable ChatView route already retained a pane visit.
- Data flow reviewed: All Chats/card open → pane route → child transcript read → page/scroll region → Back/unmount → same child reopen.
- Correction: Retain only bounded numeric reading state in this window, scoped by the explicit pane, parent identity and exact managed/native child route; fresh transcript reads and execution permissions remain independent.
- Regression: Three actual public navigation cases fail before correction and pass afterwards, including delayed read readiness and sibling/two-leaf isolation. The initial native fixture read-count mismatch is separately retained and corrected before claiming its behavioral RED.
- Validation: Original three navigation REDs and independent off-tail finding with two additional actual REDs precede the corrections; final129/129 connected,2237/2237 unit and23/23 serialized performance cases pass. The actual private browser preserves page/top after fresh reopen and65→81 message growth; saved screenshot inspected. Production frontend and signed app build pass. Receipts: `<private working evidence>` and linked logs.
- Residual risk: Required Claude Review, real packaged/Windows checks and David acceptance remain outstanding. This does not enable follow-up for any additional provider.


## BUG-033 — Native child clocks were discarded

- Status: Corrected locally with focused native/UI and adjacent gates passing; current full native, package, required review and acceptance remain pending.
- Found from: Takeover trace of the official Copilot SDK lifecycle schema and retained official Claude child task notifications through actual native projections.
- Symptom: Child cards showed missing timing despite exact provider-reported child clocks or terminal duration.
- Root cause: Lifecycle normalization and shared persistence/frontend projections discarded child timing; Claude saved-tool restoration also omitted the child metadata vector.
- Data flow reviewed: Exact native child lifecycle or journal → tracked child → synchronized saved tool → save/reload and deferred page fingerprint → CEF → frontend normalization/reconciliation → card.
- Correction: Preserve bounded optional Copilot lifecycle epochs and native durations through shared child metadata, preserve Claude terminal-notification duration without inventing epochs, and retain independent child identity, terminal disposition and saved restoration.
- Regression: Two initial native REDs plus one unknown/invalid control and four UI REDs precede corrections; independent unsafe Copilot session identity, cancellation and Claude terminal-duration/zero cases also reached actual RED before their corrections. The later Claude task-association defect is recorded separately as BUG-034.
- Validation: Focused native10/10 passes in1.10s and the separate107/107 adjacent inventory passes in53.92s; the subsequently expanded live/saved identity case passes1/1 in0.14s on its rebuilt binary. Focused UI36/36, full UI2242/2242, typecheck and serialized performance23/23 in7.98s pass. These are distinct receipts, not a single current full native gate. Evidence: `<private working evidence>`.
- Residual risk: Current3042-case full native, signed package/CEF/auxiliary gates, required Claude Review, live providers, Windows and David acceptance remain pending. Earlier ENOSPC failures are preserved but no longer describe the resumed focused build state. Other providers' native individual timing and additional two-way controls remain incomplete; parent timing never substitutes for missing child evidence.

## BUG-034 — Foreign Claude task replaced its launch

- Status: Corrected locally with focused and adjacent native verification; broader current gates, required review and acceptance remain pending.
- Found from: Independent timing source review followed by an actual public durable-association regression.
- Symptom: A conflicting task notification overwrote the original launching tool's saved task ID, so the legitimate later completion without a tool ID could no longer resolve its original tool.
- Root cause: HandleClaudeTaskEvent assigned the incoming task ID before rejecting mismatched child timing metadata.
- Data flow reviewed: Exact Agent/task admission → foreign task reusing tool ID → saved association → legitimate task-only terminal notification → original child status/duration and deferred CEF projection.
- Correction: Reject conflicting nonempty task IDs before restoration or mutation, using the known live association and a saved-history fallback only when needed.
- Regression: The first10-case run passes9 and fails at the actual reloaded ToolCall.task_id assertion before timing checks; the correction then passes10/10. A strengthened fresh-fixture case separately executes live and saved-fallback rejection, retains one original tool/child and its9000ms terminal duration, and confirms a rejected saved-fallback notification does not repopulate runtime state.
- Validation: Corrected focused10/10 in1.10s, separate adjacent107/107 in53.92s, and the latest rebuilt two-arm case1/1 in0.14s pass. Original test and timing/ownership assertions remain. Logs: `provider-parity-takeover-native-timing-association-red.log`, `provider-parity-takeover-native-timing-focused-green.log`, `provider-parity-takeover-native-timing-adjacent.log` and `provider-parity-takeover-native-timing-saved-arm-green.log` under `<private working evidence>`.
- Residual risk: Current full native/package/CEF/auxiliary gates, required Claude Review, live provider, Windows and David acceptance remain pending. The guard preserves existing task routing; no additional provider execution, approval or native child continuation is enabled.


## BUG-035 — Native policy setup preceded admission

- Status: Connected correction passes seven focused cases; remaining86 handler cases also pass, while core, package and review gates remain pending.
- Found from: Actual Copilot policy integration failure after genuine initial completion/save/reap, followed by bounded private diagnostics.
- Symptom: Managed native setup ran before its authored task had a real row/turn serial, so strict policy negotiation could not start and otherwise supported children remained unbound.
- Root cause: Reconnect could create the native session before the ordinary ready-only caller drained its managed queued task; weakening proof ownership would have hidden the ordering defect.
- Data flow reviewed: Captured managed queue → owned process initialize → setup preflight → actual row/serial admission → policy readback → input/save/completion/reap → continuation witness.
- Correction: Prepare the exact owned queued task through the existing drain/save/CAS pipeline before native setup, retaining pending permission/question waits and every actual row, proof, save and final-input boundary.
- Regression: First candidate and withdrawn compatibility attempt each pass2/7; explicit private diagnostics establish serial0/row-1 at first setup with all other first gates true. Temporary instrumentation was restored before the acceptance build.
- Validation: Corrected build and direct seven-case run pass7/7 in39.06seconds, including two optional-policy initial arms, genuine same-native resume,14 refusal arms and transient configuration drift. Static driver count is corrected separately from88 to93; unchanged remaining86 cases pass separately in135.52seconds, covering all93 unique handler cases across a driver-only expected-count rebuild.
- Residual risk: These are private native protocol peers, not installed/live-provider or operating-system filesystem containment evidence. Full current native, package/CEF/auxiliary, required review, Windows and David acceptance remain outstanding. Manual approval ceiling findings retain their separate owner gate.


### BUG-032 Claude follow-up corrections

Claude round1 identified mounted off-tail page growth and optional transcript-hint visit splitting, including an in-place route hint remount. Four new reading-position regressions fail before correction while the genuine-tail control passes; the separate focus regression also fails, producing80/85 actual RED. Source now pins off-tail geometry, checkpoints numeric visits before remount, and uses pane/parent/run reading identity while preserving fresh transcript validation. All85 connected cases pass after correction. Full frontend, package and Claude round2 remain pending; earlier verification and acceptance flags are preserved. Receipts: `provider-parity-takeover-claude-ui-red.log`, `provider-parity-takeover-claude-ui-focused.log` and `provider-parity-takeover-claude-ui-fixes-verification.json`.

## BUG-036 — Background reads removed retry focus

- Status: Actual focused correction passes; full frontend, package and Claude closure pending.
- Found from: Required Claude review followed by actual lost-response/poll regression.
- Symptom: Polling an active child unmounted and recreated the unknown-result follow-up form, losing its focused textarea and interrupting keyboard editing.
- Root cause: A background read cleared presentation state together with the authoritative control target.
- Data flow reviewed: Actual lost submit response → retained unknown draft → active child poll → target revocation → form DOM → keyboard focus and explicit same-request retry.
- Correction: Clear the authority ref and hold sends during revalidation while retaining the same-child presentation/draft DOM; only fresh exact response restores authority.
- Regression: Delayed poll preserves the same connected and focused textarea; Enter, form and retry cannot send while readPending, and explicit later retry retains the original envelope and accepts its replay acknowledgement.
- Validation: Actual failure precedes production correction; all85 focused cases pass. Current full frontend/build/review gates remain separate.
- Residual risk: No automatic retry, changed request identity, new provider capability or user approval is introduced; required Claude review, live/platform and David acceptance remain pending.

## BUG-037 — Discovery fixture raced async stop

- Status: Fixture correction verified by the current full native3076/3076 run; original failure retained, timing assertions unchanged and user/platform acceptance pending.
- Found from: Current canonical full run3040/3042, followed by source review and bounded unchanged-binary reproductions.
- Symptom: The malformed ephemeral discovery test asserted cleanup immediately after a single Poll even though its owned asynchronous stop may still be unfinished.
- Root cause: The test assumed synchronous stop completion; production deliberately retains ephemeral discovery until its actual stop receipt finishes. This behavior predates the takeover changes.
- Correction: Poll the public owned-stop state within the existing five-second pattern, then retain all original cleanup, pending/error, no-durable-chat and retry assertions.
- Validation: Original full-run failure preserved; exact two cases, three fresh malformed processes and both original32-case batches pass without source changes. Those passes do not prove the originally failing receipt state or disposition the independent100ms cold poll RED. Current fixture build/exact/final full gates remain pending.
- Residual risk: No production cleanup, native assertions or timing thresholds are weakened; cold polling attribution remains unproved.

BUG-036 follow-up (Claude round3, 2026-10-18): Enter and Retry genuinely dropped focus to BODY in private Chromium before correction; four new unit oracles failed and one external-focus control passed. Pending submission now keeps a focusable read-only editor and transfers only this form’s existing focus before button mutation; removal hands owned focus to the surviving active child heading. Focused91/91 and actual Enter/Retry/accepted-removal browser checks pass; full gates and Claude round4 remain pending. No user acceptance is claimed.

BUG-036 review closure: Claude round4 approves the pinned correction with no supported P0-P2; full frontend2254/2254 passes101.24seconds. Optional scope-loss button-focus P3 is deferred separately in the V5 Incomplete Work table. Current quiet native/performance/package gates are still pending; acceptance is not claimed.

## BUG-038 — Revoked preparation rejected fresh receipt

- Status: Correction verified by full native3076/3076 and scoped Claude permission round3 source approval; live/platform and user acceptance pending.
- Symptom: A failed save preparation whose publication ownership had been revoked could reject a later manual-input receipt attached to the same save task.
- Root cause: The failed-preparation branch collected completion callbacks without requiring current publication ownership.
- Correction: Only the currently owned failed preparation may reject its completions; a revoked task retains later callbacks for the rearmed save.
- Regression: A genuine private attachment-validation failure reproduces the receipt rejection before the guard; the corrected case and current-owned failure control pass in the26-case and127-case gates and the later153/154 connected run.
- Residual risk: This independently proven race is not established as the cause of the earlier full-run Claude quota failure; the separate100ms cold polling failure remains unexplained.

## BUG-039 — Captured read decisions widened access

- Status: Shared correction verified by full native3076/3076, managed93/93, CEF47/47, goal3/3, auxiliary20/20 and performance23/23; Claude permission round3 source-approved; live/platform/display and user acceptance pending.
- Symptom: Manual and saved native approvals could exceed a captured agent-read or goal-review restriction; command display text could masquerade as the native command, and a same-name OpenCode MCP action could masquerade as a continuation question.
- Root cause: Final manual/replay paths did not share typed current-request authority, while native command provenance and question identity were inferred from display fields or names.
- Correction: Share the immutable response ceiling with the DTO projection, require typed native command and offered one-time decisions, validate saved replies against fresh decoded requests, save bounded safe replacements before delivery with rollback, and distinguish the known OpenCode producer shapes.
- Regression: Separate actual REDs cover manual bypass, unsafe replay, persistent replacement, missing native command, session-first auto selection, MCP name collision and a hidden Copilot implementation choice; all these behavioral checks pass in the latest connected run.
- Residual risk: The command filter trusts executables and repository configuration; OpenCode metadata is producer-shape evidence rather than plugin attestation. Native CLI View, live providers, Windows/SSH combinations, display acceptance and David's sign-off remain separate.

## BUG-040 — Restored fixture read old frames

- Status: Test-only correction verified by the exact case, original26-case batch and full native3076/3076; Claude permission round3 approves the unchanged assertions and exact fixture patch.
- Symptom: The saved-response rollback test sometimes counted two wire frames instead of its single fresh delivery fence.
- Root cause: A restored sink reused the prior capture path, and the reader could observe old data before the new shell process opened and truncated that file.
- Regression: Numeric diagnostics reproduced capture generation2 containing IDs31/32 while the new process had sent only ID32; original-process diagnostics identify the earlier two responses.
- Correction: Give every attached process a unique capture path; retain all original rollback, pending-state, disk and real-wire assertions, and remove the temporary diagnostics.
- Validation: Exact regression passes0.724seconds and the original ordered26-case batch passes9.908seconds, including unchanged100ms cold controls.
- Residual risk: No production behavior changed; this does not explain the earlier full-suite cold polling or quota failures.

Verification closeout (2026-10-18): the current full native3076/3076, managed93/93, CEF47/47, goal3/3, auxiliary20/20 and serialized performance23/23 gates pass. The unchanged current UI retains2254/2254 full unit evidence and Claude round4 approval; the full signed Mac archive includes all requested flags and three SSH helpers, unpacks with2079 identical file/symlink entries and passes strict signing. This closes automated/package verification for BUG-032 through BUG-040 while retaining their provider, platform, visual and user-acceptance limits. Earlier cold/quota failures are retained without claiming their original causes.

## BUG-041 — Final harness used wrong inputs

- Status: Ignored verification harness corrected; original failed reports retained; no production change.
- Symptom: CEF reported14/47, the goal validator rejected an exit-zero3/3 log, and an archive check compared different frontend build directories.
- Root cause: The CEF environment omitted `UAM_TEST_CEF_APP_BUNDLE`; `Counter` interpreted registration dictionary values as counts; the archive check used the old manual `UI-V2/dist` instead of CMake's isolated output.
- Correction: Supply the exact packaged bundle, count dictionary keys, and compare archived UI with `Builds/agent/UI-V2/dist`.
- Validation: CEF47/47 passes on the same binary/profile, the original goal log proves each exact registered name once, and all2079 unpacked entries plus the isolated UI output match the signed app. Successful core/performance/managed gates were retained without rerunning them.
- Residual risk: These checks establish fixture and artifact correctness, not actual rendered desktop journeys, native provider operation or user acceptance.


## BUG-042 — Early Copilot drift retained proof

- Status: Corrected locally after two schema-correct actual REDs; rebuilt exact2/2 pass; Claude source round2, full integration and revised package pending.
- Symptom: Fresh root model or managed-settings drift after its matching readback but before final tools could still allow resumed follow-up input.
- Root cause: Event rejection waited for beforeReady, even after individual current observations had been accepted.
- Correction: Begin model/session rejection after its matching snapshot and policy rejection after its matching settings readback; retain beforeReady for later phases, exact child/replay filters and sticky rejection.
- Regression: Each case completes genuine initial binding, completion, save and reap, resumes the exact native session on the owned second process, accepts the first four readbacks, then sends a schema-complete drift event followed by valid final tool metadata in one ordered packet. Both cases actually send forbidden input before correction; both preserve prior bindings/history and reject input after correction.
- Evidence: `<private working evidence>`. The earlier incomplete-event preliminary log is preserved but does not establish the valid reproduction.
- Residual risk: Native dispatch enforcement, broader principals/nesting/restart/SSH, platform and David acceptance remain separate; missing-scope handling is unchanged without an evidenced input bypass.

## BUG-043 — Withdrawn Retry lost focus

- Status: Corrected locally after an actual focus RED; exact2/2 and related93/93 pass; required Claude UI round5, full frontend/performance and revised package pending.
- Symptom: Scope or capability withdrawal could remove a child follow-up form while Retry owned focus, leaving focus on BODY.
- Root cause: The textarea ref detach callback only recorded focus on the textarea itself, missing a focused button in the same form.
- Correction: Capture focus ownership anywhere within that exact form before detach; retain the existing surviving active-child heading fallback and external-focus guards.
- Regression: Genuine lost-response, running refresh and held poll precede capability withdrawal; Retry focus fails before correction while external Back focus passes. Both pass after the one-line correction, preserving transcript, parent, single-submit and earlier pending/removal assertions.
- Evidence: `<private working evidence>`.
- Residual risk: Focus preservation does not restore withdrawn authority or resend work. Native desktop, Windows and user acceptance remain pending.


## BUG-044 — Managed inventory count stayed stale

- Status: Driver-only correction independently checked and fresh complete95/95 suite exits0; original failed report retained.
- Symptom: Every managed case passed, but the executable returned1 after two new tests increased the registry from93 to95.
- Root cause: The independent static Copilot count remained7 while the same guarded source group now registers9 cases.
- Correction: Change that exact guarded expected count to9 and update its comment; no product code, case body, assertion, timeout or platform guard changes.
- Validation: Source, binary inventory and95 unique PASS names match exactly. The new full rerun passes95/95 in184.608seconds; other already-passing current native/frontend/connected gates remain valid and were not repeated for this driver-only correction.
- Evidence: `<private working evidence>` and preserved original `final-gates-20261018T013838Z-65d8cd1a/result.json`.
- Residual risk: Inventory correctness is separate from native/provider/platform/user acceptance.

Final R2 verification closeout (2026-10-18): current core3076/3076, frontend2256/2256, performance23/23, managed95/95, CEF47/47, goals3/3 and auxiliary20/20 pass, with scoped ClaudeUIR5 and independent Codex drift-source review. The current full signed Mac archive matches2079 extracted entries and all four compiled helper fingerprints; original-app private startup passes19/19 canaries and leaves no owned processes. This closes implementation/automated/package verification for BUG-042 through BUG-044, retaining native dispatch assumptions, actual desktop visual inconclusiveness, provider gaps and live/Windows/SSH/user acceptance. The original full report remains FAIL for its stale expected-count exit, rather than being rewritten as green.

## Provider/mobile central action integration

- Status: Integrated locally; all29 protocol cases pass after three exact-upstream-policy REDs; combined runtime verification pending.
- Symptom: The current companion default-deny policy rejected provider-native model-option and managed-child requests before their existing native handlers.
- Root cause: The retained provider snapshot added actions to an obsolete server-local allowlist while latest V5 moved admission and read-only authorization into the shared companion policy.
- Fix: Retain the current server/transport and add six bounded chat-scoped actions to the central policy, with read-only list/transcript access and separately authorized mutations.
- Regression: Three additive protocol cases cover action capabilities, malformed or privileged fields, read-only mutation denial and committed-upload final policy; their CTest registration is retained.
- Validation: Current tests compiled against the exact upstream policy fail all three new cases at the expected missing-action boundary; the integrated policy passes all29 cases under identical all-six-provider definitions. The first isolated compile omitted required provider definitions and remains a setup failure, not a product RED. Raw proof: `Builds/agent/mobile-rebase-policy-proof/red-green-result.json`. Full desktop/CEF verification is pending.
- Residual risk: The installed phone UI still needs its own native-option and managed-subagent controls; no phone rebuild or user acceptance is inferred.

## New Chat borrowed another model

- Status: Fixed locally after actual Default RED and passing explicit-model control;150/150connected cases and Claude R2 source review pass; full current frontend/app verification pending.
- Symptom: New chat kept Default selected while displaying and submitting reasoning supported only by an unrelated live chat model.
- Root cause: NewChatModal merged a same-workspace live binding including currentModelId, which the shared selected-model helper legitimately prioritizes for chat-owned callers.
- Fix: Apply the existing provider-default catalog normalization only at the NewChatModal boundary, preserving available models, live configuration and loading/error/discovery metadata.
- Regression: Distinct low-only catalog model and high-only live model exercise both actual Default menu/submission and explicit-model control without changing the live binding or triggering discovery.
- Validation: Genuine Default failure and explicit control precede correction; new2/2 and connected150/150 pass; source hashes and raw logs are retained in `<private working evidence>`.
- Documentation impact: Repairs the existing model/reasoning isolation agreement; no expected behavior, provider capability classification or architecture relationship changes.
- Residual risk: Native provider-default model identity and real desktop/provider/platform/user acceptance remain separate.

## Companion captured-skill branches were rejected

- Status: Implemented after an actual public-policy RED and independent source approval; rebuilt public-policy GREEN and real private HTTPS exact/empty/forged-source branches pass; full current gates pending.
- Symptom: The shared browser delivered-edit path received400 when retaining or removing captured skill context.
- Root cause: Latest companion branch-field admission lacked the provider work’s retainedMarkdownStoreFiles field, so the native captured-source guard was unreachable.
- Fix: Admit only the bounded retained selection, preserving omission versus explicit empty selection and rejecting malformed/NUL/oversized identities; native source membership, uniqueness and captured prompt snapshots remain authoritative.
- Regression: Existing protocol case retains all prior refusals and adds direct/committed-upload exact/empty selections plus invalid and privileged-field controls. The real CEF/Caddy lifecycle case adds HTTPS exact/empty/forged-source branches with saved byte-exact snapshots, untouched source and desktop draft, and no provider/worktree runtime.
- Evidence: `<private working evidence>`, `-applied.json` and `<private working evidence>`.
- Validation: Focused real CEF5/5 passes with the packaged Caddy/app environment and no setup skip; exact source snapshot bytes, explicit removal, forged-source409, durable branch contents, no runtime start and unchanged desktop draft are asserted. Focused core77/77 also passes.
- Residual risk: Full current gates and native desktop/provider/platform/user acceptance remain pending. Private HTTPS proof does not establish a real phone journey.

Combined mobile/provider checkpoint (2026-10-18): companion provider actions, captured-skill branch policy and New Chat Default reasoning corrections now pass the signed desktop refresh and current full3087core/95managed/55CEF/3goal/23performance/52auxiliary gates, plus frontend2283and production build. Real Caddy HTTPS branches execute without setup skips. Phone followup `a97e1db6` is integrated after the frozen run with1439unaffected files byte-identical; source/runtime tests are not repeated for iOS/documentation-only changes. Archive and real native visual checks remain pending; native phone controls, broader provider gaps, live/platform and David acceptance remain explicitly open. Protected pre-followup stash/patch/tar and raw current gate results are retained under `<private working evidence>`.

Combined checkpoint delivery: verified231854827-byte archive SHA256 `edcae6d0cae91b7e76f7a83cb3cf9bfa90dcb22cfab9d1f031395587962dd4e4` passes2078-entry extraction/signature/four-helper proof. Actual private packaged native create/pin/rename/settings/quit-relaunch and draft persistence now pass under19isolation canaries. Broader provider gaps/live/platform/user acceptance remain open; next test-only Gemini and Antigravity packets do not alter this immutable checkpoint.

## Gemini retained images had no Retry route

- Status: Conditional local implementation; five actual warm/cold delivery and refusal cases pass after unsupported-source REDs, with connected/full/live/platform/user acceptance pending.
- Cause: Gemini lacked the saved-original adapter hooks; protocol-only opt-in also admitted a known unsupported running session before delayed preflight.
- Correction: Separate cold eligibility from negotiated capability, retain current native/process/model/mode evidence through the owned preparation and sealed ACP packet, and refuse known missing capability before admission.
- Evidence: `<private working evidence>`, `provider-parity-gemini-warm-diagnostic-result-r1.log` and `provider-parity-next-frontier-green-r2/gemini.log`.

## Owned Antigravity children stayed passive

- Status: Conditional local implementation; valid warm/cold native cases and seven configured-provider UI cases pass, with required Claude review and broader gates pending.
- Cause: Durable lineage excluded Antigravity UUIDs and the UI execution allowlist omitted already-owned local sessions; the existing provider and imported-read-only controls remain required.
- Correction: Validate canonical native identity and existing ownership before saving display lineage and opening the existing child Chat pane; do not invent an exporter or control the original spawned process.

## Cold child opening erased branch ancestry

- Status: Implemented after actual valid-family cold RED and warm-loaded control; two exact native cases pass, connected/full verification pending.
- Cause: A temporary partial catalogue normalized an existing saved child without its valid independent branch ancestors.
- Correction: Restore only authoritative branch parent/root/source index from the exact matching writable saved record after birth and revision checks, without copying queue or execution authority.
- Evidence: `<private working evidence>` retains the genuine cold failure; `provider-parity-antigravity-cold-branch-green-r1.log` records corrected warm/cold and refusal passes.


Current next-frontier validation, 2026-10-18: the negotiated Gemini original-image, owned Antigravity navigation and cold child ancestry corrections above now have passing frozen core3093/3093, managed95/95, CEF57/57, goals3/3, frontend2285/2285, performance23/23 and auxiliary52/52 gates, plus the immutable combined r2 archive/extraction/signature/current4helper verification. Actual Claude R1 scoped source approval is recorded. The new native child-navigation visual attempt remains INCONCLUSIVE (`cgWindowNotFound`) after19private isolation canaries and successful actual app launch; it is not a visual or live-provider PASS. Earlier bounded native journeys remain separate, and all platform/live/David acceptance flags remain.

## Gemini terminal imports refused inline originals

- Status: R6 plus independently reviewed R5 passes26 current public and305 connected native cases after the retained caller/post-stop and Claude follow-up REDs; full desktop gates pass3110core,95managed,57CEF,3goals,23performance and52auxiliary cases, but actual Claude R2 identifies shared parser-pool starvation and orphan-state retention; both reproduce in actual public regressions, with a third shared-owner control reaching the last-owner failure; independently source-reviewed parser-pool R3 now passes29focused and310connected native cases with actual Claude R3 scoped source approval; current full3113core/95managed/57CEF/3goals/23performance/52auxiliary, signing and immutable r3 archive verification pass.
- Symptom: Loaded terminal history, final close, existing-owned original refresh and fresh retry cannot save a newly parsed Gemini journal containing user and tool-result image originals.
- Cause: The remaining callers reach plain chat saving with `NativeInlinePending`; its required persistence guard refuses the complete import.
- Reproduction: Independently reviewed R3 regression-only test build passes, then all four exact cases reach actual import-failed status and fail durable saved/live completion; source original, old primary, draft/unrelated state and text-only subarm controls remain intact.
- Source correction: Bounded prepared-media publication retains exact owner/revision and terminal lifecycle admission, post-stop source reload, metadata-only reprepare and cold-save coordination, worker acknowledgement and prompt cancellation of unconfirmed-stop exit work; global plain-save guards remain.
- Evidence: `<private working evidence>` and preserved baseline `result.json`/`exact.log`.
- Acceptance: Full automated checkpoint and focused private native/process evidence only; actual Claude R3 closes the prior parser-capacity finding at source level and the r3 full gates/archive pass; no GUI performance, live, Windows, SSH or David acceptance inferred.

Terminal parser review reproduction, 2026-10-18: regression-only compile passes in17.231seconds; both exact cases fail against unchanged production in15.965seconds. Two actual completed terminal parsers block third-chat Pending admission, and actual stop/reap/erase without final capture leaves orphan task states after the15-second poll bound. Later successful-publication, exact-original and fresh-admission subarms have not yet run. <private working evidence> (private evidence retained locally).

Current terminal/parser correction checkpoint: all29focused and310connected cases pass after the retained three actual parser failures; actual Claude R3 has no supported P0-P2. Full current app/package and inventory gates remain pending, and no live/platform/visual/user acceptance is inferred. <private working evidence> (private evidence retained locally).

Terminal r3 delivery: full frozen automated gates and exact archive extraction pass; actual Claude R3 source approval is retained. Current native visual remains untested, prior CUA failure is separate, and live/platform/David acceptance remain open. The goal continues into actual passive child-original baseline; no passive candidate is applied at this checkpoint. <private working evidence> (private evidence retained locally).

## Passive Gemini child originals refused or stale

- Status: Valid unchanged-production baseline reproduces three image-bearing public Open save refusals and a successful rename/refresh that retains the old answer; cached text and foreign-reference controls pass. Production R9 now compiles and all11 private public-CEF cases plus the generic empty-native-ID core control pass, including exact originals, cached reopen, renamed refresh, stale records, supersession and cancellation; full current app/package gates remain pending; both actual Claude review attempts returned rate limits without a review.
- Cause: The plain-save import guard correctly refuses pending inline bytes, while generic saved-message timestamp overlay can replace an explicitly read native conversation after organisation changes.
- Correction: Actual local-reader admission behind an explicit provider adapter capability prepares owned originals on a worker, retains exact parent/child/request/revision fences, preserves UAM organisation with the genuine native conversation, and leaves native-file-independent cached reopen and generic empty-native-ID refusal intact.
- Evidence: <private working evidence> (private evidence retained locally); prior shared reader and timestamp prerequisite failures are retained separately and are not product REDs.
- Acceptance: In Progress; Un-Reviewed; Claude Review; Not User Tested; no live, Windows, SSH, native visual or David acceptance.

Passive residual boundaries: actual legacy service-created live projections can retain unknown archive/cache representation and require a separate genuine lifecycle check; no currently reachable CEF passive-refresh creator is inferred. The ACK-uncertainty response warning is present in callback JSON, but child navigation currently consumes chatId without explicitly surfacing that warning; user-visible warning delivery and deterministic ACK-fault verification remain open. These are retained acceptance/implementation gaps under Gemini Binary History Parts, not a claim that acknowledged originals were lost.

Passive ACK-warning follow-up: real pre-fix receipt-obstruction test reaches the missing native-status assertion only after successful callback warning and actual committed originals; R9 passes all11 focused CEF cases, including unchanged committed primary/summary, originals and serialized status after real ACK failure. The shared status notification route is source-traced, not a newly rendered native-visual acceptance; true directory-fsync failure remains untested.

Passive child-original R10 checkpoint (2026-10-18): actual public shorter-native-history RED is corrected without relaxing equal-count refresh or owner/revision guards; focused12/12 and frozen full3114core/95managed/69CEF/3goals/23performance/52auxiliary PASS. Actual Claude R3 gives scoped source approval with no P0-P2; nonblocking wording/digest-cost/repeated-warning limits are retained in the V5 incomplete table. Combined r4 zip is extracted/signature/four-helper verified; native visual, live providers, Windows, SSH and David acceptance remain unverified. See `<private working evidence>`; no Complete or phone rebuild/install is claimed.


### Remote native originals — 2026-10-18

The actual private-helper/public-handler baseline rejected healthy remote Gemini inline originals at plain history save. The reviewed R3 route now prepares and publishes originals on bounded workers, revalidates saved/live ownership and revisions, confirms the supported platform phase before the normal response, preserves committed files on uncertainty and serializes the current owner. Seven public cases across ten arms pass, including actual original-store refusal/retry, host/profile/storage drift, supersession, receipt obstruction and current-owner/replacement response controls. Current combined r5 archive is verified; live SSH/provider, foreign-OS, native visual and David acceptance remain pending. Evidence: <private working evidence> (private evidence retained locally).

### Cleanup gate test retirement — 2026-10-18

A full run failed the immediate cleanup-acquisition oracle after a worktree retry had already rejected changed artifacts. Source review supports a future-ready versus detached lease-retirement window; the original failing holder and arm remain unobserved. The test now waits up to30seconds for the actual exclusive cleanup guard and keeps it through the original survivor checks, retaining both arms and existing bounds. Its exact case and fresh3114-case core run pass; no production lease behavior changed. Original failed evidence and the reviewed narrow test patch remain linked in the r5 checkpoint.

## Pending workspace Send and Stop could strand captured intent

- Status: Source integrated and focused-validated; final build/full gates, shipping/live and user acceptance remain pending.
- Root cause: fresh direct Send returned failure at pending workspace preparation, and interrupted-input conversion could drop captured policy or clear raw input when the recovery carrier could not represent it.
- Fix: retain the exact eligible invocation through owned workspace preparation and durable admission; preflight shared Stop/settlement conversion and conserve raw intent plus existing policy records when it is refused.
- Locally verified scope: Private Direct-input Stop/Plan Capacity Managed And Worker Controls Verified Across Separate Receipts; evidence: application evidence (private evidence retained locally).
- Boundaries: normal pending direct input is internally unheld; actual save acknowledgement is distinct from native delivery, and other save outcomes retain their own rollback/uncertainty rules. Existing codec limits and read ceilings are unchanged.
- Remaining: packaging and installed/live acceptance, broader runtime/platform coverage and historical full-core/cold-image failures remain open; no consent outcome, safe replay, Complete or David approval is implied.


## Helper wait discarded blocker reason — 2026-10-20

- Status: Diagnostic extension source implemented; affected native handler compile and six existing semantic guard/rollback cases pass; delayed callback, Claude review, numbered delivery, live and David acceptance remain pending.
- Found from: User-reported normal Windows helper update refusal plus source data-flow review; the actual active actor and abort cause remain unknown.
- Symptom: The later reserved-install wait reported only “Remote work has not finished stopping” while its existing strict guard already supported bounded fixed blocker categories.
- Root cause: The terminal wait callback discarded the guard reason rather than observing it for its existing failure response; its `expired` argument can mean deadline, task-repost failure or exception.
- Data flow reviewed: `installRemoteHost → owned reservation and delayed UI task → strict runtime guard → existing 409 callback → normal settings failure surface`.
- Fix: Observe the current strict reason before unchanged reservation restoration and append the existing privacy-safe code, ignoring its boolean for admission; no deadline, guard, token, Stop, cleanup, retry or authority change.
- Regression/validation: Existing background-scan, first-match/host-scope, unknown-stop, physical-cleanup and two reservation-rollback cases pass 6/6 using the unchanged qualified core binary; only the affected app handler object compiled and the normal app target linked successfully. No copied-lambda/source-string mirror test or timing reduction was added.
- Residual risk: This does not prove the delayed CEF callback output, actual Windows actor, a cleanup fix or the cause of the observed refusal; `none` means only no currently matched actor, not quiescence or safe replay, and historical full-suite failures remain. Existing owner 01a13aa1-ff51-7fb2-9c91-330e52ee3fa2 and Un-Reviewed/Claude Review/Not User Tested remain.
- Evidence: Scoped owner ACK (private evidence retained locally), independent native review (private evidence retained locally), integration receipt (private evidence retained locally).

## Windows OpenCode rejected equivalent separators — 2026-10-20

- Status: Confirmed comparison defect source-corrected and focused-validated; signed alpha132 installed and verified; matching Windows-helper activation, live incident cause and acceptance remain pending.
- Found from: Source/data-flow investigation of the reported Windows OpenCode managed-child workspace refusal, followed by deterministic private regressions.
- Symptom: Equivalent slash and backslash spellings of a strict absolute Windows DOS workspace could fail native ownership checks in structured chat and Terminal View.
- Root cause: Both remote native-session consumers compared workspace-directory strings byte-for-byte even when the ordinary DOS path components and case were identical and only corresponding separators differed.
- Data flow reviewed: `captured Windows host/workspace + native root snapshot → shared directory-identity predicate → structured companion ancestry/admission and Terminal View activity → normal owned-session/permission decision`.
- Fix: A shared strict-DOS grammar predicate permits corresponding slash/backslash substitution for captured Windows hosts while retaining exact native IDs, drive/component bytes and case, ancestry checks and permission/read ceilings; local canonicalization, Linux comparisons and unsupported path forms retain their existing rules.
- Regression: Two private public-boundary cases failed before production changes; eight canonical connected cases pass, including grammar refusals, structured/Terminal separator acceptance, foreign identity/ancestry and read-only controls.
- Validation: Eight actual canonical cases PASS against inventory3251 (3243 unrun) on the unchanged alpha131/packageOFF core binary; dependency/flag/link/binary joins qualify that evidence across the metadata-only alpha132/packageON transition without a claimed rerun. The genuine four-helper source contract f2ff5ec83c246df1b63ef153d5dad90bead29929635fdc6af8f11af16b069803 and signed2348-entry alpha132 bundle were built; actual installation verifies the tree/signature and4196 unchanged saved-data witnesses with rollback131 retained. GUI and cooperative-wait candidates are not included; historical aggregate FAIL remains.
- Residual risk: Later durable metadata for the failed child has exact native ID and separator-only directory equivalence, but does not recover its original HTTP response or prove physical workspace ownership or the live incident's cause. The subsequent normal matching-helper update refused with current observed category background-scan and left the alpha131 helper selected; this does not identify an actor or prove stale work or a cleanup defect, and no new child was launched. No fresh installed Windows child success, global completion, safe resend or David acceptance is claimed. Related runtime Owner chat-1791755529536-1b146e and contact UNSENT/no ACK remain; exact current UAM ID is unknown; Un-Reviewed and Not User Tested remain.
- Evidence: Canonical integration (private evidence retained locally), private RED/GREEN (private evidence retained locally), alpha132 envelope (private evidence retained locally), one durable-history read (private evidence retained locally).
- Installed evidence: Actual Alpha132 Installation (private evidence retained locally), Startup And Helper Refusal (private evidence retained locally).

## Same-scope native model choices were missing — 2026-10-20

- Status: Source-corrected and focused-validated across two qualified executions; broader native verification, matching helper/desktop delivery and live acceptance remain pending.
- Symptom and root cause: Public `agent_list` target descriptions read the legacy models array but omitted same-scope native config-only model choices; legacy-first merging could also displace authoritative bounded native choices.
- Data flow reviewed: `owned provider/host/workspace/profile metadata → shared native model parser → scoped catalogue authority/limits → public managed target.models`.
- Fix: Reuse the native parser and the first native model-config authority, including an explicitly empty choice list, while retaining workspace/profile binding, validation and output bounds. This restores existing catalogue agreements; no UI change or provider upgrade is included.
- Regressions: `AgentListIncludesSameScopeOpenCodeConfigOnlyNativeModels` and `AgentListConfigModelChoicesRespectNativePrecedenceAndBounds` first exposed two intended projection REDs. The public case passes in the first post-fix execution; after one explicit JSON-object fixture-construction correction, the bounds case passes all six providers × seven variants plus profile/workspace controls.
- Validation qualification: The original post-fix one-PASS/one-fixture-exception result01b957 remains FAIL. The fixture initializer inserted three arrays at variant2; exact correction47dea679 changes only that construction, preserving assertions, expected IDs, timing, the first test body and production. The corrected previously failed case alone was rerun; there is no same-binary two-case, full-suite or global PASS claim. Native iteration temporarily uses packageOFF after a stale-helper configuration block; all nine features/tests remain enabled and no new shipping helper/desktop is claimed.
- Residual risk: Actual Windows picker/target scope equivalence and the native `Normal` registration failure’s cause remain unknown; this source fix does not establish their cause or a successful remote child. Related runtime Owner `chat-1791755529536-1b146e` and contact/no-ACK gates remain, exact current UAM root is unknown, and Un-Reviewed/Not User Tested remain. No David acceptance or Complete status.
- Evidence: Qualified focused results (private evidence retained locally), fixture-only correction (private evidence retained locally), original intended REDs (private evidence retained locally).

## Internal continuation input could become a user-history row — 2026-10-21

- Status: Source-corrected with focused core/native-handler/CEF verification; matching helper/desktop delivery, actual provider export/hydration and ended-parent live acceptance remain pending.
- Symptom and root cause: If Codex exported UAM’s internal continuation input as ordinary user history, native decoders/reconciliation could publish it as an authored User row; the CEF manual-refresh snapshot omitted the proof required to retain the typed association. Result-delivery DATA also needed explicit preservation across history overlay. No actual human-history loss or automatic Unsent/replay occurrence was established.
- Data flow reviewed: `owned internal native input/ACK → bounded durable receipt DATA → local/remote native history decode → guarded import/reconciliation or CEF manual refresh → saved user rows/result DATA`.
- Fix: Carry and validate typed owned continuation-history proof through decoder/publication and CEF snapshot boundaries, preserve result-delivery DATA, and retain human-authored lookalike text and existing ownership/recovery gates; no text-prefix-only execution authority is introduced.
- Regression: `CodexOwnedParentResultImportPreservesHumanRowsAndDeliveryData` reached the intended users-equality RED, then passes within core63. The first real public CEF test separately passed saved-DATA equality before failing user classification; both final public CEF cases pass after the reviewed CEF-only snapshot fix. Cumulative14-arm and bounded-capacity controls remain in the focused core scope.
- Validation qualification: Qualified73 unique passes combine63 core and eight original handler cases before the CEF-only fix with two CEF cases afterward (0.674s); unchanged other production/tests are joined, not a claim that all73 ran on one binary or that the full native suite passed. Core3299 was listed only. Prior semantic/fixture failures and historical aggregate78b6 FAIL remain immutable; all29 previously observed fake processes retired. Temporary native packageOFF/all nine features+testsON does not establish new helper/desktop shipping.
- Residual risk: No current installed-app/live provider-history result, Windows-wide acceptance, automatic ended-turn live wake, actual loss/replay cause or David approval is claimed. Exact current UAM root identity remains unknown; existing runtime Owner `chat-1791755529536-1b146e`, contact/no-ACK, Un-Reviewed and Not User Tested gates remain.
- Evidence: Final qualified focused results (private evidence retained locally), prior71 and real CEF RED (private evidence retained locally), reviewed CEF fix application (private evidence retained locally).

## First advertised model authority could be displaced — 2026-10-21

- Status: Source-corrected with six focused canonical native cases passing; broader verification, matching desktop delivery and live acceptance remain pending.
- Symptom and root cause: The ACP config parser overwrote current-model projection for every model option, while model-choice resolution used the first valid advertised model option.
- Data flow reviewed: `provider configOptions → ACP option parser/current model → serialized session projection`; existing mode and provider-specific options are retained.
- Fix: Keep the first valid model option authoritative; an empty first current value retains the prior current-model projection and later model options cannot replace it.
- Regression: `OpenCodeConfigModelProjectionUsesFirstAuthoritativeOption` covers populated and empty first-current values, invalid option IDs, later conflicting model options and mode/custom-option preservation.
- Validation: Two new and four connected canonical native cases pass once in0.27s; inventory3301 retains3299 prior names plus the exact two new cases with none removed; private RED c59b1fe4 and private GREEN e9b0eb99 remain separate evidence. The earlier canonical build failure7f39 occurred before compilation because of a stale helper contract and remains historical, not a semantic regression outcome.
- Residual risk: No full-suite, installed-app, actual Windows/Qwen incident-cause or successful live child result is claimed. Installed137/live HOLD and immutable frozen138 remain unchanged. Exact current root UAM identity is unknown and Owner remains Unassigned; existing actual owner scopes/contact history, Un-Reviewed and Not User Tested remain, with no ownership transfer, David acceptance or Complete.
- Evidence: Exact four-file integration (private evidence retained locally), scope decision (private evidence retained locally), private results (private evidence retained locally), canonical six-case proof (private evidence retained locally).

## Malformed Antigravity steps could overwrite valid evidence — 2026-10-21

- Status: Source-corrected with the same six focused canonical native cases passing; broader verification, matching desktop delivery and live acceptance remain pending.
- Symptom and root cause: Counter coercion mapped malformed native step indices to zero when constructing tool and child identities, allowing malformed events to replace valid step-zero evidence.
- Data flow reviewed: `native step payload → validated signed-range step index → tool/child identity → session tool projection`; usage-counter processing is separate.
- Fix: Require a present nonnegative integer within signed64 range before creating tool or child identity, preserving genuine step zero and other valid indices.
- Regression: `AntigravityMalformedStepIndexCannotReplaceZeroIndexedToolEvidence` checks malformed index refusal and preservation of valid zero-indexed tool/child evidence; the connected documented-tool and validated-child controls remain covered.
- Validation: The pinned canonical six-case run passes once in0.27s and includes this new case, the model-precedence case and four connected controls; inventory3301 is a compiled inventory, not3301 executed tests. Private RED c59b1fe4 and private GREEN e9b0eb99 remain separate; later loop arms were not independently RED. Earlier canonical build failure7f39 before compilation remains historical.
- Residual risk: No actual live collision, full-suite/global PASS, installed-app fix or provider incident cause is established. Installed137/live HOLD and immutable frozen138 remain unchanged. Exact current root UAM identity is unknown and Owner remains Unassigned; existing actual owner scopes/contact history, Un-Reviewed and Not User Tested remain, with no ownership transfer, David acceptance or Complete.
- Evidence: Exact four-file integration (private evidence retained locally), scope decision (private evidence retained locally), private results (private evidence retained locally), canonical six-case proof (private evidence retained locally).

## OpenCode-excluded runner retained native entry points — 2026-10-21

- Status: Source-corrected with genuine matching helper builds and four focused canonical all-ON cases passing; refreshed desktop delivery, wider artifact exclusion and user acceptance remain pending.
- Symptom and root cause: An OpenCode-OFF runner still advertised `openCodeNativeControl` and `openCodeTerminalObservation` and dispatched `opencode.native` and `opencode.terminal.observe` because the hello advertisement and shared request dispatcher lacked the provider build-flag guard.
- Data flow reviewed: `OpenCode build exclusion → runner hello capabilities → shared native request dispatcher → existing private-process or endpoint validation`.
- Fix: Gate the two capability keys and reject both request routes with the existing unsupported-request result when OpenCode is compiled out; enabled-provider behavior and existing authority checks remain intact.
- Regression: `RemoteRunnerOpenCodeNativeSurfacesRespectBuildExclusion`; genuine private baseline OFF compilation across22 runner units exposed both keys, corrected private OFF omitted them/refused both routes, and the separate22-unit ON control preserved advertisement/validation. No measured HTTP launch counter or complete excluded-content proof is claimed.
- Validation: All four genuine c75ff115/protocol3 helpers were built and published with28 actual file pins joined. One canonical core-only build passed in31.673s, inventory3302 retained3301 names plus exactly this new case, and the new case plus three connected controls passed once4/4 in0.36s;3298 cases were unrun. Root independently joined19 references/internal consistency. Release/all nine feature flags/tests/packageON/authentic139 remain, with no counter allocation; inherited SDK qualification is not a fresh SDK-content audit.
- Residual risk: Full excluded-content removal remains Partial, including retained implementation/content beyond these two boundaries. Existing desktop139 remains unchanged and all-ON; this is source and helper-build verification, with no new desktop shipment, installed/remote helper deployment, saved runtime-availability bypass or provider-live fix acceptance established. Historical aggregate78b6/global FAIL and cold Claude owned-image Retry RED9f6cd09d remain distinct. The original default-metadata failure0b2b and rootdf5d content qualification remain; default timestamps are not claimed unchanged, while frozen139 remains exact. Exact root UAM identity is UNKNOWN/OwnerUnassigned; existing Owner/contact, Un-Reviewed and Not User Tested flags remain, with no David approval or Complete.
- Evidence: Source integration (private evidence retained locally), [private OFF RED](Builds/observer-off-verification-r1/baseline-off-red-receipt-r1.json), [private OFF GREEN/separate ON control](Builds/observer-off-verification-r1/private-off-green-on-control-receipt-r1.json), genuine helper handback (private evidence retained locally), canonical exact-four (private evidence retained locally), root independent join (private evidence retained locally).

## Incomplete workspace journals blocked Send — 2026-10-22

- Status: Source-corrected with94 connected native cases passing and installed147/About verified; affected history remains loading202s, actual Send restoration and David acceptance remain pending.
- Root cause: An interrupted pre-artifact journal with empty branch/base was retained after preparation failed, then rejected on every later opening; automatic worktree creation also required a source commit, and optional pull refused dirty or in-progress sources.
- Fix: Resume only the uniquely validated, uncreated owned journal; prepare a verified unborn source through an enabled UAM-owned baseline without committing into the original; preserve configured bases and skip optional pull with a warning for dirty or in-progress sources.
- Regression: Eight intended assertion failures were demonstrated before their corrections, with the occupied-target control passing; final verification passes nine focused cases and 85 existing connected ownership/save/recovery/base/checkpoint/port cases, plus docs integrity. All original source/authored-input guards remain; the 3,402-name inventory is not a full-suite execution.
- Limits: Other unsafe or uncertain Git states can still refuse preparation, real ownership conflicts are not bypassed, existing held input is not replayed, and installed147 retains that input while a separate history-loading defect is investigated. No stable/all-provider/user acceptance is claimed.
- Evidence: Final native result (private evidence retained locally), receipt/unborn baseline (private evidence retained locally), warning baseline (private evidence retained locally), configured-base baseline (private evidence retained locally).

## Missing history falsely animated — 2026-10-22

- Status: Source-corrected;660 connected,2827 full frontend unit and23 performance checks pass with no skipped cases; Claude R2 source review resolves three P2 findings, and installed148 shows the exact affected retained input without a settled loading skeleton or history notice; sending and user acceptance remain pending.
- Symptom and source defect: The affected147 chat remained on a loading skeleton202 seconds after startup; the renderer equated missing rows plus a saved count with active work, and an unanswered native history callback had no deadline. The actual missing-page cause is still unknown.
- Fix: Track a current request and chat birth for selection/history preparation, end a silent stage after30 seconds in a retryable history failure, reject late/superseded ownership and keep retained input visible without a replacement skeleton; true empty pages remain empty and incomplete history still suppresses workspace reviews.
- Regression: Real deferred CEF selection/read, silent callbacks, same-ID replacement, supersession, late success, search loading, queue-only retained input and workspace-review guards pass; failed baselines, prerequisite corrections and initial review findings remain preserved separately.
- Limits: No provider prompt, automatic retry expansion, queue/recovery permission, native protocol or saved-input mutation; installed148 verifies only the retained-input display, with no restored Send/provider acceptance, independently proven backend-read success or underlying missing-page cause claimed, four bounded-review P3 qualifications and historical broad-native failures remain open.
- Evidence: Exact source and verification (private evidence retained locally), qualified Claude R2 (private evidence retained locally), original installed147 state (private evidence retained locally).

Alpha148 installed display checkpoint: actual preserved receipts (private evidence retained locally) record About148/Back, retained251B labelled Delivery not confirmed, no settled skeleton/history notice, zero history/input Retry or Send, all ten intent guards and raw5,238 files/journal conserved, and147 rollback. The initial148 unavailable notice and original147202s failure remain immutable; no native delivery, stable release or David acceptance is inferred.

## Retained input recovery was unreachable — 2026-10-22

- Status: Source correction verified; packaging, installed recovery and user acceptance pending.
- Found from: David’s Alpha148 report in the original Email Client chat.
- Symptom: The retained input was visible but its receipt permanently disabled Retry and native Remove refused an unloaded referenced row.
- Root cause: Receipt controls had no explicit native Retry route, and Remove required full native history without preparing it from the displayed cold receipt.
- Data flow reviewed: saved input and captured policy → cold native receipt → serializer/store → message actions → owned history preparation → native recovery and save.
- Fix: Exact receipt actions prepare saved history asynchronously, recheck ownership and revisions, then use existing native known-unsent Retry or guarded Remove; failures retain the input and propagate their reason.
- Regression: Seven intended UI assertions failed with three negative controls passing; four real-handler Remove cases failed at the absent read prerequisite before the correction.
- Validation: Six cold-handler, two native Retry,16 existing connected,2837 frontend unit and23 performance checks, UI build and docs pass; the real handler also accepts both actions on separate private copies of the exact saved input without provider execution or mutation of the originals.
- Residual risk: Claude R3 finds no unresolved P0/P1/P2; qualified P3 notes, historical broad native failures, packaging, the affected installed recovery journey and David acceptance remain pending; no automatic resend, deletion or delivery inference.

## Alpha149 snapshot and retained recovery follow-up

- Reported failure: an ignored Swift dependency path blocked workspace snapshot preparation; Alpha149 also displayed the retained row as Sending after hydration while the runtime was stopped. The exact historical symlink target and exclusive display cause remain unproven.
- Source correction: select unborn-repository files with Git ignore rules, preserve contained relative symbolic-link nodes, retain nested current source, skip staged deletions, validate listed ancestors, and traverse relative ancestry so equivalent source-root spellings remain usable.
- Recovery correction: preserve the workspace wrapper’s held settlement instead of scheduling an unreachable reconnect; inactive retained rows remain unconfirmed, exact native references share one presentation, and explicitly withheld native Retry stays disabled. No automatic replay, delivery confirmation or cancellation authority is added.
- Verification: earlier intended regression failures and intermediate native89/full2841 results are retained in `<private working evidence>`; current combined native/UI verification and final Claude source review are pending. Alpha149 remains installed; original input is untouched. Packaging, real affected-chat recovery and David acceptance are not established.

- Final source verification follow-up: 112 unchanged connected native cases passed; the corrected captured-cancellation regression separately passed after a fixture-only prerequisite correction, with fresh3415 inventory and docs. Current2841 unit tests,23 performance checks and production UI build passed; scoped Claude R4 found no P0/P1/P2, with recorded P3 residuals. This is source verification, not an installed affected-send or user acceptance result.


Alpha150 follow-up, 2026-10-22 — In progress, not fixed: David’s affected Email Client worktree is created but local launch rejects ownership before process creation and the unconfirmed retried row disappears; the actual installed failure and focused/main-app-only iteration scope are retained in incident evidence (private evidence retained locally), with original input actions forbidden and user acceptance pending.

Local Alpha150 test-patch source, 2026-10-22 — The local-launch fingerprint now ignores background runner-health observations while retaining host connection/configuration ownership; native known-unsent direct receipts remain serialized with their exact raw-row reference during frontend history loading, with native Retry guards unchanged. Three focused native regressions are under verification; this is not installed acceptance, and the failed fixture that selected the real Codex executable is retained separately without a user-prompt submission claim.

David confirmation, 2026-10-22 — “it worked.” records successful manual affected Email Client send on the installed same-version150 Mac patch5834b5c2 after eight focused native and83 outgoing UI checks passed; the reported launch blocker is resolved for this observed workflow, while broader recovery/Remove UX review, other-provider acceptance and full-release approval remain pending. Scoped confirmation (private evidence retained locally).

## Managed child transcript refresh and recovery — 2026-10-23

- Status: Fixed locally and validated; installed verification and David acceptance pending.
- Found from: David’s report that child conversations needed manual Refresh.
- Root cause: The open inspector did not subscribe to owning-root revisions, and a read error disabled recurring reads; retained status and queued initial-read closures also misrepresented refresh state.
- Data flow reviewed: root managed revision → scoped transcript read → validated snapshot and listing → shared transcript renderer.
- Fix: Coalesce owning-root catch-up reads, retry failed reads at capped backoff, preserve prior content with last-reported status and keep post-publication catch-up in the background.
- Regression: Three missing-wakeup and seven missing-recovery assertions failed before their corrections, as did the retained-status, initial-loading and queued-publication controls; original fixtures and explicit policy corrections remain in the evidence.
- Validation: Managed64/64, full frontend2856/2856, performance23/23, production UI build and docs pass; Claude R3 reports no remaining scoped findings.
- Residual risk: No read event grants follow-up authority or resubmits input; passive-history behaviour, real-provider/live acceptance and the separate historical lifetime failures remain qualified.

## Managed delegation inherited permission policy — 2026-10-23

- Status: Fixed locally and validated; installation and provider acceptance pending.
- Found from: David’s report that delegation required YOLO, followed by a confirmed captured-terminal read-ceiling review finding.
- Root cause: Two generic mutation prefilters denied delegation before its dedicated admission checks, while scheduler admission omitted the validated terminal’s captured read-only floor.
- Data flow reviewed: validated public capability → delegate admission → scheduler permission ceilings → persisted child policy and read projection.
- Fix: Exempt only delegation from those generic prefilters, pass a narrowing-only captured read ceiling into scheduling, and report delegation policy separately from mutation permission.
- Regression: Actual public delegation and terminal-ceiling failures were reproduced; the canonical acceptEdits migration and old saved-definition denial fixtures were explicitly corrected without dropping cancellation, depth, ownership or read-ceiling checks.
- Validation: Six public and nine connected native cases pass after a fresh3424-case inventory; Claude R2/R3 find no remaining scoped P0–P2 issue.
- Residual risk: Child command approvals, Plan/read ceilings, goals, cancellation and Computer Use retain their guards; process-free tests do not establish all-provider execution or repair the excluded R37/lifetime/C16 gaps.

## Managed child draft capacity eviction — 2026-10-23

- Status: Fixed locally and validated; packaging, installed verification and David acceptance pending.
- Found from: A scoped source finding followed by the real child-editor capacity regression.
- Root cause: The 32-entry draft cache treated every unmounted idle draft without an unresolved request as recyclable, including authored text and refusal or validation feedback.
- Data flow reviewed: child editor input and feedback → scoped retained draft entry → capacity admission → same-child reopen.
- Fix: Recycle only empty settled entries; preserve the existing capacity, mounted-owner and unresolved-request protections.
- Regression: The old source lost authored bytes under 33 child-route visits while empty-draft recycling passed; both controls and all original managed cases pass after the correction.
- Validation: Managed66/66, full frontend2858/2858, performance23/23, UI build and docs pass; Claude R4 found no scoped P0–P3 issue.
- Residual risk: This is current-application-lifetime retention, not desktop-reload persistence; full protected capacity can truthfully refuse a new editor and never authorizes replay or discarding unknown requests.

## CLI warning, retained announcements and rail focus — 2026-10-23

- Cause and correction: a cached structured warning remained visible in CLI View; popup suppression also suppressed retained-notice announcements; pointer exit reset a focused pending rail target, and its preview lost vertical centring when animation finished. Narrow render, announcement-input, focus and CSS corrections retain configuration, notification placement and navigation authority.
- Verification: actual focused baseline78PASS/8FAIL becomes86/86PASS; all six real-store delivery-fact controls passed before and after, so no delivery-consumer change is justified. A browser using production preview CSS reproduces a38.195px completion jump before correction and0px afterward. Evidence: <private working evidence> (private evidence retained locally). One post-fix command used an incorrect npm prefix from the existing UI cwd and exited254 before test execution; that harness failure remains distinct from the corrected86PASS run.
- Review follow-up: Claude R5 found no P0-P2 and identified stale evidence on return to Chat; two pending-reply assertions genuinely fail before clearing the old warning on CLI entry. The rail agreement now explicitly preserves existing pointer-hover target changes, and the delivery test header reflects its executed consumer scope.
- Final verification: current focused86/86 and full2874/2874 unit tests pass with zero skips, performance23/23 and production UI build pass, and docs integrity passes188pages/959links/six26-capability checklists. Claude R6 finds no new scoped P0-P3 and closes R5 follow-ups; source pins match. Native15/inventory3424 are reused only after exact native source/binary equality, with no full-native claim.
- Bounds: standalone browser geometry is not real CEF or assistive-technology acceptance. The installed Alpha150 patch is unchanged, delivery-indicator latency remains unverified, and existing owner, review and David acceptance flags remain.

## Native Codex activity used managed-style cards — 2026-10-23

- Cause and requested correction: saved/live native collaboration calls took the blanket subagent-group route before ordinary tool rendering. Exact native collaboration metadata now selects compact agent-icon tool entries, original tool details and disclosed compact child links; accepted managed launches and results keep full cards and existing Open/Stop authority.
- Verification: initial26 cases give18 intended assertion failures and8 child-opening controls PASS; an added no-full-card assertion in those eight existing disclosure cases gives8 genuine failures before correction. All58 connected inline-subagent cases pass after the two-source correction. A browser check uses captured real React DOM and packaged production CSS; a first app-shell wrapper error is retained as fixture-only, and the corrected wrapper shows the compact row, icon and closed child disclosure.
- Review follow-up: Claude R7 found missing accessible status/time on compact child buttons; eight existing disclosed-child cases reproduce that P2 before unique described status/time IDs restore it. The corrected137 connected cases pass, followed by full2900/2900 unit tests with zero skips, performance23/23, production UI build and documentation checks; Claude R8 finds no scoped P0-P3. Original failures and review-time limits remain distinct.
- Bounds: no provider/native/API behavior, input or delivery change; generic native metadata and managed controls remain. No installed/CEF/user acceptance claim. Evidence remains under <private working evidence> (private evidence retained locally).

## Passive summary viewport continuity — 2026-10-23

- Status: Fixed in canonical source and source-reviewed; main-app packaging, installed/CEF/Windows/live-provider verification and David acceptance pending.
- Cause: Summary freshness remounted or reset the pane, hydration lacked positive accepted-page evidence, shared-range replacement revoked local read evidence, and held refresh/reopen scrolls could overwrite the reader’s newer intent.
- Fix: Separate stable display visits from strict read generations, mint bounded-page evidence only at guarded adoption, retain local ranges across other readers, and preserve actual latest user intent through asynchronous refresh/restoration while skipping initial bottom writes over saved visits.
- Verification: Original off-tail/shared-reader/held-scroll and asynchronous reopen assertion failures precede the corrections; current116 focused and2935 full UI cases,23 performance checks, app typecheck and isolated UI build pass with zero skips, and Claude R3 finds no scoped P0–P2. Earlier collection/harness errors, the excluded invalid cold-search fixture and intermediate regressions remain in evidence.
- Bounds: No input, provider, runtime, search, selection or persisted authority added; P3 concurrent-publication/explicit-recovery/manual-overlap and future-caller validation limits remain. Existing feature agreement is restored without cosmetic documentation changes. Evidence: <private working evidence> (private evidence retained locally).

- Packaging follow-up: same-version Alpha150 R5 main app builds in30.16s, passes strict default/frozen signatures and retains2348 bundle entries, genuine local helpers, historical foreign helpers, counter150 and the installed patch. Fresh exact-path CUA binding then returns `cgWindowNotFound`; no current window is established, no Quit/staging/replacement is attempted, and installed/CEF/David acceptance remains pending. Actual build (private evidence retained locally).


### 2026-10-23 — Timing-card test parent prerequisite

The existing timing-card regression omitted a saved parent and an explicit provider, so it failed before the intended result/timing route. The test now creates a valid Codex parent through the existing domain constructor, saves and exactly reloads it, then retains every original delivery and timing assertion. Original failure, intermediate prerequisite failures and key-only diagnosis remain recorded; the final private 3424-name build passes the selected case and two connected DATA controls. This is test-only coverage, not a production behavior change or a full-native/installed/provider acceptance claim. Feature agreements and provider capability rows are unchanged. The unconfirmed managed-deadline Stop defect and unresolved memory-save outcome contract remain separate.

Evidence: `<private working evidence>`.


## MAIN history hydration wakeup — 2026-10-23

- Status: Fixed in canonical source and source-reviewed; same-version main-app rebuild, installed/CEF/Windows and David acceptance pending.
- Symptom and cause: a restored visible pane can attempt history loading before its Session owner arrives; summary-only hydration keeps the same visible ID and the old effect does not wake again.
- Data flow reviewed: persisted pane → MAIN visible-owner effect → real store bounded history request → current request/birth/page adoption → transcript projection.
- Fix: compare finite owner births per visible ID before ordinary loading, preserving other panes' pending reads, the search skip and all native/store authority guards; metadata-only summary changes do not trigger another read.
- Regression and validation: one real mounted caller/store/fake-CEF baseline fails at expected-one/actual-zero history requests; corrected46 MAIN and2984 full unit tests pass with zero skips, performance23, app typecheck, production UI build and docs pass. The first corrected-source run reached adoption then exposed a test-only explicit-zero-offset expectation; normalized zero plus accepted-page and native-row index assertions preserve the intended oracle. All original tests remain.
- Review and bounds: Claude R2 finds no supported P0-P2; missing-timestamp native-import reachability and optional second-pane payload assertions remain P3. No app input, delivery, search, provider or persistence authority added. The conditional regression is not established as R9 startup causality; its later readable capture with zero Retry remains separate. Existing feature agreement is restored, so no cosmetic feature/provider/architecture edit is needed.

Evidence: <private working evidence> (private evidence retained locally).

- Installed follow-up: Alpha150 R10 exe729f71ab is independently installed/strict-signature verified with R9 rollback. Initial and19.806s settled history are readable without a history notice or Retry; ten current intent guards and raw5342 conserved. Optional About remains unobserved after a user-changed guard and fresh Usage capture, with no navigation retry. All message/provider actions remain zero, automation handed back; no incident-cause, full-native/platform/stable or user-acceptance claim. Qualified result (private evidence retained locally).
