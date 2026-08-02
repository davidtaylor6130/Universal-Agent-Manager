# Found Issues

This is the durable bug-swarm ledger for the 4.5.2 release working tree. Every entry below has a
reproduction path and code evidence suitable for recreating it as a GitHub issue. Items that
were only suspicions or design preferences are deliberately excluded.

## Pre-publication evidence audit

- All 98 ledger entries were re-read against the current production path by three independent
  audit passes: 96 are confirmed defects, UX-005 is a confirmed accessibility defect, and
  IMPROVEMENT-004 is an accepted feature request.
- Eight confirmed defects remain unresolved: BUG-SWARM-017, 026, 053, 058, 059, 078, 079, and
  080. They must remain open and must not be closed by the release PR.
- The audit disproved the old “fixed” claim for BUG-SWARM-029: native settings persistence still
  erased Gemini/OpenCode reasoning defaults. That native path is now fixed and covered.
- Every fixed root cause has a deterministic automated regression. CEF handlers that are not
  linked into the unit-test executable have narrow production-source contract checks in addition
  to their frontend/native behavioural coverage. The Windows atomic-path regression is present
  and will execute in the repository's Windows CI matrix.
- A pre-publication privacy/security pass found no chats, project names, credentials, private keys,
  or personal identifiers in the issue evidence. Fourteen new Markdown Store entries did expose
  the local absolute skill-source path; those metadata fields were removed before publication.

## Audit baseline

- Branch at audit start: `codex/release-v4.5.0`
- Baseline frontend tests: 384/384 passed across 32/32 files.
- Baseline frontend production build: passed.
- Baseline native tests: 3/3 CTest targets passed.
- Baseline native build: passed, including the signed macOS application bundle.
- The worktree was already dirty before the swarm; pre-existing changes were preserved.

## Final validation

- Frontend: 441/441 tests passed across 32/32 files.
- Frontend production TypeScript/Vite build: passed.
- Native Debug application and test build: passed; the macOS bundle is signed and verifies.
- Native CTest: 3/3 targets passed.
- `git diff --check`: passed.

## Markdown Store installation audit

- Codex source skills found: 33.
- Existing repository Markdown Store entries: 26.
- Missing Codex skills added without replacing existing entries: 14, including
  `bug-find-swarm`.
- Final repository Markdown Store entries: 40 (33 installed Codex skills plus 7 existing UAM
  or Ponytail entries).
- The live UAM setting `markdown_store_directory` is blank. The repository store is populated,
  but UAM must be pointed at this directory (or another chosen directory) before the running
  application can list it.
- During this path audit, BUG-SWARM-030 confirmed that provider skill imports also collected
  reference Markdown; that importer bug is fixed and regression-covered.

## Speed path audit

The complete Speed path was traced rather than checking only the visible selector:

`Codex model catalog → native parser/cache → CEF state → model-specific menu → optimistic
state → native validation/persistence → ACP turn JSON or terminal argv → later state
reconciliation`

Confirmed failures were found at four separate boundaries:

1. Current catalog fields were not parsed.
2. The UI invented `Flex` even when the selected model did not advertise it.
3. Clearing Speed omitted the ACP field even though Codex treats the previous value as sticky.
4. Terminal fallback never received the selected Speed.

The installed Codex 0.145.0 bundled catalog was inspected directly. GPT-5.6 models advertise
the `priority` service tier (mapped to UAM's existing `Fast` label); the catalog does not justify
offering `Flex` globally. The current upstream schema exposes structured `service_tiers` and
marks `additional_speed_tiers` as deprecated:
<https://github.com/openai/codex/blob/main/codex-rs/protocol/src/openai_models.rs>.

## GitHub Copilot provider audit

The complete Copilot configuration and lifecycle path was traced:

`version probe → compatibility gate → model/effort catalog → Settings/New Chat defaults →
provider-aware persistence → optimistic reconciliation → ACP launch argv → cancellation,
steering, retry, and restart`

The installed Copilot CLI is 1.0.75 and exposes `none`, `minimal`, `low`, `medium`, `high`,
`xhigh`, and `max`. UAM's accepted version floor was older than the ACP reasoning behavior
its UI depends on. GitHub's changelog records Max/all effort support and the ACP reasoning flag
fix in 1.0.60:
<https://github.com/github/copilot-cli/blob/main/changelog.md>.

Copilot's ACP server configuration is fixed when the server starts and cannot be changed per
session:
<https://docs.github.com/en/copilot/reference/copilot-cli-reference/acp-server>.
That lifecycle constraint is the basis of BUG-SWARM-018.

---

## BUG-SWARM-001 — GitHub Copilot `Max` reasoning is erased from new-chat defaults

- Severity: P1
- Status: Fixed in working tree; red-first native regression now passes
- Labels: `bug`, `provider:copilot`, `settings`, `P1`

### Symptom

Selecting `Max` reasoning for GitHub Copilot could create a chat with provider-default
reasoning instead. The UI accepted `Max`, but native defaults normalization treated every
provider as Codex and erased Copilot-only values.

### Reproduction

1. Choose GitHub Copilot.
2. Set the provider's default reasoning effort to `Max`.
3. Create a new Copilot chat or reload the saved defaults.
4. Observe `max` missing from the persisted default/chat.

### Expected

Copilot's supported `max` effort survives settings save, reload, chat creation, and launch.

### Root cause and blast radius

Shared defaults normalization had no provider identity:

`Settings/New Chat → provider defaults parser → settings persistence → chat creation →
Copilot ACP/terminal argv`

### Proof and resolution

- Regression: `NewCopilotChatPreservesMaxReasoningEffort`
- Before fix: `chat.reasoning_effort` was empty.
- Fix: provider-aware normalization is now shared by payload parsing, settings load/save,
  frontend serialization, and chat creation.

---

## BUG-SWARM-002 — A stale native push overwrites a successful Copilot reasoning change

- Severity: P1
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `provider:copilot`, `state`, `P1`

### Symptom

After a Copilot reasoning change succeeded, an older native state push could visibly revert the
selector until a later update arrived.

### Reproduction

1. Start with a Copilot chat at `medium`.
2. Change it to `xhigh` and let the CEF request succeed.
3. Deliver an already-in-flight native state push containing `medium`.
4. Observe the UI revert.

### Root cause and blast radius

The store created pending options for Codex and Copilot, but reconciliation protected only
`codex-cli`:

`reasoning selector → optimistic state → successful CEF response → stale native state →
visible selector and next launch`

### Proof and resolution

- Regression: `keeps pending Copilot reasoning when CEF succeeds before a stale state push`
- Before fix: received `medium` instead of `xhigh`.
- Fix: pending option reconciliation now covers both providers.

---

## BUG-SWARM-003 — Failed provider switches leave Small-model mode enabled

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `providers`, `rollback`, `P2`

### Symptom

When switching providers failed after applying target defaults, the original provider was
restored but the target provider's Small-model mode remained active.

### Reproduction

1. Start with Small-model mode off.
2. Give the target provider a default with Small-model mode on.
3. Switch providers and make the native save fail.
4. Observe the original provider restored with Small-model mode still on.

### Root cause and blast radius

`smallModelMode` was applied by the optimistic provider switch but omitted from its rollback:

`provider selector → target defaults → optimistic mutation → CEF failure → rollback →
prompt batching behavior`

### Proof and resolution

- Regression extends `keeps newer workspace state when a provider switch rolls back`.
- Before fix: expected `false`, received `true`.
- Fix: restore `previousSession.smallModelMode` with the sibling provider fields.

---

## BUG-SWARM-004 — Failed settings writes leave rejected values active in memory

- Severity: P1
- Status: Fixed in working tree; deterministic failure-path audit completed
- Labels: `bug`, `persistence`, `data-integrity`, `P1`

### Symptom

When the settings file could not be written, the UI received an error but several native
handlers retained the rejected values in memory. A later unrelated save could persist values
the user had been told were not saved.

### Reproduction

1. Make the settings path unwritable.
2. Change Memory, provider chat defaults, editor associations, or the Markdown Store path.
3. Observe the request fail.
4. Trigger another state push or settings save.
5. Observe the rejected value still active or subsequently written.

### Root cause and blast radius

Four handlers mutated `m_app.settings` before saving and lacked the rollback used by adjacent
handlers:

- `HandleSetMemorySettings`
- `HandleSetProviderChatDefaults`
- `HandleSetEditorSettings`
- `HandleSetMarkdownStoreDirectory`

`Settings UI → in-memory mutation → failed disk write → later state/save`

### Proof and resolution

There is no isolated CEF callback harness in the native test target. Each handler now snapshots
and restores its previous state when persistence fails, and
`SettingsHandlersRestoreRejectedValuesAfterSaveFailure` guards the exact
snapshot → mutation → failed save → restoration order in all four production handlers.

---

## BUG-SWARM-005 — Provider-default menus are not keyboard operable

- Severity: P2
- Status: Fixed in working tree; red-first accessibility regression now passes
- Labels: `bug`, `accessibility`, `settings`, `P2`

### Symptom

Keyboard-only users could open the default Model, Reasoning, Agent, Provider, or Speed menu in
Settings, but could not focus, navigate, or select its options.

### Reproduction

1. Open Settings → Chat Defaults → Codex default Speed.
2. Focus the control and press Enter.
3. Press Arrow Down or Tab.
4. Observe that no option receives focus and arrow keys do nothing.

### Root cause and blast radius

Settings had a bespoke click-only listbox instead of the application's keyboard-capable menu:

`Settings defaults → inaccessible menu → provider default persistence → every new chat`

### Proof and resolution

- Regression: `changes provider chat defaults with the keyboard`
- Before fix: the selected option was expected to be focused; `document.body` remained active.
- Fix: the bespoke implementation was removed and the existing `MenuSelect` reused.

---

## BUG-SWARM-006 — Speed can change while runtime configuration is locked

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `speed`, `runtime-state`, `P2`

### Symptom

During an active provider request the Speed dropdown was disabled, but the active Speed chip
and `/speed` command still changed the stored value.

### Reproduction

1. Start a Codex response with Speed set.
2. While processing, click the chip's clear action or enter `/speed fast`.
3. Observe Speed mutate despite the disabled options control.

### Root cause and blast radius

The runtime lock existed only on the dropdown:

`chip/slash command → optimistic store → CEF persistence → next provider request`

### Proof and resolution

- Regression: `blocks speed changes from chips and slash commands while Codex is processing`
- Before fix: the Speed clear button remained active.
- Fix: the chip, slash-command path, store, and native handler now enforce the same active-work
  lock.

---

## BUG-SWARM-007 — New Chat hides model-discovery failure and blocks retry

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `models`, `new-chat`, `P2`

### Symptom

If provider model discovery failed in New Chat, loading stopped without an error. Switching
providers away and back did not retry because the failed provider remained permanently marked
as requested.

### Reproduction

1. Open New Chat with an existing provider chat available.
2. Make model discovery fail.
3. Observe no error or Retry action.
4. Switch away and back; observe no second request.

### Root cause and blast radius

The modal treated “request started” as “request completed” and ignored the serialized
`modelRefreshError`:

`provider picker → discovery failure → stale/empty model list → chat creation → provider launch`

### Proof and resolution

- Regression: `surfaces failed model discovery and lets the user retry`
- Before fix: no alert was rendered.
- Fix: failed requests leave the dedupe set, and the existing error now renders with Retry.

---

## BUG-SWARM-008 — Repeating the same terminal error after Retry stays hidden

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `terminal`, `error-handling`, `P2`

### Symptom

After dismissing a terminal steering error, retrying and receiving the same error text produced
no visible feedback.

### Reproduction

1. Cause terminal steering to fail.
2. Dismiss the banner.
3. Retry and return the same error.
4. Observe the banner remain hidden.

### Root cause and blast radius

Dismissal was keyed only by text, and a new attempt did not clear it:

`Retry/steer → backend failure → same error string → stale dismissal predicate`

### Proof and resolution

- Regression: `shows the same terminal error again when a retry fails the same way`
- Fix: a new explicit attempt clears the previous dismissal.

---

## BUG-SWARM-009 — Live terminal palette does not follow app theme

- Severity: P3
- Status: Fixed in working tree; red-first frontend regression now passes
- Labels: `bug`, `terminal`, `theme`, `P3`

### Symptom

An open xterm retained its original palette after the app moved between Light, Dark, and Focus
themes.

### Reproduction

1. Open a CLI session in Light mode.
2. Switch to Dark mode.
3. Observe the shell update while the terminal canvas keeps the light palette.

### Root cause and blast radius

The palette was set only in the terminal constructor. The theme effect only called `fit()`:

`global theme → React shell → existing xterm canvas`

### Proof and resolution

- Regression: `updates the live terminal palette when the app theme changes`
- Fix: the shared palette function now updates `terminal.options.theme` before fitting.

---

## BUG-SWARM-010 — Generic ACP cancellation always falls through to a hard restart

- Severity: P1
- Status: Fixed in working tree; red-first native regression now passes
- Labels: `bug`, `acp`, `cancellation`, `P1`

### Symptom

Cancelling or steering a Copilot, Gemini, or OpenCode turn waited for the five-second cancel
timeout and restarted the provider even when the original prompt returned a normal cancelled
result.

### Reproduction

1. Track prompt request ID 7 for a generic ACP provider.
2. Call `CancelAcpTurn`.
3. Process a cancelled response for request 7.
4. Observe it diagnosed as unknown; cancellation remains pending until restart.

### Root cause and blast radius

Generic cancel is an untracked notification. UAM erased the original prompt request ID before
the only completion response arrived:

`Stop/Steer → cancel notification → original prompt response discarded → timeout →
process/session restart → queued prompt replay`

### Proof and resolution

- Regression: `GenericAcpCancelCompletesOnOriginalPromptResponseWithoutRestart`
- Before fix: expected prompt request 7, received 0.
- Fix: generic cancellation retains the original prompt identity and treats its response as
  cancel completion without normal goal completion.

---

## BUG-SWARM-011 — Copilot versions 1.0.51–1.0.59 are incorrectly marked compatible

- Severity: P2
- Status: Fixed in working tree; red-first compatibility regressions now pass
- Labels: `bug`, `provider:copilot`, `compatibility`, `P2`

### Symptom

UAM accepted Copilot CLI 1.0.51 while exposing reasoning behavior that only became reliable in
1.0.60. Older accepted installations could reject newer efforts or ignore ACP reasoning flags.

### Reproduction

1. Install Copilot CLI 1.0.59.
2. Let UAM run its compatibility check.
3. Observe the provider marked supported.
4. Select a new reasoning effort and start ACP.

### Root cause and blast radius

The compatibility floor tracked earlier session support, not the current ACP reasoning contract:

`version probe → supported state → effort UI → ACP launch argv → ineffective configuration`

### Proof and resolution

- Before fix: compatibility tests accepted 1.0.51.
- Fix: minimum supported Copilot version and user-facing install guidance are now 1.0.60.
- Current installation checked during audit: 1.0.75.

---

## BUG-SWARM-012 — Clearing Codex Speed leaves the previous sticky tier active

- Severity: P1
- Status: Fixed in working tree; red-first native request regression now passes
- Labels: `bug`, `provider:codex`, `speed`, `cost`, `P1`

### Symptom

Clearing Fast/Flex made UAM display `Default`, but a loaded Codex thread continued using its
previous tier.

### Reproduction

1. Select Fast and start a turn; request contains `"serviceTier":"fast"`.
2. Clear Speed and start another turn.
3. Before the fix, the second request omitted `serviceTier`.
4. Codex retained the previous sticky value.

### Expected

Default explicitly clears the prior thread/turn override.

### Root cause and blast radius

UAM represented both “Default” and “do not change the existing setting” as an omitted JSON
field:

`Speed chip → empty persisted tier → turn/start omission → Codex sticky override →
continued cost/latency behavior`

The Codex app-server contract requires a nullable clear:
<https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md>.

### Proof and resolution

- Regression checks Fast followed by a cleared request.
- Before fix: the cleared request had no field.
- Fix: every turn carries `serviceTier`; Default is serialized as `null`.

---

## BUG-SWARM-013 — Speed options ignore model-specific capabilities

- Severity: P2
- Status: Fixed in working tree; red-first frontend/native regressions now pass
- Labels: `bug`, `provider:codex`, `speed`, `models`, `P2`

### Symptom

UAM offered Fast and Flex for every Codex model, including models that advertised no extra tier
or only Fast. Unsupported selections could persist and be sent.

### Reproduction

1. Load a selected model with no tiers, or a model with Fast only.
2. Open `/speed`.
3. Before the fix, both Fast and Flex appeared.
4. Select Flex and observe native persistence accept it.

### Root cause and blast radius

The current structured catalog fields were discarded, the frontend appended static choices, and
native persistence had no model-specific backstop:

`model/list → parser/cache → menu → store → CEF setter → turn/start`

### Proof and resolution

- Regression: `uses selected model speed catalog instead of adding unsupported tiers`
- Before fix: expected Default/Fast; received Default/Fast/Flex.
- Fixes:
  - parse current `service_tiers` and legacy fields;
  - map Codex `priority` to UAM's existing `fast` identity;
  - show only selected-model tiers when runtime metadata exists;
  - clear invalid persisted values during reconciliation/turn startup;
  - reject unsupported native setter requests.

---

## BUG-SWARM-014 — Current Codex `Max` and `Ultra` reasoning levels are discarded

- Severity: P2
- Status: Fixed in working tree; red-first parser/normalization regressions now pass
- Labels: `bug`, `provider:codex`, `reasoning`, `models`, `P2`

### Symptom

Installed GPT-5.6 models advertised `max` and `ultra`, but UAM removed them from catalog parsing,
menus, persistence, and native validation.

### Reproduction

1. Run Codex 0.145.0 model discovery.
2. Select GPT-5.6 Sol or Terra.
3. Before the fix, Max and Ultra were absent.

### Root cause and blast radius

UAM used a stale global allowlist and recognized only older camel-case catalog aliases:

`Codex catalog → native parser/filter → CEF serializer → frontend options → native setter`

### Proof and resolution

- Regressions:
  - `CodexOptionNormalizationUsesSharedAllowlists`
  - `AcpModelJsonParserHandlesCodexModelAliasesAndVisibility`
- Fix: support `max`/`ultra`, current snake-case model fields, and model-advertised effort lists.
- Copilot remains provider-aware and rejects unsupported `ultra`.

---

## BUG-SWARM-015 — Codex terminal fallback ignores selected Reasoning and Speed

- Severity: P2
- Status: Fixed in working tree; red-first native argv regression now passes
- Labels: `bug`, `provider:codex`, `terminal`, `speed`, `P2`

### Symptom

A Codex chat configured for High/Fast behaved differently when opened through terminal fallback:
the CLI received the model but neither reasoning nor Speed.

### Reproduction

1. Configure a Codex chat with High reasoning and Fast speed.
2. Open terminal fallback.
3. Inspect launch diagnostics/argv.
4. Before the fix, only the model and general flags were present.

### Root cause and blast radius

Session-specific options were implemented only in ACP turn requests:

`visible chat options → persisted chat → terminal launcher → Codex CLI`

### Proof and resolution

- Regression: `CodexCliInteractiveArgvUsesResumeModelAndFlags`
- Before fix: expected 13 arguments, received 9.
- Fix: fresh and resumed interactive launches pass validated `model_reasoning_effort` and
  `service_tier` configuration.

---

## BUG-SWARM-016 — Native-history refresh turns off Small-model workflow

- Severity: P2
- Status: Fixed in working tree; red-first native regression now passes
- Labels: `bug`, `history`, `state`, `P2`

### Symptom

Refreshing a matching native-history chat could silently replace the local
`small_model_mode=true` setting with false/default.

### Reproduction

1. Persist a local chat with Small-model workflow on.
2. Merge matching native history whose field is false/default.
3. Apply local overrides.
4. Before the fix, the merged chat remained false.

### Root cause and blast radius

The local-over-native overlay copied adjacent model controls but omitted this field:

`local chat JSON → native-history reconciliation → app state → ACP prompt batching`

### Proof and resolution

- Regression: `NativeHistoryRefreshPreservesIndependentModelControls`
- Fix: copy `small_model_mode` with the other local provider controls.

---

## BUG-SWARM-017 — A provider's first chat cannot discover its current models

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `models`, `new-chat`, `architecture`, `P2`

### Symptom

On an empty data root, New Chat cannot discover models for a provider because discovery requires
an existing chat from that provider. Copilot offers only Default; Codex falls back to a static
list that can lag the installed catalog.

### Reproduction

1. Start UAM with an empty data root.
2. Open New Chat and choose Copilot or Codex.
3. Observe no discovery request because no matching provider session exists.
4. Copilot shows only Default; current Codex models may be absent.

### Root cause and blast radius

Both frontend and native discovery are chat-scoped:

- New Chat requires `providerSessions[0]`.
- Settings disables refresh without a provider chat.
- Startup loops only existing chats.
- Native `StartAcpModelDiscovery` requires a chat ID.

`empty install → New Chat → stale/default model → first provider launch`

### Required regression

With zero sessions, choosing a provider must start discovery and populate that provider's
catalog before chat creation.

### Smallest viable direction

Expose provider/workspace-scoped ephemeral discovery and serialize provider catalogs separately
from per-chat ACP binding state. This was not forced into the current fix set because a synthetic
persistent chat would pollute history and still leave the state contract wrong.

---

## BUG-SWARM-018 — Copilot model changes can desynchronize visible and launch-time effort

- Severity: P2
- Status: Fixed in working tree; deterministic lifecycle trace and frontend regression pass
- Labels: `bug`, `provider:copilot`, `reasoning`, `lifecycle`, `P2`

### Symptom

Switching Copilot models can normalize and persist a new effort while the already-running ACP
server continues using the old `--effort` value fixed at process launch.

### Reproduction

1. Launch a Copilot ACP chat on model A with Max.
2. Switch live to model B whose advertised efforts do not contain Max.
3. Start the next prompt.
4. UAM normalizes the stored effort for model B, but the process was launched with
   `--effort max` and is not restarted.

### Root cause and blast radius

The shared live-model path assumes effort can follow a model independently. Copilot applies
effort only when its ACP server starts:

`model selector → session/set_model → next-turn effort normalization → UI/persistence →
unchanged Copilot process`

### Proof and resolution

- Regressions: `reconciles reasoning and speed atomically when the selected model changes` and
  `CopilotEffortChangingModelSwitchRejectsBusyAndStopsIdleRuntime`.
- The native model handler now normalizes effort against the target model before saving.
- If that changes Copilot's launch-time effort, an idle server is stopped so the next prompt
  starts it with the new persisted `--effort`.
- A busy Copilot server rejects this compound change instead of displaying values that are not
  active.
- Compatible model changes continue to use the live model path without a restart.

---

## BUG-SWARM-019 — Settings Chat Defaults ignores the discovered model catalog

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression passes
- Labels: `bug`, `settings`, `models`, `speed`, `P2`

### Symptom

The active chat composer used runtime model capabilities, while Settings continued to offer
static models, reasoning efforts, and Fast/Flex choices. A default saved in Settings could
therefore be invalid for the selected model.

### Reproduction

1. Discover a Codex model that supports Low/Medium/High and Fast only.
2. Open Settings → Chat Defaults for Codex.
3. Before the fix, observe Max/Ultra or Flex in the default menus.
4. Save one and create a chat; native code must later reject or rewrite it.

### Root cause and blast radius

Settings did not read the provider session's `availableModels`:

`ACP discovery → per-chat binding → Settings static menu → provider default → every new chat`

### Proof and resolution

- Regression: `uses discovered provider models and model-specific defaults options`.
- Settings now uses the same catalog builders as the chat composer for Model, Reasoning, and
  Speed.
- Discovery failures are shown beside the affected provider with the existing refresh action.

---

## BUG-SWARM-020 — Model changes retain incompatible Reasoning and Speed

- Severity: P1
- Status: Fixed in working tree; frontend regression and native boundary validation pass
- Labels: `bug`, `models`, `reasoning`, `speed`, `terminal`, `P1`

### Symptom

Changing from a model that supports Ultra/Flex to one that supports only Low–High/Fast left
Ultra/Flex stored. The composer could mask the stale values, but terminal fallback or a later
state load could still launch with them. An explicit unknown model ID also borrowed the first
catalog model's capabilities.

### Reproduction

1. Select model A with Ultra and Flex.
2. Switch to model B whose default is Medium and whose only extra tier is Fast.
3. Before the fix, inspect persisted chat options or launch terminal fallback.
4. Observe model B paired with Ultra/Flex.

### Root cause and blast radius

Model, effort, and tier were mutated independently:

`model menu → optimistic model only → native model save → prompt/terminal launch`

The model lookup also fell back to an unrelated first entry when a non-empty ID was absent.

### Proof and resolution

- Regressions: `reconciles reasoning and speed atomically when the selected model changes` and
  `ChatModelHandlerCanonicalizesReasoningAndSpeedBeforePersistence`.
- Frontend and native boundaries now update model/effort/tier as one compatible tuple.
- Invalid non-empty effort uses the target model's declared default, then its first supported
  effort; invalid Speed clears to Default.
- A non-empty unknown model no longer inherits another model's capabilities.

---

## BUG-SWARM-021 — Terminal startup failure has no retry path

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression passes
- Labels: `bug`, `terminal`, `recovery`, `P2`

### Symptom

If `startCliTerminal` failed transiently, the banner could be dismissed but the terminal could
not be started again without leaving and remounting the view.

### Reproduction

1. Make the first terminal start request fail.
2. Keep the CLI view open.
3. Before the fix, observe an error with no Retry action.

### Proof and resolution

- Regression: `retries a transient terminal startup failure without remounting`.
- A stopped terminal error now offers Retry.
- The retry attempt is explicit and no longer restarts again merely because the returned
  terminal identity hydrates into state.

---

## BUG-SWARM-022 — Native option normalization can be masked forever

- Severity: P1
- Status: Fixed in working tree; frontend canonical-response regression passes
- Labels: `bug`, `state`, `reasoning`, `speed`, `P1`

### Symptom

If native code accepted a request but canonicalized its values, the optimistic pending map
continued forcing the original values because it cleared only when a later push exactly
matched them.

### Reproduction

1. Send an invalid/noncanonical effort with a Speed change.
2. Let native code persist a canonical effort and return success.
3. Receive native state containing the canonical tuple.
4. Before the fix, observe the optimistic tuple remain indefinitely.

### Root cause and blast radius

Success returned `{}` and pending reconciliation had no authoritative canonical tuple:

`optimistic options → native normalization → success/push → exact-match pending guard → stale UI`

### Proof and resolution

- Regression: `uses canonical native options instead of permanently masking state`.
- Native option setters now return canonical effort and tier.
- The current matching request applies that result and clears its pending overlay; old
  responses cannot overwrite a newer request.

---

## BUG-SWARM-023 — A stale state push can revert a successful model selection

- Severity: P1
- Status: Fixed in working tree; frontend stale-push regression passes
- Labels: `bug`, `state`, `models`, `P1`

### Symptom

Reasoning and Speed had pending-state protection, but the selected model did not. An older state
push could put the previous model back while the successful change was still settling.

### Reproduction

1. Change from model A to model B.
2. While the native request is in flight, deliver a state push containing A.
3. Before the fix, the selector reverted to A.

### Proof and resolution

- Regression: `protects an in-flight model change and accepts the canonical native result`.
- Pending model state now protects the whole model/effort/tier tuple and accepts the canonical
  native response only for the latest request.

---

## BUG-SWARM-024 — Codex cancellation drains queued work before the turn completes

- Severity: P1
- Status: Fixed in working tree; native ordering regressions pass
- Labels: `bug`, `provider:codex`, `cancellation`, `queue`, `P1`

### Symptom

UAM treated the `turn/interrupt` response as completion and immediately sent the next queued
prompt. Codex can acknowledge the interrupt before emitting `turn/completed`, so two turn
lifecycles could overlap. Cancelling before the turn ID arrived also sent an undocumented
generic `session/cancel` notification.

### Reproduction

1. Start a Codex turn and queue a steering prompt.
2. Cancel before or after receiving the turn ID.
3. Return the interrupt response before `turn/completed`.
4. Before the fix, observe the queued prompt start immediately; early cancellation used
   `session/cancel`.

### Proof and resolution

- `CodexCancelIgnoresLateApprovalAndClearsInterruptState` now covers cancel-before-turn-ID.
- `AcpQueuedPromptManagementRemovesAndPrioritizesSelectedPrompt` proves an interrupt response
  does not drain the queue.
- Codex cancellation waits for a turn ID, sends `turn/interrupt`, keeps cancellation pending
  through its acknowledgment, and drains only from `turn/completed`.

---

## BUG-SWARM-025 — Changing only Speed can rewrite Reasoning

- Severity: P2
- Status: Fixed in working tree; native prompt regression passes
- Labels: `bug`, `provider:codex`, `speed`, `reasoning`, `P2`

### Symptom

A chat using provider-default Reasoning (`""`) could acquire the last/highest advertised effort
when the user changed only Speed.

### Reproduction

1. Use Default Reasoning on a model advertising Low/Medium/High.
2. Change Speed.
3. Before the fix, native validation treated empty as unsupported and stored High.

### Proof and resolution

Empty remains the explicit provider-default choice. Only a non-empty incompatible effort is
canonicalized, using the model's declared default before its first supported value.

---

## BUG-SWARM-026 — Default Speed cannot distinguish inherit from explicit clear

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `provider:codex`, `speed`, `configuration`, `architecture`, `P2`

### Symptom

BUG-SWARM-012 correctly sends `serviceTier:null` when a user clears a sticky UAM Speed.
However, a new chat that has never chosen Speed uses the same empty string. Its first turn also
sends `null`, which can clear a Fast value inherited from the user's global Codex configuration.

### Reproduction

1. Configure Fast globally in Codex.
2. Create a UAM chat and never touch Speed.
3. Send its first ACP turn.
4. Observe `serviceTier:null`, making UAM override rather than inherit the global setting.

### Root cause and required regression

Persistence has two states (`""` or a tier) while the protocol needs three intents:

`inherit/unspecified`, `explicit tier`, `explicit clear`.

Add a tri-state persisted intent and prove both paths:

- untouched Default omits the field and inherits global configuration;
- clearing a previously selected UAM tier sends `null`.

---

## BUG-SWARM-027 — Split catalog aliases lose valid Speed tiers

- Severity: P2
- Status: Fixed in working tree; native parser regression passes
- Labels: `bug`, `provider:codex`, `models`, `speed`, `P2`

### Symptom

When one model payload supplied legacy `additionalSpeedTiers:["fast"]` and current
`serviceTiers:[{"id":"flex"}]`, UAM kept only the first non-empty alias.

### Root cause and resolution

The alias helper returned early rather than merging normalized unique values. It now unions all
recognized alias arrays. `AcpModelJsonParserHandlesCodexModelAliasesAndVisibility` proves both
Fast and Flex survive.

---

## BUG-SWARM-028 — Cancel timeout is checked before buffered completion

- Severity: P3
- Status: Fixed in working tree; ordering regression and native suite pass
- Labels: `bug`, `acp`, `cancellation`, `race`, `P3`

### Symptom

At the timeout boundary, a valid cancellation completion already buffered on stdout could be
ignored because polling restarted the runtime before reading it.

### Root cause and resolution

`PollAllAcpSessions` checked the five-second deadline before draining provider output. It now
drains stderr/stdout first, then applies the timeout only if cancellation remains pending.
`AcpPollingDrainsBufferedCompletionBeforeCancelTimeout` guards that ordering directly.

---

## BUG-SWARM-029 — Generic provider reasoning defaults are silently discarded

- Severity: P2
- Status: Fixed in working tree; frontend and native-save regressions pass
- Labels: `bug`, `providers`, `settings`, `reasoning`, `P2`

### Symptom

Settings showed Reasoning for a Gemini/OpenCode model that advertised efforts at runtime, but
the frontend settings action cleared the selected effort because the provider had no static
reasoning capability flag.

### Reproduction

1. Discover a generic ACP model advertising Low and High.
2. In Settings choose High as that provider's default.
3. Save.
4. Before the fix, the outgoing/default state contained an empty effort.

### Proof and resolution

- Regressions: `preserves generic provider defaults advertised by the selected runtime model`
  and `ProviderDefaultSettingsSavePreservesRuntimeReasoningEffort`.
- Static capability support or selected-model runtime support now permits the default.
- The CEF settings-save path now preserves that validated effort for generic ACP providers while
  continuing to clear Codex-only Speed tiers.
- Native provider-default normalization already preserves valid shared efforts.

---

## BUG-SWARM-030 — Importing provider skills also imports their reference documents

- Severity: P2
- Status: Fixed in working tree; native import regression passes
- Labels: `bug`, `markdown-store`, `skills`, `P2`

### Symptom

“Import provider skills” recursively treated every Markdown file below a `skills` directory as
a separate UAM skill. A skill with `references/workflow.md` therefore produced both the actual
`SKILL.md` entry and a misleading standalone command for its internal reference.

### Reproduction

1. Put `SKILL.md` and `references/workflow.md` under one Codex skill directory.
2. Preview provider imports.
3. Before the fix, observe two supported candidates.

### Root cause and resolution

The default source walker filtered extensions but not the skill entrypoint contract. For a
directory named `skills`, it now imports only files named `SKILL.md`; command and prompt
directories retain their normal Markdown behavior.

- Regression: `MarkdownStoreSkillImportExcludesReferenceMarkdown`.

---

## BUG-SWARM-031 — Filtering Skills can leave actions bound to a hidden entry

- Severity: P2
- Status: Fixed in working tree; frontend regression passes
- Labels: `bug`, `markdown-store`, `state`, `P2`

When the selected skill is filtered out, the list shows another entry but the preview and
actions previously remained bound to the hidden selection. Select one skill, search for a
different one, then click Attach or Edit to reproduce it. Selection is now resolved only from
the visible filtered collection.

---

## BUG-SWARM-032 — Skills child overlays are not complete accessible dialogs

- Severity: P2
- Status: Fixed in working tree; focus and Escape regressions pass
- Labels: `bug`, `markdown-store`, `accessibility`, `P2`

The Create/Edit and Import overlays had no dialog semantics, kept focus on obscured controls,
and let Escape close the entire Skills window. They now expose modal dialog names, receive
initial focus, close only the top child on Escape, and restore focus to the invoking control.

---

## BUG-SWARM-033 — A large skill-import preview can exceed the viewport

- Severity: P2
- Status: Fixed in working tree; 40-entry regression passes
- Labels: `bug`, `markdown-store`, `layout`, `P2`

Previewing enough provider skills rendered an unbounded list whose footer could move beyond the
viewport. The dialog now has a viewport-bound height and a dedicated scrollable candidate list.

---

## BUG-SWARM-034 — Skill save/import failures are hidden behind the active child overlay

- Severity: P2
- Status: Fixed in working tree; error-path regressions pass
- Labels: `bug`, `markdown-store`, `error-handling`, `P2`

The shared error was rendered only in the parent Skills window, underneath the fixed child
overlay. A failed Save or import preview therefore appeared to do nothing. The active child now
renders the same error as an alert while remaining open for correction or retry.

---

## BUG-SWARM-035 — All-skipped skill imports trap their result behind the preview

- Severity: P3
- Status: Fixed in working tree; skipped-result regression passes
- Labels: `bug`, `markdown-store`, `feedback`, `P3`

The import preview closed only when at least one item was imported. If every result was skipped,
the result summary rendered underneath the still-open preview. Any non-empty completed result
set now closes the preview and exposes the status summary.

---

## BUG-SWARM-036 — Skills directory browse/save failures are invisible in Settings

- Severity: P2
- Status: Fixed in working tree; frontend rejection regression passes
- Labels: `bug`, `markdown-store`, `settings`, `error-handling`, `P2`

Settings subscribed to neither the Skills error nor picker failures, and the browse action
collapsed backend failure into the same `null` used for user cancellation. The store now keeps
the backend error, Settings renders it beside the labelled path input, and a later successful
workflow clears it.

---

## BUG-SWARM-037 — Auto-approval can choose a persistent “always allow” option

- Severity: P1
- Status: Fixed in working tree; red-first native regression passes
- Labels: `bug`, `acp`, `permissions`, `security`, `P1`

When a provider returned both one-shot and persistent permission choices, label matching could
select `allow_always`. UAM's per-chat auto-approval must not silently create a provider-level
persistent grant. Automatic selection now excludes options whose ACP kind is `always`.

---

## BUG-SWARM-038 — Copilot ACP launch bypasses the supported-version gate

- Severity: P1
- Status: Fixed in working tree; red-first launch regression passes
- Labels: `bug`, `provider:copilot`, `compatibility`, `P1`

Terminal launch enforced Copilot's 1.0.60 floor, while the structured ACP process could still
start an unchecked or unsupported installation. The shared compatibility reason now gates both
terminal and ACP lifecycle entry points.

---

## BUG-SWARM-039 — State patches miss edits to earlier transcript messages

- Severity: P1
- Status: Fixed in working tree; red-first digest regression passes
- Labels: `bug`, `state`, `transcript`, `P1`

The chat fingerprint hashed only the final message. Editing, hydrating, or enriching an earlier
message without changing the last one produced no message patch. The digest now covers every
message and each frontend-relevant nested field.

---

## BUG-SWARM-040 — Renaming a folder directory leaves its chats on the old workspace

- Severity: P1
- Status: Fixed in working tree; red-first persistence regression passes
- Labels: `bug`, `folders`, `workspace`, `data-integrity`, `P1`

Folder and chat records duplicate the workspace directory. Rename updated only the folder, so
existing chats continued launching in the old path. Rename now blocks while a folder chat is
running, migrates matching chat workspace/source paths, persists both stores, and rolls both
back if either write fails.

---

## BUG-SWARM-041 — Retrying a message can use a changed skill instead of the sent snapshot

- Severity: P1
- Status: Fixed in working tree; red-first save/reload/branch regression passes
- Labels: `bug`, `markdown-store`, `retry`, `data-integrity`, `P1`

The original prompt embedded skill version V1, but the message persisted only its file path.
After that file changed to V2, Retry/Branch rebuilt the prompt from V2. Messages now persist the
immutable prompt blocks used for the original send and reuse them after save/reload.

---

## BUG-SWARM-042 — A ready-then-crash loop never exhausts ACP reconnect attempts

- Severity: P1
- Status: Fixed in working tree; native recovery regression passes
- Labels: `bug`, `acp`, `recovery`, `loop`, `P1`

Reconnect attempts reset as soon as a session became ready. A provider that repeatedly reached
ready and crashed before completing work therefore received an unlimited retry loop. The count
now resets only after a prompt turn completes successfully.

---

## BUG-SWARM-043 — Startup model/mode requests can hold a queued prompt forever

- Severity: P1
- Status: Fixed in working tree; red-first timeout regression passes
- Labels: `bug`, `acp`, `models`, `timeout`, `P1`

After `session/new` succeeded, UAM's setup timeout stopped applying. If the provider never
answered the startup `session/set_model` or mode request, the queued user prompt was never sent
and never failed. Pending startup controls now use the same bounded inactivity recovery while
preserving undelivered queued work.

---

## BUG-SWARM-044 — Claude structured launch emits unsupported permission modes

- Severity: P1
- Status: Fixed in working tree; red-first argv regression passes
- Labels: `bug`, `provider:claude`, `permissions`, `P1`

UAM passed `--permission-mode default`, which current Claude rejects, and collapsed Auto/Yolo to
that value. Structured launch now omits the flag for Default, maps safety tiers to `auto`, Yolo
to `bypassPermissions`, and preserves `acceptEdits` and `plan`.

---

## BUG-SWARM-045 — Folder storage ignores a valid backup when the primary file is absent

- Severity: P1
- Status: Fixed in working tree; red-first backup-only regression passes
- Labels: `bug`, `folders`, `recovery`, `data-integrity`, `P1`

The loader returned an empty folder list before checking `.bak` whenever the primary JSON file
was absent. That is a normal atomic-write crash state. Backup fallback is now attempted for both
missing and unreadable primary files.

---

## BUG-SWARM-046 — Atomic helper paths can corrupt Unicode on Windows

- Severity: P2
- Status: Fixed in working tree; platform invariant added, Windows execution still required
- Labels: `bug`, `windows`, `unicode`, `persistence`, `P2`

Temporary and backup names were built through `path.string() + suffix`. On Windows that narrows
the filesystem path through the active code page and can corrupt or reject non-ASCII user
paths. The helpers now append ASCII suffixes directly to `std::filesystem::path`. This was
deterministic from the platform API path, but the audit host was macOS, so the Windows regression
must still run in CI.

---

## BUG-SWARM-047 — One Escape press closes both Skills and Settings

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression passes
- Labels: `bug`, `modals`, `keyboard`, `P2`

Settings and Skills each registered a window-level Escape listener. When Skills was opened from
Settings, both handlers ran for the same keypress. Settings now ignores Escape while its Skills
child is open.

---

## BUG-SWARM-048 — A previous Skills error leaks into a new workflow

- Severity: P3
- Status: Fixed in working tree; red-first frontend regression passes
- Labels: `bug`, `markdown-store`, `state`, `P3`

After a failed edit or picker request, opening Create/Edit/Import again displayed the stale
error before the new action ran. Child workflow start/cancel and successful browsing now clear
the shared error deliberately.

---

## BUG-SWARM-049 — Claude live permission changes can keep the old launch configuration

- Severity: P1
- Status: Fixed in working tree; red-first launch-decision regression passes
- Labels: `bug`, `provider:claude`, `permissions`, `lifecycle`, `P1`

Claude permission policy is fixed in process argv. Yolo and Default both map to UAM's generic
`default` mode, so the live-update handler considered Yolo→Default unchanged and left
`bypassPermissions` active. The decision now compares provider launch argv as well as the
generic mode and restarts Claude when launch-only configuration changes.

---

## BUG-SWARM-050 — Native-history reconciliation erases saved skill snapshots

- Severity: P1
- Status: Fixed in working tree; red-first native-overlay regression passes
- Labels: `bug`, `history`, `markdown-store`, `data-integrity`, `P1`

An equal Gemini-native transcript can win over the local copy while lacking UAM-owned message
metadata. This erased attached skill files, immutable prompt blocks, and attachment records, so
a later Retry could load changed content. Equivalent messages now receive those locally owned
fields from the persisted overlay.

---

## BUG-SWARM-051 — Folder migration does not match `~/…` against an expanded chat path

- Severity: P1
- Status: Fixed in working tree; red-first path regression passes
- Labels: `bug`, `folders`, `paths`, `data-integrity`, `P1`

Chat creation expands a folder such as `~/repo`, while the folder record can retain the tilde.
Directory rename compared the raw folder value with the absolute chat workspace and skipped the
migration. Shared folder-path comparison now expands leading tilde before normalization.

---

## BUG-SWARM-052 — Idle ACP model/mode requests have no timeout

- Severity: P1
- Status: Fixed in working tree; red-first native regression passes
- Labels: `bug`, `acp`, `timeout`, `models`, `P1`

The new startup-control timeout covered a queued prompt but not a model/mode change made while
the session was idle. A provider that never answered left configuration locked indefinitely.
All pending control requests now use the inactivity timeout, and live request submission resets
the timer to the request start.

---

## BUG-SWARM-053 — Copilot compatibility probing can suppress automatic model discovery

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `provider:copilot`, `models`, `startup`, `P2`

### Reproduction

1. Start UAM with at least one Copilot chat and no fresh Copilot model cache.
2. Let browser readiness occur while the asynchronous Copilot version check is still pending.
3. Automatic discovery marks the provider attempted, then ACP launch returns “Checking GitHub
   Copilot CLI compatibility.”
4. Let the supported version check complete.
5. Observe that automatic discovery is not retried; only a manual refresh can recover it.

### Root cause and required regression

`BeginDiscoveryIfStale` records the one-shot attempt before the compatibility gate runs.
`RememberRefreshFailure` clears pending state but deliberately retains the attempted marker, and
browser-ready discovery is not revisited after compatibility changes. Defer the first Copilot
attempt until compatibility is known, then trigger stale discovery once when a supported result
arrives. The regression must prove unsupported versions remain blocked and a supported delayed
probe starts exactly once.

---

## BUG-SWARM-054 — Move-to-collection submenu remains open after the pointer leaves

- Severity: P2
- Status: Fixed in working tree; pointer-boundary regression passes
- Labels: `bug`, `sidebar`, `collections`, `menus`, `P2`

The submenu portal had no shared close boundary with its parent menu item. Once opened, neither
surface scheduled a close after the pointer left both. A single 120 ms close timer is now shared
by the trigger and portal, so moving between them remains usable and leaving both dismisses it.

---

## BUG-SWARM-055 — Move-to-collection submenu is not a complete keyboard submenu

- Severity: P2
- Status: Fixed in working tree; keyboard regression passes
- Labels: `bug`, `accessibility`, `collections`, `menus`, `P2`

The pointer-only submenu did not implement nested-menu focus movement. Right Arrow, Enter, and
Space now open and focus it; Escape and Left Arrow close it and restore focus to the trigger.

---

## BUG-SWARM-056 — Workspace rescan omits GitHub Copilot chats

- Severity: P1
- Status: Fixed in working tree; native import regression passes
- Labels: `bug`, `provider:copilot`, `folders`, `import`, `P1`

Folder rescan imported Codex rollouts but never inspected Copilot's actual
`~/.copilot/session-state/<uuid>/workspace.yaml` and `events.jsonl` storage. The provider importer
now matches the workspace, skips subagents, tolerates an incomplete final JSONL record, imports
user/assistant messages, and participates in the same folder rescan.

---

## BUG-SWARM-057 — Rescan can replace a hydrated chat and lose an unsaved response tail

- Severity: P1
- Status: Fixed in working tree; live-tail regression passes
- Labels: `bug`, `rescan`, `messages`, `data-integrity`, `P1`

Reloading sidebar summaries after import could replace the current hydrated in-memory chat with
its older disk summary. Rescan now merges saved discoveries while preserving current hydrated and
live chats, including an unsaved streaming assistant tail.

---

## BUG-SWARM-058 — Startup history discovery remains limited to the default native provider

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `startup`, `provider:copilot`, `provider:codex`, `P2`

Startup native-history discovery still routes through the single default native overlay provider.
The new workspace Rescan path covers Copilot and Codex, but their histories are not automatically
discovered at startup. A safe fix needs provider-specific discovery without rescanning every
external history root on each state refresh.

---

## BUG-SWARM-059 — Import failure is reported like a successful zero-result rescan

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `import`, `errors`, `observability`, `P2`

Provider import results expose only total/imported counts. Permission, traversal, and parse-root
failures can therefore look identical to “no chats found.” Add an explicit error/warning channel
before claiming success in the UI; no speculative error strings were added during this pass.

---

## BUG-SWARM-060 — Delete rollback can erase messages from an unloaded chat summary

- Severity: P1
- Status: Fixed in working tree; rollback regression passes
- Labels: `bug`, `delete`, `rollback`, `data-integrity`, `P1`

The sidebar holds summary-only chats. A failed deletion rollback saved that summary as a full chat,
overwriting the transcript with no messages. Deletion now hydrates every target before any write
or delete and refuses to start if safe rollback data cannot be prepared.

---

## BUG-SWARM-061 — A deleted chat can be recovered from its orphan `.bak` file

- Severity: P1
- Status: Fixed in working tree; backup-resurrection regression passes
- Labels: `bug`, `delete`, `recovery`, `data-integrity`, `P1`

Deletion removed the primary chat file but left its recovery backup. The next load could restore
the supposedly deleted chat. Local deletion now removes the primary, backup, summary, summary
backup, and legacy directory as one reported operation.

---

## BUG-SWARM-062 — Deleted Codex/Copilot imports return on the next rescan

- Severity: P1
- Status: Fixed in working tree; delete/rescan/restart regression passes
- Labels: `bug`, `delete`, `provider:copilot`, `provider:codex`, `P1`

UAM does not own Copilot/Codex source logs, so deleting only the local copy made the same native
identity import again. A durable native-identity tombstone now blocks reimport after both rescan
and restart without deleting the external source.

---

## BUG-SWARM-063 — Settings deliberately delays every open and section change

- Severity: P2
- Status: Fixed in working tree; focused Settings regression passes
- Labels: `bug`, `performance`, `settings`, `ui`, `P2`

Settings applied 150–180 ms entrance animations to the overlay, dialog, and every section switch.
Those forced delays were removed. Motion that communicates a running CLI operation remains.

---

## BUG-SWARM-064 — Opening Settings starts a duplicate custom-theme refresh

- Severity: P2
- Status: Fixed in working tree; single-owner regression passes
- Labels: `bug`, `performance`, `settings`, `themes`, `P2`

AppShell already owns startup theme discovery, but Settings subscribed to and started the same
refresh again on mount. The redundant selector/effect was removed; manual refresh behaviour is
unchanged.

---

## BUG-SWARM-065 — Sidebar branch lookup becomes quadratic as chat count grows

- Severity: P2
- Status: Fixed in working tree; no-rescan regression and benchmark pass
- Labels: `bug`, `performance`, `sidebar`, `scalability`, `P2`

Each `SessionItem` filtered the complete session list to find its branch family. FolderTree now
builds the family map once and passes the matching IDs down. The measured render-path lookup fell
from 46.17 ms to 0.64 ms at 2,000 chats (about 72× faster in the audit benchmark).

---

## BUG-SWARM-066 — Inactive status filtering still subscribes to every runtime binding

- Severity: P2
- Status: Fixed in working tree; subscription regression passes
- Labels: `bug`, `performance`, `sidebar`, `state`, `P2`

FolderTree subscribed to every CLI/ACP binding even when the status filter was off, triggering
unnecessary full search grouping and sorting during streaming. It now uses the shared empty
binding maps unless status filtering is active.
`does not rerender for runtime binding updates while status filtering is off` covers both CLI
and ACP binding changes.

---

## BUG-SWARM-067 — Hidden chats can remain selected for bulk deletion

- Severity: P2
- Status: Fixed in working tree; collapse reconciliation regression passes
- Labels: `bug`, `sidebar`, `selection`, `delete`, `P2`

After a Shift range was selected, collapsing its workspace could leave invisible IDs in the
selection toolbar. Selection and its anchor are now intersected with the rendered, non-inert chat
rows whenever visibility changes.

---

## BUG-SWARM-068 — Grid cleanup can leave the active pane pointing outside the visible layout

- Severity: P2
- Status: Fixed in working tree; storage regression passes
- Labels: `bug`, `chat-grid`, `delete`, `state`, `P2`

Removing assigned chats searched all four stored slots for a fallback, even in a one- or two-pane
layout. The active pane can now fall back only within the visible pane count.

---

## BUG-SWARM-069 — Single-chat deletion leaves keyed frontend state behind

- Severity: P2
- Status: Fixed in working tree; store cleanup regression passes
- Labels: `bug`, `delete`, `frontend`, `state`, `P2`

The new batch path cleared messages, attachments, goals, runtime bindings, and chat-grid slots, but
the legacy single-delete success path did not. Both now use the same state cleanup helper.
`clears every keyed chat record after a single delete succeeds` guards the single-delete caller.

---

## BUG-SWARM-070 — Valid Copilot histories above 12 MiB are silently skipped

- Severity: P1
- Status: Fixed in working tree; streaming-cap regression passes
- Labels: `bug`, `provider:copilot`, `import`, `data-loss`, `P1`

The streaming Copilot JSONL parser first applied Gemini's whole-file 12 MiB cap. Two actual local
Copilot logs measured 12,656,581 and 12,656,221 bytes, just above the 12,582,912-byte limit, and
were rejected before parsing. The whole-file gate is removed for append-only JSONL; the existing
message-count bound still stops parsing work.

---

## BUG-SWARM-071 — A malformed tombstone file fails open and resurrects deleted chats

- Severity: P1
- Status: Fixed in working tree; primary/backup/corruption regressions pass
- Labels: `bug`, `delete`, `recovery`, `data-integrity`, `P1`

Missing, unreadable, and malformed tombstone storage all previously became an empty set. The next
rescan could resurrect every deleted import and the next delete could overwrite the evidence.
Missing storage is a valid empty set; malformed, wrong-schema, or unreadable storage checks the
atomic-write `.bak` and otherwise fails closed for imports and native tombstone writes. Purely
local deletion does not depend on this external-import ledger.

---

## BUG-SWARM-072 — Chat deletion ignores pending ACP control requests

- Severity: P1
- Status: Fixed in working tree; pending-control deletion regression passes
- Labels: `bug`, `delete`, `acp`, `lifecycle`, `P1`

Deletion checked active turns but missed initialize, session setup, startup model/mode, live
model/mode, and cancel request IDs. Chat and folder deletion now use the existing complete ACP
blocking-work predicate before touching storage.

---

## BUG-SWARM-073 — Single-chat deletion leaves Gemini's native transcript behind

- Severity: P1
- Status: Fixed in working tree; native-file regression passes
- Labels: `bug`, `delete`, `provider:gemini`, `data-integrity`, `P1`

When single deletion was routed through the new batch service, provider-native Gemini deletion was
lost. The shared batch path now deletes Gemini-native history after local preconditions succeed.
Native cleanup is the final, best-effort phase: failure is reported without undoing a safely
committed local deletion or fabricating a lossy provider transcript.

---

## BUG-SWARM-074 — Deleting a workspace can resurrect its Copilot/Codex chats

- Severity: P1
- Status: Fixed in working tree; folder-delete Copilot regression passes
- Labels: `bug`, `folders`, `delete`, `provider:copilot`, `provider:codex`, `P1`

Folder deletion did not add native-identity tombstones. Recreating/rescanning the workspace brought
externally owned chats back. Folder deletion now creates the same durable guards as chat deletion
and removes them again if the folder transaction rolls back.

---

## BUG-SWARM-075 — Summary deletion errors are discarded

- Severity: P2
- Status: Fixed in working tree; error-reporting regression passes
- Labels: `bug`, `delete`, `errors`, `persistence`, `P2`

`DeleteChatStorageFiles` reported metadata failures but ignored errors removing the summary and its
backup. Those errors are now part of `ChatStorageDeleteResult::Failed()`, preventing a false-success
response with stale metadata still present.

---

## BUG-SWARM-076 — Empty bulk-delete input returns a conflict instead of bad request

- Severity: P3
- Status: Fixed in working tree; request-boundary regression passes
- Labels: `bug`, `api`, `delete`, `validation`, `P3`

`chatIds: []` passed JSON validation and reached the lifecycle service, returning 409. The handler
now rejects an empty array at the request boundary with 400.
`BulkDeletePayloadRejectsEmptyAndMalformedChatIds` covers empty, malformed, and valid arrays.

---

## BUG-SWARM-077 — Auto Decide High warns as often as Medium outside version control

- Severity: P2
- Status: Fixed in working tree; policy matrix regression passes
- Labels: `bug`, `permissions`, `auto-decide`, `ui`, `P2`

The visible descriptions were reversed, and the approval matrix made High and Medium identical for
ordinary commands outside a VCS workspace. Low now warns about more potentially risky commands,
Medium warns about a moderate number, and High warns only for the highest-risk classification. The
tier icons now follow the same strict-to-permissive direction.

---

## BUG-SWARM-078 — Multi-chat deletion is not crash-atomic

- Severity: P1
- Status: Confirmed; unresolved
- Labels: `bug`, `delete`, `crash-recovery`, `data-integrity`, `P1`

The batch preflights and rolls back ordinary in-process failures, but local files are still removed
sequentially without a journal or recoverable staging directory. Process termination midway can
leave a partial batch. A durable transaction journal/trash phase is required; it was not added as
an unrequested storage subsystem.

---

## BUG-SWARM-079 — Folder native-workspace deletion is not staged atomically

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `folders`, `provider:gemini`, `cleanup`, `P2`

Deleting a Gemini workspace history tree is recursive. If that filesystem operation fails after
removing some files, UAM completes its own tombstoned deletion and reports that provider-native
cleanup was incomplete, but it cannot retry only the remaining files. A recoverable rename/staging
transaction is needed for atomic external cleanup.

---

## BUG-SWARM-080 — Existing Copilot imports remain frozen snapshots

- Severity: P2
- Status: Confirmed; unresolved
- Labels: `bug`, `provider:copilot`, `rescan`, `history`, `P2`

Once a Copilot native identity exists locally, rescan skips it. Messages appended to an active
`events.jsonl` after the first import are therefore not merged. Refresh must preserve UAM-owned
metadata and any live unsaved tail; that merge needs a dedicated regression before enabling writes.

---

## BUG-SWARM-081 — Folder deletion removes history before stopping idle runtimes

- Severity: P1
- Status: Fixed in working tree; ordering regression and native suite pass
- Labels: `bug`, `folders`, `delete`, `runtime`, `data-integrity`, `P1`

The preflight correctly allowed an idle runtime, but folder deletion did not stop it until after
provider-native files were removed. Shutdown/final-flush behaviour could race with that cleanup or
recreate history. Idle runtimes now stop after all rollback-capable local/folder persistence has
succeeded and immediately before provider-history cleanup.
`FolderDeleteStopsIdleRuntimesBeforeNativeCleanup` guards both native cleanup calls.

---

## IMPROVEMENT-004 — Rebuild workspace folders from Unsorted chat metadata

- Severity: P3
- Status: Implemented in working tree; frontend and native regressions pass
- Labels: `enhancement`, `folders`, `unsorted`, `recovery`, `data-safety`, `P3`

The Unsorted pseudo-folder now has a right-click action and an accessible overflow action named
“Rebuild workspace folders…”. Its preview groups chats by their recorded workspace location, reuses
an existing matching workspace, and otherwise proposes a top-level workspace named from the actual
directory. Git worktree chats use their recorded source workspace instead of the temporary worktree.

Missing, inaccessible, non-directory, and unknown locations are separated in the preview. They stay
in Unsorted by default; only missing/inaccessible chats can be sent to the existing permanent-delete
confirmation. The rebuild does not create collection references and saves all available chat
assignments plus folder metadata through one rollback-capable backend operation.

Proof: `previews and applies workspace recovery from the Unsorted context menu`, `keeps unavailable
chats by default and reuses bulk confirmation before deletion`, and
`UnsortedWorkspaceRecoveryPreviewsAndRebuildsAvailableFolders` pass. The native
`UnsortedWorkspaceRecoveryRollsBackChatsWhenFolderSaveFails` regression also proves failed folder
persistence does not leave chats half-organised.

---

## BUG-SWARM-082 — Large selected chats silently break live state updates

- Severity: P1
- Status: Fixed in working tree; native and frontend regressions pass
- Labels: `bug`, `live-update`, `cef`, `performance`, `P1`

CEF rejected state-patch scripts larger than 2 MiB, while native fingerprints still advanced as if
delivery had succeeded. A selected chat's complete message history could therefore freeze both the
active transcript and compact runtime/sidebar progress until the user left and reopened the chat.
Live patches no longer carry full histories. Streaming stays incremental, and ACP completion makes
one authoritative message query through the existing CEF request path.

Proof: `StatePatchKeepsLargeLiveChatUpdatesBelowTheCefScriptLimit` and
`refreshes the active transcript after a summary-only completion patch` pass.

---

## BUG-SWARM-083 — Terminal-fallback processing is omitted from sidebar progress

- Severity: P1
- Status: Fixed in working tree; focused sidebar regression passes
- Labels: `bug`, `sidebar`, `progress`, `terminal`, `P1`

The native terminal binding can report `processing: true` while its lifecycle is still `idle`, but
the sidebar only inspected lifecycle state. Pending terminal work could therefore look inactive.
The sidebar now includes the explicit processing flag and shows the provider-neutral label “Agent
running”.

Proof: `shows terminal-fallback progress when a pending call becomes active` passes.

---

## BUG-SWARM-084 — Workspace chat drags are reclassified as folder drags

- Severity: P1
- Status: Fixed in working tree; focused frontend regression passes
- Labels: `bug`, `drag-and-drop`, `sidebar`, `chat-grid`, `P1`

A chat row inside an expanded workspace is nested under the workspace's draggable row. The chat's
`dragstart` therefore bubbled into the folder handler, which changed `effectAllowed` from `copy` to
`move` and added folder MIME types. Chromium could reject the pane's copy-only drop, while top-level
pinned or Unsorted chats still appeared to work. The chat drag source now stops propagation.

Proof: `does not reclassify a workspace chat drag as a folder drag` failed with `move` before the
fix and now passes with only `text/x-uam-chat-id` present.

---

## BUG-SWARM-085 — Summary digest is mistaken for a transcript loaded in the browser

- Severity: P1
- Status: Fixed in working tree; focused frontend regression passes
- Labels: `bug`, `live-update`, `messages`, `tool-calls`, `cef`, `P1`

A summary-only patch updates the session's native digest without supplying the matching message
array. First-open, selection, pane, and completion queries then sent that summary digest, so native
correctly replied `unchanged` while the browser remained empty or stale. UAM now tracks the digest
of the transcript actually installed in the browser separately and only advances it after adopting
a complete response. Completion still forces an authoritative response.

Proof: the completion regression and `does not mistake a newer summary digest for a transcript
already loaded in the browser` both failed against native's realistic `unchanged` contract and now
restore the complete message list while retaining cheap repeat `unchanged` checks.

---

## BUG-SWARM-086 — A new turn's first token mutates the previous assistant message

- Severity: P1
- Status: Fixed in working tree; focused frontend regression passes
- Labels: `bug`, `live-update`, `messages`, `concurrency`, `P1`

CEF prompt submission left the browser history ending in the previous assistant while native added
the new user message only to its summary-backed state. Queued, steered, goal, and Ultra-driven turns
can also advance directly from one processing turn to the next without an idle patch. Stream tokens
now reuse a persisted assistant only when `turnAssistantMessageIndex` proves it belongs to the
current turn; otherwise a new placeholder is created. A successful idle prompt still inserts its
user message locally, while continuous turns hydrate the committed previous turn at the serial
boundary.

Proof: the idle and continuous-turn regressions failed with `FirstSecond`/`First tailSecond` and now
preserve each user/assistant turn as distinct bubbles.

---

## BUG-SWARM-087 — Visible background panes remain stale after completion

- Severity: P2
- Status: Fixed in working tree; focused frontend regression passes
- Labels: `bug`, `chat-grid`, `live-update`, `concurrency`, `P2`

The completion refresh watched only the active chat, while pane hydration watched only changes to
the pane IDs. A visible non-active pane that completed without a layout change therefore kept its
stream placeholder and omitted final tool blocks. MainPanel now detects both a one-time
`processing: true` to `false` transition and a continuous processing turn-serial advance, hydrating
the committed turn without loading hidden background chats.

Proof: both visible-background completion and continuous-next-turn regressions pass with one request
for only the affected pane.

---

## BUG-SWARM-088 — Codex Ultra collaboration tool items are discarded

- Severity: P2
- Status: Fixed in working tree; native regression passes
- Labels: `bug`, `provider:codex`, `ultra`, `sub-agents`, `tool-calls`, `P2`

Codex 0.145 emits spawn/send/wait/resume/close activity as `collabAgentToolCall`, but UAM's Codex
item allowlist stopped at command, file, MCP, and dynamic tool calls. Collaboration activity was
silently dropped, explaining missing sub-agent/tool bubbles and poor child-thread attribution under
Ultra. UAM now recognises the current schema type, uses its tool name, normalises `inProgress`, keeps
the structured item details, marks it as a sub-agent action, and retains the first receiver thread
for the existing single-child compatibility path.

Ultra itself was not downgraded: UAM sends both `effort: "ultra"` and collaboration-mode reasoning
`ultra`, and Codex's local 0.145 schema states that Ultra enables proactive multi-agent behaviour.

Proof: `CodexAppServerPersistsCollaborationAgentToolCalls` covers started/completed
spawn/send/wait/resume/close activity, multiple receiver IDs, and all persisted tool blocks.

---

## BUG-SWARM-089 — Deferred live summaries emit revision-only UI patches

- Severity: P2
- Status: Fixed in working tree; native and frontend regressions pass
- Labels: `bug`, `performance`, `live-update`, `cef`, `P2`

Selected-chat summaries are intentionally limited to ten updates per second, but intervening 16 ms
polls still posted state patches containing only a revision. The frontend then rebuilt session and
goal collections despite having no changed data, causing avoidable React work during streaming.
Native now keeps the summary deferred without posting an empty patch, while the frontend preserves
collection identity if a revision-only patch is received.

Proof: `StatePushSkipsRevisionOnlyPatchWhileSelectedSummaryIsDeferred` and
`preserves collection identity for a revision-only patch` pass.

---

## BUG-SWARM-090 — Forced transcript refresh preserves longer corrupt browser history

- Severity: P1
- Status: Fixed in working tree; focused frontend regression passes
- Labels: `bug`, `messages`, `reconciliation`, `duplicates`, `P1`

Message reconciliation rejected any authoritative array shorter than the browser's current
non-streaming list. Ghost or duplicate frontend bubbles could therefore block a correct stored
history containing a missing middle message, and caching that response's digest would make the
stale list permanent. Forced refreshes now replace the browser suffix authoritatively; ordinary
mid-stream refreshes retain the conservative guard, and a digest is cached only after adoption.

Proof: `lets a forced authoritative refresh replace a longer corrupt browser transcript` failed
with two ghost messages and now restores the shorter stored sequence including its missing middle.

---

## BUG-SWARM-091 — Reasoning deltas are streamed into ordinary answer text

- Severity: P2
- Status: Fixed in working tree; native ACP and fast-path contract regressions pass
- Labels: `bug`, `messages`, `reasoning`, `streaming`, `provider:codex`, `provider:claude`, `P2`

Generic ACP thoughts, Codex reasoning/summary deltas, and Claude thinking blocks were persisted as
thoughts but also emitted through the untyped assistant-token fast path. The browser temporarily
appended those bytes to answer content; authoritative hydration then moved or removed them, making
bubbles visibly change. Thought deltas now travel only through the existing typed turn timeline and
persisted thought fields. Assistant answer deltas retain fast token streaming.

Proof: native thought/reasoning persistence regressions and the complete ACP/core test binary pass;
`ThoughtHandlersDoNotUseAssistantTokenFastPath` guards generic ACP, Codex, and Claude thought
branches against reintroducing `PushStreamToken`.

---

## BUG-SWARM-092 — Replay filtering confuses history snapshots with legitimate repeated replies

- Severity: P1
- Status: Fixed in working tree; red-first native regression and ACP/core suite pass
- Labels: `bug`, `messages`, `acp`, `replay`, `data-loss`, `P1`

History replay filtering scanned past the next expected historical event and could match a later
assistant event by content. A legitimate new reply such as “OK” was then discarded before it
reached persistence or the UI. A purely ordered fix would regress Gemini's supported assistant-first
cumulative history snapshots. Replay is now ordered by default, but a mismatched assistant update
switches to cumulative replay only when it begins with a distinctive known prefix of at least the
existing replay threshold. Short repeated replies end filtering and remain live.

Proof: `AcpAssistantReplayIsStrippedFromNewTurn` and
`AcpNewShortAssistantReplyIsNotMistakenForOutOfOrderReplay` now initialise replay through the real
prompt-start path. The former suppresses a long assistant-first snapshot and keeps its new suffix;
the latter persists a new repeated “OK”. The complete ACP/core binary passes.

---

## BUG-SWARM-093 — Stale turn indexes can hide transcript bubbles or the live timeline

- Severity: P2
- Status: Fixed in working tree; focused ChatView regression passes
- Labels: `bug`, `messages`, `rendering`, `concurrency`, `P2`

ChatView originally checked only that retained ACP turn indexes were in bounds before replacing a
persisted message with the live timeline. Role checks alone still accepted an older user/assistant
pair with matching roles, and an accepted pair outside the 200-message render window suppressed the
fallback entirely. Indexed placement now requires visible current-tail anchors: the assistant is
the final message and the user is the latest user immediately associated with that tail. Otherwise
the timeline uses its separate fallback without hiding old transcript content.

Proof: regressions cover role-inverted indexes, same-role stale indexes, and otherwise valid anchors
outside the rendered window. All persisted bubbles and exactly one visible current timeline remain.

---

## BUG-SWARM-094 — Completed fallback duplicates an already-persisted assistant answer

- Severity: P2
- Status: Fixed in working tree; focused ChatView regressions pass
- Labels: `bug`, `messages`, `rendering`, `duplicates`, `P2`

When turn anchors were stale, ChatView rendered every persisted bubble and then rendered the complete
turn timeline again as a fallback. A completed answer already present in the latest persisted
assistant bubble therefore appeared twice. Completed fallback is now suppressed only when its
concatenated assistant-text events exactly match the latest assistant after the latest user. Active
processing always keeps its fallback, because matching partial text is not proof of completion.

Proof: completed matching text renders once; the same setup while processing still renders the live
fallback. The focused ChatView suite passes 76/76.

---

## BUG-SWARM-095 — Delayed turn-boundary hydration erases newer streamed text

- Severity: P2
- Status: Fixed in working tree; red-first frontend regression passes
- Labels: `bug`, `messages`, `streaming`, `race-condition`, `ultra`, `P2`

A continuous queued, goal, steer, or Ultra turn starts an authoritative transcript request at the
turn boundary. If its response arrived after the next turn had already created a streaming
assistant placeholder, authoritative reconciliation removed that placeholder and its text. A forced
request now captures the chat message-array identity and discards the whole stale response,
including digest updates, if newer message work changed that array. Completed-turn authoritative
refresh remains exact and can still remove ghost bubbles.

Proof: the deferred-response regression streams the next answer before resolving the older
hydration request and verifies that the live token remains visible.

---

## BUG-SWARM-096 — Empty chained turns can hide the next queued user message

- Severity: P2
- Status: Fixed in working tree; active and background regressions pass
- Labels: `bug`, `messages`, `queue`, `multi-pane`, `ultra`, `P2`

Continuous-turn hydration was gated on the previous turn owning an assistant message. A valid turn
can finish with no assistant bubble; when the next queued prompt started, its user message therefore
remained absent until final completion, and the next answer appeared without its question. A
strictly increasing turn serial while processing remains true is already the boundary invariant, so
the unnecessary assistant-index gates were removed for active and visible background panes.

Proof: active and two-pane regressions advance from an empty assistant turn and observe one
transcript hydration for the new queued turn.

---

## UX-005 — Default-theme text selection is too subtle

- Severity: P2
- Status: Fixed in working tree; selection-token regression passes
- Labels: `ux`, `accessibility`, `theme`, `selection`, `P2`

The global selection background used the 9% accent tint, producing little visible separation from
the default dark surface and making copy selection hard to confirm. It now uses a 65% accent/base
mix while keeping the existing light selected-text colour.
`keeps default text selection visibly distinct from the page surface` enforces a minimum 50%
accent mix and the selected-text token.

---

## Requested interface enhancements implemented

- `IMPROVEMENT-001`: Unsorted is now a visual-only collapsible pseudo-folder. Search temporarily
  reveals matching chats without changing the user's collapsed preference.
- `IMPROVEMENT-002`: Plain click establishes an anchor; Shift-click selects the visible range in
  exact sidebar order. One confirmation and one backend transaction delete the batch.
- `IMPROVEMENT-003`: A sidebar chat can be dragged directly onto any visible grid pane, including
  an empty pane. The target is outlined and the existing pane assignment/swap persistence is reused.
- `IMPROVEMENT-004`: Unsorted can preview and rebuild top-level workspace folders from recorded chat
  locations. Missing or inaccessible locations remain unchanged unless separately confirmed for
  deletion.
- `IMPROVEMENT-005`: Reliable pane drag/drop replaces the old right-click “Show in pane” colour
  buttons. Pane colour indicators remain as read-only placement feedback.

---

## Investigated and not classified as bugs

- Full transcript hashing has a theoretical cost for many simultaneously hydrated multi-megabyte
  chats, but no latency failure or meaningful benchmark regression was demonstrated. Caching the
  digest would add broad invalidation risk, so no performance issue was filed.
- Stderr output currently counts as ACP runtime activity and can postpone the setup inactivity
  timeout. The audit did not establish whether provider stderr heartbeats occur in this state or
  whether they should count as liveness, so this remains an observation rather than a bug.
- The current Copilot 1.0.75 binary accepted UAM's `--acp --stdio` and `--effort none` launch
  forms in direct smoke checks. No additional Copilot argv defect was found beyond the recorded
  version gate, launch-time effort lifecycle, and startup-discovery timing issues.
- Codex Ultra transport is correct in the inspected route. The observed confusion came from current
  `collabAgentToolCall` events being discarded and concurrent pane transcripts not finalising, not
  from UAM silently changing Ultra to another reasoning level.
- Structured ACP `current_mode_update` is not appended to chat text: the handler normalizes
  `auto_edit` to `acceptEdits`, updates `currentModeId`, and returns before message or turn-event
  creation. A scan of 280 saved chats found no short “mode update”/“auto edit” assistant message.
  `/safety` does show a temporary structured status banner, and the composer already shows the
  persistent `Auto Decide: Low/Medium/High` chip. No brittle English-text parser was added.
- No additional Settings bottleneck survived the second performance pass after the forced
  animations, duplicate theme refresh, quadratic branch lookup, and inactive binding subscriptions
  were removed. Backdrop blur remained only a suspicion, so it was not changed.
- If both the primary tombstone ledger and its retained backup are externally deleted, UAM cannot
  distinguish that data loss from a first run and will start with an empty ledger. No third marker
  was added for this catastrophic/manual deletion case.

---

## GitHub recreation checklist

For each entry:

1. Use the `BUG-SWARM-NNN` heading as the issue title without the numeric prefix if preferred.
2. Copy Severity, Symptom, Reproduction, Root cause/blast radius, and Proof/Required regression
   into the issue body.
3. Apply the listed labels.
4. For fixed entries, link the eventual commit/PR and keep the red-first test name.
5. Keep BUG-SWARM-017, BUG-SWARM-026, BUG-SWARM-053, BUG-SWARM-058, BUG-SWARM-059,
   BUG-SWARM-078, BUG-SWARM-079, and BUG-SWARM-080 open until their required regressions pass.

## Published GitHub issue map

The 98 audited entries were consolidated into 86 root-cause records: reopened issue #125 and 85
new issues. “Fixed by release PR” means the issue must remain open until the release PR merges.

| GitHub issue | Ledger IDs | Intended status | Title |
| --- | --- | --- | --- |
| [#125](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/125) | BUG-SWARM-029 | Fixed by release PR | Preserve selected reasoning effort when creating a new chat |
| [#197](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/197) | BUG-SWARM-001, BUG-SWARM-014 | Fixed by release PR | Preserve provider-advertised Max and Ultra reasoning efforts |
| [#198](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/198) | BUG-SWARM-002, BUG-SWARM-022, BUG-SWARM-023 | Fixed by release PR | Reconcile optimistic model options with canonical native state |
| [#199](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/199) | BUG-SWARM-003, BUG-SWARM-016 | Fixed by release PR | Preserve Small-model mode across rollback and history refresh |
| [#200](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/200) | BUG-SWARM-004 | Fixed by release PR | Failed settings writes leave rejected values active in memory |
| [#201](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/201) | BUG-SWARM-005 | Fixed by release PR | Provider-default menus are not keyboard operable |
| [#202](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/202) | BUG-SWARM-006 | Fixed by release PR | Speed can change while runtime configuration is locked |
| [#203](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/203) | BUG-SWARM-007 | Fixed by release PR | New Chat hides model-discovery failure and blocks retry |
| [#204](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/204) | BUG-SWARM-008, BUG-SWARM-021 | Fixed by release PR | Make terminal startup and repeated errors retryable |
| [#205](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/205) | BUG-SWARM-009 | Fixed by release PR | Live terminal palette does not follow app theme |
| [#206](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/206) | BUG-SWARM-010, BUG-SWARM-024, BUG-SWARM-028 | Fixed by release PR | Complete ACP cancellation only after provider completion is drained |
| [#207](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/207) | BUG-SWARM-011 | Fixed by release PR | Copilot versions 1.0.51–1.0.59 are incorrectly marked compatible |
| [#208](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/208) | BUG-SWARM-012, BUG-SWARM-026 | Open — unresolved | Represent Codex Speed as inherit, explicit tier, or explicit clear |
| [#209](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/209) | BUG-SWARM-013, BUG-SWARM-027 | Fixed by release PR | Preserve and enforce model-specific Codex Speed tiers |
| [#210](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/210) | BUG-SWARM-015 | Fixed by release PR | Codex terminal fallback ignores selected Reasoning and Speed |
| [#211](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/211) | BUG-SWARM-017 | Open — unresolved | A provider's first chat cannot discover its current models |
| [#212](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/212) | BUG-SWARM-018 | Fixed by release PR | Copilot model changes can desynchronize visible and launch-time effort |
| [#213](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/213) | BUG-SWARM-019 | Fixed by release PR | Settings Chat Defaults ignores the discovered model catalog |
| [#214](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/214) | BUG-SWARM-020, BUG-SWARM-025 | Fixed by release PR | Normalize model, Reasoning, and Speed as one compatible tuple |
| [#215](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/215) | BUG-SWARM-030 | Fixed by release PR | Importing provider skills also imports their reference documents |
| [#216](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/216) | BUG-SWARM-031 | Fixed by release PR | Filtering Skills can leave actions bound to a hidden entry |
| [#217](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/217) | BUG-SWARM-032 | Fixed by release PR | Skills child overlays are not complete accessible dialogs |
| [#218](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/218) | BUG-SWARM-033 | Fixed by release PR | A large skill-import preview can exceed the viewport |
| [#219](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/219) | BUG-SWARM-034 | Fixed by release PR | Skill save/import failures are hidden behind the active child overlay |
| [#220](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/220) | BUG-SWARM-035 | Fixed by release PR | All-skipped skill imports trap their result behind the preview |
| [#221](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/221) | BUG-SWARM-036 | Fixed by release PR | Skills directory browse/save failures are invisible in Settings |
| [#222](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/222) | BUG-SWARM-037 | Fixed by release PR | Auto-approval can choose a persistent “always allow” option |
| [#223](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/223) | BUG-SWARM-038 | Fixed by release PR | Copilot ACP launch bypasses the supported-version gate |
| [#224](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/224) | BUG-SWARM-039 | Fixed by release PR | State patches miss edits to earlier transcript messages |
| [#225](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/225) | BUG-SWARM-040 | Fixed by release PR | Renaming a folder directory leaves its chats on the old workspace |
| [#226](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/226) | BUG-SWARM-041 | Fixed by release PR | Retrying a message can use a changed skill instead of the sent snapshot |
| [#227](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/227) | BUG-SWARM-042 | Fixed by release PR | A ready-then-crash loop never exhausts ACP reconnect attempts |
| [#228](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/228) | BUG-SWARM-043, BUG-SWARM-052 | Fixed by release PR | Timeout unanswered ACP model and mode control requests |
| [#229](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/229) | BUG-SWARM-044 | Fixed by release PR | Claude structured launch emits unsupported permission modes |
| [#230](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/230) | BUG-SWARM-045 | Fixed by release PR | Folder storage ignores a valid backup when the primary file is absent |
| [#231](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/231) | BUG-SWARM-046 | Fixed by release PR | Atomic helper paths can corrupt Unicode on Windows |
| [#232](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/232) | BUG-SWARM-047 | Fixed by release PR | One Escape press closes both Skills and Settings |
| [#233](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/233) | BUG-SWARM-048 | Fixed by release PR | A previous Skills error leaks into a new workflow |
| [#234](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/234) | BUG-SWARM-049 | Fixed by release PR | Claude live permission changes can keep the old launch configuration |
| [#235](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/235) | BUG-SWARM-050 | Fixed by release PR | Native-history reconciliation erases saved skill snapshots |
| [#236](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/236) | BUG-SWARM-051 | Fixed by release PR | Folder migration does not match `~/…` against an expanded chat path |
| [#237](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/237) | BUG-SWARM-053 | Open — unresolved | Copilot compatibility probing can suppress automatic model discovery |
| [#238](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/238) | BUG-SWARM-054, BUG-SWARM-055 | Fixed by release PR | Make Move to Collection a dismissible, keyboard-complete submenu |
| [#239](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/239) | BUG-SWARM-056 | Fixed by release PR | Workspace rescan omits GitHub Copilot chats |
| [#240](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/240) | BUG-SWARM-057 | Fixed by release PR | Rescan can replace a hydrated chat and lose an unsaved response tail |
| [#241](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/241) | BUG-SWARM-058 | Open — unresolved | Startup history discovery remains limited to the default native provider |
| [#242](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/242) | BUG-SWARM-059 | Open — unresolved | Import failure is reported like a successful zero-result rescan |
| [#243](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/243) | BUG-SWARM-060 | Fixed by release PR | Delete rollback can erase messages from an unloaded chat summary |
| [#244](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/244) | BUG-SWARM-061 | Fixed by release PR | A deleted chat can be recovered from its orphan `.bak` file |
| [#245](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/245) | BUG-SWARM-062 | Fixed by release PR | Deleted Codex/Copilot imports return on the next rescan |
| [#246](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/246) | BUG-SWARM-063 | Fixed by release PR | Settings deliberately delays every open and section change |
| [#247](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/247) | BUG-SWARM-064 | Fixed by release PR | Opening Settings starts a duplicate custom-theme refresh |
| [#248](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/248) | BUG-SWARM-065 | Fixed by release PR | Sidebar branch lookup becomes quadratic as chat count grows |
| [#249](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/249) | BUG-SWARM-066 | Fixed by release PR | Inactive status filtering still subscribes to every runtime binding |
| [#250](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/250) | BUG-SWARM-067 | Fixed by release PR | Hidden chats can remain selected for bulk deletion |
| [#251](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/251) | BUG-SWARM-068 | Fixed by release PR | Grid cleanup can leave the active pane pointing outside the visible layout |
| [#252](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/252) | BUG-SWARM-069 | Fixed by release PR | Single-chat deletion leaves keyed frontend state behind |
| [#253](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/253) | BUG-SWARM-070 | Fixed by release PR | Valid Copilot histories above 12 MiB are silently skipped |
| [#254](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/254) | BUG-SWARM-071 | Fixed by release PR | A malformed tombstone file fails open and resurrects deleted chats |
| [#255](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/255) | BUG-SWARM-072 | Fixed by release PR | Chat deletion ignores pending ACP control requests |
| [#256](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/256) | BUG-SWARM-073 | Fixed by release PR | Single-chat deletion leaves Gemini's native transcript behind |
| [#257](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/257) | BUG-SWARM-074 | Fixed by release PR | Deleting a workspace can resurrect its Copilot/Codex chats |
| [#258](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/258) | BUG-SWARM-075 | Fixed by release PR | Summary deletion errors are discarded |
| [#259](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/259) | BUG-SWARM-076 | Fixed by release PR | Empty bulk-delete input returns a conflict instead of bad request |
| [#260](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/260) | BUG-SWARM-077 | Fixed by release PR | Auto Decide High warns as often as Medium outside version control |
| [#261](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/261) | BUG-SWARM-078 | Open — unresolved | Multi-chat deletion is not crash-atomic |
| [#262](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/262) | BUG-SWARM-079 | Open — unresolved | Folder native-workspace deletion is not staged atomically |
| [#263](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/263) | BUG-SWARM-080 | Open — unresolved | Existing Copilot imports remain frozen snapshots |
| [#264](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/264) | BUG-SWARM-081 | Fixed by release PR | Folder deletion removes history before stopping idle runtimes |
| [#265](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/265) | IMPROVEMENT-004 | Fixed by release PR | Rebuild workspace folders from Unsorted chat metadata |
| [#266](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/266) | BUG-SWARM-082 | Fixed by release PR | Large selected chats silently break live state updates |
| [#267](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/267) | BUG-SWARM-083 | Fixed by release PR | Terminal-fallback processing is omitted from sidebar progress |
| [#268](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/268) | BUG-SWARM-084 | Fixed by release PR | Workspace chat drags are reclassified as folder drags |
| [#269](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/269) | BUG-SWARM-085 | Fixed by release PR | Summary digest is mistaken for a transcript loaded in the browser |
| [#270](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/270) | BUG-SWARM-086 | Fixed by release PR | A new turn's first token mutates the previous assistant message |
| [#271](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/271) | BUG-SWARM-087 | Fixed by release PR | Visible background panes remain stale after completion |
| [#272](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/272) | BUG-SWARM-088 | Fixed by release PR | Codex Ultra collaboration tool items are discarded |
| [#273](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/273) | BUG-SWARM-089 | Fixed by release PR | Deferred live summaries emit revision-only UI patches |
| [#274](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/274) | BUG-SWARM-090 | Fixed by release PR | Forced transcript refresh preserves longer corrupt browser history |
| [#275](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/275) | BUG-SWARM-091 | Fixed by release PR | Reasoning deltas are streamed into ordinary answer text |
| [#276](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/276) | BUG-SWARM-092 | Fixed by release PR | Replay filtering confuses history snapshots with legitimate repeated replies |
| [#277](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/277) | BUG-SWARM-093 | Fixed by release PR | Stale turn indexes can hide transcript bubbles or the live timeline |
| [#278](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/278) | BUG-SWARM-094 | Fixed by release PR | Completed fallback duplicates an already-persisted assistant answer |
| [#279](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/279) | BUG-SWARM-095 | Fixed by release PR | Delayed turn-boundary hydration erases newer streamed text |
| [#280](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/280) | BUG-SWARM-096 | Fixed by release PR | Empty chained turns can hide the next queued user message |
| [#281](https://github.com/davidtaylor6130/Universal-Agent-Manager/issues/281) | UX-005 | Fixed by release PR | Default-theme text selection is too subtle |
