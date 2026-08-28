# Computer Use and Remote Helper Progress

This is the append-only implementation ledger for the Computer Use and remote-helper effort.
Earlier release work remains preserved in [`../progress.md`](../progress.md).

## Goal

Integrate, harden, and exhaustively verify UAM's own local Computer Use system first. Only after
that sign-off, implement and verify a headless remote execution helper using the approved
SSH-bridged architecture, with remote Computer Use disabled fail-closed.

## Fixed constraints

- Work on `codex/computer-use-remote-helper` from the current `codex/release-v4.5.7` state.
- Do not modify or replace the UAM bundle installed under the user's Applications directory.
- Build and test only inside repository build locations and isolated temporary data roots.
- Do not use Gemini CLI on this local system.
- Do not modify, publish, or mark ready the existing draft release pull request.
- Computer Use means UAM's own local Computer Use implementation, not Codex Computer Use.
- Remote execution retains the shared capability model, but screen, mouse, and keyboard control
  are unavailable for remote hosts and must be rejected by both UI and backend.
- Append evidence here; never rewrite prior entries.

## 2026-08-27 — Start

- Approved architecture: the desktop UAM backend remains the controller and canonical data owner;
  a headless helper reuses UAM runtime services; a long-lived SSH bridge carries framed
  bidirectional messages; a local user-scoped helper service owns persistent remote jobs.
- Confirmed the current React/native bridge is CEF Message Router (`window.cefQuery`), not HTTP.
  A remotely exposed HTTP control API is therefore not an existing boundary to reuse.
- Created isolated branch `codex/computer-use-remote-helper` at `37d7c8dc`.
- Preserved all pre-existing untracked planning files and root `node_modules/`.
- Status: `IN_PROGRESS` — auditing and reconciling the existing Computer Use branch before edits.

## 2026-08-27 — Current-release baseline

- Frontend suite: PASS — 37 files, 551 tests.
- Isolated native configure: PASS in `Builds/computer-use-remote/`; the first sandboxed attempt
  could not resolve GitHub while fetching pinned dependencies, then the approved network-enabled
  retry completed normally.
- Production frontend/native packaged build: PASS.
- Packaged repository app signature: PASS.
- Native CTest suite: PASS — 5/5 targets in 46.27 seconds.
- `git diff --check`: PASS.
- The user Applications bundle was not opened, modified, replaced, or used by these checks.
- Branch analysis found two Computer Use commits (`3f4c9da3`, `126fef59`) based on an ancestor
  before several releases. The safe integration boundary is those Computer Use changes, reconciled
  onto the current branch; a wholesale historical branch merge would import unrelated stale code.

## 2026-08-27 — Computer Use integration checkpoint

- Reconciled only the two Computer Use commits onto the current release architecture. Shared files
  kept their current-release implementations; Computer Use was reconnected through the current CEF,
  ACP, persistence, provider-runtime, and React store paths instead of accepting stale branch code.
- Local UAM Computer Use now has explicit backend routing (`auto`, `provider`, `uam`), exact
  window/display targeting, runtime-only enablement and target grants, cooperative pause/stop,
  bounded redacted history, per-action native approval, and one active UAM-controlled chat.
- Provider routing is conservative: Codex provider-native Computer Use is opt-in; the default path
  disables the Codex built-in and injects UAM's packaged `uam-computer` MCP companion. Other
  structured providers receive the same UAM MCP surface through their existing launch protocol.
- Added the visible React workflow: `/computer`, Composer option and active chip, backend choice,
  target refresh/selection, enable/disable, and pause/resume controls.
- Added 10 focused native Computer Use tests plus two updated launch-contract expectations.
- Frontend production build: PASS.
- Frontend suite: PASS — 37 files, 551 tests.
- Full isolated packaged native build: PASS, including nested `UAM Computer Use.app` creation,
  signing, and final bundle verification.
- Full native CTest suite: PASS — 5/5 targets in 47.54 seconds.
- The Applications-installed UAM bundle was not opened, modified, replaced, or used.
- Status: `IN_PROGRESS` — automated integration gates pass; focused frontend regression checks and
  real packaged GUI/companion stress testing remain before Computer Use sign-off.

## 2026-08-28 — Model-requested Computer Use checkpoint

- Added the acceptance requirement that OpenCode with Qwen3.6 35B A3B must prove the model can
  request UAM Computer Use before the user enables it; Codex must then prove the same UAM-owned
  path with Codex native Computer Use explicitly disabled. Gemini CLI remains excluded locally.
- Found and fixed a real gap: the UAM MCP companion was previously injected only after a manual
  target grant, so a model could not request Computer Use. The companion is now advertised for
  portable UAM-backed chats while inactive; `computer_observe` creates a bounded runtime-only
  request that opens the UAM approval panel and tells the model to wait and retry.
- Idle companions no longer acquire the single-controller lock. Only a companion launched with a
  configured target owns it, preserving concurrent provider chats without weakening the one-active
  desktop-controller invariant.
- A pending request may safely interrupt only its own active structured turn so the user can select
  a target and activate UAM control. The request is cleared on approval/stop and on app startup;
  target and enablement remain runtime-only.
- Added native coverage for pre-enable MCP request creation, disabled-state polling, request
  clearing, and provider launch routing. Added ChatView coverage for automatic request-panel opening.
- Focused frontend store tests: PASS — 131/131.
- Focused ChatView tests: PASS — 98/98.
- Focused native request/launch tests: PASS — 4/4.
- Full isolated packaged native build and nested companion signing: PASS.
- Full native CTest suite: PASS — 5/5 targets in 46.39 seconds.
- Status: `IN_PROGRESS` — rebuild the distinct test bundle, then perform the OpenCode/Qwen and
  Codex end-to-end GUI acceptance runs against a disposable TextEdit target.

## 2026-08-28 — Isolated GUI launch crash triage

- Reviewed macOS incident `A764CF1E-CA43-4400-91A1-738C7B596DA1` for the deliberately renamed
  `UAM Computer Use Test.app` bundle. The main thread aborted inside HIServices
  `_RegisterApplication` while `NSApplication.sharedApplication` was initializing, before CEF,
  provider startup, state loading, or any Computer Use code executed.
- The report identifies Codex as the parent because the test harness launched the bundle's inner
  executable directly. Launching the same signed isolated bundle through macOS LaunchServices now
  succeeds and keeps its test-only bundle identifier and data root isolated from the Applications
  build.
- Classification: test-harness launch-path failure, not a verified UAM release or Computer Use
  defect. No product-code change or GitHub issue is justified unless the supported LaunchServices
  launch path reproduces it.
- Status: `IN_PROGRESS` — continue the OpenCode/Qwen model-request acceptance run in the cleanly
  launched isolated bundle.

## 2026-08-28 — OpenCode/Qwen packaged-GUI checkpoint

- Found and fixed a real ACP composition defect: workspace MCP injection replaced the request's
  existing MCP array, silently removing `uam-computer`. New, load, resume, and invalid-session retry
  now merge the UAM companion with configured workspace servers. Regression tests cover all paths.
- Verified in the packaged isolated GUI that OpenCode used `inferdeck/qwen3.6-35b-a3b`, Qwen itself
  called `uam-computer_computer_observe` while UAM control was inactive, and UAM opened its target
  approval panel without a pre-enabled toggle.
- Removed only OpenCode's redundant provider-level prompt for the two exact UAM Computer Use tools.
  UAM's native observation/action confirmations remain mandatory. Generated adapter JSON and agent
  Markdown permissions both have regression coverage.
- Production frontend/native packaged build: PASS. Nested helper signing: PASS. Full native CTest
  after the ACP merge: PASS — 5/5. A final CTest rerun remains after the OpenCode permission change.
- The user manually granted Screen Recording. The existing helper retained the old denial until UAM
  control was toggled off/on; the isolated helper was then restarted and retargeted to TextEdit.
- Reviewed incident `6959E5B0-3766-410C-81E4-98D0AF218ACF`: the faulting process was Google Chrome
  for Testing, parented by ChatGPT Computer Use's Node runtime in a separate Inferdeck workspace.
  UAM appeared only as macOS's responsible process and did not crash. Classification: external
  direct-GUI-launch test-harness noise; no UAM issue or code change justified.
- Status: `IN_PROGRESS` — Qwen end-to-end observe/edit/verify remains active; remote work has not
  started.

## 2026-08-28 — Qwen action-path root cause

- Traced the repeated Screen Recording prompt to a stale macOS TCC code requirement: the isolated
  bundle had been replaced/re-signed while its old process was still running. Resetting only
  `com.universalagentmanager.desktop.computerusetest`, relaunching the unchanged bundle through
  LaunchServices, and restarting it once after approval produced a successful packaged observation.
- The authoritative provider path is proven: OpenCode used Qwen3.6 35B A3B and Qwen called the
  packaged `uam-computer_computer_observe`; observations of the exact TextEdit window complete.
- Found the action blocker from the persisted ACP tool results. `computer_observe` returned
  `frameId` only in MCP `structuredContent`; OpenCode delivered the text/image content to Qwen but
  not that structured field. Qwen therefore had no valid frame id, and every action failed stale
  before native confirmation. The user was not denying or missing confirmation dialogs.
- Fixed the shared MCP result boundary so observation/action text contains the exact `frameId` and
  `actionApplied` state as well as retaining structured metadata. This also keeps action-applied
  failures from being retried by adapters that omit `structuredContent`.
- A separate verified defect remains deferred until this acceptance run finishes: branching or
  restarting a chat can leave a stopped controller recorded as the single active owner, and the
  one-controller policy conflicts with UAM's required multi-chat support. Restarting only the
  isolated unchanged bundle cleared the stale in-memory lock for continued testing.
- Status: `IN_PROGRESS` — run focused/native gates, rebuild the isolated bundle without changing
  the installed Applications build, then repeat Qwen observe/action/verify with visible approval.

## 2026-08-28 — Packaged Qwen permission-boundary evidence

- Verified the final unchanged isolated bundle in macOS System Settings: Screen & System Audio
  Recording is enabled for `UAM Computer Use Test.app`. Restarted only that exact isolated bundle;
  the Applications-installed UAM was not touched.
- Repeated the model-request journey with OpenCode 1.18.20 and Qwen3.6 35B A3B. Qwen called the
  packaged `uam-computer_computer_observe` while inactive, UAM opened the target panel, and the user
  granted only the exact TextEdit `Untitled.rtf` window. Provider-native computer use remained out
  of the path.
- The real ACP transcript now proves the metadata repair: three completed observations exposed
  `frameId: 1`, `frameId: 2`, and `frameId: 3` in model-visible text with
  `actionApplied: false`.
- The earlier unmatched-confirmation flood is quantified rather than inferred. The same chat stored
  278 observation calls, including 256 OpenCode `MCP error -32001: Request timed out` results. The
  hidden native modal blocked the serial MCP server while Qwen/OpenCode queued parallel observation
  calls; after the client had marked them failed, those stale calls drained through the helper.
  The alert-fronting fix removes the initiating hidden state, but duplicate/cancellation protection
  remains an acceptance item unless the fixed build proves the one-call contract under stress.
- Action execution is not yet accepted. Qwen's first two action calls referenced element ids that
  were absent because the helper had no Accessibility access, so UAM rejected them before native
  confirmation. A coordinate action reached the input boundary but safely applied no input and
  returned the Accessibility requirement.
- macOS System Settings proves the user granted Accessibility to `UAM Computer Use Test.app`, while
  the process checking `AXIsProcessTrusted()` is the separately signed nested
  `UAM Computer Use.app` (`com.universalagentmanager.desktop.computer-use`), which is absent from the
  Accessibility list. The user's grant is valid; the current helper identity does not inherit it.
- Status: `IN_PROGRESS` — prove the nested helper grant and action confirmation without rebuilding,
  then fix the permission onboarding/identity contract before declaring local Computer Use usable.

## 2026-08-28 — Computer Use authority-model correction

- User testing invalidated the request-panel/manual-target workflow: the model requested control,
  but the user still had to open Computer Use, choose a target, enable it, and prompt the model to
  retry. The approved window could become stale during that loop, so this was a product defect, not
  user error or a missing permission.
- Replaced it at the MCP boundary with one target-scoped grant. The model now supplies an
  application, window, or display name in its first `computer_observe`; the companion resolves the
  live target and presents one native Deny/Allow decision. The same call continues immediately
  after Allow, and later observations/actions require no UAM confirmations until Pause, Stop,
  Computer Use off, or target closure revokes the grant.
- Deleted the runtime request-file/manual-modal handoff rather than retaining a second competing
  workflow. Manual target selection remains available only as an optional user-initiated fallback.
- UAM runtime state now hydrates from the companion-written target grant, so the GUI can display and
  stop/pause model-initiated control without preselecting the target.
- Preserved the unavoidable macOS split: Screen Recording permits observation; the separately
  signed least-privilege helper needs one OS Accessibility grant for input. This is machine setup,
  not an action-by-action UAM approval.
- Fixed packaging evidence discovered during the same boundary review: final deep signing had been
  overwriting the nested helper's empty entitlements with the outer CEF JIT entitlements. Final app
  signing is now non-deep while verification remains deep/strict, preserving an empty helper
  entitlement set.
- Automated evidence after the authority-model change: focused Computer Use native tests PASS;
  frontend tests PASS — 554/554; production frontend build PASS; native test target build PASS.
- Status: `IN_PROGRESS` — rebuild and sign the isolated application, then require OpenCode/Qwen to
  choose TextEdit, receive exactly one UAM approval, edit it without further UAM prompts, and verify
  the result before any remote-helper work begins.

## 2026-08-28 — One-grant packaged Qwen evidence and macOS rebuild boundary

- Rebuilt only the isolated `UAM Computer Use Test.app`; the Applications-installed UAM remained
  running and untouched. OpenCode used Qwen3.6 35B A3B for the acceptance turn.
- The user supplied no UAM target or Active-toggle input. Qwen independently called
  `uam-computer_computer_observe`, naming TextEdit as its requested target.
- The nested companion presented exactly one native dialog: “Allow UAM computer control”; it named
  the exact TextEdit window, offered Deny/Allow, and stated that later actions would not prompt until
  Pause, Stop, Computer Use off, or target closure. Allow was selected once.
- The same MCP call continued immediately. `control.json` was written by the companion with live
  target id, process id, title, input mode, and `state: running`; the history contains one approved
  `grant` entry followed by the observation result. No request file, manual target modal, Active
  toggle, continuation message, or per-call UAM confirmation was used.
- The observation then failed at macOS Screen Recording preflight because rebuilding the ad-hoc
  nested helper changed its code identity and invalidated the prior development-build TCC grant.
  This is separate from UAM's target grant. A Developer-ID-signed release has a stable designated
  requirement; repeated ad-hoc development rebuilds do not.
- Fixed the packaging root cause found while proving that boundary: helper signing now occurs after
  the copied main executable receives its CEF JIT signature, the CEF framework is signed explicitly,
  the final outer sign is non-deep, and deep/strict verification remains mandatory. Reproduced build
  evidence now shows empty companion entitlements, the outer CEF JIT entitlements, and a valid
  deep/strict bundle.
- Focused Computer Use native tests after the signing/order change: PASS. `git diff --check`: PASS.
- Status: `IN_PROGRESS` — macOS requires the user to grant Screen Recording once to the newly built
  `UAM Computer Use` helper. After that external security action, restart only the isolated bundle
  and resume the Qwen action/edit/verify acceptance without rebuilding it.

## 2026-08-28 — AI-driven target grant and provider-level YOLO correction

- The user rejected the remaining manual UAM panel workflow as an inverted authority model. The
  AI must choose and request its target; the user's only workflow decision is one native Allow/Deny.
- Removed the UAM target picker, refresh action, and manual Active switch from the Computer Use
  panel. While inactive it now reports `Ready for an AI request`; after approval it shows the exact
  granted target with Pause/Resume and `Stop and revoke access` controls.
- The UAM MCP server remains available while inactive, so an ordinary user prompt can cause the
  model to call `computer_observe(target=...)`. The companion resolves the live target, asks once,
  and continues the same call after Allow. No enable/retry handoff is required.
- Changed only Codex's injected `uam-computer` MCP server from write-prompting to the Codex
  `approve` policy. This removes the redundant provider approval layer for UAM observations and
  actions; all non-UAM tools retain the chat's normal permission policy. OpenCode's exact two UAM
  tool names were already configured as `allow`.
- Fixed the Computer Use protocol-surface test so listing tools does not claim the global controller
  lock; this keeps the test valid while an isolated acceptance controller is legitimately active.
- Evidence: Computer Use modal regression PASS; production frontend build PASS; full frontend suite
  PASS — 555/555; all 10 focused native Computer Use tests PASS; installed Codex CLI accepted and
  listed a temporary MCP server using `default_tools_approval_mode="approve"`; `git diff --check`
  PASS.
- Status: `IN_PROGRESS` — create one final isolated signed bundle, then perform the external macOS
  permission regrant once and complete Qwen observe/action/visible verification without rebuilding.

## 2026-08-28 — Final isolated AI-driven Computer Use build

- Closed only `Builds/computer-use-remote/UAM Computer Use Test.app` through its GUI and confirmed
  no process from that isolated path remained. The Applications-installed UAM and its processes
  remained running and were not modified.
- Reconfigured and rebuilt `Builds/computer-use-remote` with tests enabled, copied the fresh bundle
  to the isolated `UAM Computer Use Test.app` identity, and preserved the dedicated bundle id
  `com.universalagentmanager.desktop.computerusetest`.
- Final isolated bundle deep/strict signature verification: PASS. Nested `UAM Computer Use.app`
  entitlements remain an empty dictionary; the least-privilege helper was not accidentally given
  the outer CEF JIT entitlements.
- Recoverable previous isolated bundle moved to
  `/tmp/UAM Computer Use Test.pre-ai-driven.app`.
- Status: `IN_PROGRESS` — do not rebuild this bundle again before macOS permission and Qwen/Codex
  acceptance, because another ad-hoc signature would invalidate the development TCC grant.

## 2026-08-28 — Visual proof of the corrected inactive workflow

- Launched only the final isolated bundle with `UAM_DATA_DIR=/tmp/uam-computer-use-gui.23dZGL`.
- Opened the real Composer options and Computer Use panel through the GUI. The rendered inactive
  UAM surface contains `Ready for an AI request`, explicitly says the AI chooses the target, and
  says UAM asks once to Allow or Deny.
- The rendered UAM surface has no target selector, refresh action, or manual Active switch. It
  retains backend selection, provider/model disclosure, privacy scope, and the one-target approval
  boundary.
- Status: `IN_PROGRESS` — the final bundle is frozen for TCC continuity. External macOS permission
  refresh is now the only prerequisite to resume Qwen's visible edit and action verification.

## 2026-08-28 — Approval-timeout and multi-chat hardening

- Confirmed the reported loop was not a missing user decision. OpenCode timed out the still-running
  `computer_observe` while the single native target dialog waited, then drained already-failed
  queued calls through the serial helper. No per-observation or per-action UAM approval was added.
- Kept the intended authority model: the model names its target, the user answers one Deny/Allow
  decision, and the approved chat may observe and act without repeated UAM prompts until Pause,
  Stop, Computer Use off, or target closure revokes the grant.
- Configured the exact OpenCode UAM tools as `allow` with `experimental.mcp_timeout=120000`, the
  longest effective request wait in the installed OpenCode 1.18.20 runtime. Configured Codex's
  `uam-computer` server with `default_tools_approval_mode="approve"` and
  `tool_timeout_sec=300`. Other provider tools keep their normal approval policy.
- Replaced the lifetime global controller lock with a short native-input lock. Multiple chats can
  retain separately approved targets and observe concurrently; clicks, typing, and other input are
  still serialized across the instance and fail safely if another action is already in progress.
- Evidence: installed OpenCode 1.18.20 resolved the generated timeout and both exact tool policies;
  installed Codex 0.150.1 resolved the five-minute tool timeout and approval mode; 12 focused
  Computer Use/config tests PASS; the full native regression suite PASS; production frontend build
  PASS; deep/strict bundle verification PASS; nested companion entitlements remain empty;
  `git diff --check` PASS.
- Replaced only the isolated repository bundle and moved its previous build recoverably to
  `/tmp/UAM Computer Use Test.pre-timeout-lock.app`. The Applications-installed UAM was not closed,
  changed, or rebuilt.
- Status: `IN_PROGRESS` — this isolated ad-hoc build is now frozen. macOS Screen Recording must be
  refreshed once for this new signature, after which Qwen and Codex visible action acceptance can
  resume without another rebuild.

## 2026-08-28 — AI-only activation boundary cleanup

- Owner clarification made the final authority contract explicit: UAM-managed Computer Use is not
  a mode the user turns on and configures. The model names the app/window/display, UAM asks one
  native Deny/Allow question, and Allow grants target-scoped observation and input without further
  UAM prompts until Pause, Stop/revoke, Computer Use off, or target closure.
- Tracing every frontend and CEF caller found a dormant legacy path that still listed targets,
  accepted a user-selected target, required manual enablement, and rejected a second active UAM
  chat. It was no longer rendered by the panel but contradicted both the intended contract and the
  new multi-chat runtime.
- Deleted the manual target-list and target-selection bridge/store APIs and the single-UAM-chat
  guard. Manual UAM enablement now fails with an instruction to ask the AI; provider-native
  Computer Use retains its provider-owned Active switch. UAM Pause/Resume and Stop/revoke remain
  available as user safety controls.
- Evidence: focused Computer Use/store tests PASS — 132/132; full frontend suite PASS — 555/555;
  production frontend build PASS; full native regression binary PASS; application target build and
  deep/strict signature verification PASS; nested companion entitlements remain empty;
  `git diff --check` PASS.
- Replaced only the isolated repository bundle. Its prior version is recoverable at
  `/tmp/UAM Computer Use Test.pre-ai-authority-20260828-1134.app`; the Applications-installed UAM
  stayed running and untouched.
- Status: `IN_PROGRESS` — launch the final isolated build, refresh its development TCC grant once if
  macOS requires it, then complete OpenCode/Qwen one-approval observe/action/verify acceptance.

## 2026-08-28 — macOS permission-loop root cause and prompt bound

- Ran a fresh OpenCode structured chat with Qwen3.6 35B A3B from an ordinary user prompt. Qwen
  selected the already-open TextEdit target itself and called UAM Computer Use without any user
  target selection, manual Active toggle, or continuation prompt. The request completed with one
  Screen Recording denial rather than the prior queued-call flood.
- Read-only System Settings evidence showed `UAM Computer Use Test.app` visibly switched on under
  Screen & System Audio Recording while the newly rebuilt helper still received denial. The user
  had granted the permission; macOS retained a stale TCC code requirement after the ad-hoc bundle
  changed. This is a development-signing identity issue, not missing user approval.
- Removed the second custom Accessibility alert and bounded both macOS permission requests with
  process-local `std::once_flag` guards. Provider retries can no longer repeatedly invoke Screen
  Recording or Accessibility prompts within one helper process. Denial text now explains the exact
  off/on refresh and restart required when a listed switch is already on after an update.
- Rebuilt and replaced only the isolated bundle after this fix. The previous build is recoverable at
  `/tmp/UAM Computer Use Test.pre-permission-loop-20260828-1145.app`; the Applications-installed UAM
  remained running and untouched.
- Evidence: application/native targets build PASS; full native regression binary PASS; deep/strict
  isolated bundle verification PASS; companion entitlements remain empty; `git diff --check` PASS.
- Status: `BLOCKED_ON_EXTERNAL_TCC_REFRESH` — while the final isolated app is closed, toggle only
  `UAM Computer Use Test.app` off and on in Screen & System Audio Recording. Then launch it and
  repeat the same Qwen action acceptance without rebuilding.

## 2026-08-28 — Exact-tool YOLO instead of global YOLO

- Owner clarification rejected any workflow where the user manually enables UAM Computer Use or
  chooses its target. The required contract is now explicit: the model requests a concrete target,
  UAM presents one native Deny/Allow decision, and Allow gives that chat target-scoped observation
  and input without later provider or UAM action confirmations.
- Deliberately did not switch the whole chat to global YOLO, because that would also bypass normal
  shell, file, URL, and other provider permissions. Only the two injected UAM tools are trusted:
  `computer_observe` and `computer_action`. UAM's native target decision remains their authority
  boundary.
- OpenCode's base structured environment now allows those exact two tool names even when no
  provider-native agent adapter is active. Claude Code receives exact `--allowedTools` MCP names;
  GitHub Copilot receives exact `--allow-tool` MCP server/tool patterns; Codex already receives its
  per-server approve policy. Unrelated provider tools retain their existing policy.
- Evidence: installed CLI help and primary provider documentation confirmed the exact Claude and
  Copilot MCP allow-list syntax; build PASS; CTest PASS 5/5 in 51.14 seconds; Computer Use launch
  contract tests cover OpenCode, Claude, Copilot, and Codex; `git diff --check` PASS.
- Status: `IN_PROGRESS` — the isolated macOS test bundle must be rebuilt once for these final launch
  arguments, then frozen. Its Screen Recording entry must be toggled off/on once more after that
  last ad-hoc signature change before Qwen GUI acceptance resumes.

## 2026-08-28 — Final frozen AI-driven Computer Use test bundle

- Rebuilt `Builds/computer-use-remote` after the exact-tool permission fix and replaced only the
  repository's isolated `UAM Computer Use Test.app`. The Applications-installed UAM remained
  running and untouched.
- Preserved the dedicated bundle id `com.universalagentmanager.desktop.computerusetest` and version
  `4.5.7`. The previous isolated build remains recoverable at
  `/tmp/UAM Computer Use Test.pre-exact-tool-yolo-20260828-1153.app`.
- Final deep/strict code-signature verification: PASS. The nested `UAM Computer Use.app` still has
  an empty entitlement dictionary; the outer test app retains its CEF JIT entitlements.
- `git diff --check`: PASS. This exact isolated bundle is now frozen; do not rebuild it during the
  remaining Qwen and Codex GUI acceptance run.
- Status: `BLOCKED_ON_EXTERNAL_TCC_REFRESH` — because the final ad-hoc signature changed, macOS must
  refresh only `UAM Computer Use Test.app` once. With the isolated app closed, toggle its Screen &
  System Audio Recording entry off and on, then resume testing without another rebuild.

## 2026-08-28 — AI-driven approval flow proven; TCC stale hash proven

- Re-ran the real OpenCode/Qwen3.6 35B A3B GUI journey against the frozen isolated bundle and the
  already-open `Untitled.rtf` fixture. Qwen selected TextEdit and called
  `uam-computer_computer_observe` from an ordinary chat prompt; the user did not enable Computer
  Use, choose a window, or approve a provider tool call.
- UAM displayed exactly one native `Allow UAM computer control` dialog for the exact TextEdit
  target with only `Allow` and `Deny`. After `Allow`, the original observation continued and a
  retry produced no second UAM target dialog. This proves the intended model-driven,
  target-scoped exact-tool YOLO boundary.
- Corrected the acceptance harness to launch the `.app` through macOS LaunchServices instead of
  executing its binary as a Codex child. TCC attribution changed from `com.openai.codex` to the
  correct responsible application `com.universalagentmanager.desktop.computerusetest`.
- The remaining Screen Recording denial is now proven, not inferred. macOS TCC logged:
  `Failed to match existing code requirement` for Screen Capture, comparing the stored ad-hoc
  cdhash `27b8ffacd5849c3d749b1bd51098df635729e41d` with the frozen bundle's current cdhash
  `d9329b9b62c5efa43d7ccb3119126b7f83d02ddc`. The visible enabled switch therefore refers to the
  previous build identity and cannot authorize the frozen build.
- Accessibility is not the current blocker and no action confirmation is missing: Qwen cannot
  reach `computer_action` until `computer_observe` succeeds. The first real input action will ask
  macOS for Accessibility only if the current bundle does not already have it.
- Closed only `UAM Computer Use Test.app` after the run. The Applications-installed Universal Agent
  Manager remained running and untouched. The frozen bundle was not rebuilt or modified.
- Status: `BLOCKED_ON_EXTERNAL_TCC_REFRESH` — toggle the closed frozen test app's Screen & System
  Audio Recording entry off and on once to replace the stale cdhash, then relaunch it through
  LaunchServices and continue Qwen observe/action/visual verification without rebuilding.

## 2026-08-28 — Model retries can no longer request macOS permissions

- Re-tested after the owner toggled the visible Screen Recording switch. The macOS TCC event log
  recorded both modifications but retained the previous ad-hoc code requirement. It continued to
  compare stored cdhash `27b8ffacd5849c3d749b1bd51098df635729e41d` with current cdhash
  `d9329b9b62c5efa43d7ccb3119126b7f83d02ddc`; toggling alone does not replace a stale requirement
  on this macOS version.
- Did not trust Qwen's statement that it retried seven times. UAM's own append-only history proves
  the latest turn produced one `observe` failure at `11:22:30Z`. The model's retry count was a
  hallucination.
- Confirmed a real UAM bug at the shared macOS platform boundary: after capture or input preflight
  failed, the model-owned MCP process invoked `CGRequestScreenCaptureAccess` or
  `AXIsProcessTrustedWithOptions`. A provider retry or a new helper process could therefore
  re-request host permissions even though permission management is not the model's authority.
- Removed both interactive macOS permission-request APIs from model-triggered observation and
  action paths. Those paths now preflight only and fail closed with a stable diagnostic. They can
  never open or repeat a macOS permission request. UAM's one exact-target native Allow/Deny
  decision remains unchanged.
- Corrected denial guidance: after an app update, remove the stale privacy entry and add the
  current app rather than toggling the stale entry. Updated `docs/computer-use.md` accordingly.
- Added `uam_computer_use_permission_contract`, which fails if either interactive macOS request API
  is reintroduced or either preflight disappears. Native compile PASS; serial CTest PASS 6/6 in
  49.63 seconds; `git diff --check` PASS. One earlier concurrent run was invalid because two test
  binaries correctly contended for the global input lock; it was rerun serially.
- No valid local code-signing identity is installed, so repository development bundles remain
  ad-hoc signed and TCC binds them to changing cdhash values. A normally Developer-ID-signed
  release has a stable designated requirement; the isolated acceptance bundle must be rebuilt once
  for this fix, then its stale Screen Recording entry removed and the current app added once.
- Status: `IN_PROGRESS` — rebuild and freeze the isolated test bundle with the no-request fix,
  replace its TCC entry once, then rerun Qwen observe/action/visual verification. The
  Applications-installed UAM remains out of scope and untouched.

## 2026-08-28 — Frozen no-permission-request acceptance bundle

- Rebuilt only `Builds/computer-use-remote` after removing the interactive macOS permission APIs.
  Production frontend build PASS and the native application/core test targets compiled.
- Replaced only the repository's isolated `UAM Computer Use Test.app`. The previous test bundle is
  recoverable at `/tmp/UAM Computer Use Test.pre-no-os-request-20260828-1230.app`; the
  Applications-installed UAM was not opened, closed, changed, or rebuilt.
- Frozen acceptance identity: bundle id
  `com.universalagentmanager.desktop.computerusetest`, version `4.5.7`, ad-hoc cdhash
  `dd57a6f47bf8259c891cdab01af3571bc78b02c0`.
- Deep/strict signature verification PASS. The nested `UAM Computer Use.app` entitlement
  dictionary remains empty; the outer app retains only the required CEF JIT entitlements.
- Binary inspection found neither `CGRequestScreenCaptureAccess` nor
  `AXIsProcessTrustedWithOptions`. This exact GUI build cannot request macOS Screen Recording or
  Accessibility from a model tool call; only non-interactive preflight checks remain.
- Status: `BLOCKED_ON_EXTERNAL_TCC_REPLACEMENT` — macOS still stores the obsolete ad-hoc code
  requirement. With the isolated app closed, remove `UAM Computer Use Test` from Screen & System
  Audio Recording, add the current
  `Builds/computer-use-remote/UAM Computer Use Test.app`, enable it, and restart the app. Do not
  rebuild before Qwen acceptance.

## 2026-08-28 — Screen Recording acceptance proven; Accessibility isolated

- The owner removed the stale Screen & System Audio Recording entry, added the exact frozen
  repository test bundle, and enabled it. Relaunched only that bundle through LaunchServices with
  its existing `/tmp/uam-computer-use-gui.23dZGL` data root; the Applications-installed UAM was not
  launched or modified.
- Ran a fresh real GUI turn through OpenCode with local Qwen3.6 35B A3B. The model first supplied an
  imprecise target and received one bounded mismatch error, then requested `TextEdit — Untitled.rtf`.
  UAM showed exactly one native target-scoped Allow/Deny dialog. After Allow, UAM history recorded
  the grant and two successful observations at `11:55:03Z`; Screen Recording is therefore proven
  working in the frozen bundle.
- The first requested hotkey then failed once at `11:55:08Z` because macOS denied Accessibility.
  No OS permission prompt was opened and no repeated request loop occurred. This separates the
  remaining stale Accessibility grant from the now-working Screen Recording grant.
- Did not trust Qwen's prose as evidence. The acceptance result comes from UAM's append-only
  `computer-use/<chat>/history.jsonl`, the visible native approval dialog, the live TextEdit fixture,
  and the isolated process tree. The live provider was OpenCode/Qwen; Gemini CLI was not invoked.
- Gemini compatibility was checked only by static inspection of the locally installed CLI's bundled
  policy documentation/source and offline UAM launch-argument tests. Added a bundled policy allowing
  exactly `uam-computer`'s `computer_observe` and `computer_action`; no wildcard or title-based trust.
  Serial CTest PASS 6/6 in 51.49 seconds, including the no-interactive-permission and exact-policy
  contract. Live Gemini testing remains prohibited on this machine.
- Status: `BLOCKED_ON_EXTERNAL_ACCESSIBILITY_REPLACEMENT` — in Privacy & Security → Accessibility,
  remove the stale `UAM Computer Use Test` entry, add the exact frozen repository test app again,
  enable it, and restart only the isolated app. Do not rebuild it before completing Qwen and Codex
  GUI acceptance.

## 2026-08-28 — Permission-loop fix and provider policy regression gate

- macOS TCC independently confirmed the remaining Accessibility failure is another stale ad-hoc
  requirement: stored cdhash `78d70948165f58bafa1bf2f729e5068e80baaf97` versus frozen bundle
  cdhash `dd57a6f47bf8259c891cdab01af3571bc78b02c0`.
- A prompt-only TCC query over the complete live acceptance interval found no Screen Capture or
  Accessibility prompt from the model tool call. The sole prompt event was
  `kTCCServiceSystemPolicyDocumentsFolder` at application launch because the development bundle
  itself lives under `Documents/GitHub`; it is unrelated to Computer Use authorization.
- Full frontend suite PASS: 38 files, 555/555 tests. Production frontend build PASS. Serial native
  CTest PASS: 6/6. `git diff --check` PASS.
- Built and deep/strict verified the separate `Builds/tests/universal_agent_manager.app`; both its
  main Resources directory and nested Computer Use companion contain byte-identical copies of the
  exact Gemini policy. The frozen `Builds/computer-use-remote/UAM Computer Use Test.app` was not
  rebuilt or modified, preserving its refreshed TCC identity.
- Live Gemini was not started, authenticated, or contacted. The current GUI process tree contained
  OpenCode and the isolated UAM helper only.

## 2026-08-28 — Multi-chat Computer Use isolation coverage

- Reaffirmed the acceptance-provider boundary: Gemini CLI is unavailable without the required
  business account and remains excluded from all live testing. Only OpenCode/local Qwen and Codex
  will be used for live UAM Computer Use acceptance.
- Audited the existing native multi-chat test and found its name overstated its evidence: it proved
  a second chat's MCP server can start while the machine-wide input lock is held, but did not issue
  an input action. Renamed it to state that narrower contract precisely.
- Added a separate native regression that gives two chats independent targets, pauses and stops the
  first chat, and proves the second chat remains enabled, running, and bound to its original target.
  `uam_core_tests` PASS in 50.86 seconds.
- Rechecked the frozen acceptance bundle identity: bundle id
  `com.universalagentmanager.desktop.computerusetest`, version `4.5.7`, cdhash
  `dd57a6f47bf8259c891cdab01af3571bc78b02c0`. It was not rebuilt or modified.
- Status remains `BLOCKED_ON_EXTERNAL_ACCESSIBILITY_REPLACEMENT`: live action acceptance still
  requires the owner to replace the stale Accessibility entry for this exact frozen bundle. Screen
  Recording already passes and must not be changed.

## 2026-08-28 — Accessibility blocker independently revalidated

- Re-read the current macOS TCC log after the third consecutive continuation at this gate. The OS
  still reports `Failed to match existing code requirement` for
  `kTCCServiceAccessibility`: stored cdhash
  `78d70948165f58bafa1bf2f729e5068e80baaf97` versus the unchanged frozen bundle cdhash
  `dd57a6f47bf8259c891cdab01af3571bc78b02c0`.
- This is authoritative OS evidence that the Accessibility entry has not yet been replaced; it is
  not inferred from Qwen output. Gemini remained excluded and was not launched or contacted.
- No further local GUI action test can produce valid acceptance evidence until the owner removes the
  stale Accessibility entry, adds the exact frozen repository app, enables it, and confirms the
  change. Screen Recording already passes and must remain unchanged.

## 2026-08-28 — Computer Use engine separated from provider/frontend testing

- Corrected the acceptance order at the owner's direction: test the frozen UAM Computer Use MCP
  helper directly first, driven by Codex as the MCP client; only after the engine passes will the
  OpenCode/Qwen and Codex provider paths be tested through the UAM frontend.
- Stopped the isolated UAM frontend and launched only its frozen nested
  `UAM Computer Use.app` with a fresh `/tmp/uam-computer-use-direct.oCCET4` data root and chat id
  `direct-engine-test`. No provider process, UAM frontend, or Gemini process participated.
- Direct MCP `initialize` PASS with protocol `2025-06-18`; `tools/list` PASS and exposed exactly
  `computer_observe` and `computer_action` with the expected bounded schemas.
- Direct `computer_observe` of `screen 1` presented exactly one UAM Allow/Deny dialog. After the
  authorized Allow, it returned a real bounded PNG, `frameId: 1`, `actionApplied: false`, and
  1024x661 geometry. This proves the standalone engine, MCP transport, authorization dialog, and
  full-display capture path operate without the frontend or a provider.
- The helper's direct window selector is `TextEdit` on this macOS session because the ungranted
  standalone identity cannot read the document title. It presented one exact-window Allow/Deny
  dialog and accepted the authorization, then window capture failed with the correct Screen
  Recording denial. This identifies a clean TCC boundary: the outer test app's grants do not grant
  the separately launched nested helper identity.
- Cleanly stopped the direct helper. Next gate: add the exact nested `UAM Computer Use.app` to both
  Screen & System Audio Recording and Accessibility. Then rerun the same direct MCP session through
  observe, hotkey/type, final observe verification, pause/stop, stale-frame rejection, and target
  revocation before returning to provider integration.

## 2026-08-28 — Standalone helper TCC grants installed directly

- With the owner's explicit authorization, drove macOS System Settings and added the exact frozen
  nested helper bundle at `Builds/computer-use-remote/UAM Computer Use Test.app/Contents/Frameworks/UAM Computer Use.app`.
- Accessibility now lists `UAM Computer Use.app` with its switch on.
- Screen & System Audio Recording now separately lists `UAM Computer Use.app` with its switch on;
  the existing outer `UAM Computer Use Test.app` grant remains enabled and unchanged.
- No password or Touch ID prompt appeared. No other permission entry was modified, and the installed
  Applications build was not opened, closed, rebuilt, or changed.
- Next: restart only the nested helper and run the direct MCP observe/action/verify and fail-closed
  control sequence before any provider/frontend acceptance testing.

## 2026-08-28 — Direct UAM Computer Use engine acceptance PASS

- A direct helper launched as a child of Codex was still denied Screen Recording despite the exact
  helper grant. macOS TCC logs showed the responsible process was `com.openai.codex`, whose separate
  Screen Recording switch is off. This was a test-launch attribution issue, not evidence that the
  UAM helper grant was missing. Codex's permission was not enabled or used as a workaround.
- Ran a minimal direct MCP harness from Terminal, whose existing permission provides an independent
  responsible-process context. The component under test remained the frozen nested UAM helper; no
  UAM frontend, provider, or Gemini process participated.
- MCP initialize PASS (`2025-06-18`) and tool discovery PASS (exactly `computer_observe` and
  `computer_action`). The helper requested one approval for `TextEdit — Untitled.rtf` and made no
  per-action approval requests after Allow.
- UAM window observation PASS with a real bounded capture, frame 1, 24 accessibility elements, and
  `actionApplied: false`.
- UAM action sequence PASS: Command-F, type `WAITING`, Escape, and type `COMPLETE`; every successful
  input returned a new frame and `actionApplied: true`. A deliberately reused frame was rejected as
  stale before input.
- Final UAM observation PASS. Independent UI inspection confirmed `Status: COMPLETE` and confirmed
  the paused-test sentinel `SHOULD_NOT_APPEAR` was never entered.
- Pause PASS: an action was rejected with `Computer use is paused by the user.` Resume PASS: a new
  full observation succeeded. Stop PASS: a subsequent action was rejected with
  `Computer use is stopped by the user.`
- Redacted history contains only server start, exact-target grant, observations, hotkeys, and
  `Typed text (content redacted).`; rejected stale/paused/stopped calls caused no input record.
- The helper and temporary harness exited cleanly with no matching processes left. Direct local
  Computer Use engine acceptance is signed off; next gate is provider/frontend integration with
  OpenCode/local Qwen first, then Codex with provider-native Computer Use disabled.

## 2026-08-28 — OpenCode/Qwen integration exposed a runaway-call defect

- Relaunched only the isolated test bundle from Terminal with
  `/tmp/uam-computer-use-gui.23dZGL`; the installed Applications build remained untouched. The chat
  was confirmed as `opencode-cli` with local `inferdeck/qwen3.6-35b-a3b`. Gemini was not started or
  contacted.
- Reset the safe TextEdit fixture to `Status: WAITING` and sent an explicit sequential acceptance
  prompt requiring one UAM Computer Use call at a time, latest-frame use, and immediate stop on any
  error.
- The provider ignored that contract and produced a rapid alternating observe/action sequence. UAM
  correctly rejected unsafe actions (red error states) while observations continued to succeed,
  but the integration did not circuit-break the turn; the helper history records repeated completed
  observations at roughly one per second.
- At the owner's instruction, stopped the turn immediately through the UAM UI, confirmed it left
  Active Chats, then closed only the isolated test app. Process inspection confirmed no isolated UAM
  frontend, helper, or harness process remains.
- This is now a reproduced integration defect, not model output to trust: UAM needs a bounded
  frontend/runtime circuit breaker for runaway Computer Use tool calls before OpenCode/Qwen can be
  accepted. Direct engine acceptance remains valid and separate.

## 2026-08-28 — Fresh single-chat Qwen3.8 attempt blocked by local registration

- At the owner's direction, deferred the circuit-breaker patch and created one brand-new structured
  OpenCode chat named `Qwen3.8 Computer Use`, selected cached model
  `inferdeck/qwen3.8-27b`, and closed the old active pane so only the new Qwen3.8 chat was open.
- The first prompt failed before inference or Computer Use with OpenCode ACP error
  `model not registered: qwen3.8-27b`; no UAM Computer Use tool call ran and the TextEdit fixture
  remains `Status: WAITING`.
- `opencode models inferdeck` independently lists `inferdeck/qwen3.8-27b`, but a minimal direct
  `opencode run --model inferdeck/qwen3.8-27b` fails with the same registration error. This proves
  the blocker is the current OpenCode/InferDeck model registration state, not UAM's chat model value
  and not the UAM Computer Use engine.
- The isolated UAM test app remains open on the single Qwen3.8 chat and error. Next: register/load
  Qwen3.8-27B in the local InferDeck backend, then retry this same single chat.

## 2026-08-28 — OpenCode/Mimo provider acceptance and macOS cursor discrepancy resolved

- The owner independently completed the provider/frontend journey through the isolated UAM build
  using OpenCode's free Mimo V253 model. It successfully invoked UAM's own Computer Use system; the
  slower image processing was model latency, while the earlier Qwen failures came from the local AI
  server stack. This supplies the missing real UAM frontend/provider evidence without Gemini.
- Traced the observed cursor difference to `PostEvent` in
  `src/computer_use/computer_use_platform_macos.mm`. Exact-window actions retain the window owner PID,
  prefer Accessibility `AXPress`, and otherwise use `CGEventPostToPid`; display actions have no owner
  PID and correctly use system-wide `CGEventPost(kCGHIDEventTap, ...)`, which moves the physical
  cursor.
- Kept the native fail-safe behavior. Restoring the pointer after a global click would still visibly
  teleport it and could race the user or another application. The documented workflow is to use the
  display only for discovery/opening, then grant the exact application window for nonintrusive
  control.
- Local Computer Use is now accepted at both layers: direct MCP engine acceptance and a real
  OpenCode provider journey through UAM. Gemini remained unlaunched and uncontacted. Next: preserve
  this working state in a local checkpoint commit, then advance the existing remote-runner route.
- Checkpoint verification PASS: frontend Vitest 38/38 files and 555/555 tests; production frontend
  build PASS; native CMake build PASS; native CTest 6/6 PASS in 54.24 seconds; `git diff --check`
  PASS. The frozen accepted bundle under `Builds/computer-use-remote/` was not rebuilt or modified.

## 2026-08-28 — Computer Use checkpoint committed and remote boundary established

- Preserved the accepted local Computer Use implementation in local commit `5f389de0` without
  pushing it. The commit excludes the owner's unrelated untracked planning files and root
  `node_modules/`; the frozen accepted bundle and Applications-installed build remain untouched.
- Added a persisted execution-host model with one immutable `local` host and validated SSH host
  records that store only an exact `~/.ssh/config` alias—never passwords, keys, or arbitrary SSH
  arguments. Legacy chats migrate to `local`; new chats retain their selected host and workspace.
- Kept the existing local New Chat behavior byte-for-byte at its bridge boundary. When more than
  one host exists, New Chat exposes `Runs on`; a ready remote host accepts a path interpreted only
  on that host, skips local provider discovery, and passes the host identity into chat creation.
- Added backend no-fallback guards before terminal launch and ACP prompt routing. Until a runner is
  connected, a remote chat fails explicitly instead of silently running on this Mac.
- Disabled Computer Use fail-closed at every implemented remote boundary: UI guidance, enablement,
  control-state persistence, ACP MCP injection, and Codex provider-native launch flags. A remote
  chat cannot create Computer Use runtime files or launch either UAM or provider-native control.
- Remote-boundary verification PASS: full frontend suite 38/38 files and 555/555 tests; focused New
  Chat remote-host test 20/20; native CTest 6/6 in 50.40 seconds; focused host persistence and
  Computer Use fail-closed tests PASS.

## 2026-08-28 — Headless runner protocol slice PASS

- Added a separate CEF-free `uam-runner` executable. Its first surface is intentionally only
  `uam-runner bridge` over stdin/stdout plus `--version`; it opens no network listener and executes
  no provider yet.
- The bridge uses a four-byte big-endian length followed by one JSON object, capped at 1 MiB. It
  rejects empty, oversized, truncated, non-object, and invalid JSON frames before dispatch.
- Protocol negotiation requires a bounded request id, protocol version 1, and a bounded nonce that
  is echoed in the response. The response reports runner version/platform/architecture and
  explicitly advertises both `computerUse: false` and `processExecution: false`.
- Unit coverage PASS for round-trip framing, protocol mismatch, oversized frames, truncated frames,
  and the Computer Use capability boundary. A real subprocess smoke test against
  `Builds/tests/uam-runner bridge` returned version `4.5.7`, macOS/arm64, the exact nonce, and
  `computerUse: false` with a clean exit.
- Next frontier: add the minimum typed process/session protocol behind this tested frame boundary,
  then exercise it through a local fake transport before any SSH installation or remote mutation.

## 2026-08-28 — Typed process bridge and reconnecting service PASS

- Added the minimum process/session surface needed by both ACP and terminal transports:
  `process.start`, `process.write`, `process.closeInput`, `process.poll`, `process.stop`, and
  `process.remove`. Commands remain typed argv arrays; no local shell string is assembled.
- The runner rejects unsafe or oversized session ids, paths, argv arrays, environment names/values,
  writes, and duplicate/missing sessions. Process output is base64 encoded so terminal bytes cannot
  corrupt JSON framing, and each poll is bounded below the 1 MiB protocol ceiling.
- Removed an accidental GUI coupling discovered by the real runner build: the reusable macOS
  parent-death watchdog now lives in its own headless source, while the runner compiles out the
  desktop-only “open Terminal” method. `uam-runner` links neither CEF nor the React application.
- Added a desktop-side runner client that validates response correlation ids, version and nonce,
  requires `processExecution: true`, and rejects any runner that advertises Computer Use. Its SSH
  argv uses one already-validated config alias and fixed OpenSSH safety options; it never accepts
  passwords, key paths, or caller-supplied SSH arguments.
- Added a user-owned Unix-socket service with mode `0600` and same-UID peer validation. `serve`
  owns processes, `bridge` only relays framed requests, and `start` detaches the service so SSH
  disconnection cannot destroy active jobs. `stop` performs an authenticated graceful shutdown.
- A real bridge-subprocess test PASS launched and polled a typed child through the actual runner.
  A separate service test PASS started a delayed job, destroyed the first bridge, connected a new
  bridge, recovered the same session, received `persistent-ok`, and removed it. The sandbox blocks
  Unix socket binding by policy, so the socket-only tests were rerun with the approved isolated
  `/tmp` permission; the first restricted run correctly reported `Operation not permitted`.
- Next frontier: checksum-verified user-space bootstrap over an exact SSH alias, followed by routing
  UAM's ACP stdio path through this client. Background output spooling and Windows named-pipe parity
  remain explicit gates before declaring long-disconnected jobs production-safe.

## 2026-08-28 — Explicit SSH setup, ACP proxy, and disconnected output spool PASS

- Added the explicit Settings workflow for remote hosts. UAM accepts only one exact
  `~/.ssh/config` alias, shows every fixed `ssh`/`scp` setup step before mutation, and requires a
  second **Connect and install** action. It stores no password, key path, or custom SSH argument.
- The helper installs in a versioned user-owned directory under `~/.local/share/uam/runner`, is
  SHA-256 verified before activation, and uses an atomic `current` symlink. Platform and CPU are
  checked before the copy; this macOS/arm64 build fails before mutation on an incompatible host.
- Added a nonblocking local proxy process between UAM's existing ACP stdio runtime and the SSH
  runner. The CEF thread retains the proven local parser, permission, cancellation, and restart
  paths; SSH latency cannot block UI polling. A reconnect attaches to the same runner-owned process
  rather than starting a duplicate provider turn.
- Kept remote Computer Use explicitly disabled in both Settings guidance and the existing backend
  fail-closed gates. Removing a host is blocked while any chat still owns it, and removal leaves the
  independently installed helper intact.
- Closed the long-disconnect pipe-fill failure: the service continuously drains each child into
  private bounded disk spools even with no bridge connected. A real service test disconnected the
  first bridge, produced 512 KiB while absent, reconnected, recovered every byte plus the final
  marker, and removed the session. The initial test exposed a partial-final-drain bug; `process.poll`
  now remains logically running until all spooled output is delivered.
- Verification PASS: Settings remote preview/explicit install test; real proxy/provider stdin/stdout
  relay test; typed protocol tests; direct bridge test; and Unix-socket reconnect/spool test. All
  builds stayed under `Builds/tests`; the accepted frozen bundle and Applications build were not
  opened or modified. Gemini remained unlaunched and uncontacted.
- Next frontier: terminal fallback parity, remote-safe UAM agent/MCP resource placement, Windows
  service parity, then an owner-selected real SSH alias for installation and vivid GUI acceptance.

## 2026-08-28 — Remote ACP, terminal, multi-chat, and explicit-stop integration PASS

- End-to-end tracing found and removed a stale shared `SendAcpPrompt` rejection that prevented every
  remote prompt from reaching the new proxy. Remote host readiness is now checked at the actual
  launch boundary, so a missing/offline helper fails explicitly and never falls back to this Mac.
- Remote OpenCode and Copilot launches no longer use this Mac's CLI-version verdict or write
  provider-native agent adapters containing local paths. UAM-managed agent instructions are
  prompt-injected for remote chats; a real fake-transport ACP test completed initialize,
  `session/new`, prompt, streamed assistant output, and turn completion through `uam-runner proxy`.
- Remote session setup omits local workspace MCP executables and the local UAM-control executable;
  those paths cannot be validly handed to a remote provider. Computer Use remains rejected before
  session creation. Remote-safe MCP/control placement remains an explicit unresolved parity gate.
- Added terminal fallback through a forced OpenSSH PTY and the already-installed versioned runner.
  Provider argv and remote cwd are encoded as validated JSON; no local provider environment or
  native-history scan is used. The remote path must be absolute and New Chat no longer pre-fills it
  from a local folder. Direct runner execution and a fake-SSH PTY launch both PASS.
- Found and fixed a multi-chat service defect: the Unix-socket helper accepted only one active bridge
  at a time, so one long-lived chat starved all others. It now handles concurrent same-UID bridges
  against a locked shared process registry. A regression keeps chat A connected while chat B starts,
  finishes, and is removed, then reconnects to recover chat A's complete 512 KiB spool.
- Reconnect now validates cwd, argv, and environment before attaching to an existing session id, so
  a changed provider configuration cannot silently bind to the wrong surviving process.
- Added an out-of-band local proxy stop line. An unexpected SSH/bridge loss leaves the runner-owned
  provider alive for reconnection, while UAM's explicit Stop path asks the helper to terminate and
  remove that exact session before killing a stuck proxy. The marker is intercepted and never sent
  to the provider; the real subprocess regression PASS.
- Verification in this checkpoint: focused remote ACP boundary and full proxy ACP tests PASS;
  terminal argv/direct-exec/PTY-routing tests PASS; explicit-stop and attach-conflict tests PASS;
  concurrent bridge/reconnect/spool test PASS with the isolated Unix-socket permission; New Chat
  frontend 20/20 and production frontend build PASS. Only `Builds/tests` was used; the frozen
  Computer Use bundle and Applications build were untouched, and Gemini was not launched.
- Next frontier: design and prove remote-safe UAM-control/custom MCP transport, run the complete
  native/frontend regression and packaged test build, then perform install/GUI acceptance on an
  owner-selected real SSH alias. Windows/Linux artifact parity remains blocked by the absence of
  matching packaged runner binaries, not hidden behind a false compatibility claim.

## 2026-08-28 — Remote UAM Control transport and full isolated regression PASS

- Extended the existing authenticated runner service with bounded bidirectional byte channels;
  remote UAM Control does not open another network listener or reverse port. The remote provider
  receives only the versioned runner MCP shim, while the desktop relay owns the existing
  capability-scoped UAM Control process and keeps local executable paths off the remote host.
- Proved the real protocol rather than an echo substitute: MCP `initialize` crossed the remote shim,
  shared helper service, SSH-side proxy boundary, and desktop relay; a real `goal_get` tool call was
  then processed by `UamControlService` and returned with `isError: false`. Invalid base64 and all
  channel size limits fail closed.
- Kept custom user MCP servers out of remote chats for now. UAM cannot safely infer whether an
  arbitrary configured executable/path belongs on the desktop or remote host; silently copying a
  local path would be false parity. UAM-managed agents remain prompt-injected remotely and UAM
  Control itself now has the explicit safe placement above.
- The complete native suite exposed a legacy local-host regression: blank in-memory settings made
  ordinary local ACP sessions look like an unknown host. Fixed it once in shared execution-host
  lookup so blank/`local` resolves to the immutable built-in local host while unknown remote ids
  still fail closed.
- Full verification PASS: CTest 6/6 in 50.36 seconds; frontend Vitest 38/38 files and 557/557 tests;
  production frontend build; native packaged app build; `git diff --check`; and strict deep
  codesign verification. The standalone and packaged runner SHA-256 values both equal
  `39931f478bf04c8197f89660bcc9dc53aceecacacf173b5e8aefaf6cf0b7bf9d`.
- Computer Use GUI acceptance PASS against only
  `Builds/tests/universal_agent_manager.app` with isolated data root
  `/tmp/uam-remote-gui-20260828`: Remote Hosts is discoverable, the remote Computer Use prohibition
  is explicit, setup previews every fixed SSH/SCP operation and checksum step, and mutation is
  separately gated behind **Connect and install**. The preview was cancelled; no SSH connection or
  remote mutation occurred. The Applications-installed build and frozen accepted Computer Use
  bundle were untouched, and Gemini was not launched or contacted.
- Remaining external gate: install and vivid end-to-end acceptance against an owner-selected exact
  `~/.ssh/config` alias. No alias is currently discoverable, so UAM will not guess a target. Matching
  Windows/Linux runner artifacts remain a later platform gate rather than an untested claim.

## 2026-08-28 — Real OpenSSH compatibility rejection PASS

- Followed the existing `Include` in `~/.ssh/config` and found the configured `colima` alias. A real
  read-only OpenSSH call identified it as `Linux aarch64`; loopback port 22 itself is not running, so
  Colima is the only currently available SSH target.
- Drove the packaged test app through Remote Hosts with Computer Use, previewed `colima`, and invoked
  the explicit setup action. UAM connected over real OpenSSH, reported that this build supports only
  macOS/arm64 while the host is Linux/aarch64, recorded the host as `error`, and did not continue to
  directory creation or copy. A separate read-only SSH assertion proved
  `~/.local/share/uam/runner/4.5.7` does not exist on the VM.
- Removing the failed host exposed inaccurate UI copy claiming a helper “was left installed” even
  though the compatibility gate ran before installation. Changed the shared removal result to the
  truthful “Any helper files on that machine were left untouched” and added a focused regression;
  SettingsModal now passes 44/44 tests.
- Removed the isolated failed-host record and closed the test app. The Applications-installed build,
  frozen Computer Use bundle, and real remote machines remain untouched. Full positive remote
  execution acceptance still requires a configured macOS/arm64 SSH alias.

## 2026-08-28 — Remote setup failure visibility GUI PASS

- The real Colima rejection revealed that the setup preview remained above the completed error,
  hiding the actionable result until the user manually cancelled. On any failed install, the shared
  Settings workflow now dismisses the preview before presenting the backend error.
- Added a focused regression for the failure transition, rebuilt only `Builds/tests`, and repeated
  the real OpenSSH Linux/aarch64 rejection through the packaged GUI. The preview now closes
  automatically and the exact compatibility error plus `error` host status are immediately visible.
- Verification PASS: SettingsModal 45/45; full frontend 38/38 files and 559/559 tests; production
  frontend build; packaged native build and signing. The isolated failed record was removed and the
  test app closed; installed/frozen builds and Gemini remained untouched.
