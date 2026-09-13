# Wayfinder: remaining UAM audit decisions

## Destination

Resolve the remaining implementation and verification route for the user's six audit areas:
code simplicity, performance, stability/connections, provider parity, recoverable GUI errors,
and native CLI passthrough. A clear route is not proof that the product goal is complete.

## Constraints

- Preserve all five providers and the existing dirty worktree. No commits or releases requested.
- Native CLI means the provider's actual tool, without added UAM terminal controls.
- Use Luna for bounded parallel work; the parent owns major decisions and assigns file ownership.
- Substantial UI changes require distinct mocks and a user pick. Routine bug fixes are authorized.
- Protect daily-driver channels and user-used audit apps. Use Builds/audit-gui and isolated data.
- Queue incidental user bug flags for a later batch; do not interrupt active work unless explicitly urgent.
- Preserve feedback and progress in the existing numbered checkpoint and backlog below.
- Input Audit replacement still awaits the existing restart approval; elapsed time is not approval.

## Current frontier

2026-09-12 latest: Alpha33 candidate built and signed, bundled4helper versions/hashes verified. BUG072 Windows SSH loss reproduced RED installed30/GREEN isolated breakaway candidate. BUG073 direct Computer Use toggle/settings/armed mode implemented, native16 tests pass and focused UI/build pass. Alpha33 installed on explicit restart approval; PID51358/version/signature verified. Alpha32 rollback retained. Remote configured helpers unchanged. Visual and actual MCP consent/question verification remain outstanding.

2026-09-12: User closed the InferDeck-dependent investigation for this pass and will test it later. The Windows post-cancellation stream timeout remains unverified, not fixed. Do not block unrelated UAM work on InferDeck diagnostic access.

- BUG071 repeated memory/agent context is fixed locally with native821/821 validation. Alpha32 installed and restarted on explicit user authorization; normal Applications path, PID119, version and signature verified. Alpha31 previously passed BUG067–070 checks, including actual CEF helper-update concurrency/host-scope/release and directory failure/retry/Ready acceptance.
- Daily app is now Alpha32; Alpha30 rollback retained. Further restarts require fresh authorization when work is active.
- Reconcile previously requested remaining work against current code before starting another task. Historical mobile/alpha/checkpoint entries below are not current status.


Checkpoint207 LIVE: alpha22 helpers built/verified; desktop build62892 running.
Poll same handle, /tmp/uam-alpha22-app-build.log. Version files nowalpha22; no
installed/frozen changes. Verify resulting package before any handoff.

Checkpoint205: audit app incorporates all source through204, UI193/helper191;
build/signature and focused boundary/package checks pass. Internalalpha21 only,
not installed. OpenCode priority, low usage, release/runtime gates still apply.

Checkpoint199: OpenCode-specific native tests all pass; audit app rebuilt with
197/198 fixes and signed. Internal only, no daily install. Continue high-priority
OpenCode work; release/runtime gates remain open.

Checkpoint198: OpenCode worker adds -- before prompt to prevent option-like text
being parsed as flags. Focused argv and creation recovery tests pass. Source197/198
not yet in audit app194; prioritize remaining OpenCode blockers, conserve usage.

Checkpoint197: OpenCode valid session ID is retained after nonzero teardown exit;
red process fixture then focused creation/child tests pass. Source only; app194.
Test binary no longer has deferred195 test. OpenCode high-priority focus remains.

User priority override195: usage constrained. High-impact OpenCode blockers first,
Codex second; defer polish and broad repeated verification. Confirmed Codex
functions.spawn_agent import gap queued, temporary regression removed from source.
Native test binary must rebuild before next use (contains deferred test).

Checkpoint194: native CEF scroll/focus/menu verification passed for agent prefix
fix193. Inspected actual screenshots; label clips with draft and menu remains
visible. Audit app nowUI193, stopped; no install/SSH changes. Next queued child
transcripts/processing display, with release target-runtime gates still open.

Checkpoint193: fixed static agent prefix over scrolling textarea by following
scrollTop and clipping input row. Existing regression red then fullUI778/43 and
TS/Vite pass. Source only; next actual rendered clipping/focus/menu verification
before declaring descriptor report resolved. Audit app stillUI186.

Checkpoint192: target execution unavailable locally (Colima daemon stopped, no
Wine/QEMU); did not start shared infrastructure. Dependency inspection confirms
Windows OS/UCRT imports only; Linux needs glibc>=2.30, not musl. Evidence saved.
Keep target execution and unique alpha staging open; continue queued source audits
without claiming packaged runtime validation. No live host/app changes.

Checkpoint191: rebuilt Linux arm64/x86-64 and Windows x86-64 helpers from current
source into helper191-remote-artifacts; builds and architecture checks pass. Audit
app now packages exact fresh hashes, full build/signature pass. Frozen artifacts
unchanged. Target-OS execution is not proven by cross-build; internal alpha21
label is not ready for distribution. No install/live SSH mutation.

Checkpoint190: packaging now reruns existing helper validation immediately before
copy, closing configure-to-package replacement gap. CMake regression valid trio/
changed bytes/wrong version/missing helper passes; full app and signature pass.
Audit includesUI186, no install. Source freshness remains unproven for cached
alpha21-remote-artifacts despite matching version/checksum. Resolve before shipping.

Checkpoint189: unconfirmed-payload replay passed after isolated GUI crash and
explicit pre-receipt state injection. Real local helper deduplicated same delivery;
provider received one steer, transcript ordered, pending state cleared and Send
restored. All fixture processes stopped. Combined recovery gates188/189 pass;
next inspect packaging compatibility. No live SSH/install changes.

Checkpoint188: combined GUI crash/relaunch with real local helper and synthetic
Codex passed. Pending steer response restored, one start/steer delivery, original
turn identity, ordered transcript and usable Send. All fixture processes stopped.
Evidence /tmp/uam-remote188-result.json. Receipt was persisted before crash;
unconfirmed-payload replay is separate. No install or real SSH changes.

Checkpoint187: inspected current recovery coverage. Saved request correlation and
real helper detach survival are tested separately; combined fresh-GUI recovery of
a pending Codex steer is unproven. Next run the synthetic Codex fixture through
real runner serve/socket with fake SSH, crash/relaunch only isolated GUI, and
verify exactly one delivery, original turn and ordered transcript. Packaging held
for this gate; no live host access is necessary.

Checkpoint186: temporary stream keys now include transcript position; frozen-clock
regression reproduced duplicate IDs before fix. UI778/43 and TS/Vite pass.
Source UI186, native audit app185; no install/SSH changes. Remote recovery/package
gates remain next; mobile companion investigation is queued.

Checkpoint185: native stream tokens now carry messageIndex through all producer paths;
validated frontend buffering separates message identities before summary arrival. Red
token-before-summary regression fixed. UI777/43, native7/7 and full app build pass. Actual
native two-steer check records indexes1,3,5,5 and ordered bubbles/shared collapse/completion.
All owned apps stopped; no install/SSH mutation. Audit main backend/UI185 internal only.
Next verify same-millisecond placeholder IDs, then remote recovery/package gates and backlog.

Checkpoint184 verified: delayed hydration no longer loses steered history when the previous
durable assistant still has isStreaming set. Stream appends require current assistant ownership
or a genuine temporary placeholder. Red regression fixed; UI775/43 + native app build pass.
Actual native two-steer journey passes after one controlled relaunch: six ordered bubbles,
one header, original turn identity, collapse/completion and Send/no alerts. First app cleanly
exited before checker connection for unknown reason; preserve as unclassified evidence.
No installed/SSH change. Audit main backend179/UI184 is internal only. Remaining: event-order
limits before binding updates, remote recovery/packaging gates and queued visual/audit items.

Checkpoint183: native steering bubble now hydrates on appended active messages, even before
any assistant output. Red empty-turn regression fixed; UI773/43 + full app build pass. Actual
silent-provider GUI confirms user/user before output, shared collapse through completion.
Relaunch of disposable profile preserves grouping and messages, Send available, no alerts.
No install/SSH changes. Next: rapid steering and delayed-history/raw-token ordering, then
remote recovery and packaging gates. Audit main backend179/UI183 is still an internal test build.

Checkpoint182 verified in actual native audit app: missing steering bubble/duplicated reply
reproduced despite correct saved backend history. Shared frontend hydration now recognizes
same-turn assistant-segment advance. Store regression red→green; UI772/43 and full app build
pass. Fresh synthetic GUI queue/steer/late-tool/collapse/completion/cancel/send-again journey
passes with exact expected request ledger. All owned apps stopped, no install/SSH change.
Next: native same-turn delayed hydration/rapid steering and relaunch persistence, then remaining
remote recovery and packaging gates. Audit main backend179/UI182 remains internal test build.

Checkpoint181 verified: original section time/provider/model survive windowing at both user
and assistant boundaries; loading earlier history keeps one header/control and collapsed state.
UI771/43 + TypeScript/Vite pass. Owned audit main app fully rebuilt and signature valid, now
backend179/UI181, stopped. No installed/frozen package changed. Next: native synthetic provider
end-to-end steering, late updates, completion/interrupt/retry and recovery checks before packaging.
Trace clone and bundled remote helper artifacts are still older; do not deploy this test build.

Checkpoint180: shared work-section rendering implemented for explicit native continuation.
One timer/header and one controlled collapse span steered assistant segments; user-facing text
and steering bubbles stay visible. Default setting reused; stable context avoids historical row
rebuilds. UI769/43 + TypeScript/Vite pass; isolated actual browser collapse/reopen and centered
focus-theme layout verified. No native rebuild/install/SSH changes. Review truncated-history
section boundaries next, then actual native app steering/recovery verification before packaging.
Rolling active highlight and continuous trace rail remain queued.

Checkpoint179 verified: native assistant regrouping no longer discards uniquely matched user
continuation provenance. Duplicate/missing/changed identities fail closed; native metadata
is retained. Red regression then three focused history checks pass; full native7/7 in72.73s.
Source only; no install/SSH changes. Next: shared work-section rendering and controlled collapse;
ChatView currently emits every-user headers and per-message disclosure states.

Checkpoint178 verified: native same-turn provenance persists through repository/CEF/frontend
store; legacy false. Native7/7 (76.94s), UI768/43 and TypeScript/Vite pass; existing chunk
advisory remains. Source only, renderer unchanged, no install or SSH mutation. Next: preserve
proven continuation identity across native export splits and use controlled section disclosure
for shared work-section rendering. Mobile companion remains queued below.

Checkpoint177 verified: removed frontend five-second/next-turn steering lock; controls follow
request completion and remain disabled while unresolved. Red then deferred accepted/rejected/error
checks pass; UI768/43 and TypeScript/Vite pass (Builds/steer-ui177), existing chunk advisory.
Backend176 unchanged; all handles terminal, no installed app/data or SSH changes. Shared-section
UI continuity remains next: persist explicit native continuation provenance before grouping.

Checkpoint176 locally verified: original remote user anchor and earlier assistant tool owners
survive steering/recovery; latest events stay in latest segment. Invalid/legacy anchors fail
closed to latest user; duplicate tool IDs cannot select arbitrary owners. Full run also exposed
ordering-dependent branch rollback selection bug; deterministic red then one-line fix verified.
Final native7/7 passed74.09s (CTest69790 terminal0). All handles terminal, source only. Next:
shared work-section UI continuity, with explicit persisted grouping rather than unsupported
historical assumptions. No installed app/data or SSH changes; broad goal remains active.

Checkpoint175 locally verified: remote active turn identity survives after pending request
ACK retirement, using provider-interface capture/restore; remote turn serial survives GUI
and provider-process resets. Reload test verifies native steer/interrupt target and completion
clears identity. Native7/7 passed77.42s (CTest15413 terminal0), all handles terminal; no compiler
warnings found. Source only. Next: earlier tool-owner/original turn-anchor recovery and shared
work-section UI. No installed app/data or SSH changes; broader goal remains active.

Checkpoint174 locally verified: durable remote request correlation, concurrent steering
delivery records, consumed-response tombstones and cursor ACK persistence now prevent
replayed steering rejection from failing the active turn. Original red, saved recovery,
receipt/ACK failure and send-path tests pass; native7/7 passed77.14s (CTest49942 terminal0).
All handles terminal. Before packaging, finish durable active turn identity/serial after
pending requests retire, earlier tool-owner restoration and shared-section UI. Source only;
no installation or SSH changes. See plan174 for exact validation and prior failed builds.

Checkpoint173: native Codex steering now uses turn/steer through the provider interface,
preserves turn clock/anchors, and keeps late tool updates in their original assistant segment.
Direct/queued input rollback, rejection, late acknowledgment and unconfirmed shutdown handling
have focused coverage. Final native7/7 passes73.62s; all handles terminal. Source only,
no installed app or SSH changes. Before packaging: persist pending steering request correlation
through remote recovery, restore earlier same-turn tool ownership, and finish shared-section UI.
Mobile viewing/messaging companion investigation stays queued below. Broad goal remains active.

- [Q-001](#q-001-can-the-real-gui-complete-native-cli-to-chat-history-refresh)
- [Q-002](#q-002-how-can-a-new-native-codex-cli-own-an-exact-session-id)
- [Q-003](#q-003-can-remote-native-clis-bind-and-resume-exact-provider-sessions)
- [Q-004](#q-004-does-the-current-build-meet-long-session-and-platform-reliability-gates)

## Evidence index

Detailed evidence and test artifacts live in the existing
[implementation checkpoint](codex-opencode-parity-performance-plan.md).
Its opening Alpha-2/Alpha-5 comparison is historical; the latest numbered checkpoints describe
current implementation and unresolved evidence. Provider behavior is documented in
[provider-runtime-parity](../../docs/provider-runtime-parity.md).
Checkpoint105 installed alpha19 separately in ~/Applications, preserving alpha17 and alpha18.
The user subsequently moved alpha19 to the canonical application path. Checkpoints106 onward
are source-only; consult the latest State/checkpoint before choosing build or verification work.
Frontend736/43 and native7/7 pass; packaged UI bytes, all runner hashes/versions and deep strict
signing pass. Foreign helpers are crosscompiled, not runtime verified. Prior104 live GUI
hydration/CLI recheck remains open because CUA returned stale frames.

## Decisions

### Q-001: Can the real GUI complete native CLI-to-Chat history refresh?

- Status: open.
- Why now: local OpenCode root refresh and reopening now pass; Codex generation and actual child
  selection/failure rollback still need GUI evidence.
- Blocked by: no planning dependency. Current credential-free GUI observation works; the older
  stalled captures do not block independent child-history verification.
- Evidence: checkpoint92's owned History Check app launched Codex, but window captures stayed
  stale and controls intermittently failed. Restart displayed the CLI, then captures stalled again.
  Samples did not prove a JS busy loop. The app and owned child processes were stopped.
  Batch98's fresh credential-free profile produced trustworthy captures and exposed a real local
  OpenCode export-argument bug: an ID after "--" opened a picker and timed out. The corrected
  invocation imports two native turns, preserves all four messages on restart, and resumes the
  same native ID. Native output comes from a deterministic loopback HTTP fixture through the
  installed OpenCode1.18.20 CLI. Expanded/restored populated terminal layouts redraw correctly.
- Decision: retain the failed92 observation as historical evidence; use the owned98 profile and
  loopback fixture for further bounded checks without copying credentials or adding GPU flags.
- Consequences: root OpenCode success is verified at this scope. Next exercise actual CEF child
  opening and failure rollback; Codex generation and external provider behavior remain separate.

### Q-002: How can a new native Codex CLI own an exact session ID?

- Status: open.
- Why now: rejecting multiple index candidates does not prove ownership when two unbound
  terminals see the same first candidate, or an external CLI creates it.
- Blocked by: no planning dependency.
- Evidence: installed0.153.3 no-prompt app-server probes in /tmp/uam-codex-empty-probe.log and
  /tmp/uam-codex-empty-legacy-probe.log returned IDs without persisted rollouts. A fresh resume
  failed. There is no supported persistExtendedHistory field or explicit new-session CLI ID flag
  in the inspected installed schema/help. Repeated index records independently caused false
  ambiguity; batch93 removes that bug without claiming concurrent ownership is solved.
  Batch94 rejected plaintext logs as an authoritative owner signal. Private Unix WebSocket
  transport works, but the unauthenticated TUI stopped at sign-in and loaded-list exposes no
  primary-owner metadata. Automatic approval review denied the authenticated retry; no such
  process started and the temporary auth copy was removed. Do not work around this denial.
- Decision: reject a stopped empty-thread bootstrap and fabricated history/dummy prompts.
  Investigate an exact native ownership signal before changing launch architecture.
- Consequences: concurrent native binding remains an explicit gap. Native CLI behavior must stay
  intact; a persistent app-server/remote-CLI design would need separate evidence and parent review.

### Q-003: Can remote native CLIs bind and resume exact provider sessions?

- Status: open.
- Why now: local OpenCode's explicit ACP session/new bootstrap is verified, while remote creation,
  prior unbound sessions and session switches inside a native CLI remain unverified.
- Blocked by: an actual configured test host is needed for the final live execution gate; local
  source and fixture investigation can continue independently.
- Evidence: checkpoints89–91 and provider-runtime-parity describe current local/remote readers,
  identity guards and fixture coverage. Simulated host fixtures are not Windows/SSH execution.
  Batch94 fixes remote OpenCode launching without resume while advertising a saved native ID;
  disabled/blank resume configuration now fails before handoff or SSH. Fake-SSH regression passes.
  Batch95 passes cancellation through root/child history reads to active remote provider polls,
  with cleanup and pre-canceled launch rejection verified by the framed fake-SSH regression.
  Batch96 implements direct-runner creation with the shared local handshake, host revalidation,
  transient process leases and save-before-launch. Correlation, cancellation after session/new,
  invalid/provider responses, teardown failures and remote persistence guards pass fixtures.
  Batch97 fixes UTF-8 conversion through remote process, terminal, attachment and file paths.
  Real bridge/file tests pass on macOS; Windows system-code-page behavior remains unexecuted.
- Decision: reuse provider-native session APIs and existing runner transport where supported.
  The bounded investigation in checkpoint95 establishes a direct RunnerClient route for remote
  OpenCode initialize/session-new. Do not wrap the unchanged local helper in the normal proxy:
  EOF detaches, and controller path validation rejects remote Windows paths. Preserve operation
  IDs/tokens through retries, use transient leases, revalidate host configuration and save before
  PTY launch. Unknown IDs after lost session-new replies must remain explicit failures.

### Q-004: Does the current build meet long-session and platform reliability gates?

- Status: open.
- Why now: short native checks cannot prove long-running connection or renderer behavior.
- Blocked by: actual Windows/SSH environments for those gates; credential-free GUI observation
  is now available for a bounded local soak.
- Evidence: historical performance comparisons are in the checkpoint; do not present them as
  current-release measurements. Microphone recovery, thirty-minute growth, logging growth,
  bundle size and intermittent runner-stop behavior still require their scoped evidence.
- Decision: keep the existing controlled workload and acceptance criteria where applicable;
  record build identity, data root, workload and process handles for each live run.

### Q-005: Are provider details confined behind the runtime interface?

- Status: open; checkpoints106–107 route linking and history identity restoration through the
  runtime contract and remove ledger dependence on Codex UUID helpers. Broader
  orchestration/config/ACP migration remains. Checkpoint108 moves Copilot option reconciliation
  behind runtime hooks and fixes malformed entries dropping later valid config options.
- Why now: the user identified direct OpenCode/Copilot imports in terminal handlers and shared
  core/terminal/remote tests and explicitly requires all provider-specific behavior behind the interface.
- Evidence: batch99 moves terminal launch validation, environment, native session creation,
  resume/preparation, ID discovery and input-prompt policies behind IProviderRuntime. Shared tests
  now exercise that interface. Native compilation exposed remaining Gemini history-store test uses;
  corrected public-contract tests pass the final native suite. A scoped CMake import guard covers shared terminal
  consumers and all tests. This is not proof that every shared subsystem is clean.
- Decision: use the existing runtime registry and provider implementations. Keep the native CLI
  unchanged. Preserve behavior tests through the public contract rather than re-exporting private
  helpers or merely relocating includes. The earlier alpha18 delivery prerequisite is complete;
  protect the installed alpha19 while continuing tested source batches.
- Consequences: remaining direct imports in app orchestration/model catalogs,
  CEF chat config and ACP still need migration. Linking, ledger and chat history identity were
  migrated in106–107. Shared ACP wire parsing and
  provider-ID branches also need review; widen the guard after those consumers are migrated.

### Q-006: Do running indicators reflect actual activity and intended motion?

- Status: open; composer ownership fixed, native OpenCode activity now explicitly unknown in source.
- Why now: user reports paused/non-looping indicators in CLI and ACP/Chat and recurring incorrect
  statuses. They explicitly requested this investigation after their new alpha build.
- Evidence: static sidebar/tool icons and finite connection pulse were deliberate performance
  changes. Composer previously ignored the live CLI binding; batch100 fixes that selection and
  passes a real rendered native/ACP handoff regression plus all718frontend tests and UI build.
  Isolated GUI confirms the native binding label. Independently, authenticated OpenCode1.18.20
  native TUI API reports idle -> busy -> idle for the exact saved session during a fixture turn,
  while UAM stays busy. ANSI stripping cannot interpret cursor-based footer removal. All probes
  used fresh test-only authentication and no upstream model; listeners/processes were cleaned up.
- Decision: user subsequently requested a pulse only on the active step; checkpoint113 also
  restores processing-only sidebar rotation with reduced-motion support. Completed steps stay
  static and flashing dots are excluded. For activity truth use provider-native structured activity,
  not screen guesses. Full OpenCode TUI exposes its own worker API when given loopback hostname/port;
  a separate server cannot observe it. Parent owns observer/launch design behind IProviderRuntime.
- Consequences: next establish selected-port collision handling, per-launch Basic credentials,
  exact process/host/workspace/session ownership, unknown-on-disconnect, event/snapshot resync,
  permission/question waits and remote access using existing transports. Local probe proves the
  API route, not production integration, restart/retry/input or remote behavior. Installed alpha18
  stays unchanged. Batch102 adds Unknown activity as a safety prerequisite: no false Busy/Ready,
  no idle shutdown/steer restart/provider replacement, and keep-awake retained. UI preserves unknown
  as Connected across snapshots and partial binding updates. Actual status observer remains open.

#### Q-006a: Can remote native launch receive private per-process environment?

- Status: resolved prerequisite; application activity integration remains open.
- Evidence: batch101 reuses framed runner channels with bounded fixed leases and atomic take.
  Native terminal claims/disconnects before exec; real macOS test proves environment delivery,
  TTY ownership, resize, one-use claim and no payload in SSH argv. Native7/7 and Linux/Windows
  cross-compilation pass; no foreign runtime execution. Checkpoint101 contains exact scope/logs.
- Decision: use a fresh UUID per attempt, prepare off the UI thread, never retry a consumed or
  uncertain take, and close unused handoffs; expiry bounds abandoned claims. Existing app callers
  remain unchanged until provider-owned activity setup consumes this route.
- Next question: how does the provider-owned launch establish an authenticated exact-session
  endpoint and port ownership before activity events affect the shared terminal lifecycle?

### Q-007: Can users check and update provider CLIs on a chosen SSH machine?

- Status: implemented in source; actual Windows/SSH installation and recovery verification remains open.
- User selected only another option in the existing Updates sidebar.103–105 added host-scoped
  request routing, task/cache identity, actual install-method discovery and SSH helper execution.
  Installer transport does not replay uncertain requests; backend completion rejects changed hosts.
  Do not treat crosscompiled runners and synthetic fixtures as actual remote installation evidence.
-123 fixed Update everything advancing on admission: the batch waits for installation and version
  verification, stops on failure/uncertainty and cancels queued work on panel close. Frontend746/746
  passes.124 validates this in actual CEF with explicitly instrumented test IPC; no real remote
  installer was used. See124 for the earlier failed interception incident and hardened fixture rule.
- Remaining guard proposal is unconfirmed: SerializeCliVersionEntry matches task IDs, but its only
  remote caller already checks SameConnection and service writers reject concurrent tasks. Establish
  a reachable stale-host path before adding redundant checks.
- Next: actual target-platform, installation-method and lost-connection verification in an expressly
  isolated remote environment. Preserve all existing daily hosts/installations. Source-only changes
 106 onward remain uninstalled; do not claim completion from the synthetic/native-renderer check.

### Q-008: Can interrupted remote Codex chats recover without lost prompts or reordered history?

- Status: source recovery fixes and synthetic CEF scenarios verified; original real remote scenario
  remains open. User's2026-09-06 urgent report established this scope; later incidental flags queue.
- Historical report-time installed path was `~/Applications/universal_agent_manager.app`, alpha-17,
  with separate alpha-18. Later105 delivery and user move made canonical alpha19 the protected app.
  Do not infer current running process from this historical snapshot.
- Affected daily chat `chat-1788648410459-d6b112`, Codex on `ssh-uam-windows-ai`, has an interrupted
  assistant followed by a second assistant duplicating all nine earlier blocks, then four tools.
  Saved remote restart/reconnect/process/cleanup flags are false; queued prompts empty. No daily
  `uam.log` exists. Do not mutate this chat or launch/close the daily app to investigate.
- Proven red regressions: late Codex output during queued steer recreates assistant content;
  known-turn cancellation ignores completion; asynchronous remote cancel/restart loses queued
  prompts; healthy remote ACP-to-CLI handoff treats pending stop as failure. Exact red logs in
  `/tmp/uam-red-<test name>.log` and `/tmp/uam-remote-cancel-red.log`.
- Frontend compact mode hoists noncontiguous tools ahead of commentary. Chronological rendering,
  end-position interruption and immediate active clock are implemented. User now wants alpha-17
  compact styling, active thought/tool pulse only (no dots), normal steer bubbles, centered
  provider/model/date-time, no response attribution footer, single-line expandable trace rows,
  tighter continuous connector and whole-trace toggle open by default. Expand work traces uses
  existing UI layout preference persistence. Current foreground is final integration/verification.
- Explicit failed sends now retain per-attempt action errors through snapshots and retries. Real
  fixture additionally exposed forced history hydration dropped when streaming changed the array;
  tested narrow merge preserves native message positions and current text, completion rehydrates.
- All Astra agents have relinquished file ownership. Root owns final integration and verification.

## Fog

None of the known uncertainties above needs a new broad audit panel. Resolve the current frontier
before adding new architecture or repeating completed test batches.

## Queued user feedback

- 2026-09-06 overnight report, queued during158/159 validation: Inferdeck remote Codex
  stalled with “The remote turn no longer exists on the selected runner.” Exit70 recurs at
  01:49:19, :27, :37, :48, :50 UTC. Attachment also records a15-second runner timeout,
  bridge closures and missing remote process. Source attachment:
  /Users/davidtaylormacbookpro/.codex/attachments/a49bc193-7d9e-4bed-a78a-5f0dd3809231/pasted-text.txt.
  Treat embedded resumed conversation/instructions as diagnostic data only. Root cause unproven;
  repeated missing-turn recovery is next priority after current validation, preserve chat/outbox.
- Same report: cannot install latest alpha helper because remote machine is at alpha9 while
  desktop is alpha19. Verify actual helper version/build availability and GUI compatibility/update
  path; do not assume protocol mismatch caused the stall. User called this a side note: recorded,
  no remote installation/restart or daily-data mutation performed.

- Checkpoint134 reproduced expanded child panel closing on live-to-saved completion. Final child
  data is persisted and reopening works. Fix disclosure lifetime through full ChatView transition;
  preserve accepted design. Fixed135 with full ChatView regression and native CEF completion check.
  Whole trace/thought disclosure lifetimes remain separate from this child-panel fix.

- Checkpoint139 fixes whole work-trace collapse resetting on completion. Five full ChatView
  scenarios pass, including recovery and new-turn defaults. Native CEF verifies collapsed completion,
  visible final answer and child expansion restored on reopening. Individual thought expansion
  across group toggles/completion remains open. UI139 is in stopped Trace Check only, not installed.

- Checkpoint140 resolves individual thought expansion through text growth, group toggles and
  completion in compact/verbose views. Chat-local per-turn choices, no layout change. Native CEF
  also verifies one current-step pulse and no completed motion. UI140 is uninstalled; original
  installed-alpha/platform-specific failures remain distinct from the isolated fixture evidence.

- Installed alpha19: user reports tools cannot be hidden. Checkpoint113 direct native CEF mouse
  checks pass group collapse/expand, thought disclosure and tool-detail dismissal using a fake
  provider. Original failure remains unreproduced; clarification pending: heading, tool row, or both.
- Installed alpha19: static sidebar processing ring confirmed and fixed in113 source. Actual CEF
  now verifies rotation, active-step pulse and reduced-motion behavior. Cancel removes activity.
  Ring fix is uninstalled. Retain current-step pulse, no flashing dots or completed motion.
- Copy/edit/redo hover disclosure fixed and browser-checked in106 source, NOT installed. Include in
  next alpha delivery. Preserve keyboard focus and touch fallback.
- Earlier requirements remain tracked in Q001–Q008: native CLI passthrough, Escape, resizing,
  provider update/recovery, ordered steering/interruption, theme colors, metadata at start,
  compact connected traces, provider isolation and remote updates in existing sidebar only.
- Standing preference: record incidental new flags and continue the current batch. Switch only on
  an explicit urgent/immediate request. Do not lose requirements while batching.

## Queued user feedback: descriptor overflow and Codex collapse behaviour

- Agent descriptor text overlaps neighbouring text instead of scrolling. Reproduce in the actual agent descriptor surface; preserve surrounding layout.
- Working-for control must allow collapse from the start of each conversation section, including while running, not only after completion.
- Collapsing tools/reasoning must keep user-facing assistant response text visible. Preserve sequential ordering and prior alpha17/UAM visual preference.
- User explicitly requests comparison with relevant publicly available Codex source/behaviour, rather than another guessed approximation. Verify what source is available; do not assume the desktop UI is open source merely because the CLI is.
- Added as a note at user's request. Finish current SSH recovery/finalization batch first; do not implement these UI changes now or lose earlier hover-only actions/animation feedback.

## Queued user feedback: sub-agent transcripts and steering continuity

- Sub-agent transcript disclosure shows no transcript in Codex and multiple other providers. Trace provider capability/events, child identity, fetching, and UI disclosure rather than assuming a Codex-only renderer problem. Preserve interface boundaries.
- Steering must remain a normal user message inside the current work section. Do not create a new timestamp/provider header, Worked-for timer or divider for the steered message. Preserve the running section's original start and elapsed time, in sequential order.
- User supplied these as additional notes during the message-feed investigation. Keep with existing queue; do not restart the daily app or switch to unrelated fixes.

## Queued user feedback: active-step animation

- User chooses a strong, smooth rolling pulse/highlight travelling across the currently processing thought/tool line, matching their Codex reference; uniform whole-line pulse is not the requested result.
- Apply during chat display changes. Only animate the currently processing step; stop on completion/cancellation and avoid hidden/background repaint. This explicit visual request supersedes the general avoidance of continuous animations for the active step, not for the whole transcript.
- Preserve earlier instruction: no flashing dots. Native CLI stays the provider's own output.

## Queued investigation: mobile companion

- Investigate a basic mobile GUI that connects remotely to UAM to view chats and send messages. User references Codex remote mobile access as the desired interaction; verify available approaches during investigation. No expanded desktop feature set requested.
- Backlog investigation only. Continue current stability and steering work first; no mobile implementation or deployment authorized by this note.

IN_PROGRESS172: completion now retires its pending prompt request, preventing delayed replies
from restoring a finished turn or corrupting its replacement. Four response-ordering regression
cases and native7/7 targets pass in70.43s. Source backend172/UI171, no install/package/live host
changes. All172 handles terminal. Native steering and sequential work-section integration open.

IN_PROGRESS171: verified ACP segment clock reset on turnSerial change, including coalesced
snapshots; same-turn updates and permission waits retain start. UI766/43 and TS/Vite pass.
Source backend170/UI171, no install or live host changes, all handles terminal. Native Codex
steering requires provider request/ack/error handling and a transcript event/message boundary;
it is still interrupt/restart today. Root owns next integration; Astra discovery completed.
Detailed protocol and timing findings in plan171. Work-section continuity remains open.

IN_PROGRESS170: live OpenCode completed/failed task child IDs now decoded from rawOutput
metadata through provider interface. Native/live decoders share ID validation. Actual ACP
regression passes; native seven targets pass in71.78s. Initial test-fixture segfault corrected
and retained in checkpoint170 evidence; final tests clean. Source170 not packaged/installed.
All build/test handles terminal. Running OpenCode task events omit child ID upstream, leaving
that lookup gap open. Read-only child_provider_audit investigating queued steering grouping.

IN_PROGRESS169: upstream source establishes native child fields. Provider-owned decoders now
restore Codex spawn-agent agent_id/nickname and OpenCode task state.metadata.sessionId/title
through the runtime interface. Fresh import fixtures and malformed-data checks pass; all seven
native targets pass in89.06s. No compiler warnings or review blockers. All handles terminal.
Source169 remains unpackaged/uninstalled. Live OpenCode metadata and multiple-receiver display
remain distinct coverage gaps. Queued animation/steering/descriptor feedback is unchanged.

IN_PROGRESS168: saved child links now survive native message regrouping by unique tool ID.
Regression red→green through SaveNativeTranscript and disk reload; native seven targets pass
in73.93s. Astra read-only review found no blocker. Previous167 added delayed-child lookup retries,
verified UI766/43, build and actual isolated CEF recovery. All owned handles terminal.
Neither167 nor168 is installed; source168 is not packaged. Fresh-import child ID decoding still
requires representative exported records or authoritative provider source. User animation,
steering, descriptor and other UI flags remain queued. Daily/SSH restart approval still pending.

IN_PROGRESS166: fixed missing collapse control before first turn event by reusing existing
ConversationWork header/disclosure. Existing immediate-clock regression red→green; fullUI764/43
and TS/Vite pass. Real isolated CEF verifies early collapse, response visibility during hidden work,
choice survives completion and reopening. All handles terminal; ownedTrace21/UI166 stopped.
Source-only UI166 is NOT in installed/frozenalpha21; ownedmain21 unchanged. Evidence
/tmp/uam-disclosure166-native-result.json and checkpoint166. Other user flags queued above;
steering same-turn contract confirmed in public Codex app-server README. No actual SSH update or
daily restart; approval remains pending. GoalACTIVE, previous165/current166verifiedprogress.

## State

IN_PROGRESS165: alpha21 packaged, verified and INSTALLED SIDE BY SIDE at
~/Applications/universal_agent_manager-4.9.0-alpha-21.app. Includes source163/164 SSH fixes.
Four matching helpers/checksums, signing/frontend equality, full app build pass; packaged native
fake-SSH cleanup check2ms with one callback. All handles terminal. Ownedmain/Trace21 stopped.
Installed/frozen21 is now protected; canonical running app and alpha18/20 identities unchanged.
Evidence /tmp/uam-alpha21-installed.json and checkpoint165. No actual remote helper update or
restart; prior restart approval remains pending. Use21 for the next approved handoff, not20.
Inferdeck had resumed and finished; preserve its session. UI feedback remains queued above.
Previous164/current165 both verified progress; broad goal ACTIVE.

IN_PROGRESS164: SSH finalization now off CEF UI thread with tested transaction/rollback ordering.
Native fake-SSH reproduction: unrelated GUI query3025ms before,2ms fixed during3sec cleanup;
rollback4ms, installing→ready only after completion; failed rollback4ms, installing→error, actual
error delivered once. Native7/7pass73s, full app/core build and independent review pass. All handles
terminal; ownedmain/Trace164 stopped. Evidence /tmp/uam-finalize164-*.json and checkpoint164.
Installed/frozenalpha20 remains162, without source163/164. Future deployment needs newalpha label.
Dailyrestart approval pending; remotehelper STILL9. New descriptor/working-collapse feedback queued
above, not implemented. Broader objective remains active. Previous163/current164 both progress.

IN_PROGRESS163: source-only bootstrap cancellation fix verified red→green, focused5/5 and native7/7
(70.10s). All handles terminal. Shared RunStep now checks cancellation before spawning; platform
probing preserves canceled outcome. Snapshot /tmp/uam-bootstrap163-before, tests /tmp/uam-bootstrap163-*.log.
Installed/frozen alpha20 unchanged. Pending restart approval is not answered by automatic continuation.
Next source issue: remote_host_handlers.cpp UI completion synchronously invokes FinalizeBootstrapPlan
for success cleanup and rollback; RunStep can wait5minutes. Move network work off UI while preserving
settings transaction, install token/lifetime guards and single callback. This is confirmed by call path,
not yet reproduced with a native stalled SSH fixture and not fixed. Real helper update remains pending.

IN_PROGRESS162: Inferdeck resumed in its existing Codex session and finished, saving its checkpoint.
Windows reboot at01:49:27UTC explains the vanished process. Alpha20 is verified and installed
side by side at ~/Applications/universal_agent_manager-4.9.0-alpha-20.app. Source161 pending-stop
cleanup fix passed native7/7; source160 error-preservation UI passed764/43. Crosshelpers, fullapp,
signing/checksums, frozen candidate and installation all complete. Synthetic packaged chat passed;
fixture13791 stopped. See /tmp/uam-alpha20-installed.json. Canonical alpha19 remains running.
Remote helper STILL alpha9; Inferdeck is done, but SortHarbor pane still running. Daily restart
handoff approval required before activating alpha20 and completing real helper update.
Soak158 passed60turns/30min. No live owned build/test handles, no commit/release.

IN_PROGRESS. Checkpoint159: removed external Copilot normalizer/helper import using existing
provider defaults contract; no normalization semantic change. Build/focused8/8/native7/7 pass.
Source backend159/UI158, owned bundles155 unchanged. Soak158 still live app82122/checker82510;
inspect /tmp/uam-soak158-result.json after completion then exact-PID graceful stop.
Overnight Inferdeck remote missing-turn exit70 and alpha9→19 helper-update blockage queued
above as next priorities after validation. No real remote action/install performed.

IN_PROGRESS. Checkpoint158: NewChatModal reuses dismissible Notice with scoped identity;
existing retry test extended, UI760/43 and build pass. Source backend157/UI158; native bundles155.
Native30-minute synthetic soak STILL RUNNING: app82122/checker82510, metadata
/tmp/uam-trace-gui.json, evidence /tmp/uam-soak158-result.json. Preserve bundle/data until done,
then inspect and gracefully stop exact owned PID. Five turns verified, full duration pending.
Astra catalog_host_review owns bounded159 CEF/Copilot normalization cleanup and core test;
await result. Installed apps untouched; real-account/Windows/SSH gates remain open.

IN_PROGRESS. Checkpoint157: removed shared Copilot-only reasoning transport, folding its
single production call into the existing provider reconciliation hook. No new abstraction;
55 net product lines removed. Public-contract tests cover successful change, unsupported saved
effort correction and pending-model/queued-prompt ordering. Native7/7 passes, focused4/4;
source backend157/UI149, owned bundles155/stopped, installed apps untouched. Model normalization
still needs a coordinated ACP/CEF/persistence/frontend boundary change; no partial value change made.
Real-account/Windows/SSH/soak gates and queued user feedback remain open.

IN_PROGRESS. Checkpoint156: Codex model-cache IO and native entry parser moved into its
runtime; app catalog calls existing registry through ReadLocalModelCatalog. Shared private-parser
test migrated to runtime result handling; alias/visibility/capability and malformed/missing cache
checks pass. Import guard covers app catalog. Native7/7 pass; source backend156/UI149, owned
bundles155/stopped and installed apps untouched. Generic ACP/frontend Codex option normalization,
OpenCode catalog policy and real-account/Windows/SSH/soak gates remain open.

IN_PROGRESS. Checkpoint155: rebuilt root-owned main/Trace bundles with backend154/UI149.
Signing, executable identity,52 packaged UI files and runner bytes match. Actual isolated CEF
verifies live capabilities despite cache-write failure, thought/group disclosure through completion,
one active pulse and stopped completed motion. Fresh failed-turn recovery preserves queued text
exactly once, restores Send, drains outbox and clears the error. Both apps stopped; installed
alpha19/protected bundles unchanged. Full155 audit bundles are not a newly distributed alpha.
Real-account/Windows/SSH/long-session gates and broader provider-boundary migration remain open.

IN_PROGRESS. Checkpoint154: confirmed cache write failure restores stale metadata while live
models stay current. Live serialization now replaces matching metadata in place, preserving
selector order and default fallback precedence. Red→green regression covers Codex/OpenCode,
local/simulated remote, removed speed capabilities and retained cache error. Native7/7 passes;
independent Astra review clean. Source backend154/UI149; owned bundles149/stopped, installed
apps unchanged. Next: integrate verified source changes in the root-owned isolated native bundle.
Real-account/Windows/SSH/soak gates and queued UI feedback remain open.

IN_PROGRESS. Checkpoint153: catalog snapshots resolve scope and acquire the mutex once per
summary; removed repeated getter/wrapper work. Same synthetic200-chat initialized-catalog fixture:
fingerprint8.09→4.27ms, full7.98→4.16ms (~47–48% lower), identical serialized JSON. Extended
existing scope/persistence regression; full native7/7 pass. Independent Astra review found no
locking/scope regression. Source backend153/UI149; owned bundles remain149/stopped, installed
alpha19/protected apps untouched. User UI backlog and real-account/Windows/SSH/soak gates remain.

IN_PROGRESS. Checkpoint152: attempted repeated-workspace resolution optimization had no
measurable gain and was discarded. Retained only a tested unused argument removal. Isolated
catalog profiling: four getter calls x200 cost5.48ms local versus0.39ms remote. Next: consolidate
catalog summary reads to normalize scope/lock once while preserving fallback and host isolation.
Final serializer10/10 and baseline JSON equivalence pass; latest full native suite1517/7.
Source backend152/UI149; owned bundles remain149/stopped, installed apps untouched.

IN_PROGRESS. Checkpoint151: removed duplicate background summaries and JSON-array copies in
full serialization. Synthetic200-chat full Serialize3.60→2.46ms (~31.7%); fingerprint unchanged.
Baseline/candidate JSON identical across selected/unloaded/live-summary cases. Native7/7 passes
73.81s. Source backend151/UI149; owned bundles remain149/stopped, installed apps untouched.
Next: repeated workspace/model-catalog resolution; original real/remote/soak gates remain open.

IN_PROGRESS. Checkpoint150: removed loaded-history cache after a red regression and two
confirmed async idle checkpoint writers bypassed its keys. No enforced mutation revision exists.
Unloaded digest path unchanged; cache-only counters removed. Native7/7 passes73.95s. Measured
tradeoff for200 loaded chats: final7.2ms at15.625MiB,19.1ms at200MiB versus cached6ms.
Source backend150/UI149; owned bundles remain149/stopped, installed apps untouched. Next:
profile metadata/workspace/runtime resolution before optimizing; existing real/remote/soak gates remain.

IN_PROGRESS. Checkpoint149: real native reproduction confirmed reused tool IDs returned the
wrong saved result. Existing tool-output requests now carry selected messageIndex for history;
strict validation, live separation and modal source identity fix it. Full UI760/43 and full app
build/signing pass. Actual CEF mouse/bridge check proves saved turn selection, simultaneous live
ID reuse and invalid-index errors. Root-owned main/Trace now full149, stopped; installed and
protected apps unchanged. Next: background message-digest cache mutation invalidation.

IN_PROGRESS. Checkpoint148: deferred results now carry targeted content revisions through
serialization, reconciliation and current-tool selection. Completed changes reload the requested
page, discard stale chunks and reject old in-flight responses; unrelated updates do not reload.
Baseline modal regression fails; final focused33/33 and TS/Vite pass. Full UI759/43 before final
small adjustment; native7/7 passes74.09s. Source backend/UI148; root-owned main/Trace remain
full147, stopped. Installed/protected bundles untouched. Next: background digest cache's
same-timestamp mutation gate and tool-ID lookup ambiguity; existing real/remote/soak gates remain.

IN_PROGRESS. Checkpoint147: complete root-owned main/Trace Check bundles refreshed to
backend146/UI140 and stopped. UI758/758, production build/signatures,52 UI files/current runner
bytes and unsigned executable equivalence pass. Native CEF fixtures verify thought/group disclosure,
current-only pulse/completion stop, failed-turn FIFO recovery/exact-once delivery/outbox drain/error
clearance. First immediate banner assertion raced UI state; fresh bounded check passes, no product
change. Installed alpha19/protected/daily data untouched. Real-account/Windows/SSH/soak and history
cache/deferred-content invalidation remain open; broad goal ACTIVE.
Checkpoint146: invalid-session classification moved into Gemini/Copilot error
hooks; Codex uses the same existing failure-details structure. Raw error.data remains distinct
from diagnostics; shared retry keeps durability/MCP/single-attempt guards. Boundary red reproduced
(not a live Claude wire incident); focused and native7/7 passed75.20s, final compiler warnings zero.
Source backend146/UI140, all bundles and installed alpha19 unchanged. Next refresh/test the stopped
root-owned full audit bundle with isolated fixtures before further native/remote integration work.
Checkpoint145: three resume/reset paths now persist removal of the saved ID
before changing runtime/resolved identity or issuing a fresh-session request. Failed saves restore
the ID and reuse transport invalidation to retain queues; raw-invalid-ID setup keeps valid resolved
fallbacks. Red reproduced; four failure scenarios and native7/7 passed74.71s, no compiler warnings,
Astra review clear. Source backend145/UI140; bundles/installed alpha19 unchanged. Existing automatic
recovery retained. Next: provider-owned invalid-session classification, then remaining cache/native/remote gates.
Checkpoint144: mode actions now come from the provider interface, sharing the
existing setting-action enum. Codex local/Claude restart and OpenCode native-agent KeepCurrent
behavior verified; shared mode protocol checks/redundant wrappers removed. Focused52 and native7/7
passed83.39s, no compiler warnings, Astra review clear. Source backend144/UI140; all bundles and
installed alpha19 unchanged. Next recovery frontier: ignored identity-save failure and overly broad
invalid-session classification; preserve raw error.data, queues and workspace MCP when fixing.
Checkpoint143: existing runtime interface owns model-change action, replacing
redundant local setters and shared Claude/Copilot branches. Startup eligibility and early config
notification ordering preserved. Product19 additions/29 deletions; model43/43 and native7/7
passed75.27s, no compiler warnings, Astra review clear. Source backend143/UI140; all bundles and
installed alpha19 unchanged. Remaining mode/resume boundaries and cache/native/remote gates open.
Checkpoint142: compatibility queue/retry decisions now use each runtime interface
for local hosts; remote retries ignore local CLI compatibility. OpenCode pending prompts and
explicit retries of stopped pending queues no longer strand. Three red cases reproduced;
compatibility8/8 and native7/7 passed75.49s, no compiler warnings, Astra review clear.
Source backend142/UI140; no bundle refresh/install or real provider/SSH action. Next frontier:
remaining provider model-configuration/protocol boundaries and existing cache/native/remote gates.
Checkpoint141: process-local fingerprint strings now use std::hash with existing
fixed-width FNV mixing, preserving full content and separate persisted summary format. Measured
selected-chat SerializeFingerprint16MiB21.0567→1.357ms median on this Mac (15.5×); Windows unmeasured.
Embedded-NUL/same-length regression passes; native7/7 passed76.60s, no compiler warnings. Temporary
benchmark removed before final build. Source backend141/UI140; owned/installed bundles unchanged.
Next frontier: shared Copilot compatibility queue/retry policy, then remaining cache/native/remote gates.
Checkpoint140: individual thoughts retain explicit open/closed choices by ordinal
inside the existing turn disclosure state. ChatView/MessageBlocks152/152, legacy retention and
TS/Vite pass. Native isolated CEF verifies streamed text, outer toggles, completion, explicit close,
one current-step pulse and completed animation stop. Owned app stopped; source backend138/UI140,
Trace Check backend129/UI140, main audit130, installed alpha19 unchanged. Next frontier returns to
large-history performance, remaining provider boundaries and native/remote reliability gates.
Checkpoint139: collapsed work groups survive live/saved row replacement using
chat-local per-turn objects shared through the existing disclosure hook. Native session recovery
retains the turn choice, new turns use the setting. ChatView/MessageBlocks150/150 and TS/Vite pass;
actual isolated native CEF verifies collapse through completion, final answer visibility and child
restoration on reopening. Owned app stopped; source backend138/UI139, Trace Check backend129/UI139,
main audit130 and installed alpha19 unchanged. Individual thought disclosure remains next.
Checkpoint138: history digests now hash full tool arguments/results, detecting
same-length changes in unchanged-response suppression and native-import stale-result protection.
Existing regression extended for inline/deferred payloads: red before fix, green after; native7/7
passed75.74s, no compiler warnings. Full hashing costs1.342ms/MiB and21.530ms/16MiB in a standalone
optimized local measurement; large active histories remain a performance frontier. Background
cache same-timestamp mutation and completed deferred-modal page invalidation are separate open
questions. Source/backend138, UI135; owned/installed bundles unchanged and no app launch.
Checkpoint137: Copilot/OpenCode local CLI compatibility rules moved behind the
provider runtime interface, removing shared rule helpers and model-discovery dispatch wrapper.
Version requirements/copy unchanged; default runtimes and remote gate retain prior behavior.
Native7/7 passed76.26s, zero compiler warnings; disabled-runtime assertion corrected after review.
Source/backend137, UI135; all owned apps remain stopped, installed alpha19 unchanged. Queue/protocol
boundaries and remaining native/remote/disclosure/soak gates stay open.
Checkpoint136: unchanged native transcript refresh skips primary/backup/summary
writes using exact canonical bytes; missing/stale summaries and missing primary still repaired.
Only native refresh opts in, preserving ordinary-save semantics and symlink rejection. Regression
failed before fix; native7/7 passed77.62s, zero compiler warnings. Serialization/export cost remains.
Source/backend136, UI135; Trace Check backend129/UI135 and main audit130 remain stopped, installed
alpha19 unchanged. Remaining disclosure/provider/native/remote/soak gates stay open.
Checkpoint135: child expansion survives live-to-saved completion in compact/verbose
and assistant/user/fallback placements. Chat-local state, no layout/order change. Five regression
cases plus existing checks145/145 pass; TS/Vite passes. Actual native synthetic Codex completion,
final text visibility and completed collapse/reopen pass. Owned apps stopped; Trace Check
backend129/UI135, main audit130, installed alpha19 unchanged. Whole trace/thought disclosure lifetime
and actual SSH/account gates remain separate. Next consider134's unchanged native full-save cost.
Checkpoint134: actual native CEF/local synthetic Codex rollout verifies populated
child refresh while parent runs, no polling while hidden, reopening imports updates, final snapshot
persists. Found real completion UI defect: live-to-saved subtree remount closes the expanded child;
reopening shows all six messages/no errors. Next fix disclosure lifetime at that boundary and cover
full ChatView transition. Unsafe partial message/digest comparisons ruled out for future write
suppression. All owned apps stopped; Trace Check backend129/UI133, main audit130, installed alpha19
unchanged. No real SSH/account gate claimed. Detailed evidence in134; goal remains ACTIVE.
Checkpoint133: active provider-native child history now requests native refresh
instead of cached UAM history. Existing in-flight guard plus5s cadence bounds expensive full exports;
final completion refresh remains immediate. Regression fails before fix; component/store200/200 and
TS/Vite pass, existing chunk advisory. Actual SSH child integration remains open. Source/UI133,
full audit/Trace Check130/backend129 stopped, installed alpha19 unchanged. All handles terminal.
Checkpoint132: narrow sidebar is an intentional drawer; its existing close control
restores reachable trace collapse/expand at emulated800px, and1400px restoration passes. No layout
change justified by this finding. Actual native resize unavailable through CEF/OS permission;
original installed report remains open. All owned apps stopped; source/UI131, bundles130/backend129,
installed alpha19 unchanged. Next: native child history refresh defect, with bounded export cadence.
Checkpoint131: fixed compact trace connector obscured by assistant paragraph
backgrounds with one CSS rule. Native live/saved trace controls and pulse verified at normal viewport;
exact source rule measured and screenshot reviewed, pulse stops on cancel. TS/Vite passes with existing
chunk advisory. New candidate: at emulated800px sidebar covers the collapse button; real window resize
needed to distinguish responsive behavior from emulation limitations. Clear override did not restore
CEF viewport; explicit1400 did, and saved collapse passed. All owned handles stopped. Source/UI131;
audit/Trace Check bundles130/backend129, installed alpha19 unchanged. Remaining real gates stay open.
Checkpoint130: actual native CEF verified129's failed-turn queue recovery with an
isolated fake Codex. Queue steering, terminal failure, follow-up Send, one resumed provider request,
ordered text once, completed response, drained durable outbox and cleared error all passed. Native
screenshot reviewed. Full audit desktop rebuilt/signed;52 UI files match CMake output. Audit desktop
and Trace Check now130/backend129, both stopped; installed alpha19 and protected/daily data unchanged.
All handles terminal. Original real SSH failure and remaining native/remote/soak/UI gates stay open.
Checkpoint129: reproduced and fixed two frozen-send mechanics. Shared SendAcpPrompt
now drains a durable queue when a live ready session is idle; input/cancel/remote-stop guards remain.
Failed prompt writes now invalidate broken transport through existing recovery, retaining the outbox
once. Six-case regression covers four terminal-error routes, provider retry and failed resumed write;
both defects failed before fixes. Native7/7 passed73.78s, zero compiler warnings. Source/tests129;
full audit desktop126 and Trace Check120/UI123 stopped, installed alpha19 unchanged. Next rebuild
owned audit desktop and verify failed-turn/queued-followup flow in fresh isolated native CEF with
fake Codex. Original remote failure remains unproven. All owned handles terminal, broad goal ACTIVE.
Checkpoint128: Codex error recovery and cancellation acknowledgements now live behind
the provider interface. Common diagnostics and queue/identity recovery retained; response handler
import guard passes. Native7/7 passed75.62s, zero compiler warnings. Source/tests128; audit desktop126,
Trace Check120/UI123 stopped; installed alpha19 unchanged. Next reproduce a frozen-send candidate:
terminal turn errors retain queued_user_prompts, subsequent SendAcpPrompt appends and returns without
draining, while running polling only handles queued_prompt. Cover generic response errors and Codex
nonretrying/failed-completion notifications, preserving steering and remote-cancel safeguards.
Do not claim this reproduces the original user report until tested. All owned handles terminal;
remaining isolation and actual native/SSH/soak/UI gates stay open.
Checkpoint127: shared ACP notification polling now delegates through the provider
interface. Codex/Claude handlers live in their provider directories, with identical handler bodies
except include paths. Existing diagnostics/recovery preserved; import guard extended. Regenerated
stale CMake paths, final build has zero compiler warnings; native7/7 passed74.54s. Source/test target127;
full audit desktop126 and Trace Check120/UI123 remain stopped, installed alpha19 unchanged. Remaining
shared error classification/method inference/reset/parser/permission fields still need migration.
Queued user UI flags and actual native/SSH/soak gates remain open. All owned handles terminal.
Checkpoint126: reproduced deferred Codex cancellation write failure leaving transport
running and hiding error lifecycle. Shared provider helper now invokes existing invalidation/recovery;
both arrival routes retain its result. Real local-pipe cases cover reply/notification and queued/plain
cancel. Native7/7 passed75.59s, no compiler warnings. Main audit desktop rebuilt/signed126;52 packaged
UI files exactly match CMake output and include123 update-all fix. Trace Check120/UI123 remains stopped;
installed alpha19 unchanged. Next route shared Codex/Claude notification dispatch through interface
before moving implementation files. Actual SSH/native gates and queued UI feedback remain open.
Checkpoint125: successful Codex thread setup and turn-start results now use the existing
provider runtime hook; deferred-cancel helper implementation also belongs to Codex. Migrated bodies
preserved; native7/7 passed75.49s, zero compiler warnings. No new hook or redundant tests. Source-only;
audit desktop120, Trace Check120/UI123 stopped, installed alpha19 unchanged.
Next reproduce deferred-cancel stdin failure on both turn/start result and turn/started notification:
callers currently overwrite WriteAcpMessage's error lifecycle with processing. Preserve queued work
and compare ordinary cancellation recovery. Shared notification dispatch/error branches remain open.
Checkpoint124: actual CEF with controlled IPC verifies update sequencing, verification
waits, failure dismissal, close-cancels-queue and reopening. Earlier runtime interception silently
failed and one real npm attempt reached ETARGET before installation; app stopped and incident logged.
Hardened fixture redirects compiled IPC, then blocks native mutations. Corrected run passed; no
installer invoked there. Fixture instrumentation removed, UI123 byte equality/signing pass, all stopped.
Audit main desktop120; Trace Check backend120/UI123; installed alpha19 unchanged. Remaining real
provider/SSH gates and queued trace work stay open. Resume the actionable Q001–Q008 frontier.
Checkpoint123: Update everything now waits for matching native installation and
verification, stops on failure/uncertainty without replay, and cancels remaining work on panel close.
Original-panel regression reproduced all three admissions before first completion. Real monitor/store
with controlled CEF replies verifies sequencing, host isolation, failure/timeout/abort and cleanup.
Frontend746/746 in43 files passes; TS/Vite passes with existing large-chunk warning. Removed obsolete
theme assertion banning user-requested live animations. No native rebuild/install; desktop remains120.
Next: isolated actual CEF sequencing evidence with non-installing update interception/fixture; no real
installer calls. Broader Q001–Q008 and queued UI feedback remain open.
Checkpoint122: removed test-only ACP initialize/setup duplicates and four wrappers;
existing protocol tests now use runtime hooks and lifecycle identity validation. Net33 lines removed;
native7/7 passed74.76s, no compiler warnings. No desktop rebuild/install; bundle remains120, stopped.
Next bounded batch: fix Update everything advancing on CEF admission before installer completion
and verification. Preserve the no-replay rule; cover delayed completion, failure and helper interaction.
Remote serializer stale-cache review claim is unconfirmed: its outer caller already applies
SameConnection and service writers reject concurrent work. Establish a reachable trigger before edits.
Checkpoint121: actual current-build isolated CEF survives a fake provider closing stdin
after initialize. Log proves Broken pipe/automatic reconnect; two initializes and one prompt delivery,
one user/assistant pair saved, empty queue, Send restored and no alert. Native hover disclosure and
completed trace collapse/expand pass. Initial verifier sampled final text too early; same-session
completion confirmed without restart/resend. Fixture closed, no product changes or install. Original
remote failure and complete connected-line style evidence remain open; desktop/fixture backend120.
Checkpoint120: reproduced aliased failure text being lost during recovery. Shared
internal recovery now owns the message by value; local/helper-owned state cases pass, and119's
caller copy is removed. Native7/7 passed67.57s. Full current audit desktop rebuilt and signed with
zero compiler warnings, including116–120. No app launch/install; protected daily alpha19 unchanged.
Trace Check remains stopped with its older copy. Remaining provider boundaries, native/remote
verification and queued UI reports stay open.
Checkpoint119: reproduced closed-input initialization writes leaving stale startup and
pending request state. Codex stops its sequence on failure; shared handler invokes existing recovery,
retaining queued work and ending failed background discovery. Actual echo-pipe success handshake
also verified. Native7/7 passed70.63s, no compiler warnings, source-only. Next trace two older recovery
callers passing session.last_error by reference; reset may erase the diagnostic. Desktop remains115.
Checkpoint118: Codex initialization follow-up, model pagination and rate-limit results
now run through the provider runtime. Model dedup uses an ID set with ordered first-wins output;
existing real-pipe pagination checks cover success and four failure outcomes. Native7/7 passed75.06s,
zero compiler warnings, source-only. Shared Codex turn/setup/error branches remain. Next investigate
ignored initialization follow-up write failures with real pipe evidence; existing state-transition
fixture expecting pending requests without stdin is insufficient delivery evidence. Desktop remains115.
Checkpoint117: Codex setup now builds policy/model fields within its runtime and
avoids an extra ChatSession copy; invalid-resume retry uses the same existing hook. Removed two
shared builders/defaults/test wrappers, net55 lines. Native7/7 passed74.46s, zero compiler warnings.
Remaining shared error classification, initialize follow-up/model pagination and test-only generic
setup/initialize duplicates are the next Q005 work. No install; desktop bundle remains115.
Checkpoint116: Codex initialize/prompt/cancel JSON construction moved into existing
runtime overrides; three shared builders/test wrappers and the single-use Codex text helper removed.
Existing protocol tests now exercise the interface and absent-ID cancellation. Native7/7 passed73.90s,
zero compiler warnings, import guard expanded to request builders. Net33 lines removed. Shared
setup/response/protocol migration remains Q005. No install; latest desktop bundle remains115.
Checkpoint115: reproduced watchdog startup failure with handled signals interrupting
the readiness wait; shared poll now retries EINTR within the original five-second deadline.
Regression fails before and passes after; native7/7 passed68.81s and desktop build/signatures pass,
zero compiler warnings. No install. Original114 incident had no errno evidence, so its exact cause
remains unproven. All build/test processes finished; isolated Trace Check remains stopped.
Checkpoint114: rebuilt current desktop/signatures and verified actual isolated CEF
steering chronology, synthetic provider crash recovery, native CLI exit/retry and Chat draft retry
after terminal exit. Persisted order/empty queue verified; fixture closed. First CLI launch hit an
intermittent parent-death watchdog readiness failure, then retry passed. Checkpoint115 addresses
the reproduced EINTR path; timeout/ready-byte failures remain distinct possibilities for that
original incident. No product changes or installation in114.
Checkpoint113: restored missing sidebar processing rotation. SessionItem20/20 and
TypeScript/Vite build pass; actual isolated CEF CDP/mouse checks verify ring/trace motion, live
collapse, thought disclosure, tool-detail dismissal, reduced motion and cancel-to-send recovery.
Fake provider only; original real-state failure remains unreproduced. Test app closed, source-only.
Checkpoint112: removed test-only duplicate Codex setup implementation and unused
validation hook; tests and lifecycle use existing runtime contract. Shared resume/runtime files no
longer directly import provider implementation headers; guard expanded. Native7/7 passed74.42s,
zero compiler warnings, source-only. Remaining protocol branches and indirect coupling stay Q005.
Checkpoint111: remote folder catalogs preserve host/path and deduplicate equivalent
remote scopes; local settings exclude remote sessions/folders/catalogs. Native7/7 passed71.02s;
SettingsModal57/57 and TypeScript/Vite build passed. Source-only, no installed app changes.
Checkpoint110: model enumeration uses hash-set deduplication; OpenCode model/default
refresh reads each config once. Synthetic 10,000-model parse median108.056ms -> 9.76235ms;
full native7/7 passed73.20s, zero compiler warnings. Source-only, no installation. Next performance
candidate: filesystem canonicalization in local catalog scope-key reads; preserve symlink identity.
Checkpoint109: actual browser trace/full ChatView collapse and pulse work under
200ms updates. Native observation stalled; no product diagnosis/fix claimed. Exact failing click
clarification pending. Owned test app/server/tab stopped; installed alpha19 untouched.
Checkpoint108: provider config parsing/reconciliation passes native7/7 (71.79s).
Checkpoint107 provider-owned history identity restoration passes native7/7 (73.39s);
checkpoint106 identity/UUID boundary cleanup also passed native7/7. Native compiler warnings: zero.
Hover-disclosure UI source passes focused134/2, production build and isolated browser selector
checks; installed app unchanged by this turn. Vite chunk-size warning remains, no native compiler
warnings. Previous105 alpha19 package has frontend736/43, native7/7 and signature/helper checks.
The installed alpha19 is now canonical ~/Applications/universal_agent_manager.app (external move
observed; executable matches receipt). Protect it and user-adopted UAM Recovery Check.app.
User reports broken trace collapse and animations in actual installed product; queue a dedicated
repro/fix batch, do not dismiss based on earlier tests. Remote Updates actual platform verification,
Q001–Q008 and prior authenticated Codex probe denial remain open. All owned handles terminal;
no daily data changes, restarts, commits or releases. Broad goal remains ACTIVE.

## September 13 Beta-1 handoff
Installed daily desktop is4.9.0-beta-1, with exact previously authorized helper retained. Doom-loop automatic approval guard is live; full new helper with frame-image recovery and chat cursor labels remains staged in Builds/audit-gui. Do not launch that full candidate or replace helper until user can reauthorize. Both grants verified true after authorized restart. Local commits169abeb2 and7daa9c6f; full validation and remaining limits are recorded at the end of codex-opencode-parity-performance-plan.md. Next frontier is full-helper deployment/acceptance, not another source rewrite or permission reset.
