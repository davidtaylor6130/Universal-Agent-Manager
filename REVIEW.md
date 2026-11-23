# V5.2.0 performance issue triage

Read-only triage of `uam/v5.2-sync-main` at `6f235eb9` (V5.1.0 merged into `uam/v5.2-performance`). The only open PR is #1030, the branch-sync PR, so no other work is in flight for these issues. Evidence comes from code, tests and PR bodies on that base; nothing was built or run for this triage. An acceptance criterion without code or test evidence counts as missing.

## Summary

None of the seven issues is fully closed out. #990, #958 and #948 are resolved in code; #986, #961, #947 and #356 are only partly resolved. All issues stay open by design until `uam/v5.2-performance` reaches the default branch, and David's acceptance is still pending on each one.

| Issue | Status | Evidence | What is missing |
|---|---|---|---|
| #990 first-word cold launch | Fully resolved | #1003; `src/app/runtime_activity.h:152`; test `tests/acp_session_tests.cpp:3554` covers selected (16 ms), background (250 ms) and held input (1000 ms) | The before/after (1,952→630 ms, with spread) is a combined measurement of several PRs together, recorded only in the PR text; the committed baseline wasn't updated. |
| #986 chat saves block the poll loop | Partly resolved | #1002 + fix `23f0da878`; SSH-host saves moved to the background writer (`src/common/runtime/acp/acp_session_lifecycle.cpp:5019`); remote output acknowledged only after a durable save (`src/common/runtime/acp/acp_polling.cpp:829`, test `tests/chat_save_stall_tests.cpp:657`, measurement `tests/chat_save_stall_tests.cpp:220`); read-only UAM-Control replies wait for the audit record (`src/app/uam_control_service.cpp:629,691`, tests `tests/uam_control_transport_tests.cpp:1965,1982`); the mutating-tool rollback is kept (`src/app/uam_control_service.cpp:1984`) | Mutating UAM-Control tools still save synchronously. `UserBoundary` (`src/app/managed_parent_continuation_service.cpp:33`) and the scheduler's whole-parent compare (`src/app/agent_run_scheduler.cpp:610`) are untouched. No new test proves exactly-once result delivery for these paths. The before/after (384.8→7.1 ms) is from an "earlier experiment" in the PR text. |
| #961 window waits for startup work | Partly resolved | #975 + #1004; animated sidebar skeleton (`UI-V2/src/components/layout/Sidebar.tsx:182`, shimmer in `UI-V2/src/index.css:2550`); turned-off providers skipped (`src/app/model_discovery_service.cpp:77`, `src/app/startup_native_import_service.cpp:268`, tests `tests/model_discovery_startup_tests.cpp:740` and `StartupNativeImportSkipsOpenCodeWhereItIsTurnedOff` in `tests/startup_native_import_tests.cpp`); startup ordering test `tests/chat_icon_handler_tests.cpp:6117`; parallel catalogue read test `tests/core_chat_repository_tests.cpp:3047`; relaunch window 6.65→0.25 s | First-launch figures are the faster of 2 runs, not a median of at least 3 with spread. The #975 relaunch table gives no spread. On first launch the chat list still takes 7.5 s. No test guards "no per-chat rebuild with valid caches". |
| #958 performance suite | Fully resolved (all acceptance criteria) | #962, #985, #1005; pieces merge into one file (`scripts/perf_suite.py:92`); report-only job adds the job summary and artifact (`.github/workflows/ci.yml:365,391`); baseline records machine, revision and commands (`tests/performance_baseline.json:3,12`); unmeasured metrics listed with reasons (`docs/reference/performance-suite.md:54`) | Outside the acceptance criteria: the `driven` piece doesn't run on hosted CI, and the baseline is still the pre-optimisation one (`fca49ea35`/`0420d8e73`). No test exercises the merge of two pieces into one file. |
| #948 DeliverRootResult re-reads the parent | Fully resolved | #974; one cached read per saved version, shared by waiting results (`src/app/agent_run_scheduler.cpp:464`); tests `tests/agent_result_delivery_regression_tests.cpp:273,348,390` (exactly-once after restart included); 23.6→0.019 ms median | Still a full read and parse on the poll thread about every 2 s while a parent streams. It is cached, not incremental, which the issue's Expected allows. |
| #947 patches resend catalogues | Partly resolved | #1000; chats reference catalogues by id (`src/cef/state_serializer.cpp:2535`), each catalogue sent once (`src/cef/cef_push.cpp:242`); UI store tests in `UI-V2/src/store/useAppStore.test.ts` and companion reset test `DesktopStatePullResetsSentModelCataloguesAndPhonePullDoesNot` in `tests/companion_connection_handler_tests.cpp`; size case `tests/interaction_performance_tests.cpp:1135` | No before/after bytes or time on the same fixture. The regression guard only checks a stopped chat's patch (`tests/interaction_performance_tests.cpp:1175`), not a live ACP session's, which is the issue's case; the live patch is still 261 KB. Live catalogues are still rebuilt and hashed every patch. |
| #356 full-state fingerprint cost | Partly resolved | #724 (push tick 93/96→17/17 ms p50/p95), #825 (reuse of unchanged content); test `tests/interaction_performance_tests.cpp:937` | The Refresh and initial-load fingerprint path is still unmeasured, the live 643-chat cost was never measured, and that path has no regression check. #1004 didn't change this. |

## Per-issue notes

### #990 — selected cold launch waits between startup stages

- Criterion 1 (selected owned cold launches poll interactively while the turn has blocking work): met. `NeedsInteractiveRuntimePolling` now also counts `pending_local_launch != nullptr` (`src/app/runtime_activity.h:152`).
- Criterion 2 (background launches and held input without ownership keep bounded cadence): met and tested in `PollCadenceAdvancesSelectedColdLaunchAtInteractiveCadence` (`tests/acp_session_tests.cpp:3554`).
- Criterion 3 (compare fake-provider first word on the same fixture with spread and combined-candidate limits): reported in the #1003 body only (1,952.30 ms [1,906.35–1,967.15] → 630.20 ms [603.90–653.80], combined candidate). `tests/performance_baseline.json` still holds the pre-change `first_word_ms` values.

### #986 — remote and control-tool chat saves block the poll loop

Sites listed in the issue:

- `SaveChatSoon` on SSH hosts: fixed; `BackgroundChatSaveCovers` no longer requires a local host (`src/common/runtime/acp/acp_session_lifecycle.cpp:5019`).
- Remote output acknowledgement: fixed; acknowledgement waits for a background save that holds the cursors (`src/common/runtime/acp/acp_polling.cpp:829`).
- UAM-Control tool calls: fixed only for read-only tools (`agent_status`, `agent_list`, `goal_get`, `skill_list`, `skill_read`; `src/app/uam_control_service.cpp:629`). Mutating tools still copy the root chat and save synchronously (`src/app/uam_control_service.cpp:1984`).
- `UserBoundary` serialise-and-hash: not addressed (`src/app/managed_parent_continuation_service.cpp:33`).
- Scheduler whole-parent compare: not addressed (`src/app/agent_run_scheduler.cpp:610`).

Acceptance criteria:

- Remote batch acknowledged only after a durable write: met, `RemoteOutputAckFollowsDurableSaveOffPoll` (`tests/chat_save_stall_tests.cpp:657`).
- UAM-Control reply only after a durable audit; mutating tool keeps rollback: met, `UamControlReadOnlyReplyFollowsDurableAuditOffPoll` (`tests/uam_control_transport_tests.cpp:1982`); rollback path unchanged for mutating tools.
- Exactly-once result delivery unchanged: no new test targets it for these paths; relies on the existing suite.
- Same workload measured before and after on the same machine: measurement cases exist (`tests/chat_save_stall_tests.cpp:220`, `tests/uam_control_transport_tests.cpp:1965`), but the #1002 figure (384.8→7.1 ms) is described as an earlier synthetic experiment.

### #961 — window waits for all startup state work

- Relaunch with valid caches no longer spends seconds before the window: met (#975, 6.65→0.25 s window shown, median of 8).
- Window before slow work with an animated loading state: met (`UI-V2/src/components/layout/Sidebar.tsx:182`, `UI-V2/src/index.css:2550`; reduced-motion disables the shimmer).
- First-launch summary cache rebuild does not block the window: met for the window (0.40 s), but the chat list is ready only at 7.5 s.
- History discovery skipped for turned-off providers: met for model discovery and OpenCode native import (`src/app/model_discovery_service.cpp:77`, `src/app/startup_native_import_service.cpp:268`).
- Pre-window and to-usable times, median of at least three with spread: not met for first launch (faster of 2 runs) and no spread in the #975 relaunch table. #1004 gives relaunch 1,284 ms [1,223–1,286] → 656 ms [639–663] as a combined candidate.

### #958 — in-repo performance suite with a recorded baseline

- Each piece reruns alone and merges into one file: `merge_out` (`scripts/perf_suite.py:92`); CI writes native and app pieces into `Builds/perf.json`.
- PRs into `uam/v5.2-performance` get a table as job summary and artifact without a failing gate: `Performance report` job with `continue-on-error` steps (`.github/workflows/ci.yml:365,391`).
- Recorded baseline with machine, revision and commands: `tests/performance_baseline.json:3,12`.
- Unmeasured metrics listed with reason: token rows (`docs/reference/performance-suite.md:54`, `REFERENCE_NOTE` in `scripts/perf_suite.py`).
- Hot-spot component figures: state push on 2,000 chats via `PerformanceIdleInstallPushTick`; slow poll stages and slow `statePatch` serialization parsed from the app log (`slow_poll_metrics` in `scripts/perf_suite.py`).

### #948 — DeliverRootResult re-reads a streaming parent

- Cached work keyed by storage revision and a 2 s retry interval, shared across waiting runs (`src/app/agent_run_scheduler.cpp:464`, cache field in `src/common/state/app_state.h`).
- Exactly-once regression tests: `tests/agent_result_delivery_regression_tests.cpp:273,348,390`.
- Remaining cost: a streaming parent's file is still read and parsed on the poll thread at most about every 2 s.

### #947 — state patches resend full model catalogues

- Chats carry `modelCatalogId`; catalogues travel in a `modelCatalogs` table sent only when the page lacks the id (`src/cef/state_serializer.cpp:2535`, `src/cef/cef_push.cpp:242`).
- Frontend store resolves references and rejects unknown ids (`UI-V2/src/store/cpp/sanitizers.ts`, `UI-V2/src/store/useAppStore.test.ts`).
- Missing: same-fixture before/after; an assertion that a live chat's patch omits an already-sent catalogue; live catalogues are still serialized and digested on every patch.

### #356 — full-state fingerprint cost for large chat catalogues

- Done earlier: #724 (unchanged push tick at 5,000 chats 93.1/96.2 → 17.0/17.4 ms p50/p95), #825 (reuse unchanged transcripts, tool calls and catalogues).
- Existing check: `PerformanceLargeCatalogStateFingerprint` (`tests/interaction_performance_tests.cpp:937`).
- Missing, as the 2026-11-16 issue comment says: Refresh and initial-load path unmeasured, no live 643-chat measurement, no regression check for that path.

## Security

No security weaknesses were in scope or found in this triage.
