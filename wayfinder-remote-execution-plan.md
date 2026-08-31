# Wayfinder: provider handoff, remote execution, and companion implementation plan

## Destination

Implement the accepted product improvements without weakening UAM's local ownership or five-provider
support: truthful provider handoff, first-class execution hosts, an SSH-delivered persistent runner,
host-aware workspaces and activity, honest task/subtask presentation, and a narrowly scoped remote
companion. The route is complete only when every phase has automated contract evidence, destructive
and failure-path evidence, and a repeatable packaged-product journey exercised through Computer Use.

This file is a plan only. It does not authorize implementation, remote installation, provider account
use, or network changes.

## Constraints

- Preserve `wayfinder.md`, `wayfinder-implementation.md`, and the append-only history in
  `progress.md`; they are prior decisions and evidence, not files to replace.
- Preserve Gemini CLI, Codex CLI, OpenCode CLI, Claude Code CLI, and GitHub Copilot CLI.
- Gemini CLI must not be launched on the owner's local system. Verify its shared code paths with
  fixtures and, later, only on an explicitly authorized isolated remote host.
- Keep the desktop application and local chat store authoritative. A remote host executes work; it
  does not become a second independent UAM database.
- Reuse the existing provider runtime, ACP, terminal, Active Chats, notification, workspace, and
  subagent surfaces. Do not build parallel replacements.
- Use `~/.ssh/config`, the system SSH client, host-key verification, and the user's existing SSH
  agent/keychain. Never copy or store private keys or provider credentials.
- The runner must not require `sudo`, listen on a public port, or download its own executable.
- All installation, service creation, firewall/network changes, provider authentication, and real
  account prompts require an explicit confirmation at action time.
- No claim of certainty may rest on a screenshot or a unit test alone. “Verified” means independent
  contract, fault, packaged-UI, and applicable platform evidence agree.
- Windows behavior must be verified on Windows. This macOS system cannot substitute for that gate.
- Prefer the smallest usable release. Deferred comparison mode, provider decision assistant, a
  replacement command palette, a second attention inbox, and a native mobile app stay out of scope.

## Current frontier

- Route clear. Implementation can begin with Phase 0 after the owner approves this plan.
- The first code change is Q-R001's provider-handoff preview because it is isolated, valuable, and
  establishes the “preview before mutation” interaction used again by runner installation.

## Fog

- The exact operating systems and CPU architectures of the owner's AI desktop and home lab. This
  affects the physical acceptance matrix, not the protocol or data model.
- Whether mobile notification delivery should later use Web Push, a user-configured webhook, or a
  self-hosted service. The first companion release does not depend on push delivery.
- Provider-authentication and quota behavior on each remote machine. Runner acceptance uses fake
  provider executables first; real accounts are an optional final confidence layer.
- Upstream providers may not expose child-session transcripts. UAM must preserve that uncertainty
  rather than synthesizing unavailable conversation data.

## Decisions

### Q-R001 — What exactly does provider handoff preserve and reset?

- Status: resolved
- Why now: The accepted P0 change is small and prevents users from assuming native session state or
  permissions cross provider boundaries.
- Blocked by: none
- Evidence: `SwitchChatProvider` in `src/app/chat_lifecycle_service.cpp`; provider default changes in
  `UI-V2/src/store/slices/sessionsSlice.ts`; existing provider selectors in `ChatView.tsx`.
- Decision: Every provider change opens one confirmation dialog generated from a backend handoff
  preview. It states that UAM messages, recorded tool-call output, chat title, and workspace reference
  remain; the native provider session is discarded; the destination model, reasoning, speed/service
  tier, permission, safety, memory, and agent defaults are applied; provider-native commands and live
  tool state do not transfer. Cancel performs no mutation. The dialog is skipped only when the source
  and destination resolve to the same provider.
- Consequences: One preview object drives both the copy and the mutation test, preventing UI prose
  from drifting away from backend behavior.
- Revealed questions: none

### Q-R002 — What is the minimum first-class host model?

- Status: resolved
- Why now: Adding SSH branches directly inside five providers would multiply lifecycle bugs.
- Blocked by: none
- Evidence: `ChatSession` currently has provider and workspace fields but no host identity;
  `StartCliTerminalForChat` and ACP launch paths ultimately use local platform services.
- Decision: Add one persisted `ExecutionHost` record and one `host_id` reference on executable chat
  state and workspace profiles. The built-in immutable host is `local`. A remote host stores a user
  label, SSH config alias, runner status/version, platform facts learned by handshake, and last-seen
  health—never credentials. Existing data migrates to `local` without changing behavior. Runtime
  creation resolves `host_id` once, before choosing structured or terminal transport.
- Consequences: Provider runtimes continue producing provider argv/protocol behavior; host routing
  decides where that behavior runs. Host selection cannot silently fall back to local execution.
- Revealed questions: Q-R003, Q-R006

### Q-R003 — How is the helper installed safely?

- Status: resolved
- Why now: The helper is a new executable and persistent-access boundary.
- Blocked by: Q-R002
- Evidence: accepted owner direction and prior SSH-helper discussion.
- Decision: Default to controller-led bootstrap over an initial SSH connection, with a documented
  manual fallback. The UI first performs read-only OS/architecture/home-directory discovery, then
  previews the exact version, checksum, destination, service type, and commands. After confirmation,
  UAM uploads the matching locally packaged runner to a random temporary name, verifies its SHA-256
  remotely, atomically renames it into a versioned user-owned directory, performs a protocol
  handshake, and only then updates the `current` pointer. No `curl | sh`, PATH mutation, or `sudo`.

  Default locations:

  | Platform | Versioned runner directory |
  |---|---|
  | Linux | `~/.local/share/universal-agent-manager/runner/<version>/` |
  | macOS | `~/Library/Application Support/Universal Agent Manager/runner/<version>/` |
  | Windows | `%LOCALAPPDATA%\UniversalAgentManager\runner\<version>\` |

  The manual path copies the same artifact and runs its built-in `install-user-service` command; it
  does not introduce a second installer.
- Consequences: The controller, release manifest, and remote checksum identify the same bytes.
  Cancelled or failed bootstrap leaves the existing `current` version untouched.
- Revealed questions: Q-R004, Q-R005

### Q-R004 — Why is a persistent runner necessary, and how does it communicate?

- Status: resolved
- Why now: SSH alone cannot make a task survive a controller crash or laptop disconnect.
- Blocked by: Q-R003
- Evidence: the owner's long-running and multi-machine journeys; existing ACP/terminal process-tree
  ownership and cancellation behavior.
- Decision: Ship one headless `uam-runner` binary with subcommands rather than a service framework.
  Its per-user service owns provider process groups, ACP/terminal byte streams, cancellation, and a
  bounded append-only event journal. Linux uses `systemd --user`, macOS a LaunchAgent, and Windows a
  per-user Scheduled Task or equivalent user-session service. The service exposes only a local
  user-permissioned Unix socket or named pipe. Each SSH connection runs `uam-runner bridge`, which
  forwards length-bounded framed JSON between stdio and that local endpoint. There is no TCP listener.
- Consequences: Tasks can continue while the controller is absent. Reconnect negotiates protocol
  version and replays journal events after the last acknowledged sequence number exactly once from
  the controller's perspective.
- Revealed questions: Q-R005, Q-R007

### Q-R005 — Who owns which state?

- Status: resolved
- Why now: Two writable chat databases would create conflict and recovery ambiguity.
- Blocked by: Q-R004
- Evidence: current canonical chat JSON, lazy hydration, and runtime event reconciliation.
- Decision: Desktop UAM remains canonical for chats, folders, settings, memory, workspace profiles,
  and display history. The runner persists only job identity, launch metadata, minimal runtime state,
  monotonically sequenced events, terminal replay bytes within a configured cap, and completion or
  attention state. On reconnect, UAM idempotently folds unacknowledged events into its existing state
  and acknowledges the highest durable sequence. The runner may compact only acknowledged history.
- Consequences: Disconnect recovery is deterministic and export/import ownership remains local.
  Remote journal loss can lose unseen live output but cannot rewrite canonical history.
- Revealed questions: Q-R008

### Q-R006 — How do workspace profiles behave?

- Status: resolved
- Why now: Remote paths are meaningless without a host, and forced defaults conflict with the target
  user's provider-switching workflow.
- Blocked by: Q-R002
- Evidence: current one-level workspace folders and per-chat workspace paths.
- Decision: A workspace profile is `(host_id, path, optional label)` plus optional provider-specific
  defaults. New Chat may prefill those defaults, but every value remains editable before creation.
  Precedence is explicit chat choice, then workspace-provider default, then workspace default, then
  global provider default. Paths are interpreted and validated only by the selected host. No local
  existence check is applied to a remote path.
- Consequences: The same logical project can have separate profiles on the Mac, AI desktop, and home
  lab without pretending their filesystem paths are interchangeable.
- Revealed questions: Q-R008

### Q-R007 — What security boundary applies to remote execution?

- Status: resolved
- Why now: A runner is persistent code execution under the remote user's account.
- Blocked by: Q-R003, Q-R004
- Evidence: system SSH trust model; UAM's existing provider-child environment filtering and path
  containment work.
- Decision: SSH host-key verification and the user's SSH configuration authenticate the host. UAM
  never auto-accepts a changed or unknown key. Runner requests use typed argv arrays, bounded fields,
  a per-connection nonce, protocol-version negotiation, and job ownership checks. Workspace and file
  operations are rooted in the selected remote workspace with symlink-aware containment. Attachments
  require a size/hash/destination preview before upload. Provider children inherit the remote user's
  normal provider authentication under UAM's existing provider-scoped secret filtering; UAM never
  copies provider tokens from the controller.
- Consequences: Remote execution has the same authority as the configured SSH user, clearly shown in
  the install and run UI. Host-key changes, protocol downgrades, path escapes, and unauthenticated job
  control fail closed.
- Revealed questions: Q-R011

### Q-R008 — How should remote work appear in existing UI?

- Status: resolved
- Why now: A second activity surface would duplicate the already accepted Active Chats design.
- Blocked by: Q-R002, Q-R005, Q-R006
- Evidence: `FolderTree.tsx` already derives Active Chats; `AppShell.tsx` already has notifications.
- Decision: Extend existing chat rows, header, New Chat, workspace picker, and Active Chats with host
  label and connection/runtime state. Active Chats remains the attention inbox and gains host
  grouping/filtering only if real use shows the list needs it; the first release adds a host badge
  and aggregated per-host counts. Reconnect, replay, runner update, and host-key failures use existing
  notification patterns with direct recovery actions.
- Consequences: Local users see almost no extra chrome; remote users can always answer “where is this
  running?” and “does it need me?”
- Revealed questions: none

### Q-R009 — How far should task and subtask hierarchy go?

- Status: resolved
- Why now: Provider protocols expose different amounts of child-agent state.
- Blocked by: none
- Evidence: `MessageBlocks.tsx` already loads child history when a provider supplies a child session
  ID and explicitly reports when it does not; existing subagent tool panels.
- Decision: Treat the original chat as the task. Represent UAM-managed agents and provider children
  with stable child session IDs as transcript-capable subtasks. Represent provider events without a
  child ID as expandable events, not synthetic chats. Add host, provider, status, elapsed time, and
  attention state to the existing child panel. Do not promise recursive arbitrary-depth trees in the
  first pass.
- Consequences: The hierarchy becomes more useful without inventing inaccessible internal messages.
- Revealed questions: none

### Q-R010 — What is the smallest useful mobile/remote companion?

- Status: resolved
- Why now: The owner needs oversight and emergency control, not another full coding client.
- Blocked by: Q-R005, Q-R007, Q-R008
- Evidence: owner journey; existing Active Chats, notification, cancel, permission, and input states.
- Decision: First ship a responsive authenticated companion page from the desktop UAM instance. It
  shows hosts, active tasks, recent output, completion/failure/attention, and audit history; it may
  stop a task and answer an outstanding prompt or approval. It cannot create hosts, install runners,
  edit provider settings, browse arbitrary files, or start general terminal sessions. It binds to
  loopback by default; remote access is an explicit setting intended for Tailscale or another
  user-managed private network, with a generated revocable device token and origin/CSRF protection.
  A native phone app and custom UAM cloud are out of scope.
- Consequences: The high-value monitoring journey lands without an app-store project or internet-
  exposed control plane. Notification delivery beyond an open companion is deferred.
- Revealed questions: none blocking

### Q-R011 — How do runner updates, rollback, and uninstall work?

- Status: resolved
- Why now: Persistent software must be reversible before it is installable.
- Blocked by: Q-R003, Q-R007
- Evidence: versioned install decision and long-running job ownership.
- Decision: Install updates side by side, verify checksum and handshake, then atomically change the
  `current` pointer for new jobs. A job remains owned by the runner version that started it until it
  finishes. Keep the immediately previous compatible version for rollback. Uninstall previews active
  jobs and refuses removal until they are stopped or finished; it removes the user service and UAM
  runner directory only, never provider CLIs, SSH files, workspaces, or chat data.
- Consequences: A bad update cannot strand current work or destroy unrelated remote state.
- Revealed questions: none

### Q-R012 — Which previously discussed features are implemented now?

- Status: resolved
- Why now: “All” must not resurrect rejected or duplicate work.
- Blocked by: none
- Evidence: owner feedback and existing product surfaces.
- Decision:

  | Item | Route |
  |---|---|
  | Provider handoff confirmation | Implement first (Q-R001) |
  | First-class execution hosts and SSH runner | Implement (Q-R002–Q-R007, Q-R011) |
  | Optional host-aware workspace profiles | Implement (Q-R006) |
  | Host attribution in Active Chats | Extend existing surface (Q-R008) |
  | Honest task/subtask hierarchy | Improve existing panels (Q-R009) |
  | Mobile oversight/emergency controls | Implement narrow companion after runner (Q-R010) |
  | Adapter compliance kit | Consolidate existing fixtures only when remote routing needs a shared contract; no new framework |
  | Portable ownership beyond chats | Add host/workspace metadata to the existing local bundle only after schema stabilizes |
  | Replacement capability palette | Do not build; existing slash system already serves this purpose |
  | Replacement attention inbox | Do not build; Active Chats already serves this purpose |
  | Provider comparison mode | Parking lot/website work, not this implementation |
  | Provider decision assistant | Do not build |

- Consequences: Implementation remains aligned with the target expert, provider-fluid builder.
- Revealed questions: Q-R013

### Q-R013 — What is the safe implementation order?

- Status: resolved
- Why now: Host routing cuts across persistence, process ownership, UI, and packaging.
- Blocked by: Q-R001–Q-R012
- Evidence: current architecture and risk boundaries.
- Decision: Land vertical phases; do not merge a dormant all-purpose remote framework.

  | Phase | Deliverable | Existing boundary to extend | Exit gate |
  |---|---|---|---|
  | 0 | Provider-handoff preview and confirmation | chat lifecycle + existing provider selector | Q-R014 A |
  | 1 | `ExecutionHost`, local migration, host-aware workspace profile | app models/repository + New Chat | Q-R014 B |
  | 2 | Runner protocol and fake local runner | existing process/ACP/terminal lifecycle | Q-R014 C |
  | 3 | SSH discovery, previewed bootstrap, handshake, uninstall | settings/CEF bridge + packaged runner artifacts | Q-R014 D |
  | 4 | Persistent jobs, journal, reconnect, exactly-once fold | ACP/terminal polling and state reconciliation | Q-R014 E |
  | 5 | Remote Git/files/attachments and host-aware activity | existing workspace, diff, VCS, Active Chats | Q-R014 F |
  | 6 | Honest task/subtask refinements | existing subagent panels | Q-R014 G |
  | 7 | Responsive private-network companion | existing state/notification/action model | Q-R014 H |
  | 8 | Bundle portability metadata and release hardening | existing local bundle + CI/release workflows | Q-R014 I |

  Each phase is separately shippable or revertible. `progress.md` is appended only during actual
  implementation, after each completed evidence gate.
- Consequences: Local behavior stays usable throughout; remote execution does not ship before its
  lifecycle and recovery proofs exist.
- Revealed questions: Q-R014

### Q-R014 — How is each phase verified safely?

- Status: resolved
- Why now: The user requires direct real-product proof, not test-count confidence.
- Blocked by: Q-R013
- Evidence: existing Vitest/C++ harnesses, macOS/Windows CI packaging, packaged smoke scripts, and
  Computer Use requirements.
- Decision: A phase is verified only after the applicable layers below agree.

  #### Common evidence ladder

  1. **Pure contract tests:** serialization/migration, command construction, protocol framing,
     bounds, idempotency, state transitions, and UI reducers/components. Gemini uses only this layer
     plus fake executables on the local machine.
  2. **Hostile fixture tests:** fake SSH and provider executables produce partial frames, malformed
     JSON, Unicode and spaced paths, output floods, exit races, delayed children, permission prompts,
     crashes, and spoofed control text. No network or provider account is involved.
  3. **Disposable-host tests:** ephemeral VM/container where appropriate, a dedicated unprivileged
     user, throwaway SSH key, isolated network, and fake providers. Exercise install/update/rollback/
     uninstall, reboot, controller death, SSH loss, full disk, permission denial, version mismatch,
     host-key change, and cleanup. Snapshot/recreate after each destructive scenario.
  4. **Packaged Computer Use journey:** launch the built application with a fresh isolated
     `UAM_DATA_DIR`; use accessibility-tree targets and screenshots; re-read UI state after every
     action; exercise cancel, keyboard, focus, resize, restart, and recovery. Computer Use never
     clicks an install/service/network confirmation unless the user has confirmed that exact action.
  5. **Platform matrix:** macOS ARM/Intel and Windows x64 remain release gates; Linux x64/ARM runner
     artifacts gain headless tests. Claims name the platform actually executed.
  6. **Opt-in physical-host acceptance:** only after disposable-host gates pass, use dedicated test
     workspaces on the owner's real machines. Start with fake providers; any real provider launch,
     login, prompt, quota use, or Gemini remote run needs explicit authorization.
  7. **Evidence record:** append exact commit, artifact hash, host/OS, commands, test results,
     Computer Use screenshots, observed state, and cleanup result to `progress.md`. A failed or
     unexecuted platform is written as such, never inferred green.

  #### Phase acceptance matrix

  | Gate | Automated proof | Fault proof | Packaged Computer Use proof |
  |---|---|---|---|
  | A — handoff | Preview fields match the actual post-switch diff; same-provider is a no-op | Cancel and failed save mutate nothing | Open provider picker; inspect retained/reset copy; cancel; accept; verify messages remain and destination defaults change |
  | B — host model | Old JSON migrates to `local`; round trip preserves host/profile; precedence table tested | Missing/deleted host never falls back locally | Create local and remote profiles; override defaults; restart; verify selected host/path persist |
  | C — protocol | Framing, limits, sequence, nonce, typed argv, protocol mismatch | Truncated/malformed/flood/replay/injection inputs fail bounded and attributable | Run a fake local job and verify terminal plus structured events render identically to current local behavior |
  | D — bootstrap | Artifact selection and SHA-256 manifest; install-plan serialization | Cancel, checksum failure, no space, permission failure, unknown/changed host key leave current install untouched | Add host; inspect preview; cancel; with confirmation install to disposable host; verify ready/version; uninstall and verify clean removal |
  | E — persistence | Journal ack/compaction and idempotent fold | Kill UI/controller, drop SSH, restart runner/host, duplicate last events, cancel descendant tree | Start fake long job; close UAM; reconnect; observe uninterrupted job and exactly-once output; stop and verify all descendants exit |
  | F — workspace/activity | Remote path validation, containment, upload hash/size, host counts | Traversal, symlink escape, missing repo, disconnect during transfer, local-path collision | Create remote chat; inspect host badge/Active Chats; diff a disposable remote repo; upload approved test file; verify local repo untouched |
  | G — subtasks | Child-ID and no-ID rendering contracts | Missing child, late/out-of-order event, provider crash | Observe transcript-capable child and opaque provider event; verify UI never invents unavailable chat |
  | H — companion | Auth token, revocation, origin/CSRF, authorization and audit tests | Expired/replayed token, network loss, concurrent desktop/mobile action | From controlled browser viewport: pair device, inspect task, answer fake prompt, stop fake task, revoke token, prove subsequent denial |
  | I — portability/release | Bundle schema, backward compatibility, SBOM/checksum manifest | Import unsupported host metadata, downgrade attempt, corrupt runner archive | Export/import test data; inspect host remapping prompt; run packaged upgrade then rollback; verify old job completion |

  #### Computer Use run discipline

  - Use a dedicated test data root and unmistakable fixture names such as `UAM SAFE TEST`; never use
    an existing user chat or repository.
  - Before each journey, record the packaged app hash, data-root path, disposable host snapshot, and
    expected cleanup.
  - Read the accessibility tree before every interaction and again after it. Use stable accessible
    roles/names instead of coordinates; screenshots are supporting visual evidence.
  - Split mixed-risk journeys: perform discovery and preview first, stop at the confirmation boundary,
    then obtain approval before persistent installation, remote process launch, or network exposure.
  - Validate outcome in three places: visible UI, canonical local JSON/revision, and runner job/journal
    state. A UI toast alone is not proof.
  - End every journey by stopping fixture jobs, uninstalling the disposable runner if the scenario
    requires it, removing the throwaway key, and verifying no provider or child process remains.

- Consequences: “Verified” has a reproducible meaning stronger than “tests passed,” while the plan
  still distinguishes evidence from absolute certainty.
- Revealed questions: none

## Route

1. Approve or amend this plan; no implementation begins before that decision.
2. Implement Phase 0 only, append its evidence to `progress.md`, and present the packaged Computer
   Use result for review.
3. Continue one vertical phase at a time. Do not begin the next phase while its predecessor has a
   failed acceptance gate or unclean test-host state.
4. Before Phase 3, record the actual target OS/architecture matrix and obtain action-time approval
   for the first disposable runner installation.
5. Before Phase 6+, reassess whether real usage justifies grouping/filtering or whether the existing
   surface remains sufficient.

## ROUTE_CLEAR

The destination, trust boundary, minimum architecture, exclusions, phase order, and evidence gates
are precise enough to implement. Remaining Fog affects optional physical-host coverage and later
notification delivery, not the first implementation action.
