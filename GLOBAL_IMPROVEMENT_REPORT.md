# UAM Global Improvement Report

**Audience:** implementation agents executing fixes. Each issue is self-contained: exact files/lines, the problem, and a step-by-step fix method. Do not improvise beyond the stated method; if a step fails, stop and report. Issues marked **[BLOCKED BY x]** must wait for issue x to land first.

**Verification commands after any C++ change:**

```bash
cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

After any `UI-V2/` change:

```bash
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
```

**Severity:** P0 = active correctness bug · P1 = latent bug / drift hazard · P2 = structural debt / monolith break-up · P3 = consistency & cleanliness.

**Status:** LIVE — updated incrementally as the audit proceeds. Coverage tracker: `Progress.md`. Issue prefixes: `OC` OpenCode provider · `PR` shared provider infra · `RT` runtime/ACP/terminal · `OR` orchestration/app services · `FE` frontend · `MO` monolith decomposition · `DOC` docs/meta.

---

## Section 1 — Provider layer (OpenCode first, then shared infrastructure)

### Headline architectural finding

The provider layer has **two parallel command-building systems**:

1. **`IProviderRuntime::BuildCommand` + `BuildCommandFromTemplate` + `{prompt}/{flags}/{resume}` templates** — DEAD in production. The only callers of `ProviderRuntime::BuildCommand`/`BuildPrompt` are tests (`tests/core_tests.cpp:4949-5139, 6268, 11460-11462`). Verified by grep: no `src/` caller of the facade's `BuildCommand`/`BuildPrompt` exists.
2. **`uam::BuildProviderWorkerCommand`** (`src/app/provider_worker_command.h:178+`) — the LIVE batch path, called from `memory_service.cpp:556` and `vcs_commit_service.cpp:595`. It is a hardcoded per-provider if-chain that bypasses the `IProviderRuntime` abstraction entirely.

Interactive terminals use the third path, `BuildInteractiveArgv` (live, called from `terminal_provider_cli.h:102`). Goal-continuation prompts are injected by the live ACP runtime (`acp_session_runtime.cpp:2231,4935`), so the goal logic inside each runtime's `BuildPrompt` is also dead outside tests.

Consequence: ~40% of every `*_cli_provider_runtime.cpp` and a persisted user setting (`provider_command_template`) maintain a feature no user can reach, with **three already-drifted sources of truth** for template strings (see PR-1). The recommended direction is **PR-5 (remove the dead path)**, which subsumes several smaller issues below.

### PR-5 (P2, do first in this section) — remove the dead `BuildCommand`/template pipeline

**Files:** `src/common/provider/provider_runtime.h` (`BuildCommand`, `BuildPrompt` decls), `provider_runtime_facade.cpp:25-39`, `provider_runtime_internal.h:304-330` (`BuildCommandFromTemplate`) + placeholder constants `:24-28` + `MergeProviderSettings` template-copy `:93-96` + `MergeProviderSettingsWithoutGenericYolo`/`WithCustomYoloFlag` `:104-119`, all five `*_cli_provider_runtime.{h,cpp}` (`BuildCommand`, and `BuildPrompt` where it only serves `BuildCommand`), `src/common/models/app_models.h:208,214` (`provider_command_template`, `gemini_command_template`), `src/common/config/settings_store.cpp:35,347-348,388,429-450,562-564`, `src/app/persistence_coordinator.cpp:43-47`, `src/app/application.cpp:426-427`, `provider_build_config.h:80-95` (`DefaultProviderCommandTemplate`), `ProviderProfile::command_template` (`provider_profile.h:19`, `provider_profile.cpp` per-profile `.command_template` entries), tests `core_tests.cpp:382,393,1350-1364,4949-…,5031,5139,6268,11460-11462`.

**Why:** dead production code with persisted-settings surface; three drifting template sources (PR-1); latent dangling-flag bug (PR-2); duplicated goal-lookup loops (OC-4); codex double-file append (see CX-1). Removing it deletes all of those at once.

**Decision note for the maintainer (confirm before executing):** this deletes the *concept* of user-configurable one-shot CLI command templates. Nothing in the UI exposes it (verify in Phase 3 against SettingsModal — PENDING), and profiles are never user-edited (`application.cpp:417` always resets `provider_profiles` to `BuiltInProfiles()`). If the feature should instead be *repaired*, execute OC-1/PR-1/PR-2 below instead of PR-5, and route `BuildProviderWorkerCommand` through it.

**Fix method (staged; keep each stage compiling):**
1. Delete `ProviderRuntime::BuildCommand` + `ProviderRuntime::BuildPrompt` facade methods (`provider_runtime.h:154-158`, `provider_runtime_facade.cpp:25-39`) and the corresponding tests. Tests that assert goal-prompt composition (`core_tests.cpp:11460-11462`) should be re-targeted at `GoalService::BuildContinuationPrompt` composition via the ACP path, or dropped if duplicative.
2. Delete `BuildCommand`/`BuildPrompt` from `IProviderRuntime` and all five runtimes (keep `BuildInteractiveArgv`). `UnsupportedProviderRuntime` in `provider_runtime_registry.cpp` loses the two overrides.
3. Delete `BuildCommandFromTemplate`, placeholder constants, `ReplacePlaceholderOrAppend`, `BuildFlagsShell` from `provider_runtime_internal.h` (grep each for remaining callers first; `ReplaceAll` has other users — keep what's still referenced).
4. Remove `AppSettings::provider_command_template` and `AppSettings::gemini_command_template` plumbing: `app_models.h:208,214`, `settings_store.cpp` read/write/migration lines above, `persistence_coordinator.cpp:43-47`, `application.cpp:426-427`. Settings files with the old keys must still parse (unknown keys are skipped — verify `settings_store.cpp` parser tolerates unknown keys; it does key-by-key matching, so removed keys are simply ignored).
5. Remove `ProviderProfile::command_template` and `provider_build_config::DefaultProviderCommandTemplate()` and their tests (`core_tests.cpp:1350-1364` partially).
6. Full C++ test run. Also grep `UI-V2/src` for `commandTemplate`/`provider_command_template` (PENDING Phase 3 — if the serializer exports it, remove that too in the same PR).

### OC-1 (P1) — *superseded by PR-5.* `BuildCommand` merges the profile command template then ignores it

`opencode_cli_provider_runtime.cpp:55-78` merges `profile.command_template` into settings (via `OpenCodeBatchCommandSettings` → `MergeProviderSettings`, `provider_runtime_internal.h:89-102`) then hand-builds argv `{"opencode", "run"}` and never reads the template. Same in claude (`claude_cli_provider_runtime.cpp:75-96`) and copilot (`copilot_cli_provider_runtime.cpp:63-73`). Gemini and codex DO use `BuildCommandFromTemplate` (`gemini_cli_provider_runtime.cpp:104`, `codex_cli_provider_runtime.cpp:69`) — 2 of 5 providers honor templates, 3 ignore them. Resolution: execute PR-5 (delete the whole path). Only if maintainer chooses REPAIR: route all five through `BuildCommandFromTemplate` and fix PR-2 first.

### CX-1 (P1, dead-path) — codex `BuildCommand` emits files twice and appends them as stray positional args

`codex_cli_provider_runtime.cpp:52-70`: `effective_prompt` already embeds files via `BuildPrompt` ("Referenced files:" list), then `BuildCommandFromTemplate(provider_settings, effective_prompt, files, "", "codex exec {flags} {prompt}")` passes `files` again; the template has no `{files}` placeholder, so `ReplacePlaceholderOrAppend` (`provider_runtime_internal.h:144-156`) **appends** the file paths as extra positional args after the prompt — malformed `codex exec` invocation. Dead path today (PR-5), but if REPAIR is chosen this must be fixed: pass `{}` as the files argument to `BuildCommandFromTemplate`. A test asserting "no positional args after prompt" should be added then. Resolution: PR-5 deletes it.

### OC-2 (P3, dead-path) — opencode `BuildCommand` passes files twice (`--file` flags + prompt text)

`opencode_cli_provider_runtime.cpp:62-76`: files are appended as `--file <path>` options *and* embedded in the prompt by `BuildPrompt`. Dead path (PR-5). If REPAIR chosen: drop one mechanism (keep `--file` only if `opencode run --help` confirms it exists).

### OC-3 (P1) — interactive argv passes `--model`, live batch worker path also passes `--model`, but dead `BuildCommand` doesn't — verify the live paths instead

Original concern (batch drops model) applies only to the dead path. **Action for agents:** none for opencode/claude/copilot batch; instead verify the live `BuildProviderWorkerCommand` model handling (it does pass model for all five providers — `provider_worker_command.h:178-230` ✓). Downgraded to no-op after PR-5. Interactive paths already pass model: opencode `--model` (`opencode_cli_provider_runtime.cpp:92`), codex `-m` (`codex_cli_provider_runtime.cpp:92`), claude `--model` (`claude_cli_provider_runtime.cpp:33`), copilot `--model` (`copilot_cli_provider_runtime.cpp:21`). Gemini interactive passes **no model flag** (`gemini_cli_provider_runtime.cpp:107-115` → generic `BuildInteractiveArgv`, `provider_runtime_internal.h:332-342`) — **real gap**: a per-chat `model_id` on a gemini chat is silently ignored in interactive CLI mode. Fix method: in `provider_runtime_internal::BuildInteractiveArgv`, after `AppendResumeArgs`, add `AppendTrimmedOptionValue(argv, "--model", chat.model_id);` (gemini CLI supports `--model`/`-m`). Confirm no other runtime uses the generic helper (grep: only gemini does) so the change is gemini-scoped. Add a core test: gemini interactive argv with `model_id="gemini-2.5-pro"` contains `--model gemini-2.5-pro`.

### OC-4 (P3) — duplicated 12-line active-goal lookup loop ×3

`opencode_cli_provider_runtime.cpp:64-75`, `claude_cli_provider_runtime.cpp:82-93`, `codex_cli_provider_runtime.cpp:56-67`. All inside dead `BuildCommand` bodies — PR-5 deletes them. If REPAIR chosen instead: add `FindActiveGoal(const ChatSession*)` to `provider_runtime_internal.h` and call it from all three.

### OC-5 (P2) — eight profile-ignoring trait virtuals per runtime ≈ 60 boilerplate lines × 5 providers

**Files:** `provider_runtime.h:50-54,69,109-118`; all five `*_cli_provider_runtime.cpp`.

`IsEnabled`/`DisabledReason` return `true`/`""` in every concrete runtime (build-flag exclusion already happens in the registry, `provider_runtime_registry.cpp:95-128`). The six trait getters (`UsesNativeOverlayHistory`, `SupportsGeminiJsonHistory`, `UsesLocalHistory`, `UsesInternalEngine`, `UsesCliOutput`, `UsesGeminiPathBootstrap`) all ignore their `ProviderProfile&` parameter; only gemini deviates from the common values (true for the three gemini-specific traits, `UsesLocalHistory=false`).

**Fix method:**
1. In `provider_runtime.h`, give defaults: `IsEnabled()→true`, `DisabledReason()→""`, `UsesNativeOverlayHistory→false`, `SupportsGeminiJsonHistory→false`, `UsesLocalHistory→true`, `UsesInternalEngine→false`, `UsesCliOutput→true`, `UsesGeminiPathBootstrap→false` (convert pure virtuals to virtuals with bodies).
2. Delete the now-identical overrides from codex/claude/opencode/copilot runtimes (.h and .cpp). Keep gemini's three true-overrides + `UsesLocalHistory=false`. Keep `UnsupportedProviderRuntime`'s `IsEnabled=false`, `DisabledReason`, `UsesLocalHistory=false`, `UsesCliOutput=false` overrides (its values differ from the new defaults — check each).
3. Build + full tests; zero behavior change expected. Run after PR-5 to avoid editing soon-deleted methods twice. **[BLOCKED BY PR-5 decision]**

### OC-6 (P2) — opencode `LoadHistory` stamps blank provider ids as a hidden legacy migration

**Files:** `opencode_cli_provider_runtime.cpp:103-114`; consumer `runtime_orchestration_services.cpp:623-663` (`NormalizeLegacyOpenCodeChatsForSidebar`).

All local-history runtimes call `LoadLocalChats(data_root)` which returns **every** chat in the shared `chats/` dir; opencode alone then stamps `provider_id="opencode-cli"` onto blank-provider chats. The only consumer that relies on this is `NormalizeLegacyOpenCodeChatsForSidebar`, which re-loads opencode history and overlays chats whose `provider_id` is blank but have a `native_session_id`. Note the circularity: line 642's `IsBlank(chat.provider_id)` filter can never exclude anything because the stamp already filled provider_id. The migration semantics ("blank provider chat with a native session id ⇒ legacy opencode chat") live in the wrong layer and execute on every sidebar normalization, not once.

**Fix method:**
1. Delete the stamping loop from `OpenCodeCliProviderRuntime::LoadHistory` (make it identical to claude/copilot/codex).
2. In `NormalizeLegacyOpenCodeChatsForSidebar`, do the stamping locally: when indexing `opencode_chats`, treat blank `provider_id` as opencode (`opencode_chats_by_id[chat.id] = &chat;` for blank-or-opencode provider ids) — preserve current matching behavior exactly.
3. Better (optional second step, separate PR): convert this to a one-time migration at chat load (`ChatRepository::LoadLocalChats` callers in `persistence_coordinator.cpp`): if `provider_id` blank and `native_session_id` non-empty → stamp + save once; then delete `NormalizeLegacyOpenCodeChatsForSidebar` entirely. Requires confirming blank+native-session can only mean opencode historically (check git log of `NormalizeLegacyOpenCodeChatsForSidebar` introduction before deciding).
4. Core tests: legacy blank-provider chat with native session id still resolves to opencode after the refactor.

### OC-7 (P3) — `ProviderRecognizesSubagentTool` substring matching is overly broad; claude/codex/gemini lack overrides

**Files:** `opencode_cli_provider_runtime.cpp:151-154` (matches any tool name *containing* "task"/"subtask"/"delegate" — `"task_status"` would false-positive); no override in claude/codex/gemini/copilot.

**Fix method:**
1. Find the consumer in `acp_session_runtime.cpp` (search `ProviderRecognizesSubagentTool`) and the actual tool names each provider's structured adapter emits (PENDING Phase 2 — this issue will be finalized there).
2. Tighten opencode to exact case-insensitive names; add overrides for providers whose adapters emit sub-agent tools as plain `tool_call` updates (Claude Code's `Task` tool is the known candidate).
3. Core tests: exact name matches, superstring does not.

### PR-1 (P1) — built-in command templates exist in three places and have already drifted twice

**Files:** `provider_profile.cpp:95-178`, `provider_build_config.h:80-95`, `tests/core_tests.cpp:1354-1364`.

Drift #1: build-config gemini template `"gemini {resume} {flags} {prompt}"` vs profile `"gemini -r {resume} {flags} {prompt}"`. Drift #2: test (line 1360) expects opencode `"opencode run {flags} {prompt}"` vs build-config `"opencode run --session {resume} {flags} {prompt}"` — that assertion only runs in a gemini/codex/claude-disabled build, so CI never catches it (latent test failure for anyone building opencode-first). Resolution: PR-5 deletes all three. If REPAIR: single constants header, all three sites reference it.

### PR-2 (P1, dead-path) — `--session {resume}` / `-r {resume}` templates leave a dangling option when resume id is empty

`provider_profile.cpp:100,151`, `provider_build_config.h:83,89`; mechanism `provider_runtime_internal.h:144-156,304-330`. Empty `{resume}` → `opencode run --session {flags} {prompt}` where `--session` swallows the next flag as its value. Gemini's live-looking default `"gemini -r {resume} …"` (`gemini_cli_provider_runtime.cpp:104`) has the same shape but is also only reachable via the dead facade. Resolution: PR-5. If REPAIR: make `{resume}` expand to `<resume_argument> <id>` as a pair, or strip the flag when id empty; add tests.

### PR-4 (P3) — three different yolo-flag idioms across five providers

**Files:** `provider_runtime_internal.h:104-119` (mechanism A: bake custom flag into `provider_extra_flags`, zero `provider_yolo_mode`) vs `:182-196` (mechanism B: `BuildProviderFlagsArgv(settings, yolo_flag)`); copilot adds mechanism C (`AppendCopilot*ModeArgs`, `copilot_cli_provider_runtime.cpp:19-40`).

opencode batch uses A, opencode interactive uses B, codex batch A + interactive B, claude B only, copilot C. Combining A+B on the same settings object would emit the flag twice — currently avoided only by convention. After PR-5 removes the batch paths, the remaining users of A are `CodexTemplateCommandSettings` (deleted with PR-5) and `ProviderWorkerFlags` (`provider_worker_command.h:152-156`, uses `MergeProviderSettingsWithoutGenericYolo` + empty yolo flag = "never yolo" for workers — intentional, keep but rename).

**Fix method [BLOCKED BY PR-5]:**
1. After PR-5, delete `MergeProviderSettingsWithCustomYoloFlag`; keep `MergeProviderSettingsWithoutGenericYolo` only if `ProviderWorkerFlags` still needs it — rename to `MergeProviderSettingsNeverYolo` for clarity.
2. Standardize interactive paths on mechanism B. Copilot keeps `--allow-all` via its approval-mode logic but should pass `kCopilotAllowAllFlag` through `BuildProviderFlagsArgv` like the others where possible; preserve the `--plan` approval branch as-is.
3. Core tests per provider: yolo on → flag exactly once in interactive argv; yolo off → absent.

### PR-6 (P2) — `BuildProviderWorkerCommand` is a provider if-chain in a 230-line inline header

**File:** `src/app/provider_worker_command.h` (whole file; callers `memory_service.cpp:556`, `vcs_commit_service.cpp:595`).

Problems: (a) the LIVE batch command builder bypasses `IProviderRuntime` — adding a sixth provider requires editing this if-chain by hand and nothing enforces it; (b) ~230 lines of inline functions in a header, including filesystem scanning (`AppendProviderWorkerNvmNodeVersionBins` walks `~/.nvm/versions/node` on every call) — compile-time bloat and untestable side effects in a header; (c) PATH-building logic (`ProviderWorkerPathEntries`, macOS-only homebrew paths) is unrelated to command building and is duplicated conceptually with platform services PATH handling (verify against `platform_services_macos_impl.cpp` in Phase 2 — PENDING).

**Fix method:**
1. Add a virtual to `IProviderRuntime`: `virtual std::vector<std::string> BuildWorkerArgv(const ProviderProfile&, const AppSettings&, std::string_view prompt, std::string_view model_id) const` with each provider's current branch body moved into its runtime (.cpp). The codex read-only args (`kCodexReadOnlyWorkerArgs`) move into `codex_cli_provider_runtime.cpp`; claude's stateless args into claude's runtime.
2. Split the file: `provider_worker_command.{h,cpp}` keeps only `WithProviderWorkerPathEnvironment` + `BuildProviderWorkerShellCommand(argv, mode)`; move function bodies to the .cpp. `BuildProviderWorkerCommand(profile,…)` becomes: resolve runtime via `ProviderRuntimeRegistry::Resolve(profile)`, get argv, wrap with PATH env.
3. Keep behavior byte-identical: write core tests FIRST capturing current command strings for all five providers (gemini/codex/claude/opencode/copilot with flags/model/yolo combos), then refactor against them.
4. This makes the runtime interface the single place a new provider plugs in. Run after PR-5 + OC-5 so the interface churn happens once.

### PR-7 (P3) — `app.provider_profiles` is reset to built-ins on startup, so profile "store" plumbing overstates its role

**Files:** `application.cpp:417` (`m_app.provider_profiles = ProviderProfileStore::BuiltInProfiles();`), `provider_resolution_service.cpp:87` (`EnsureDefaultProfile` — can never add anything after a fresh reset; only meaningful if profiles ever come from persistence).

No persistence of profiles exists (settings_store has no profile section). Either profiles are intended to be configurable later (keep, add a comment) or `EnsureDefaultProfile` is dead-ish belt-and-suspenders. **Fix method:** low priority; when touching these files, document that profiles are build-defined, not user data. Do not delete `EnsureDefaultProfile` without checking `runtime_orchestration_services.cpp:269` usage (it iterates `BuiltInProfiles` separately).

### DOC-1 (P2) — CLAUDE.md materially misdescribes the codebase

States the active slice is Gemini + Codex only and that Claude/OpenCode runtime flags were removed and "intentionally fail configuration" — but five enabled runtimes exist with required build flags (`provider_build_config.h:3-21`), and `src/app/` contains goal_service, vcs_commit_service, git_worktree_service, memory_library_service, markdown_store_service not mentioned in the architecture tree. The IPC action table is also incomplete (verify against `uam_query_handler.cpp` / `frontend_actions.cpp` in Phase 2 — PENDING list will be added to this issue). Fix: rewrite the affected CLAUDE.md sections from this report after Phases 1–4 complete. No code changes.

---

## Section 2 — Runtime / ACP / terminal integration surface

### RT-1 (P0) — state serialization can run a blocking `curl` subprocess (up to ~6s) on the bridge/UI path

**Files:** `src/cef/state_serializer.cpp:290-340` (`FetchOpenCodeZenModels` → `process_service.ExecuteCommand("curl -s --max-time 4 …", 6000)`), reached via `RefreshOpenCodeZenFreeModelsForFrontend` ← `FallbackAcpModelsForChat` (`:443-454`) ← chat serialization (`:1013,1062`).

`PushStateUpdateIfChanged` is called after nearly every bridge action (`uam_query_handler.cpp` — 15+ call sites) and from the polling loop. For an OpenCode chat, serialization synchronously: (a) reads `~/.config/opencode/opencode.json` from disk **every time** (`:381,432` — twice per serialization, models + default model), and (b) once per 10-minute window (static throttle `:313-336`) spawns `curl` with a 4s network timeout / 6s process timeout. `OnQuery` runs on the CEF UI thread (`CEF_REQUIRE_UI_THREAD`, `uam_query_handler.cpp:1160`), so the first opencode-chat serialization in each window can freeze the UI for several seconds (no network → full timeout). Also shells out to `curl` rather than using any in-process HTTP, and silently does network I/O from a function named "Serialize…".

**Fix method:**
1. Create `src/app/provider_model_catalog_service.{h,cpp}` (CMake: add to the main target's source list next to the other `src/app` services). Move from `state_serializer.cpp`: `kOpenCodeZen*` constants, `IsOpenCodeZenFreeModelId`, `BuiltInOpenCodeZenFreeModelsForFrontend`, cache read/write, fixture/env handling, `FetchOpenCodeZenModels`, `RefreshOpenCodeZenFreeModelsForFrontend`, `OpenCodeConfigPath`, `ReadConfiguredOpenCodeModelsForFrontend`, `ReadConfiguredOpenCodeDefaultModelForFrontend`, and the codex equivalents (`ReadCachedCodexModelsForFrontend` — locate in same file).
2. Make the network refresh asynchronous: reuse the `AsyncCommandTask` pattern from `provider_cli_compatibility_service.cpp:131+` (jthread + shared state). The service owns: in-memory model list, last-refresh time, a `MaybeStartRefresh(data_root)` that launches the task, and a `Poll()` that harvests a finished task and updates the in-memory list + disk cache. Wire `Poll()` into the existing app polling loop in `application.cpp` (same place other services poll — find `PollPendingRuntimeCall`/compatibility polling and add alongside).
3. The serializer then only reads the service's in-memory snapshot (plus the disk cache fallback at startup). Serialization must do **zero** subprocess and zero network work. Cache the `opencode.json` parse with an mtime check so repeated serializations don't re-read it.
4. Keep env overrides `UAM_OPENCODE_ZEN_MODELS_PATH` / `UAM_DISABLE_OPENCODE_ZEN_REFRESH` working (tests use them — grep `core_tests.cpp` for both before moving).
5. Tests: existing zen-model tests keep passing; add one asserting `StateSerializer` performs no refresh (fixture env set ⇒ models come from service snapshot).

### RT-2 (P2) — model-catalog logic lives in the CEF serializer (wrong layer)

Same files as RT-1. Even after the blocking fetch is fixed, ~250 lines of provider model catalog code (opencode config parsing, zen cache, codex model cache, merge logic `MergeAcpModelArrays`) sit in `src/cef/state_serializer.cpp`, which should be a pure `AppState → JSON` mapping. RT-1's fix method step 1 resolves this; listed separately so the layering rule is explicit for reviewers: **no file I/O, subprocess, or network in `src/cef/state_serializer.cpp`** (mtime-cached config reads live in the service).

### RT-3 (P2) — structured-launch argv is the third hardcoded provider if-chain

**File:** `acp_session_runtime.cpp:783-814` (`BuildAcpLaunchArgv`).

Codex/opencode/copilot/claude/gemini argv are hardcoded with per-provider inconsistencies: claude gets `--permission-mode/--model/--resume` at launch, gemini gets `--approval-mode/--model`, opencode/copilot get nothing (model set later — see RT-4). Together with `BuildProviderWorkerCommand` (PR-6) and `BuildInteractiveArgv` (per-runtime), command construction is spread over three mechanisms in three files.

**Fix method [run with/after PR-6]:** add `virtual std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const` to `IProviderRuntime`; move each branch into the provider's runtime .cpp (claude's approval-mode helper `ClaudeLaunchApprovalMode` moves to claude's runtime; gemini's to gemini's). `BuildAcpLaunchArgv` becomes a one-line registry call. Behavior must stay byte-identical — write argv-capture tests for all five providers first (there are existing `BuildAcpLaunchDetailForTests` exports at `acp_session_runtime.cpp:5434-5445`; extend those tests).

### RT-4 (P1) — persisted model id is applied at session start only for OpenCode; Copilot chats lose their model on restart

**Files:** `acp_session_runtime.cpp:1624-1651` (`SendStartupModelIfNeeded` gated on `IsOpenCodeSession`), `:783-799` (copilot launch argv has no `--model`), `:5160-5202` (`SetAcpSessionModel` happily sends `session/setModel` for copilot when the user changes it manually).

A copilot chat with `chat.model_id` persisted gets a fresh `copilot --acp --stdio` session with the provider-default model; the stored model is applied only if the user re-picks it. OpenCode handles exactly this case via `SendStartupModelIfNeeded`.

**Fix method:**
1. Change the gate in `SendStartupModelIfNeeded` from `IsOpenCodeSession(session)` to `IsGenericAcpSession(session)` (defined `:146-149` as opencode || copilot).
2. Verify copilot ACP accepts `session/setModel` outside user-initiated flows (it is already sent via the generic branch of `SetAcpSessionModel`; if the agent can run `copilot --acp` locally, smoke-test; otherwise rely on the shared code path and watch the existing error handling).
3. Test: extend the ACP tests around startup model (search `core_tests.cpp` for `SendStartupModel` / `startup_model`) with a copilot-session case.

### RT-5 (P3) — sub-agent tool detection is title-substring guesswork (finalizes OC-7)

**Files:** `acp_session_runtime.cpp:2626-2640` (`LooksLikeSubAgentTool`: any tool *title* containing "agent" matches — e.g. a tool titled "Agentless scan" would too), `:2639` passes `tool_call.title` (display title, not a stable tool name) to `IProviderRuntime::ProviderRecognizesSubagentTool`; opencode's override matches substrings "task"/"subtask"/"delegate" (`opencode_cli_provider_runtime.cpp:151-154`).

**Fix method:**
1. Pass both the raw tool name (from the update's `toolName`/`title`/`kind` fields — check what `AcpToolCallState` captures; prefer a stable identifier if the protocol carries one) and keep title as fallback.
2. Tighten opencode's override to word-boundary or exact matches (`task`, `subtask`, `delegate` as whole tokens, case-insensitive).
3. Add a claude override for its `Task` tool **only if** claude sessions route through `HandleSessionUpdate` — they do not (claude uses `HandleClaudeMessage`, `:4588`); verify whether claude tool calls reach `ApplySubAgentMetadata` before adding (search `ApplySubAgentMetadata` callers). If they don't, drop the claude part of OC-7.
4. Core tests: title "Task: run sub-task" → sub-agent; title "Multitasking helper" → NOT sub-agent (currently false-positives).

### RT-6 (P2) — opencode local-history rebind block: duplicated persist logic + 4th copy of the blank-provider rule

**File:** `src/common/runtime/terminal_polling.h:245-446`.

(a) Lines 296-320 and 405-428 are a near-identical 25-line "persist rebound session id" block (draft-link vs direct-save + terminal/app-state updates + diagnostic log). (b) `OpenCodeLocalHistoryProviderMatches` (`:323-338`) despite the name handles **all** providers (non-opencode → plain alias match) — misleading. (c) `NormalizeOpenCodeLocalHistoryProvider` (`:340-348`) is the fourth place encoding "blank provider id ⇒ opencode" (others: OC-6's runtime stamp, `NormalizeLegacyOpenCodeChatsForSidebar`, `OpenCodeLocalHistoryProviderMatches:332-335`).

**Fix method:**
1. Extract `bool PersistRebindDiscoveredSession(AppState&, ChatSession& previous_chat, const ProviderProfile&, CliTerminalState&, const std::string& discovered, std::string_view diagnostic_event)` covering lines 296-320/405-428; call from both sites (the two sites differ only in the diagnostic label `local_history_session_rebound_from_chat_file` vs `local_history_session_rebound` — pass it in).
2. Rename `OpenCodeLocalHistoryProviderMatches` → `LocalHistoryChatMatchesTerminalProvider` (it is the general predicate; keep behavior).
3. Centralize the legacy rule: add to `provider_ids.h` (or a new `src/common/provider/legacy_provider_rules.h`) `inline bool IsLegacyOpenCodeChat(const ChatSession&)` / `inline std::string EffectiveChatProviderId(const ChatSession&)` returning opencode for blank ids, and use it in all four sites (coordinate with OC-6 — do OC-6 first, then this).
4. Tests already exist for rebinding (grep `core_tests.cpp` for `local_history_session_rebound` / `Rebind`); they must pass unchanged.

### RT-7 (P3) — pre-launch session-id snapshot is another per-provider if-chain

**File:** `src/common/runtime/terminal/terminal_launch.h:93-109`. Native-history (gemini) vs opencode local chats vs codex session index selected inline. Low priority; fold into the PR-6/RT-3 interface work as `virtual std::vector<std::string> CollectPreLaunchSessionIds(...)` **only if** PR-6 and RT-3 land cleanly; otherwise leave with a comment.

### RT-8 (P3) — heavyweight header-only implementation files in `src/common/runtime/`

`terminal_polling.h` (752 lines), `json_runtime.h` (850), `terminal/terminal_launch.h` (146 lines incl. process launch), `terminal/terminal_chat_sync.h` (295), `terminal/terminal_lifecycle.h` (309), plus `src/app/provider_worker_command.h` (230, includes filesystem scans). All function bodies are `inline` in headers; several need forward declarations of their own inline functions (`terminal_polling.h:245,251`) — a sign they outgrew the pattern. Every TU including them recompiles the bodies.

**Fix method (mechanical, one header per PR):** create matching `.cpp`, move non-template bodies, keep declarations in the header, add the `.cpp` to CMake. Start with `terminal_polling.h` (largest, most churn). Zero behavior change; full test suite must pass.

### DOC-1 (cont.) — verified bridge-action drift

`UamQueryHandler::DispatchAction` (`uam_query_handler.cpp:1072-1157`) registers **73 actions**. CLAUDE.md documents 28. Missing from CLAUDE.md include: `getChatMessages`, `openNativeSessionChat`, `setChatCodexOptions`, `setChatAutoApproveCommands`, `setProviderChatDefaults`, `setEditorSettings`, `refreshCliProviderVersion`, `applyCliProviderVersion`, markdown-store actions (×5), `searchChatMessages`, memory-library actions (×5+`listMemoryScanCandidates`,`scanCurrentChats`), workspace open actions (×3), worktree actions (×4), VCS actions (×4), `stageChatAttachments`, goal actions (×4). The dispatch itself is clean table-driven code — only the doc is wrong. Fold into the DOC-1 rewrite.

---

## Section 3 — Frontend (UI-V2)

### FE-1 (P2) — provider identity metadata is hand-duplicated across C++ and TypeScript (3+ copies)

**Files:** `UI-V2/src/utils/providerMetadata.ts:18-77` (ids, names, protocols, npm packages, alias map) duplicates `src/common/provider/provider_ids.h:35-49` (alias map), `provider_profile.cpp` (titles/protocols), `provider_cli_compatibility_service.cpp:71-110` (npm packages, titles).

Adding/renaming a provider requires synchronized edits in 4 files across 2 languages with no compile-time or test-time check. The backend already serializes a providers array (store `CppProvider`, `useAppStore.ts:373-382`) — check which fields it carries (`state_serializer.cpp:520` area) and extend it.

**Fix method:**
1. In `state_serializer.cpp`, extend per-provider JSON with `shortName`, `npmPackage`, `runtimeDescription` sourced from the C++ tables (npm package from `ProviderCliPolicy`; expose a getter on `ProviderCliCompatibilityService` if private).
2. In `providerMetadata.ts`, keep the static table **only** as the pre-bootstrap fallback; all call sites that have a live `Provider` from state must prefer its fields (`providerNpmPackageName`/`buildProviderCliInstallCommand` currently ignore state entirely — rework to accept an optional `Provider`).
3. Add a parity test: `UI-V2/src/utils/providerMetadata.test.ts` already exists — add a test that the fallback table's provider ids equal `['gemini-cli','codex-cli','claude-cli','opencode-cli','copilot-cli']` and, on the C++ side, a core test asserting the serialized provider JSON contains the new fields. The two tests pin both sides of the contract.

### FE-2 (P3) — pre-bootstrap fallback provider list omits codex and claude

**File:** `UI-V2/src/store/useAppStore.ts:55-59` — `initialProviders` = gemini, opencode, copilot only. Before the first CEF state push (and in browser dev mode), codex/claude chats render with a synthesized fallback provider instead of a listed one. Either intentional (release slice) or drift — but it contradicts `providerMetadata.ts` which knows all five. **Fix:** add the two missing `initialProvider(...)` entries (pick colors consistent with the existing three; `ProviderLogo.tsx` defines no per-provider brand colors to match), or document why the slice excludes them. Also reconcile the unrelated hardcoded fallback color `#8ab4ff` in `providerMetadata.ts:99`.

### FE-3 (P2) — ~280-line hand-rolled markdown renderer lives inside ChatView.tsx

**File:** `UI-V2/src/components/views/ChatView.tsx:553-833` (`safeHref`, `renderInlineMarkdown`, `splitTableRow`, `parseTableSeparator`, `isPotentialTableRow`, `MarkdownTextBlock`, `MarkdownContent`).

Keep it hand-rolled (no new deps in the CEF file:// context without a decision), but it's untestable in place and bloats ChatView. **Fix method:** move to `UI-V2/src/components/markdown/Markdown.tsx` + `markdownParsing.ts` (pure helpers separate from components so they get plain unit tests); re-export `MarkdownContent`. Move the relevant `ChatView.test.tsx` cases (search "markdown", "table") into `markdownParsing.test.ts`. Part of MO-3 staging.

### FE-4 (P3) — module-level mutable buffers and timers in the store file

**File:** `useAppStore.ts:2203-2226` (`pendingRequestIdsByKey`, `pendingCodexOptionsByChatId`, `pendingCliTranscriptChunksBySessionId`, `pendingStreamTokensByChatId`, flush timers/delays).

Hidden shared state across the store and the push handler; tests must know to reset them. During MO-1, encapsulate in a `pushBuffers.ts` module with an explicit `resetForTests()`. No behavior change.

### FE-5 (P3) — provider-detection helpers duplicated between ChatView and providerMetadata

**File:** `ChatView.tsx:224-238` (`isCodexProvider`, `isClaudeProvider`, `isCopilotProvider`, `isOpenCodeProvider`) wrap `providerUsesProtocol` 4×. Fine pattern, wrong home — other components will want them. Move to `providerMetadata.ts` next to `providerUsesProtocol` during MO-3.

---

## Section 4 — Monolith decomposition plans

These are the highest-effort, highest-payoff items. **Rules for every decomposition agent:** (1) pure code motion — no behavior or naming changes beyond file paths unless the issue says so; (2) one extraction stage per PR, compiling and green; (3) keep old entry points as thin re-export/forwarding shims until all callers migrate, then remove shims in a final PR; (4) update CMake / imports in the same PR as the move.

⚠️ **Working-tree note:** the repo currently has uncommitted in-flight changes touching `cef_push.{h,cpp}`, `acp_session_runtime.cpp`, `useAppStore.ts`, `cefBridge.ts`, `app_state.h`, `platform_services_macos_impl.cpp` (≈273 insertions — looks like a push-channel feature). Decomposition of those files must wait until that work lands, or be rebased over it. Check `git status` before starting any MO task.

### MO-1 (P2) — `UI-V2/src/store/useAppStore.ts` (6,156 lines) → typed modules + Zustand slices

Current layout: lines ~20-100 constants/init data · ~99-605 `Cpp*` wire types · ~606-2050 sanitizers + equivalence helpers · ~2039-2200 binding builders/reconcilers · ~2203-2490 push buffers + `parseUamPushPayload` · ~2487-2845 `deserializeState`/`applyStatePatch`/revision logic · ~2852-3010 the `AppState` interface (≈100 actions) · ~3094-6156 the single `create<AppState>()` with 86 action implementations.

**Target structure (`UI-V2/src/store/`):**
```
cpp/types.ts          // all Cpp* interfaces + push channel types
cpp/sanitizers.ts     // sanitize*, normalize*, *Equivalent — pure functions
cpp/reconcile.ts      // cliBindingFromCppChat, acpBindingFromCppChat,
                      // reconcileCppMessages, deserializeState, applyStatePatch
push/pushBuffers.ts   // pending maps + flush timers (FE-4) with resetForTests()
push/uamPush.ts       // parseUamPushPayload + window.uamPush registration
slices/sessionsSlice.ts   // session CRUD, selection, rename, pin, provider/model
slices/goalsSlice.ts
slices/foldersSlice.ts
slices/memorySlice.ts     // memory library + scan modal
slices/markdownStoreSlice.ts
slices/vcsSlice.ts        // worktree + commit panel actions
slices/cliSlice.ts        // terminal bindings + transcripts
slices/acpSlice.ts        // prompts, permissions, user input, modes/models
slices/uiSlice.ts         // theme, modals, layout widths
useAppStore.ts            // composes slices; re-exports every existing export
```

**Method (one stage per PR, in order):** (1) extract `cpp/types.ts` + `cpp/sanitizers.ts` (pure moves; `useAppStore.ts` re-exports so `useAppStore.test.ts` keeps passing untouched); (2) extract `cpp/reconcile.ts` + `push/*` — **critical invariant:** `window.uamPush` must still be registered at module load of the store entry file, before React renders (CLAUDE.md gotcha #2) — keep the registration call in `useAppStore.ts` top level, importing the handler from `push/uamPush.ts`; (3) convert actions to slices using Zustand's `StateCreator` composition, one slice per PR, largest first (memory, acp, sessions); (4) split `useAppStore.test.ts` (2,578 lines) to mirror, moving cases verbatim; (5) final PR removes any re-export no longer imported (grep each). Every stage: `npm --prefix UI-V2 run test && npm --prefix UI-V2 run build`.

### MO-2 (P2) — `src/common/runtime/acp/acp_session_runtime.cpp` (5,564 lines) → cohesive units

Measured internal clusters (line ranges from the function outline):
- 187-365 diagnostics formatting (`CapDiagnosticString`…`AppendInvalidResumeDiagnostic`)
- 367-580 wait/turn-stream state resets (`ResetAcpWaitState`…`UpdateAcpStaleWait`, `RecentStderrTail`)
- 580-660 resume-id validation per provider
- 748-1010 launch + initialize (`LaunchApprovalMode`…`SendInitialize`, `WriteAcpMessage`)
- 1015-1340 content extraction + load-history replay dedup
- 1373-1700 session lifecycle (reset, start process, setup, startup model, queued prompt)
- 1695-2030 chat persistence + turn events/message blocks
- 2041-2390 goal loop
- 2394-2665 assistant message/tool-call/plan sync + sub-agent metadata
- 2665-3076 generic session updates + permissions
- 3077-3780 **codex adapter** (~700 lines: `CodexItemTitle`…`HandleCodexMessage`)
- 3781-4400 request/response dispatch + error formatting + modes/models
- 4401-4640 **claude adapter** (`HandleClaudeAssistantMessage`…`HandleClaudeMessage`)
- 4636-4835 line processing + stdio drain + process exit
- 4839-5560 public API + ~25 `*ForTests` wrappers (5434-5541)

**Target structure (`src/common/runtime/acp/`):** `acp_diagnostics.{h,cpp}`, `acp_wait_state.{h,cpp}`, `acp_resume_rules.{h,cpp}`, `acp_launch.{h,cpp}` (or fold into runtimes per RT-3), `acp_replay.{h,cpp}`, `acp_turn_events.{h,cpp}`, `acp_goal_loop.{h,cpp}`, `acp_message_sync.{h,cpp}`, `acp_permissions.{h,cpp}`, `acp_codex_adapter.{h,cpp}`, `acp_claude_adapter.{h,cpp}`, with `acp_session_runtime.cpp` retaining dispatch, lifecycle, polling, and the public API.

**Method:** (1) first create `acp_session_internal.h` declaring the shared small helpers each unit needs (`AcpSessionMatchesProvider`, `Is*Session`, `NextAcpRequestId`, `WriteAcpMessage`, JSON value helpers) — these currently live in the anonymous namespace; (2) extract **leaf clusters first** (diagnostics → wait state → resume rules), one per PR: move functions out of the anonymous namespace into `uam::acp` namespace, update call sites, compile, run tests; (3) adapters next (codex, then claude) — they depend on turn events/message sync, so extract those before; (4) `*ForTests` wrappers: when a subject moves to a header, retarget its tests to call the real function and delete the wrapper (each wrapper deletion is test-only risk, do them as they become redundant); (5) goal loop last (it touches everything). **[BLOCKED BY in-flight working-tree changes to this file]**

### MO-3 (P2) — `UI-V2/src/components/views/ChatView.tsx` (3,913 lines) → component modules

Measured clusters: 39-355 model-option tables/builders · 356-552 status/diagnostic helpers + `CopyTextButton`/`AcpErrorDetails` · 553-833 markdown renderer (FE-3) · 835-1461 tool-call UI (`SubAgentRunningPanel`, `ToolCallInlineRows`, `PermissionInlineCard`, `UserInputInlineCard`, `ToolCallModal`) · 1462-2098 message blocks (`MessageFrame`, `ThinkingBlock`, `PlanBlock`, `GoalReviewBlock`, `PersistedMessage*`, `AttachmentList`, `TurnTimelineContent`) · 2099-2693 `ComposerToolbar` · 2694-3913 the `ChatView` component itself (~1,220 lines).

**Target structure:** `components/chat/modelOptions.ts`, `components/markdown/` (FE-3), `components/chat/ToolCallViews.tsx`, `components/chat/PermissionCards.tsx`, `components/chat/MessageBlocks.tsx`, `components/chat/Composer.tsx`, leaving `views/ChatView.tsx` as the orchestrating component. Move provider-detection helpers to `providerMetadata.ts` (FE-5).

**Method:** pure moves with re-exports, one cluster per PR in the order listed (model options and markdown have zero JSX dependencies on the rest — do them first). Split `ChatView.test.tsx` (2,519 lines) alongside, moving tests verbatim. After extraction, a follow-up PR may split `ChatView` itself (composer state vs message list vs header) — **do not** attempt that in the same PR as the moves.

### MO-4 (P2) — `src/cef/uam_query_handler.cpp` (3,503 lines) → per-domain handler files

The class is one `UamQueryHandler` with 73 `Handle*` methods and a clean table dispatch (`DispatchAction:1072`). Lowest-risk split: keep the single class declaration, define method bodies across multiple TUs:
`src/cef/handlers/chat_handlers.cpp` (session CRUD/select/provider/model), `folder_handlers.cpp`, `terminal_handlers.cpp`, `acp_handlers.cpp`, `memory_handlers.cpp`, `markdown_store_handlers.cpp`, `vcs_handlers.cpp` (incl. worktree), `goal_handlers.cpp`, `settings_handlers.cpp` (theme/editor/defaults/version manager), with `uam_query_handler.cpp` keeping `OnQuery`, `DispatchAction`, and shared private utilities (move shared static helpers into a `uam_query_handler_internal.h`).

**Method:** mechanical; one domain file per PR; add each new .cpp to CMake; verify `ctest` green. No header changes needed beyond the internal-helpers header.

### MO-5 (P3) — `tests/core_tests.cpp` (11,493 lines) → per-domain test files

Single TU slows incremental test builds and makes ownership murky. Inspect the harness first (top of file: assert macros + test registration — likely a hand-rolled list). Split into `tests/` files mirroring domains (`provider_runtime_tests.cpp`, `acp_session_tests.cpp`, `settings_tests.cpp`, `chat_repository_tests.cpp`, `terminal_tests.cpp`, `memory_tests.cpp`, `vcs_tests.cpp`, …) with a shared `tests/test_harness.h`. **Method:** move test functions verbatim; keep registration mechanism identical; one domain per PR; `ctest` count must not drop (record the before count and assert equality after each move).

### MO-6 (P3) — `src/app/runtime_orchestration_services.cpp` (2,121 lines)

Holds at least three responsibilities: pending-runtime-call polling (`PollPendingRuntimeCall:734`), native-history/sidebar sync (overlay index, `ReplaceAppChatsWithNormalized`, `NormalizeLegacyOpenCodeChatsForSidebar`, chat-source discovery ~269,540-1100), and native-session matching/claiming (1600-1800). Split into `pending_call_service.cpp`, `history_sidebar_sync_service.cpp`, `native_session_matching.cpp` after OC-6/RT-6 land (they edit the same regions). Outline the remaining unread regions before cutting (agent: generate a function outline as done for MO-2 and attach it to the PR description).

### MO-7 (P3) — platform service impls (`platform_services_windows_impl.cpp` 2,013 / `platform_services_macos_impl.cpp` 1,829)

Not deep-read in this audit (only opencode-adjacent comments checked). Before splitting, produce an outline; expected split: process execution / PTY / dialogs / clipboard / app paths per platform. **Do not start** until MO-2..MO-4 are done — lower value, and the macOS file has in-flight changes.

---

## Section 5 — Suggested execution order for the agent swarm

Waves; tasks within a wave are independent and can run in parallel. Do not start a wave until the previous wave's blocking items are merged.

**Wave 1 — correctness + quick structural wins (no interdependencies):**
- RT-1/RT-2 (blocking curl out of serializer) — one agent
- RT-4 (copilot startup model) — one agent
- OC-6 + RT-6 (legacy opencode rule consolidation; OC-6 first, same agent does both)
- FE-2 (fallback provider list) — one agent
- PR-5 decision from maintainer → if approved, one agent executes (stages 1-6)

**Wave 2 — provider-interface consolidation (after PR-5):**
- OC-5 (trait defaults) → then PR-6 (worker argv into runtimes) → then RT-3 (launch argv into runtimes) — same agent or strict sequence, all touch `IProviderRuntime`
- PR-4 (yolo idiom cleanup) — after PR-6
- RT-5/OC-7 (sub-agent matching) — independent
- FE-1 (provider metadata from state) — independent
- DOC-1 (CLAUDE.md rewrite) — after Wave 2 lands, since provider docs change

**Wave 3 — monolith decompositions (each is a multi-PR sequence):**
- MO-1 (store) and MO-3 (ChatView) can run in parallel (different files; both wait for in-flight store work to land)
- MO-2 (ACP runtime) — wait for in-flight changes + RT-3/RT-4/RT-5 merged
- MO-4 (query handler) — independent
- MO-5 (tests) — after MO-2 (test moves follow subject moves)
- MO-6, MO-7, RT-7, RT-8 — last

---

## Appendix — quick facts for agents

- Default branch for PRs: `main`. Current branch: `Human-Code-improvement-&-Simplicatation` with uncommitted in-flight changes (see MO warning).
- C++17/20 style: tabs for indentation, `uam::` namespaces, `inline` header utilities are common; match surrounding style.
- UI: tabs were recently converted to spaces in some test files (commit 2aeae84) — match per-file existing style.
- Provider ids: `gemini-cli`, `codex-cli`, `claude-cli`, `opencode-cli`, `copilot-cli`; aliases in `provider_ids.h:35-49`.
- All five providers are enabled by default; CMake flags `UAM_ENABLE_RUNTIME_{GEMINI,CODEX,CLAUDE,OPENCODE,COPILOT}_CLI`.
- OpenCode structured = `opencode acp` over stdio (generic ACP, `session/load` support, model via `session/setModel`); interactive = `opencode`; batch worker = `opencode run --model <id> <prompt>`.
- Data root resolution and chat storage: see CLAUDE.md Persistence section (accurate as audited).
