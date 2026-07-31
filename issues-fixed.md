# Issues Fixed

This ledger records reproducible bugs and interface problems found during the polish pass.
Each bug is reproduced with a regression test before its fix, reviewed to a two-hop blast
radius, and kept here with its validation evidence.

> Scope exclusion: GitHub issue #97 is intentionally untouched because its work lives on
> separate branches.

## v4.5.1 release candidate

- #188 — Fix GitHub Copilot provider failures on Windows, including ACP default-mode
  handling, Unicode launch/worktree/worker paths, Unicode-safe atomic temp/backup writes,
  and ConPTY output isolation.
- #189 — Make notification bars dismissible.
- #190 — Fix one-click collection collapsing on Windows.
- #191 — Allow the dictation surface to exit after microphone policy errors.
- #192 — Show favorited grouped skills individually in the slash menu.
- #193 — Inline attached skill content for external provider workers.
- #194 — Build, test, package, and smoke-test Windows Release output on pull requests.

Validation:

- Full frontend suite: 386/386 tests passed across 32/32 files.
- Frontend production TypeScript/Vite build passed.
- Native Release build passed; the macOS bundle was signed and verified.
- Native CTest suite: 3/3 targets passed.
- Final diff whitespace check, workflow YAML parse, and high-confidence secret scan passed.
- Local validation ran on macOS and is not proof of Windows behaviour.
- PR CI is the Windows gate: it builds and tests x64 Release, packages the actual executable,
  extracts that package, smoke-launches it, and uploads the tested ZIP for review.
- An authenticated GitHub Copilot smoke test on that Windows artifact is required before the
  release tag is approved.

## v4.5.0 historical validation

- Full frontend suite: 375/375 tests passed across 32/32 files.
- Native CTest suite: 3/3 targets passed.
- Frontend production TypeScript/Vite build passed.
- Native Release build passed; the 4.5.0 macOS bundle is signed and verifies successfully.
- All 34 previously failing frontend regressions and all 23 previously failing native
  regressions now pass.

## #186 — Make VCS filename regression tests portable on Windows

- Status: Fixed and locally validated
- Found from: PR #185 Windows CTest
- Symptom:
  - The Windows build succeeded, but two VCS regression tests failed while creating their
    fixture files and never reached the status-parser assertions.
- Root cause:
  - One filename contained `>`, and another contained a tab. Both are invalid on Windows.
- Fix:
  - Keep the literal ` -> ` filename regression on platforms that can create it.
  - Use a UTF-8 filename for the quoted-path regression; Git quotes this name in regular
    porcelain output, while `--porcelain=v1 -z` returns it raw on every supported platform.
- Validation:
  - Native Debug rebuild passed.
  - Local CTest passed 3/3 targets.

## #178 — Hide slash-command menus while an app modal is open

- Status: Fixed and verified in the release bundle
- Found from: Final Computer Use release testing
- Symptom:
  - Opening Settings while `/github` and its grouped skill submenu were active left both
    slash-command surfaces drawn over the modal.
- Root cause:
  - The chat popover remained active while a full application modal was open.
- Fix:
  - Suppress the slash palette and active submenu while New Chat, Settings, Memory,
    Memory Scan, or Skills is open, without discarding the user's draft.
- Validation:
  - Focused ChatView regression passed.
  - Full frontend suite: 375/375 tests passed across 32/32 files.
  - Computer Use verified the exact signed v4.5.0 release bundle: `/github` opens a clean
    submenu to the right, and both menu layers disappear when Settings opens.

## BUG-015 — Persist custom workspace order inside collections

- Status: Implemented locally and verified
- GitHub issue: #175
- Found from: User-reported workspace sidebar drag failure
- Symptom:
  - Dragging ungrouped workspaces changed and persisted their order, but dragging workspaces
    inside a collection appeared to do nothing and the visible order snapped back.
- Root cause:
  - Collection children render from the collection's ordered resource references, while the
    folder drop handler always updated the separate global folder order.
- Fix:
  - Route same-collection folder drops through the existing persisted resource-reference
    reorder action.
  - Preserve non-workspace reference positions when workspace references move.
  - Keep the whole workspace row draggable and support Arrow Up/Down keyboard reordering
    without a permanent drag icon.
- Validation:
  - `FolderTree.test.tsx`: 20/20 tests passed, including drag and keyboard collection reorder.
  - Full frontend suite: 373/373 tests passed across 32/32 files.
  - Frontend production TypeScript/Vite build passed.
  - Native CTest: 3/3 targets passed.
  - Native Release build and signed macOS bundle passed
    (`4.5.0-Darwin-20260726T123958Z`).

## BUG-014 — Prevent queued steering controls from getting stuck in a "Starting" UI lock

- Status: Implemented and verified
- Found from: Steering a queued prompt or steering midway through a running turn.
- Symptom:
  - The composer steering control could remain disabled and show "Starting" indefinitely
    after a steer action, even when no new turn had actually started.
- Root cause:
  - The previous lock-release condition depended mainly on `turnSerial` incrementing; in
    some recovery paths no new serial arrived while the UI state also didn’t indicate
    completion, so the steering state never cleared.
- Fix:
  - Added bounded recovery for the steering lock in `ChatView`.
  - Steering now auto-clears when:
    - the queued steer is no longer active and backend is not processing, or
    - turn serial advances, or
    - a 5-second guard timeout expires.
  - Added `prioritySteer` propagation to queued-prompt types and sanitization so queued
    steering states can be distinguished from normal queued prompts.
  - Reconciler now includes `prioritySteer` in queue equivalence checks.
  - Added regression test that simulates queued steer acceptance without immediate execution
    and verifies steering clears after timeout.
- Validation:
  - `ChatView.test.tsx` includes new steering recovery case.
  - `npm --prefix UI-V2 run test -- src/components/views/ChatView.test.tsx` passed (`65` tests).
  - `npm --prefix UI-V2 run test` passed (`32` files, `334` tests).
  - `npm --prefix UI-V2 run build` passed.
  - `cmake -S . -B Builds && cmake --build Builds --config Release` produced a signed
    working `Builds/universal_agent_manager.app`.

## UX-005 — Tighten chat/CLI edge spacing and restore CLI composer affordance

- Status: Implemented locally and verified
- Found from: Screenshot feedback (chat-window edge and message start alignment)
- Symptom:
  - The chat transcript still looked like it had a global outer inset that constrained all
    message content.
  - In CLI view, the steering composer sat too close to the panel edge and did not read as
    a distinct bordered block.
- Fix:
  - Removed assistant message `maxWidth` inline override so layout returns to CSS control,
    then aligned assistant/right bubble padding to the same horizontal profile as user messages.
  - Kept the message-content container alignment controlled by chat-side gutters and not forced
    full-width message boxes.
  - Increased the CLI steering-composer spacing so steering input remains visibly inset and
    not flush to the container edge:
    - terminal internal padding from `8px 10px 10px` → `10px 14px 14px`
    - steering composer margin from `0 8px 8px` → `0 12px 12px`
  - Kept the CLI composer bordered surface in place with the existing rounded, bordered styling.
- Validation:
  - Frontend: 333 tests passed, frontend build passed, native tests passed 3/3.
  - Native Release app build produced `Builds/universal_agent_manager.app` with version tag
    `4.4.2-Darwin-20260725T003540Z`.

## #125 — Preserve selected reasoning effort when creating a new chat

- Status: Fixed locally and validated
- Found from: Existing GitHub issue
- Symptom: A new chat could display `Low` or `Default` after the user selected another
  supported reasoning effort in the New Chat dialog.
- Root causes:
  - The dialog silently converted an empty provider-default model into the first advertised
    model, rather than keeping `Default` as the selected value.
  - Empty or invalid reasoning displayed the first supported effort while submission chose
    the highest supported effort, even when the runtime advertised a different default.
  - A newly created provider-default chat could show the first fallback model until ACP
    discovery finished, breaking continuity with the dialog's explicit `Default` choice.
  - The native prompt path replaced empty or invalid reasoning with the last advertised
    effort instead of the model's declared default.
  - Native state serialized `reasoningEffort` and `serviceTier`, but the frontend sanitizer
    discarded both fields during hydration.
  - Native provider defaults cleared reasoning for every non-Codex provider even when a
    generic ACP model advertised supported reasoning efforts.
- Data flow reviewed:
  `NewChatModal → addSession → createSession → ApplyProviderDefaultsToChat → persistence →
  state serializer → sanitizeCppChat → sessionFromCppChat → ChatView → Composer`
- Two-hop blast radius:
  - Model selection helpers used by the New Chat dialog and composer.
  - Provider-default application used by chat creation, provider switching, and shell actions.
  - Chat sanitization and reconciliation used by full state loads and incremental patches.
  - ACP prompt construction, which already suppresses unsupported reasoning efforts.
- Regression proof against pre-fix code:
  - An untouched New Chat dialog submitted the first advertised model instead of the saved
    provider-default selection.
  - The dialog displayed `Low` while submission normalized the same empty effort to `XHigh`.
  - Hydrated sessions received empty reasoning and speed values.
  - Native provider-default application discarded supported generic ACP reasoning.
- Fix:
  - Keep `Default` as an explicit New Chat model choice without exposing a runtime-reset
    option in established chats.
  - Keep that unresolved `Default` visible in the composer until ACP reports the resolved
    runtime model.
  - Resolve empty reasoning from the runtime model's declared default and use that same value
    for the dialog, composer chip, and creation request.
  - Apply the same declared default at the native prompt boundary, with the first supported
    effort as the deterministic fallback when a provider advertises an invalid default.
  - Preserve sanitized reasoning and speed fields during C++ state hydration.
  - Preserve reasoning for generic ACP providers while keeping service tiers Codex-only.
- Validation:
  - Affected New Chat, model helper, composer, and store suites passed.
  - Full frontend suite: 297 tests passed.
  - Frontend production build passed.
  - Native suite: 3 CTest targets passed.
  - Native Release build and macOS bundle signing passed.
- Files:
  - `UI-V2/src/components/chat/Composer.tsx`
  - `UI-V2/src/components/chat/modelOptions.ts`
  - `UI-V2/src/components/chat/modelOptions.test.ts`
  - `UI-V2/src/components/sidebar/NewChatModal.tsx`
  - `UI-V2/src/components/sidebar/NewChatModal.test.tsx`
  - `UI-V2/src/components/views/ChatView.tsx`
  - `UI-V2/src/components/views/ChatView.test.tsx`
  - `UI-V2/src/store/cpp/sanitizers.ts`
  - `UI-V2/src/store/useAppStore.test.ts`
  - `src/common/config/provider_chat_defaults.h`
  - `src/common/runtime/acp/acp_session_runtime.cpp`
  - `tests/acp_session_tests.cpp`
  - `tests/core_tests.cpp`

## UX-001 — Progressive-disclosure polish for chat selection and multi-pane work

- Status: Implemented locally; targeted tests and visual journeys passed
- Found from: User feedback and visual audit
- Problems:
  - The 2×2 view read as four competing card surfaces with excess glow, colour, borders,
    repeated actions, and dimmed inactive panes.
  - Chat rows did not make provider identity scannable enough.
  - Composer state wrapped or disappeared unpredictably in narrow panes.
  - Folder and collection rows repeated decoration and creation controls that were already
    available globally or contextually.
- Changes:
  - Removed pane glow, inactive-pane dimming, entrance motion, coloured side rails, hover
    lifts, repeated folder-level New Chat rows, collection cards, and decorative gradients.
  - Kept all panes legible and marks only the focused pane with a thin provider-coloured
    header edge.
  - Added provider logos and accessible pane markers to chat rows.
  - Preserved the contextual chiplet behaviour while keeping permissions, reasoning, model,
    and provider context glanceable; compact labels are used only when space is tight.
  - Centred chat messages and the composer on a shared readable measure.
  - Kept slash-command and consequential provider notices as progressive disclosures.
  - Softened search, composer, queued prompt, modal, and toolbar surfaces.
- Validation:
  - Desktop, 2×2, 900 px, 640 px, and 320 px journeys visually inspected.
  - No horizontal overflow at 320 px.
  - New Chat footer remains reachable at 320×568 and 700×400.
  - Affected component suites: 106 tests passed.

## UX-002 — Prevent compact side panels from stacking over the chat

- Status: Fixed locally; regression test and visual journey passed
- Found from: Responsive visual audit
- Symptom: At compact widths, opening notifications, updates, or commit tools could leave
  the chat selector open underneath, producing two competing overlays.
- Data flow reviewed:
  `right-rail action → AppShell panel state → shell data attribute → responsive layout CSS`
- Fix:
  - Keep side-panel widths as user preferences, but render panels as overlays at 1000 px or
    narrower.
  - Hide the left overlay while a right overlay is open.
  - Keep the chat viewport width stable and remove resize handles only in overlay mode.
- Validation:
  - App shell regression suite passed.
  - At 640 px, the main viewport remained 552 px wide before and after opening notifications.
  - Only one side overlay was visible and the document stayed within the viewport.

## UX-003 — Simplify the composer without hiding current state

- Status: Implemented locally; component tests and visual journey passed
- Found from: User feedback after the first visual preview
- Problems:
  - The composer repeated provider, model, reasoning, speed, memory, and goal information
    inside a separate settings popover.
  - An empty workspace showed a bordered status row plus disabled editor, terminal, folder,
    and worktree actions.
  - In narrow panes, controls wrapped into a second dense toolbar.
- Changes:
  - Removed the duplicate chat-settings popover and moved goal budget into Options.
  - Kept provider, model, permissions, and supported reasoning visible at a glance.
  - Put optional status chips in a compact horizontal disclosure lane.
  - Replaced the workspace action cluster with one contextual menu; active worktree actions
    remain visible because they can require immediate attention.
- Validation:
  - Chat and main-panel component suites: 60 tests passed.
  - Four-pane composer journey visually inspected.

## BUG-001 — Repair unresolved and incorrectly cascading theme tokens

- Status: Fixed locally and validated
- Found from: Visual token audit
- Symptoms:
  - Chat errors referenced an undefined `--danger` token and could inherit the wrong colour.
  - Goal and skill UI referenced an undefined `--purple` token.
  - Built-in themes declared their semantic fills before equal-specificity defaults, so the
    later defaults silently replaced the intended theme colours.
- Data flow reviewed:
  `theme selection → data-theme attribute → CSS token cascade → chat/tool/goal consumers`
- Regression proof against pre-fix code:
  - The token-contract test failed for the missing goal/skill token, invalid error token, and
    semantic declaration order.
- Fix:
  - Added one shared `--purple` alias to the active accent.
  - Replaced the three invalid `--danger` uses with `--error`.
  - Moved semantic defaults ahead of theme variants so theme-specific fills win.
- Validation:
  - Token-contract regression suite: 2 tests passed.
  - Affected visual/component suites: 108 tests passed.

## BUG-002 — Block duplicate New Chat submissions while creation is pending

- Status: Fixed locally and validated
- Found from: Independent bug hunt
- Symptom: Pressing Enter and clicking Create, or clicking Create twice quickly, could send
  multiple native creation requests and persist duplicate chats with different IDs.
- Data flow reviewed:
  `name input/Create button → NewChatModal.handleCreate → addSession → CEF createSession →
  native ID generation → chat persistence → state refresh`
- Two-hop blast radius:
  - Both modal submission entry points.
  - The shared session-creation store action and its CEF/mock implementations.
  - Modal close-on-success and retry-on-failure behaviour.
- Regression proof against pre-fix code:
  - The pending-submission test observed two calls before the first request resolved.
- Fix:
  - Added a synchronous in-flight guard at the shared modal boundary.
  - Made session creation report success or failure.
  - Show the existing button busy state while pending and unlock only after failure.
- Validation:
  - New Chat regression suite: 5 tests passed.
  - Store integration suite: 72 tests passed.

## BUG-003 — Preserve provider-managed goal ownership in incremental updates

- Status: Fixed locally and validated
- Found from: Independent state-flow audit
- Symptom: A provider-owned goal could change to the false “UAM managed” label after any
  incremental chat-state update, even though native execution still belonged to the provider.
- Data flow reviewed:
  `provider goal command → goals store → native validation/persistence → state patch serializer
  → frontend sanitizer → incremental patch projection → active goal → GoalBanner`
- Two-hop blast radius:
  - Full-state and incremental goal projection.
  - Provider goal creation and native goal loop ownership checks.
  - Goal banner ownership display and the stored provider command contract.
- Regression proof against pre-fix code:
  - Initial full hydration retained `executionOwner` and `providerCommand`.
  - The same goal lost both fields after a state patch, while its other updated fields applied.
- Fix:
  - Added the two omitted fields to the incremental mapper with the same backward-compatible
    defaults used by full hydration.
- Validation:
  - End-to-end incremental-patch regression test passed.

## BUG-004 — Preserve a newer chat selection after a failed delete

- Status: Fixed locally and validated
- Found from: Independent state-flow audit
- Symptom: If deleting one chat failed after the user selected another chat, the late rollback
  could either replace a newer selection or fail to restore the old selection after an
  unrelated session refresh.
- Data flow reviewed:
  `delete action → optimistic session removal/selection → native delete → user selects another
  chat → failed response → rollback → active pane`
- Two-hop blast radius:
  - Session restoration, terminal transcript restoration, and active selection.
  - Pane routing and subsequent message actions that consume the active chat ID.
- Regression proof against pre-fix code:
  - The deterministic delayed-failure test deleted active `chat-a`, auto-selected `chat-b`,
    then deliberately selected `chat-c` and `chat-b`; rollback incorrectly jumped to
    `chat-a` because it compared only the final chat ID.
  - The inverse test replaced the optimistic sessions array without changing selection;
    rollback incorrectly stayed on `chat-b` instead of restoring `chat-a`.
- Fix:
  - Track intentional selection actions with a small revision counter.
  - Restore the previous selection only when the active ID and selection revision are still
    unchanged; always restore the deleted chat data.
- Validation:
  - Delayed delete-rollback regression test passed.

## BUG-005 — Ignore terminal prompt bytes from the previous turn

- Status: Fixed locally and validated
- Found from: Independent provider-runtime audit
- Symptom: A prompt left in the terminal replay buffer could mark a newly submitted turn idle
  before the provider produced any new output. Prompt redraw bytes already queued by the PTY
  could also arrive just after the new turn was marked busy and trigger the same false idle.
- Data flow reviewed:
  `terminal input → busy lifecycle → PTY polling → replay buffer → prompt classifier → idle
  lifecycle → provider switching/background shutdown`
- Two-hop blast radius:
  - User input, queued steering, and new terminal launch paths that mark a turn busy.
  - Provider switching, branch availability, memory scheduling, and background shutdown,
    which all depend on accurate busy state.
- Regression proof against pre-fix code:
  - The lifecycle test classified the previous turn's `Send message` prompt immediately after
    the next turn was marked busy.
- Fix:
  - Keep the existing replay buffer for terminal restoration.
  - Classify prompts from a separate bounded current-turn buffer that resets at busy and
    stopped lifecycle boundaries.
  - Treat the first post-submit prompt as an isolated settling candidate and remove it from
    the current-turn scan, so later output cannot combine with stale bytes.
  - Cancel that candidate when real output arrives, allow a genuinely fast prompt to settle
    after 200 ms, and let the initial process-start prompt settle immediately.
- Validation:
  - Native regression and all 3 CTest targets passed.

## BUG-006 — Roll back a model rejected by a generic ACP provider

- Status: Fixed locally and validated
- Found from: Reasoning-chip data-flow audit
- Symptom: A generic ACP provider could reject `session/set_model`, while the chat, model
  selector, and reasoning chip continued to display the rejected model.
- Data flow reviewed:
  `model selector → persisted next-turn model → ACP set-model request → provider response →
  runtime model → serialized chat → model/reasoning controls`
- Two-hop blast radius:
  - Live idle model changes and startup model application.
  - Queued prompts, reasoning-option derivation, incremental state pushes, and restart
    persistence.
- Regression proof against pre-fix code:
  - After a JSON-RPC rejection, both serialized runtime and chat model remained `model-new`.
- Fix:
  - Track each generic ACP model request with its requested model, previous confirmed runtime
    model, and previous persisted chat model as separate values.
  - On rejection, restore both runtime and persisted chat state if it still represents that
    request, including an empty provider-default chat setting.
  - On acceptance, send any queued prompt only after the provider confirms the model.
  - Clear a prior model-change error and return the live selector to Ready after a successful
    retry.
- Validation:
  - Rejection rollback and model-before-prompt sequencing tests passed.
  - All 3 native CTest targets passed.

## BUG-007 — Reject provider-unoffered ACP permission choices

- Status: Fixed locally and validated
- Found from: Independent permission-boundary audit
- Symptom: A crafted or stale permission choice not offered by the provider could be sent as
  an approval and clear the active permission card.
- Data flow reviewed:
  `provider permission options → native pending state → serializer → permission card → CEF
  response → provider-specific approval payload`
- Two-hop blast radius:
  - Manual permission resolution, standard ACP auto-approval, turn cancellation, and Codex
    command/file/permissions approvals.
  - Waiting-state lifecycle and provider rejection handling after malformed responses.
- Regression proof against pre-fix code:
  - `forged-allow` returned success and cleared a request that offered only `allow-once`
    and `deny`.
- Fix:
  - Validate non-cancelled choices against the exact active provider option list at the shared
    native response boundary.
  - Preserve synthetic Cancel because it is intentionally supplied by the app.
- Validation:
  - Forged-option rejection, offered-option acceptance, and synthetic-cancel tests passed.
  - All 3 native CTest targets passed.

## BUG-008 — Roll back a branch when regeneration cannot start

- Status: Fixed locally and validated
- Found from: Independent chat-lifecycle audit
- Symptom: A failed provider launch after branching left a persisted, selected branch plus
  stale runtime state even though the UI reported that branching failed.
- Data flow reviewed:
  `message branch action → branch persistence/selection → provider retry → launch failure →
  native state push/restart`
- Two-hop blast radius:
  - Chat files, remembered selection, ACP sessions, CLI terminals, and sidebar branch trees.
  - Subsequent state refreshes and restart hydration.
- Regression proof against pre-fix code:
  - A deterministic invalid-workspace launch failure left two chats and selected the orphan
    branch.
- Fix:
  - Treat branch creation and regeneration startup as one lifecycle transaction.
  - On failure, stop and erase runtime artifacts, remove branch memory/storage, restore the
    source selection, and persist that restored selection.
  - If local-history deletion or selection persistence fails during rollback, keep and
    re-persist the stopped branch instead of claiming a clean rollback that could resurrect
    after restart.
  - If rollback must keep the branch, push the retained native selection back to React before
    reporting the failure so the sidebar and native state cannot diverge.
- Validation:
  - Branch failure rollback test passed.
  - All 3 native CTest targets passed.

## BUG-009 — Preserve UTF-8 when memory previews are truncated

- Status: Fixed locally and validated
- Found from: Final provider-runtime regression run
- Symptom: A memory preview cut at 320 raw bytes could split a multi-byte character. The
  malformed recall preface then made the provider prompt fail JSON serialization before it
  was sent.
- Data flow reviewed:
  `memory markdown → preview extraction → recall budget → queued ACP prompt → JSON request →
  provider stdin`
- Two-hop blast radius:
  - Global and workspace memory recall for every structured provider.
  - New prompts, queued prompts, branch regeneration, and goal continuations that share the
    ACP prompt builder.
- Regression proof against pre-fix code:
  - A preview with a curly quote crossing the 320-byte boundary threw an invalid UTF-8 JSON
    error.
- Fix:
  - Reuse the shared UTF-8-aware line limiter instead of truncating raw bytes.
- Validation:
  - The boundary regression passed.
  - Branch regeneration with recalled memory passed.
  - All 3 native CTest targets passed.

## UX-004 — Consolidate provider, model, workspace, and view controls

- Status: Implemented locally; component tests and production build passed
- Found from: User feedback after the composer preview
- Problems:
  - Provider/runtime and workspace occupied a full standalone row above the composer.
  - Provider and model were separate controls despite being one selection flow.
  - Text-labelled Chat and CLI switches competed with the chat title.
- Changes:
  - Combined provider and model into one progressive selector with provider and model groups.
  - Moved runtime state into that selector instead of reserving a permanent status row.
  - Moved the workspace basename under the chat title and grouped its open/editor/terminal/
    worktree actions into one contextual folder menu.
  - Replaced Chat and CLI text buttons with accessible icons.
  - Reduced goal, tool, and thinking chrome while retaining state labels and screen-reader
    names.
- Data flow reviewed:
  `chat provider/model/workspace → MainPanel title → ChatView runtime catalog → Composer
  controls → native provider/model/workspace actions`
- Two-hop blast radius:
  - Compact and 2×2 pane layouts.
  - Provider catalog loading, workspace action availability, CLI fallback, and goal state.
- Validation:
  - Main panel, chat view, goal, and message component suites passed.
  - Full frontend suite and production build passed.

## BUG-010 — Ignore a stale ACP model catalog after switching provider

- Status: Fixed locally and validated
- Found from: Composer data-flow review
- Symptom: Immediately after switching providers, the composer could briefly show the
  previous provider's model and reasoning choices under the new provider name.
- Data flow reviewed:
  `provider selection → persisted chat provider → runtime replacement → asynchronous ACP
  catalog → model/reasoning selector`
- Two-hop blast radius:
  - Provider switching, model selection, reasoning normalization, and queued prompts.
  - Full chat state hydration and incremental runtime updates.
- Regression proof against pre-fix code:
  - A delayed ACP catalog from provider A remained visible after the chat switched to
    provider B.
- Fix:
  - Consume ACP capabilities only when their provider still matches the selected chat
    provider.
- Validation:
  - Chat view regression suite and full frontend suite passed.

## BUG-011 — Match workspace action availability to native runtime rules

- Status: Fixed locally and validated
- Found from: Workspace-menu interaction audit
- Symptom: Workspace and worktree actions were disabled for an idle ACP session even though
  native code permits the change, while some genuinely busy states were not described
  consistently.
- Data flow reviewed:
  `runtime lifecycle → serialized chat state → workspace menu availability → native workspace
  and worktree handlers`
- Two-hop blast radius:
  - Idle ACP chats, waiting permissions, queued prompts, active CLI terminals, and worktree
    creation/removal.
- Fix:
  - Use the same effective-busy boundary as native code: block active/waiting/queued work and
    running CLI terminals, but allow idle ACP sessions.
- Validation:
  - Chat view workspace-action tests and full frontend suite passed.

## BUG-012 — Keep newer state when provider or model rollback arrives late

- Status: Fixed locally and validated
- Found from: Independent state-flow audit
- Symptom: A rejected provider/model change could restore an old whole-session snapshot and
  erase a workspace or other field updated while the request was in flight.
- Data flow reviewed:
  `optimistic selector update → native request → concurrent chat patch → rejected response →
  frontend rollback → persisted chat`
- Two-hop blast radius:
  - Provider and model selectors, workspace changes, incremental chat patches, and retry.
  - Generic ACP reasoning and command-safety defaults after provider changes.
- Regression proof against pre-fix code:
  - A delayed rejected model/provider request replaced a newer workspace value with the
    pre-request value.
- Fix:
  - Roll back only the rejected fields on the current session instead of replacing the
    session snapshot.
  - Preserve supported generic ACP reasoning and command-safety defaults.
- Validation:
  - Store regression suite and full frontend suite passed.

## BUG-013 — Stop phantom-running terminals after transport failure

- Status: Fixed locally and validated
- Found from: Independent terminal-runtime audit
- Symptom: A failed terminal read, write, or missing-output path could leave the chat marked
  as running even though no provider process could produce output.
- Data flow reviewed:
  `terminal process → platform transport → polling/write handler → terminal lifecycle →
  serialized chat state → CLI running indicator`
- Two-hop blast radius:
  - Prompt submission, steering, background idle shutdown, provider switching, and relaunch.
  - CLI error visibility and all five terminal-backed provider implementations.
- Regression proof against pre-fix code:
  - Deterministic read/write failures retained the busy lifecycle and allowed automatic
    relaunch.
- Fix:
  - Funnel transport failure through the shared stopped lifecycle, disable automatic
    relaunch, and expose the failure in the transcript/state response.
- Validation:
  - Terminal transport regression tests and all 3 native CTest targets passed.

## BUG-014 — Drain queued ACP prompts after setup and reconnect

- Status: Fixed locally and validated
- Found from: Independent ACP runtime audit
- Symptom: A prompt preserved during new-session, load-session, Codex setup, or requestless
  recovery could remain queued forever after setup succeeded.
- Data flow reviewed:
  `prompt submission → launch/recovery queue → ACP initialize/session setup → model setup →
  queued prompt dispatch`
- Two-hop blast radius:
  - New and resumed structured sessions, provider reconnection, model negotiation, and
    branch/goal continuations.
- Regression proof against pre-fix code:
  - Successful setup retained the prompt in memory without sending a provider request.
- Fix:
  - Drain the existing preserved prompt at the shared successful setup boundaries while
    retaining FIFO order.
- Validation:
  - ACP recovery/ordering regressions and all 3 native CTest targets passed.

## BUG-015 — Resume goals only after continuation work is queued

- Status: Fixed locally and validated
- Found from: Resume/retry lifecycle audit
- Symptom: Resume changed a paused or blocked goal to Active without sending a continuation,
  so the UI said it was running while no output could ever arrive.
- Data flow reviewed:
  `GoalBanner Resume → frontend goal action → CEF resumeGoal → native goal loop → ACP prompt
  queue → goal state push`
- Two-hop blast radius:
  - UAM-managed and provider-managed goals, blocked/paused states, queued prompts, goal review,
    and retry counters.
- Regression proof against pre-fix code:
  - Resume returned success and set Active with zero queued provider requests.
- Fix:
  - Queue the appropriate UAM continuation or provider goal command first; set Active and
    clear review/retry state only after queuing succeeds.
  - Keep the prior state on busy/failure and show Resuming or the returned error in the UI.
- Validation:
  - Native goal regressions, frontend goal/store tests, full frontend suite, and all native
    CTest targets passed.

## BUG-016 — Make CLI steering retryable and preserve terminal identity

- Status: Fixed locally and validated
- Found from: CLI-focused lifecycle audit
- Symptoms:
  - Steering could look accepted before native confirmation, then leave no visible retry path
    on failure.
  - Hydration could briefly detach and restart a terminal because two IDs represented the
    same effective session.
  - Early output could attach to the selected pane instead of its source chat.
- Data flow reviewed:
  `CLI draft → steer request → native input response → steering UI → terminal ID hydration →
  sourceChatId output routing → transcript`
- Two-hop blast radius:
  - Retry/steer controls, draft clearing, terminal attach/detach, chat switching, and 2×2
    concurrent transcript routing.
- Regression proof against pre-fix code:
  - Failed steering had no explicit retry state.
  - Equivalent hydrated terminal IDs triggered an avoidable lifecycle transition.
  - Source-tagged output could be written to the currently selected chat.
- Fix:
  - Use explicit Steer now, Steering…, and Retry steer states; clear the draft only after
    acceptance and show failures inline.
  - Resolve one effective terminal ID and route source-tagged output before pane binding.
- Validation:
  - CLI regression suite: 7 tests passed.
  - Full frontend suite and production build passed.

## BUG-017 — Explain the quiet gap before the first streamed event

- Status: Fixed locally and validated
- Found from: Message-flow audit
- Symptom: Between prompt acceptance and the first streamed event, a chat could be genuinely
  processing but show no activity at all.
- Data flow reviewed:
  `prompt acceptance → native processing state → incremental chat update → turn-event stream
  → ChatView message timeline`
- Two-hop blast radius:
  - Slow provider startup, reconnect/setup, four-pane monitoring, and retry/resume feedback.
- Fix:
  - Show one lightweight, non-persisted Starting… row only while processing and before any
    event exists; remove it as soon as real output arrives.
- Validation:
  - Chat view regression suite and full frontend suite passed.

## BUG-018 — Honour the saved CLI idle shutdown timeout

- Status: Fixed locally and validated
- Found from: Final terminal lifecycle review
- Symptom: The terminal poller used a hard-coded 60-second default and ignored the user's
  persisted CLI idle timeout.
- Data flow reviewed:
  `settings persistence → app state → terminal polling → background-idle eligibility →
  terminal shutdown`
- Two-hop blast radius:
  - Every terminal-backed provider, background chats, active chat protection, and resumed
    terminal sessions.
- Regression proof against pre-fix code:
  - A non-default saved timeout did not alter the eligibility decision.
- Fix:
  - Read the timeout directly from the shared app settings at the one lifecycle eligibility
    boundary.
- Validation:
  - Terminal timeout regression and all 3 native CTest targets passed.

## UX-005 — Remove empty pin space and tighten sidebar hierarchy

- Status: Implemented locally; regression tests and production build passed
- Found from: User feedback during native visual inspection
- Problems:
  - Every unpinned chat reserved a blank pin column, shortening titles without communicating
    state.
  - Collection, workspace, and chat rows used more vertical spacing and indentation than
    their hierarchy required.
- Changes:
  - Unpinned rows reserve no pin space.
  - Pin/Unpin and More actions share one overlay revealed by hover or keyboard focus.
  - Pinned chats retain a passive accessible marker.
  - Tightened section, collection, workspace, chat, and scroll padding.
  - Reduced collection nesting and replaced layout-consuming drag borders with inset
    indicators.
- Two-hop blast radius:
  - Pinned and unpinned sections, branch-family rows, timestamps, provider/pane/status icons,
    keyboard focus, drag/drop, and narrow sidebar widths.
- Validation:
  - Sidebar component suites: 34 tests passed.
  - Full frontend suite and production build passed.

## UX-006 — Give 2×2 panes more transcript and less permanent chrome

- Status: Implemented locally; regression tests and production build passed
- Found from: User feedback during native 2×2 visual inspection
- Problem: Four concurrent chats retained single-chat header, message, tool, goal, and
  composer spacing, leaving too little transcript visible in each pane.
- Changes:
  - Added container-based compact density only for multi-pane chats below 720 px.
  - Reduced pane header, transcript, message, tool, attention, queued-prompt, goal, composer,
    and textarea spacing.
  - Reduced compact composer height from 72 px to 52 px, and to 48 px at 320 px.
  - Kept all controls available; very narrow toolbars scroll horizontally rather than hiding
    dictation or other actions.
- Two-hop blast radius:
  - Chat/CLI switching, scroll anchoring, streaming/starting/retry rows, permissions, goals,
    tool calls, and 320 px rendering.
  - Single-chat layouts remain outside the compact selectors.
- Validation:
  - Dedicated density contract test passed.
  - Full frontend suite: 314 tests passed.
  - Production build passed.

## UX-007 — Rebalance sidebar hierarchy and make tool activity legible

- Status: Implemented locally; regression tests, signed build, and installed-app inspection
  passed
- Found from: User feedback during native visual inspection
- Problems:
  - Chat rows were too small relative to workspace rows.
  - Collection and workspace groups still used too much vertical space.
  - Nested chats were not indented by a full folder-icon width.
  - Tool calls looked like ordinary transcript lines, used too much space, and did not
    communicate that the agent had performed a distinct action.
- Changes:
  - Increased chat rows to a 26 px minimum height with 13 px labels and 16 px provider icons.
  - Removed residual group spacing and tightened collection and workspace headers.
  - Set workspace-chat nesting to 14 px, matching the folder icon width.
  - Reduced transcript and tool-timeline gaps.
  - Replaced the oversized tool treatment with compact bordered rows carrying a wrench,
    visible `TOOL` label, action title, and status.
- Regression proof against pre-fix code:
  - Six focused density expectations failed before the implementation.
- Two-hop blast radius:
  - Pinned chats, collection workspaces, folder sessions, hover actions, timestamps,
    single-chat history, and compact multi-pane history.
- Validation:
  - Focused sidebar/chat suites: 99 tests passed.
  - Full frontend suite: 317 tests passed.
  - Frontend production build and signed macOS application target passed.
  - The installed 4.4.1 app was fully restarted and visually inspected.

## BUG-019 — Do not delete a worktree before its metadata can be persisted

- Status: Fixed locally and validated
- Found from: Independent worktree lifecycle audit
- Symptom: Discarding or porting an isolated chat could physically remove its worktree and
  then fail to save cleared chat metadata, leaving the chat pointing at a path that no longer
  existed.
- Data flow reviewed:
  `workspace menu → worktree operation → Git worktree removal → chat isolation metadata →
  provider history persistence → settings persistence → workspace resolution`
- Two-hop blast radius:
  - Discard and port operations, managed and existing Git repositories, restart hydration,
    workspace resolution, and subsequent editor/terminal actions.
- Regression proof against pre-fix code:
  - With a deterministic unwritable data root, discard returned failure only after deleting
    the worktree and clearing the in-memory isolation fields.
- Fix:
  - Build and persist the cleared metadata before any destructive Git/filesystem step.
  - Leave the live chat and worktree untouched when persistence fails.
  - Restore persisted metadata if the first physical removal fails; keep cleared in-memory
    metadata after later partial cleanup so the app cannot point at deleted data.
- Validation:
  - Worktree create/discard/port regression passed.
  - Native core test executable reported no failures after the fix.

## BUG-020 — Roll back rejected ACP mode changes before sending prompts

- Status: Fixed locally and validated
- Found from: Independent ACP runtime audit
- Symptoms:
  - A provider-rejected mode change left the runtime and selector on the rejected mode.
  - A queued prompt could be sent before the provider acknowledged its required mode.
- Data flow reviewed:
  `approval/safety selector → persisted chat mode → ACP session/set_mode request → provider
  response → runtime mode → queued prompt dispatch → serialized controls`
- Two-hop blast radius:
  - Approval-mode and command-safety changes, prompt startup, provider rejection, transport
    failure, concurrent changes, and incremental state pushes.
- Regression proof against pre-fix code:
  - A rejected `plan` request left `current_mode_id` set to `plan`.
  - A queued prompt received its request ID before the mode acknowledgement.
- Fix:
  - Track the pending request, requested mode, confirmed runtime mode, and optional prior
    persisted selector/safety values as one transaction.
  - Roll back on provider rejection or transport failure without overwriting a newer user
    selection.
  - Block concurrent mode changes and prompt dispatch until acknowledgement, then drain the
    existing queued prompt on success.
- Validation:
  - Mode rejection/transport rollback and acknowledgement-order regressions passed.
  - All 3 native CTest targets passed.
  - Full signed macOS application target passed.

## BUG-021 — Keep newer chat metadata when a pin request rolls back

- Status: Fixed locally and validated
- Found from: Independent persistence race audit
- Symptom: If saving a pin change failed after a newer backend update arrived, the rollback
  replaced the entire chat snapshot and erased the newer name, provider, model, and timestamp.
- Data flow reviewed:
  `sidebar pin action → optimistic store update → CEF pin request → native persistence →
  incremental chat patch → rejected request rollback`
- Two-hop blast radius:
  - Pinned/unpinned sidebar sections, backend chat rename, provider/model changes, and
    timestamp-based ordering.
- Regression proof against pre-fix code:
  - A delayed pin failure changed `Renamed by backend` back to `Original name`, restored the
    old provider, removed the new model, and reverted `updatedAt`.
- Fix:
  - Roll back only `isPinned` on the current chat object, matching the native field-level
    transaction.
- Validation:
  - Focused pin regressions: 4 tests passed.
  - Full store suite: 79 tests passed.
  - Frontend production build passed.

## BUG-022 — Prevent buffered CLI output crossing terminal identities

- Status: Fixed locally and validated
- Found from: Independent retry/resume transcript routing audit
- Symptom: A delayed output flush from an old terminal could restore its transcript after a
  state patch rebound the chat to a new terminal.
- Data flow reviewed:
  `native terminal output → CEF push → 32 ms transcript buffer → terminal state patch →
  binding reconciliation → delayed store flush → CLI view`
- Two-hop blast radius:
  - CLI retry, resume, steering, terminal replacement, chat switching, and later CLI mounts
    in single- and multi-pane layouts.
- Regression proof against pre-fix code:
  - `term-new` became the active binding while the store retained
    `term-old: "old outputstale tail"`.
- Fix:
  - Clear a stored transcript when an authoritative patch changes terminal identity.
  - Drop buffered chunks whose terminal ID no longer matches the active binding.
- Validation:
  - Targeted store, CLI, and reconciliation suites: 89 tests passed.
  - Full frontend suite: 316 tests passed.
  - Frontend production build passed.

## BUG-023 — Time out stalled structured-runtime setup

- Status: Fixed locally and validated
- Found from: Independent provider/runtime lifecycle audit
- Symptom: A provider process that launched but never completed initialization or session
  setup could remain in `starting` indefinitely, leaving queued work suspended.
- Data flow reviewed:
  `provider launch → initialize → authentication/session setup → runtime activity →
  poll lifecycle → queued prompt dispatch → reconnect`
- Two-hop blast radius:
  - Structured sessions, background model discovery, queued prompts, active goals,
    reconnect limits, permissions, and user-input waits.
- Regression proof against pre-fix code:
  - A session with an initialize request and an hour of setup inactivity remained running
    after polling.
- Fix:
  - Start an activity clock when the provider process launches.
  - After 60 seconds without setup activity, stop and close the process, clear phantom
    processing, expose a diagnostic error, and use the existing bounded reconnect policy.
  - Preserve queued prompts; block an affected active goal; record model-discovery failure
    without reconnecting.
  - Exclude intentional permission and user-input waits.
- Validation:
  - Setup-timeout regression passed.
  - All 3 native CTest targets passed.
  - Full signed macOS application target passed.

## BUG-024 — Keep chats and folders when history deletion fails

- Status: Fixed locally and validated
- Found from: Native lifecycle and restart-integrity audit
- Symptom: A locked or unwritable metadata file made deletion fail on disk, but the app still
  removed the chat or folder from the UI and returned success. The metadata could then load
  again after restart.
- Data flow reviewed:
  `delete action → chat/folder lifecycle → branch reparenting → local metadata deletion →
  native provider history deletion → settings/folder persistence → restart hydration`
- Two-hop blast radius:
  - Single-chat deletion, folder deletion, branch children, selected-chat fallback,
    provider-native overlays, folder metadata, and restart recovery.
- Regression proof against pre-fix code:
  - With the chat metadata directory made read-only, both single-chat and folder deletion
    returned success and removed the live objects while their metadata files still existed.
- Fix:
  - Treat local and native history removal as preconditions before mutating live UI state.
  - Restore original chat and folder persistence if a later precondition fails.
  - Return a recoverable error and keep the chat/folder visible instead of committing a
    partial deletion.
- Validation:
  - Single-chat and folder deletion-failure regressions passed.
  - Native core test target passed.

## UX-008 — Progressive-disclosure working transcript

- Status: Implemented locally and validated
- Found from: User feedback and transcript visual audit
- Problems:
  - Thinking, command, and progress rows overwhelmed four-pane chats.
  - Persisted history and live turns used separate render paths, so a compact treatment could
    silently revert after reopening a chat.
  - User messages stretched across the transcript and tool output exposed raw escape codes.
- Data flow reviewed:
  `ACP events → live turn timeline → persisted message blocks → state serialization →
  reopened chat → message/tool presentation`
- Changes:
  - Added a locally persisted Compact working / Verbose working preference; Compact is the
    new default and Verbose preserves the detailed chronological view.
  - Compact mode keeps the full chronological thinking, tool, progress, permission, and
    question UI visible while a turn is running.
  - Only completed background work collapses to `Worked for …`; opening it restores the same
    proper thinking cards, tool rows, titles, statuses, and details shown live.
  - Applied the same completed-work grouping to persisted blocks and sub-agent history while
    leaving the final assistant response outside the disclosure.
  - Reworked verbose thinking into a compact brain row, tightened tool rows and details, and
    stripped ANSI/escaped newline noise from tool output.
  - Made user messages content-sized bubbles with readable single- and multi-pane gutters.
- Validation:
  - Regression coverage verifies compact live and reopened history, latest-update selection,
    verbose rows, tool output cleanup, user-message width, and local preference persistence.
  - Full frontend suite: 328 tests passed.

## BUG-025 — Persist structured-turn duration for reopened chats

- Status: Fixed locally and validated
- Found from: Compact-working data-flow review
- Symptom: The frontend timer disappeared as soon as a turn completed, so reopened chats
  could only display `Worked for 0s`.
- Data flow reviewed:
  `prompt start → ACP session clock → assistant message → chat repository → CEF serializer →
  frontend sanitizer/reconciler → compact transcript`
- Fix:
  - Record one native turn start time at the shared prompt boundaries.
  - Store the elapsed milliseconds on the completed assistant message using the existing
    persisted `processing_time_ms` field.
  - Serialize, sanitize, reconcile, and display that value in both live and reopened history.
- Validation:
  - Native completion regression verifies the stored duration and clock reset.
  - All 3 native CTest targets passed.

## BUG-026 — Preserve custom palette colors during app theme sync

- Status: Fixed locally and validated
- Found from: Theme data-flow review
- Symptom: App-level theme synchronization called the theme applicator without the saved
  custom theme list, clearing custom CSS colors immediately after selection or hydration.
- Data flow reviewed:
  `settings/custom theme storage → Zustand theme state → App effect → document CSS tokens`
- Fix:
  - Pass the current custom theme collection through the shared app-level theme effect.
- Validation:
  - Theme token contract and custom-theme application tests passed.

## UX-009 — Add a restrained Focus default and preserve OG

- Status: Implemented locally and validated
- Found from: User feedback
- Changes:
  - Replaced the overly monochrome treatment with a layered near-black `Focus` theme that
    uses a restrained ember-orange interaction accent, blue/teal information cues,
    and clear green/yellow/red semantic states.
  - Made Focus the fresh-install and invalid-setting fallback across native and React
    settings, initial HTML, and first paint, and migrate the short-lived `mono` preference.
  - Kept the existing vibrant orange/green dark palette unchanged and renamed it `OG`.
  - Kept all other built-in and custom themes available.
- Validation:
  - Frontend theme storage/token suites, the full 328-test frontend suite, production build,
    native normalization tests, and all 3 native CTest targets passed.

## UX-010 — Clarify the out-of-date Updates panel

- Status: Implemented locally and validated
- Found from: User-requested visual pass
- Problems:
  - Available updates lacked a clear hierarchy between installed and available versions.
  - Installation, release notes, refresh, and dismissal competed as similarly weighted
    controls.
- Changes:
  - Added one update-count summary, explicit Current → Available version labels, a visible
    primary Install update/View release action, secondary release notes, and a persistent
    Check again footer with a clear loading state.
  - Kept per-update and dismiss-all controls accessible without dominating the card.
- Validation:
  - Component fixtures verify available, installable, current, loading, refresh, and
    up-to-date states.

## UX-011 — Put branch navigation at the divergence message

- Status: Implemented locally and validated
- Found from: User feedback and branch data-flow review
- Problem:
  - A transcript-level branch selector forced users to scroll to the top and obscured which
    message created the alternate path.
- Data flow reviewed:
  `edit/retry message → parent chat + branch message index → sibling branch collection →
  active chat selection → transcript rendering`
- Changes:
  - Removed the global transcript branch selector.
  - Added previous/next controls and the current branch count beneath the user message where
    the branch diverges.
  - Reused the message hover/focus action strip, while keeping the controls visible on touch
    devices and preserving copy, edit, retry, keyboard focus, and accessibility labels.
- Validation:
  - Regression coverage verifies root plus sibling grouping, both navigation directions, and
    placement on the divergence message.
  - Chat view suite: 62 tests passed.
  - Full frontend suite: 328 tests passed.

## BUG-027 — Keep the displayed app version aligned with the packaged version

- Status: Fixed locally and validated
- Found from: Local packaging review
- Symptom: The package and macOS bundle were set to 4.4.1, while the native state and
  frontend fallback still displayed 4.4.0.
- Data flow reviewed:
  `CMake/package version → native app state → CEF settings payload → frontend fallback →
  About/settings display`
- Fix:
  - Aligned the native version constant, frontend fallback, test fixture, and documented
    development line with 4.4.1.
- Validation:
  - Settings version regression, production build, and installed bundle metadata verified.

## UX-012 — Rescan external Codex chats from a workspace folder

- Status: Implemented locally and validated
- Found from: User request after working in a separate Codex instance
- Problem:
  - Chats created outside UAM did not appear until they were imported through another path.
  - A global discovery scan could affect unrelated workspaces, while a sidebar reload could
    accidentally move the selected chat or clear unsent composer text.
- Data flow reviewed:
  `folder context menu → Zustand CEF request → targeted Codex rollout scan → native identity
  deduplication → local chat repository → sidebar reload → selected chat restoration`
- Fix:
  - Added `Rescan chats` to each workspace folder's right-click and overflow menu.
  - Stream Codex session and archived-session JSONL files, importing only root chats whose
    recorded working directory matches the selected folder.
  - Exclude Codex sub-agent rollouts and synthetic environment messages.
  - Reuse provider + workspace + native session identity so repeat scans do not duplicate
    chats.
  - Preserve the selected chat and unsent composer text while refreshing the sidebar.
  - Linked the existing imported-title helper into the desktop target after the package build
    exposed that missing target dependency.
- Validation:
  - Native regression verifies workspace scoping, sub-agent exclusion, parsed user content,
    persisted Codex identity, and idempotent repeat scans.
  - Frontend regressions verify the folder-menu action and exact CEF request.
  - Full frontend suite: 330 tests passed.
  - All 3 native CTest targets and the signed 4.4.1 production bundle build passed.

## BUG-028 — Settings opened a global-only memory library

- Status: Fixed locally and validated
- Found from: User report
- Symptom:
  - The homepage brain button opened the complete memory library, while Settings opened a
    separate global-only scope without Collections or Workspaces.
- Data flow reviewed:
  `homepage brain / Settings button → memory library store action → CEF memory scope →
  MemoryLibraryModal location sidebar`
- Fix:
  - Route both buttons through the existing `openAllMemoryLibrary` action.
  - Update the Settings description to accurately describe Global, Collections, and
    Workspaces.
- Validation:
  - Settings regression verifies the all-memory action is called and the global-only action
    is not.
  - Settings suite: 25 tests passed; full frontend suite: 330 tests passed.
  - Signed 4.4.1 bundle built, installed locally, and visually verified with Global,
    Collections, and Workspaces in the memory sidebar.

## BUG-029 — Transcript padding constrained both sides of every message

- Status: Fixed locally and validated
- Found from: User report
- Symptom:
  - A transcript-wide horizontal inset reduced the usable width of both message directions.
  - User messages needed only a left-side limit while staying right-aligned; assistant
    messages needed only a right-side limit while staying left-aligned.
- Data flow and two-hop blast radius reviewed:
  `ChatView transcript wrapper → shared message frame width → single and multi-pane rendering
  → readable text area in concurrent chat grids`
- Fix:
  - Removed horizontal padding from the transcript content while preserving the composer
    inset.
  - Kept user bubbles content-sized and right-aligned with a guaranteed opposite-edge
    gutter.
  - Kept assistant responses left-aligned with a matching opposite-edge gutter.
  - Reduced the opposite-edge gutter in narrow multi-pane layouts.
- Validation:
  - Added a red-first layout regression covering transcript, composer, user, assistant, and
    multi-pane spacing.
  - Layout regression: 5/5 tests passed.

## BUG-030 — Completed compact working summaries always displayed zero seconds

- Status: Fixed locally and validated
- Found from: User report
- Symptom:
  - `Worked for X` correctly updated during generation but changed to `Worked for 0s` as soon
    as the turn completed, including newly created turns.
- Data flow and two-hop blast radius reviewed:
  `native turn timer → persisted assistant processingTimeMs → CEF state reconciliation →
  completed turn timeline → compact working summary`
- Root cause:
  - The completed React timeline stopped using the cleared live start timestamp but did not
    fall back to the assistant message's persisted processing duration.
- Fix:
  - Use the live clock only while the ACP turn is processing.
  - Use the persisted assistant `processingTimeMs` after completion across every timeline
    placement.
- Validation:
  - Added a red-first regression reproducing a completed 83-second turn rendering as zero;
    it now renders `Worked for 1m 23s`.
  - Existing native regression continues to verify that completed turns persist their
    processing duration.
  - Full frontend suite: 331/331 tests passed across 32/32 files.
  - Native CTest: 3/3 targets passed.
  - Frontend production build and signed 4.4.1 macOS bundle build passed.

## BUG-031 — Provider progress plans produced repeated Plan cards

- Status: Fixed locally and validated
- Found from: User report
- Symptom:
  - Ordinary provider plan updates and repeated plan markers were rendered as multiple
    dedicated Plan cards.
  - Historical or non-actionable plans could look like decisions awaiting user input.
- Data flow and two-hop blast radius reviewed:
  `provider plan events → ACP turn/message blocks → ChatView Plan-mode eligibility →
  PlanBlock actions → transcript decision state`
- Fix:
  - Show the dedicated Plan card only for an actionable Codex turn explicitly in Plan mode.
  - Keep ordinary task-progress and historical plans out of the decision UI.
  - Collapse repeated live or persisted plan markers to the latest actionable card.
- Validation:
  - Added red-first regressions for non-actionable plans and repeated plan markers.
  - Focused plan tests: 10/10 passed.

## BUG-032 — Every thinking block pulsed for the duration of an active turn

- Status: Fixed locally and validated
- Found from: User report while running OpenCode
- Symptom:
  - Older thinking blocks continued flashing and pulsing whenever the overall assistant turn
    was active, even after a newer thought, tool call, permission, or text event took over.
- Data flow and two-hop blast radius reviewed:
  `provider turn events → ordered shared timeline → current event index → ThinkingBlock
  data-active state → provider-neutral animation styling`
- Fix:
  - Mark a thinking block active only when it is the latest event in the active turn.
  - Stop its animation immediately when any later event becomes current.
- Validation:
  - Added a red-first ordered-thinking regression and updated the older turn-wide activity
    expectation.
  - Thinking component tests: 9/9 passed.
  - Full frontend suite: 333/333 tests passed across 32/32 files.
  - Native CTest: 3/3 targets passed.
  - Frontend production build and signed 4.4.1 macOS bundle build passed.

## BUG-033 — OpenCode rejected UAM's default ACP mode alias

- Status: Fixed locally and validated
- Found from: Two user-provided OpenCode 1.17.15 ACP diagnostics
- Symptom:
  - Changing back to the normal agent mode sent `session/set_mode` with
    `modeId=default`.
  - OpenCode advertised its normal mode as `build` and rejected `default` with JSON-RPC
    error `-32602`.
- Data flow and two-hop blast radius reviewed:
  `OpenCode configOptions(build) → UAM neutral default mode → setChatApprovalMode →
  provider ACP mode mapper → session/set_mode`
- Root cause:
  - OpenCode inherited the generic ACP mode mapper, which passed UAM's cross-provider
    `default` alias through unchanged.
- Fix:
  - Translate only OpenCode's outbound `default` mode to its advertised `build` mode at the
    provider boundary.
  - Preserve `plan` and OpenCode's other provider-specific mode IDs unchanged.
- Validation:
  - Added a red-first provider regression proving `default → build` and `plan → plan`.
  - Full frontend suite: 333/333 tests passed across 32/32 files.
  - Native CTest: 3/3 targets passed.
  - Frontend production build and signed 4.4.1 macOS bundle build passed.
- Related investigation:
  - The reported branch context loss is separate from this mode error. Message branching
    deliberately clears the native session ID, creates a new ACP session, copies local
    transcript messages only for display, and retries the selected user message. The new
    OpenCode runtime therefore does not inherit the parent runtime's hidden conversation
    state.

## BUG-034 — OpenCode message branches lost their preceding conversation context

- Status: Fixed locally and validated
- Found from: User report after editing and re-sending a message
- Symptom:
  - A branched chat displayed the copied conversation in UAM, but OpenCode behaved as if the
    re-sent message had started a new conversation.
- Data flow and two-hop blast radius reviewed:
  `branch source transcript → truncated local branch → ACP retry queue → fresh OpenCode
  session/new → session/prompt`
- Root cause:
  - Branch creation correctly removed messages after the selected user message and cleared
    the parent native session ID, but `RetryLastAcpPrompt` sent only the final user message
    to the fresh runtime. Earlier messages existed only in UAM's display state.
  - OpenCode 1.17.15 advertises ACP `session/fork`, but that method can only fork the complete
    current session. It cannot select a message checkpoint, so using it would incorrectly
    leak later messages and the superseded response into historical or edited branches.
- Fix:
  - The shared ACP branch retry now sends the already-truncated prior user, assistant, and
    system transcript as context before the selected current user message.
  - The original chat remains unchanged, the branch still receives a new native session,
    and messages after the branch point are excluded.
- Validation:
  - Added a red-first OpenCode regression proving that the fresh session receives both
    earlier user and assistant context, receives the retried message, does not receive a
    later superseded response, and does not duplicate the local user message.
- Full frontend suite: 333/333 tests passed across 32/32 files.
- Native CTest: 3/3 targets passed.
- Frontend production build and signed 4.4.1 macOS bundle build passed.

## UX-005 — Chat/CLI composer spacing polish pass

- Status: Fixed and regression-tested locally
- Found from: User-provided layout feedback with annotated chat screenshots
- Symptom:
  - The shared `.uam-chat-content` wrapper capped both the transcript and composer at 960px and
    applied horizontal padding, leaving excessive empty space beside messages in fullscreen.
  - Terminal steering controls lacked a dedicated bordered composer shell in the currently used Codex CLI
    mode.
- Changes:
  - Removed the shared transcript/composer max-width and horizontal inset from `.uam-chat-content`.
  - Scoped the 960px centered width constraint to `.uam-composer-region`, preserving the intended composer
    breathing room without narrowing the transcript.
  - Added transcript-only gutters of 6px in a full chat and 5px in multi-chat—exactly half of the previous
    12px/10px values—so messages do not touch the pane edge without recreating the oversized outer border.
  - Capped both assistant and user message bubbles at 75% of the transcript width: assistant messages remain
    left-aligned, while user messages remain right-aligned.
  - Reintroduced a bordered, inset steering composer surface in `CLIView` and increased terminal/steering
    inset padding to separate the active steer box from panel edges.
- Validation:
  - Added a regression assertion covering transcript width, composer-only constraints, multi-pane transcript
    padding, and the 75% message-bubble caps in full and multi-chat layouts.
  - Red-first checks each failed 1 of 5 assertions before the wrapper-scope correction and refined half-size
    gutters, then passed all 5 after their respective CSS changes.
  - Computer Use verification of the rebuilt 4.5.0 app confirmed the 6px fullscreen gutter, independently
    inset composer, and left/right-aligned 75%-maximum assistant/user bubbles.

## BUG-035 — Large histories and worktree actions could freeze the app window

- Status: Fixed locally and validated
- GitHub issue: #176
- Found from: User report of intermittent hangs while opening and closing worktrees
- Data flow and two-hop blast radius reviewed:
  `worktree UI action → CEF handler → Git/filesystem operation → chat persistence → state
  fingerprint/patch → React reconciliation → compact transcript rendering`
- Root causes:
  - Worktree status/create/discard/port ran Git, filesystem, and large-chat persistence work
    synchronously on the CEF UI thread.
  - Metadata-only chat changes included `updated_at` in the message fingerprint, causing a
    complete selected transcript to be serialized and sent again.
  - ACP streaming used an earliest-deadline save, repeatedly serializing large histories
    during an active stream.
  - OpenCode CLI rebinding and sidebar refreshes repeatedly parsed full chat files, including
    a redundant second history load.
  - Startup native-history discovery still loaded every complete local transcript while
    applying local metadata overlays. A 79 MB chat kept the main thread at ~99% CPU before
    CEF could expose a usable window.
  - Compact completed-work groups still mounted every hidden thinking/tool row, and persisted
    tool output was included eagerly in every state payload.
  - An idle selected ACP process kept the entire app on a 16 ms poll interval.
- Fix:
  - Move worktree status and mutation work off the CEF UI thread, with one in-flight operation
    per chat and a guarded UI-thread merge of only the resulting isolation metadata. The UI
    handoff copies only the chat ID; the worker loads that one transcript itself instead of
    copying a potentially multi-megabyte chat on the UI thread.
  - Exclude metadata timestamps from the loaded-message fingerprint while still detecting
    same-length transcript edits.
  - Use trailing debounce for streaming chat saves.
  - Write and consume lightweight sidebar summary files; use summaries for OpenCode polling
    and remove the redundant OpenCode history load.
  - Apply native-history overlays from those summaries and hydrate an individual local
    transcript only when its newer messages must override the native copy.
  - Do not mount compact working rows until expanded, and fetch persisted tool details only
    when their detail view opens.
  - Reserve 16 ms polling for active selected work; idle runtimes use the normal interval.
- Regression coverage:
  - Metadata-only state patches do not resend messages, while real transcript edits do.
  - Persisted tool output is absent from state payloads and loaded on demand.
  - Sidebar summaries remain small and preserve message counts without loading messages.
  - Native-history overlay regression proves newer local messages are still hydrated and
    preserved on demand.
  - ACP streaming saves use trailing debounce.
  - Compact completed-work groups do not mount hidden tool/thinking rows.
  - Performance-repair frontend checks: 10/10 passed.
  - Performance-repair native checks: 9/9 passed.
  - Full frontend suite: 369/369 tests passed across 32/32 files.
  - Native CTest: 3/3 targets passed.
  - Frontend production build and signed 4.4.2 macOS bundle build passed.
  - Computer Use launched the exact `Builds/universal_agent_manager.app` bundle. Before the
    startup repair it remained at ~99% CPU and timed out; after the repair it rendered the
    2×2 chat UI, switched workspace groups and chats without hanging, and settled at 0.0–0.1%
    CPU with ~1.0% memory.

## BUG-036 — Updating one runtime animated every provider and used the wrong installer

- Status: Fixed locally and validated
- GitHub issue: #177
- Found from: User report and the failed OpenCode updater log
- Symptom:
  - Starting one provider update put every provider row into its loading animation.
  - The animation eventually stopped without confirming success or showing the installer
    failure.
  - Updating Homebrew-managed OpenCode ran npm and failed with `EEXIST` against Homebrew's
    `/opt/homebrew/bin/opencode` symlink.
- Data flow and two-hop blast radius reviewed:
  `provider update row → shared update monitor → CEF install request → CLI compatibility
  service → package-manager process → native provider state → update result UI`
- Root causes:
  - The Updates panel used one global `some(provider.running)` flag as every row's loading
    state.
  - The native updater always built an npm global-install command, regardless of where the
    active executable was installed.
  - Completed install output was available only in Settings and disappeared from the
    Updates panel when the update row changed.
- Fix:
  - Bind loading animation and `aria-busy` to the matching provider only; keep sibling
    update actions safely disabled without animating them.
  - Detect npm, Homebrew formula, or Homebrew cask ownership from the resolved executable
    path and use the matching installer.
  - Compare Homebrew-managed providers against the Homebrew release channel.
  - Preserve and display provider-specific success or failure status and installer output
    after the task completes.
- Validation:
  - Added red-first UI regressions for targeted row animation and persistent failure output.
  - Added catalog coverage for Homebrew-managed OpenCode.
  - Added native regressions for npm, Homebrew formula, Homebrew cask, and install-method
    detection.
  - Focused updater regressions: 11/11 passed.
  - Full frontend suite: 372/372 tests passed across 32/32 files.
  - Native CTest: 3/3 targets passed.
  - Frontend production build and signed 4.5.0 macOS bundle build passed.
  - Computer Use launched the exact rebuilt bundle and confirmed that the live Updates panel
    opens without a runtime error and resolves Homebrew-managed OpenCode to Homebrew 1.18.0
    instead of npm 1.18.5.
