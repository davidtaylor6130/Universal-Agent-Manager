# Audit Progress

Tracks every file/area examined for the global improvement report (`GLOBAL_IMPROVEMENT_REPORT.md`).
Status: ✅ fully read · 🔶 partially read / skimmed · ⬜ queued · ➖ deliberately skipped

## Phase 1 — OpenCode provider + shared provider infrastructure

| File | Status | Notes |
| --- | --- | --- |
| `src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp` | ✅ | 160 lines. Issues: dead command-template merge, duplicated goal lookup, files passed twice (`--file` + prompt text), batch path ignores model id |
| `src/common/provider/opencode/cli/opencode_cli_provider_runtime.h` | ✅ | Standard runtime interface impl |
| `src/common/provider/provider_ids.h` | ✅ | Clean. Alias table + normalize helpers |
| `src/common/provider/provider_runtime.h` | ✅ | IProviderRuntime: 8 profile-ignoring trait virtuals = boilerplate driver |
| `src/common/provider/runtime/provider_runtime_internal.h` | ✅ | 370 lines of shared helpers; `BuildCommandFromTemplate` usage to verify |
| `src/common/provider/runtime/provider_runtime_registry.cpp` | ✅ | UnsupportedProviderRuntime stub; per-flag #if dispatch |
| `src/common/provider/runtime/provider_build_config.h` | ✅ | Duplicates command templates from provider_profile.cpp (2 sources of truth) |
| `src/common/provider/provider_profile.h` | ✅ | ProviderProfile struct + store |
| `src/common/provider/provider_profile.cpp` | ✅ | Built-in profiles; `--session {resume}` dangling-flag latent bug |
| `src/common/provider/claude/cli/claude_cli_provider_runtime.cpp` | ✅ | Duplicates goal lookup; no ProviderRecognizesSubagentTool override despite Claude's Task tool |
| `src/common/provider/copilot/cli/copilot_cli_provider_runtime.cpp` | ✅ | No goal support in BuildPrompt (inconsistent with claude/opencode) |
| `src/common/provider/gemini/cli/gemini_cli_provider_runtime.cpp` | ✅ | Uses live template path (dead in prod); RebuildNativeSessionFile; no model flag in interactive argv (OC-3) |
| `src/common/provider/codex/cli/codex_cli_provider_runtime.cpp` | ✅ | CX-1 double-file append in dead path; 3rd goal-loop copy |
| `src/common/provider/runtime/provider_runtime_facade.cpp` | ✅ | Thin dispatch; BuildCommand/BuildPrompt facade has no prod callers (PR-5) |
| `src/common/provider/provider_profile_constants.h` | ✅ | Clean constants |
| `src/common/provider/codex/cli/codex_session_index.h` | 🔶 | Skimmed top 60; UUID/index helpers look clean; inline-header pattern |
| `src/common/provider/codex/cli/codex_thread_id.h` | ✅ | Clean |
| `src/common/provider/codex/codex_options.h` | ✅ | Clean allowed-option helpers |
| `src/common/provider/gemini/base/gemini_history_loader.cpp` | ⬜ | queued (Phase 2, with history sync) |
| `src/app/provider_worker_command.h` | ✅ | LIVE batch builder; provider if-chain bypasses IProviderRuntime (PR-6); 230-line inline header |
| `tests/core_tests.cpp:1340-1370,4940-5140,6260-6280,11455-11465` | 🔶 | Template/BuildCommand tests; drift PR-1 confirmed at :1360 |

## Phase 2 — OpenCode integration surface (C++)

| File | Status | Notes |
| --- | --- | --- |
| `src/common/runtime/acp/acp_session_runtime.cpp` (5,564 lines) | 🔶 | Full function outline extracted; deep-read: launch argv (783-852), session setup (1503-1693), startup model (1624), set mode/model (5116-5202), sub-agent detection (2626-2663). Issues RT-3, RT-4, RT-5 |
| `src/common/runtime/provider_cli_compatibility_service.cpp` | 🔶 | Read policy table (60-140); clean data-driven pattern — model for RT-3/PR-6 |
| `src/common/runtime/terminal_polling.h` | 🔶 | Deep-read opencode rebind block 240-455; RT-6 duplication; RT-8 header-only |
| `src/common/runtime/terminal/terminal_launch.h` | ✅ | RT-7 per-provider snapshot if-chain at 93-109 |
| `src/app/runtime_orchestration_services.cpp` | 🔶 | Read 540-740 (overlay index, legacy opencode normalize, claimed sessions); rest queued for Phase 4 outline |
| `src/app/provider_worker_command.h` | ✅ | (Phase 1) PR-6 |
| `src/cef/state_serializer.cpp` | 🔶 | Deep-read 237-475 zen/model catalog block + provider grep; RT-1 blocking curl, RT-2 layering |
| `src/cef/uam_query_handler.cpp` | 🔶 | DispatchAction route table 1072-1200 read; 73 actions; OnQuery error handling clean; rest in Phase 4 |
| `src/common/platform/platform_services_macos_impl.cpp` | 🔶 | opencode mention is a comment about Bun-based runtimes; full read deferred to Phase 4 |
| `src/common/config/frontend_actions.cpp` | 🔶 | Metadata only (groups/visibility), not dispatch |

## Phase 3 — Frontend provider handling

| File | Status | Notes |
| --- | --- | --- |
| `UI-V2/src/utils/providerMetadata.ts` | ✅ | FE-1 metadata duplication across C++/TS |
| `UI-V2/src/components/shared/ProviderLogo.tsx` | 🔶 | Grepped for colors; no per-provider brand palette |
| `UI-V2/src/store/useAppStore.ts` (6,156 lines) | 🔶 | Full structural outline (types→sanitizers→reconcile→push→86 actions); FE-2 fallback provider gap, FE-4 module-level buffers; deep line-by-line read not performed |
| `UI-V2/src/components/views/ChatView.tsx` (3,913 lines) | 🔶 | Full structural outline; FE-3 markdown renderer, FE-5 provider helpers; deep read of helpers 39-355 |
| `UI-V2/src/ipc/cefBridge.ts` | ➖ | 102 lines, currently modified in working tree — skipped to avoid auditing in-flight code |

## Phase 4 — Monolith decomposition analysis

| File | Status | Notes |
| --- | --- | --- |
| `tests/core_tests.cpp` (11,493 lines) | 🔶 | Template/BuildCommand regions read; harness not inspected — MO-5 tells agent to inspect first |
| `UI-V2/src/store/useAppStore.ts` (6,156 lines) | ✅ outline | MO-1 plan written |
| `src/common/runtime/acp/acp_session_runtime.cpp` (5,564 lines) | ✅ outline | MO-2 plan with measured cluster line ranges |
| `UI-V2/src/components/views/ChatView.tsx` (3,913 lines) | ✅ outline | MO-3 plan |
| `src/cef/uam_query_handler.cpp` (3,503 lines) | ✅ outline | MO-4 plan; 73-action route table |
| `src/app/runtime_orchestration_services.cpp` (2,121 lines) | 🔶 | MO-6 plan; regions 540-740 + grep map read |
| `src/common/platform/platform_services_windows_impl.cpp` (2,013 lines) | ➖ | Not read; MO-7 requires outline before work |
| `src/common/platform/platform_services_macos_impl.cpp` (1,829 lines) | ➖ | Not read (has in-flight changes); MO-7 |

## Not audited (future passes)

- `src/app/memory_service.cpp` (1,457), `memory_library_service.cpp` (699), `goal_service.cpp` (567), `vcs_commit_service.cpp` (936), `git_worktree_service.cpp` (508), `chat_domain_service.cpp` (661), `chat_lifecycle_service.cpp` (545), `native_session_link_service.cpp` (513), `persistence_coordinator.cpp`, `chat_repository.cpp` (1,343), `settings_store.cpp` (574), `json_runtime.h` (850), `gemini_history_loader.cpp` (319), CEF client/app/security files, `SettingsModal.tsx` (1,645), `MemoryLibraryModal.tsx` (801), `FolderTree.tsx` (796), `CLIView.tsx` (398).
- These were grepped for provider references where relevant but not line-audited. A follow-up audit wave should cover the persistence + memory cluster next (highest data-loss risk).

## Implementation status (2026-06-14)

Verified after each change: `ctest` (C++) and `npm --prefix UI-V2 run test && run build` (frontend) all green.

Completed:
- **RT-1 / RT-2** — all model-catalog file/network I/O removed from `state_serializer.cpp`; serializer now reads the in-memory `ProviderModelCatalogService` snapshot only (async refresh + mtime-cached config via the app polling loop). Guarded `Initialize` against a missing `opencode.json` (was a startup crash). Parse helper moved to the service; zen-model tests retargeted/driven through the service.
- **OC-3** — gemini interactive argv now appends `--model <id>` (trimmed) via the generic `BuildInteractiveArgv`; core test added.
- **RT-3** — structured-launch argv moved into each runtime via `IProviderRuntime::BuildStructuredLaunchArgv`; `BuildAcpLaunchArgv` is now a one-line registry call; dead approval-mode helpers removed; coverage test added for all five providers.
- **PR-5 (finish)** — the dead `BuildCommand`/template pipeline had been removed from production but left the test binary uncompilable. Removed all dangling test references; deleted the dead `command_template` builder field + per-profile initializers and the unused legacy settings key.
- **PR-6 (regression fix)** — worker argv had stopped trimming the model id when the if-chain moved into runtimes; restored `AppendTrimmedOptionValue` in all five `BuildWorkerArgv`.
- **RT-6 (regression fix)** — `TryAttachLocalHistorySessionFromChats` / `TryAttachOpenCodeLocalHistorySessionFromChatFile` were missing their trailing `return true;` after a successful rebind (UB / `-Wreturn-type`); fixed.
- **OC-6 (finish)** — sidebar normalization now stamps blank-provider legacy chats to `opencode-cli` (the overlay no longer carried it once `LoadHistory` stopped stamping); stale runtime-load test updated to the new contract.
- **FE-3** — markdown renderer extracted to `components/markdown/Markdown.tsx` + `markdownParsing.ts`, with dedicated unit/component tests; ChatView imports `MarkdownContent`.
- **FE-4** — push buffers/timers moved to `store/push/pushBuffers.ts` with `resetPushBuffersForTests()`.
- **FE-5** — `is{Codex,Claude,Copilot,OpenCode}Provider` helpers moved to `providerMetadata.ts`.
- **DOC-1** — CLAUDE.md rewritten: five providers, full `src/app` service tree, complete 64-action IPC table, accurate CMake flags + provider commands.
- **PR-7** — documented that provider profiles are build-defined (reset on startup, never persisted).
- **RT-7** — left inline with an explanatory comment (report-sanctioned fallback: branches differ in data source and the gemini branch has a side effect, so a single virtual is not a clean fit).
- **RT-5 / OC-7** — tightened opencode's `ProviderRecognizesSubagentTool` to use underscore-aware word boundaries (prevents "task_runner" from matching "task"); added superstring negative tests (`task_status`, `multitasking_helper`, `delegated`, `subtasker`). Claude override dropped per report guidance (claude uses separate `HandleClaudeMessage` path, not `HandleSessionUpdate`).

- **OC-5** — `IsEnabled`, `DisabledReason`, `UsesNativeOverlayHistory` given base-class defaults in `IProviderRuntime`; 4 redundant overrides removed from claude/codex/opencode/copilot and gemini; `UnsupportedProviderRuntime` trimmed to only override values that differ from defaults; stale `goal_service.h`/`string_utils.h` includes cleaned up.
- **PR-4** — `MergeProviderSettingsWithoutGenericYolo` renamed to `MergeProviderSettingsNeverYolo`; copilot interactive path standardized on mechanism B (`kCopilotAllowAllFlag` via `BuildProviderFlagsArgv`); dead `ProviderWorkerFlags`/`AppendProviderWorkerFlagsAndModel`/`AppendProviderWorkerPrompt` helpers removed from `provider_worker_command.h`; copilot yolo unit tests added (on → once, off → absent).
- **FE-1** — `npmPackageName` added to `Provider` type and threaded through `providerFromCppProvider` + `providersEquivalent`; `providerNpmPackageName`/`buildProviderCliInstallCommand` accept optional `Provider` and prefer live state; parity tests in `providerMetadata.test.ts`; C++ test `StateSerializerProviderJsonIncludesNpmPackageAndShortName` pins both sides of contract.
- **RT-4** (already done in prior session) — `SendStartupModelIfNeeded` already checks `IsGenericAcpSession` covering copilot; confirmed.
- **FE-2** (already done in prior session) — all five providers in `initialProviders`; confirmed.

Not done (large, explicitly multi-PR per the report's Section 4 rules — each needs its own staged PRs, not a single pass):
- **RT-8** — header→.cpp splits for the heavyweight inline headers (one header per PR).
- **MO-1..MO-7** — monolith decompositions of `useAppStore.ts`, `acp_session_runtime.cpp`, `ChatView.tsx`, `uam_query_handler.cpp`, `core_tests.cpp`, `runtime_orchestration_services.cpp`, and the platform impls. Note FE-3/FE-4/FE-5 already landed slices of MO-3/MO-1.

## Repo-level observations

- Branch: `Human-Code-improvement-&-Simplicatation`; working tree has 32 modified files + 3 untracked directories (FE-3 markdown extraction, FE-4 push buffers, MO-1 cpp/types.ts — staged but not yet committed).
- CLAUDE.md is stale: says only Gemini + Codex are in the release slice and that Claude/OpenCode runtime flags were removed; in reality all five providers (gemini, codex, claude, opencode, copilot) exist and are registered. **(Fixed — see DOC-1 above.)**
