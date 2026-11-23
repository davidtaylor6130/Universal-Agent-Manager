# V5.2.x scoping review: smallest shippable first slices

Read-only scoping of the V5.2.x milestone issues. Base: `origin/uam/v5.2-sync-main` at `6f235eb9`. All paths are relative to the repository root on that base. Issue text and every comment were read through the public GitHub API on 2026-11-23. No source, issue or PR was changed; this file is the only addition.

Evidence came from four parallel read-only research passes, with the key claims spot-checked directly against the code: `ChatView.tsx:3794`, `compute_pool_gate.cpp:92`, `acp_session_lifecycle.cpp:4379`, `vcs_provider_declaration.h:99-107`, `chat_branch_handlers.cpp:210-216`, `uam_control_service.cpp:396-407,631,1480-1488`, `vcs_commit_service.h:177-178`, `docs/settings/compute-pools.md:5,11`, `docs/documentation-review.md:136,266`, and the Gemini CLI 0.70.0 ACP `CommandHandler` registry.

## Cross-cutting constraints

- **Shared file for UAM-Control tools.** Every new UAM-Control tool (#963–#967, and the #514 PR action) edits `src/app/uam_control_service.cpp` in up to seven places:
  - schema helpers `ToolSchema`/`MutationToolSchema` (:2076-2090);
  - the MCP `readOnlyHint` list (:2174-2178);
  - `ReadOnlyTool()` (:629-631), which skips the rollback copy and replies through `ReadOnlyToolReply` (:647);
  - `IsMutation()` (:457), enforced by `MutationsAllowed` (:486) in `ExecuteTool` (:1474-1491);
  - `IsCardMethod()` (:378);
  - `ValidToolArguments()` (:388+);
  - dispatch (:1987-2003 and `ExecuteTool`).
- **Shared tests.** New tools also change the exact readOnlyHint assertion in `tests/core_agent_control_tests.cpp:1165-1190`, the per-context tool matrix in `tests/interaction_performance_tests.cpp:1991-1999`, and `docs/providers/uam-tools.md` (:17 readOnlyHint list, :19 hiding rules). These slices should run one after another, or be done by one agent.
- **One pending approval per chat.** `QueueApproval` (`uam_control_service.cpp:1210-1237`, limit at :1219-1223) allows only one approval to wait per chat. New approval-card tools compete with each other and with `goal_create`/`chat_create`.
- **No dedicated approval card UI.** `PendingApprovalForChat` (:2740) turns an approval into an Allow/Deny `AcpPendingUserInputState`, serialized as `pendingUserInput` (`src/cef/state_serializer.cpp:1871`) and resolved through `ResolveApproval` (:2802) via `src/cef/acp_handlers.cpp:1647`. The question card renders it. Any preview body (skill text, a commit file list) is new UI, and so needs Claude review and David's approval under AGENTS.md.
- **Logging.** Skill or memory bodies must not reach the diagnostic log (`docs/engineering/logging.md:5`).
- **Docs gate.** Every slice updates its owning page and runs `python3 scripts/check_docs.py`.

### Existing UAM-Control tools (18)

Defined in `Tools()` at `uam_control_service.cpp:2091-2181`; the server identifies itself as `uam-control` (:2446).

| Group | Tools |
|---|---|
| HTML views | `html_view_preview`, `html_view_publish` |
| Agents | `agent_wait`, `agent_list`, `agent_status`, `agent_delegate`, `agent_cancel` |
| Skills | `skill_list`, `skill_read` |
| Goals and chats | `goal_get`, `goal_create`, `chat_create` |
| PR watch | `pr_watch`, `pr_unwatch` |
| User interaction | `user_question`, `secret_request`, `computer_use_request` |

### Caller roles and limits

| Role or rule | Where it is decided |
|---|---|
| Read-only caller: Plan, `read_only_at_launch`, goal review turn, or workspace access other than `write` | `ReadOnlyAgent()` :463 |
| Goal reviewer | `GoalReviewer()` :478; it gets no tools at all |
| Sub-agent | `capability.agent_run_id` is set |
| Side chat | `chat.temporary_parent_chat_id` is set |
| Tools hidden at launch | `UnusableTools()` :2214-2244; passed through `HideTools` :2197; calls to a hidden tool are refused at :2461 |
| Per-tool refusals (examples) | `ChatCreateRefusal` :1024-1036, `PullRequestWatch` :1445-1452 |
| Inherited ceilings | A delegated child cannot exceed its own or any ancestor's read ceiling; `chat_create` copies the caller's permission modes (`docs/providers/uam-tools.md:9,11`) |
| Approval needed | `needs_approval` :1985 applies only to `goal_create`/`chat_create` outside YOLO |

---

## #963 Agents create skills, agents and memories

**Issue:** add `skill_create`/`skill_update`, `agent_define` and `memory_save`/`memory_search`. Every creation is user-approved. Read-only callers, goal reviewers and side chats cannot create, and inherited ceilings apply. The issue has no comments.

**What exists**
- `skill_list` reads central skill files or Markdown-store entries filtered by `AllowedSkillIds` (`uam_control_service.cpp:760-788`). `skill_read` also exists.
- Services that can already write:
  - `MarkdownStoreService::CreateEntry/UpdateEntry` (`src/app/markdown_store_service.h:85-86`);
  - `AgentDefinitionService::SaveGlobalAgent` (`src/app/agent_definition_service.h:96`), global scope only;
  - `MemoryLibraryService::ResolveScope/ListEntries/CreateEntry` (`src/app/memory_library_service.h:14-17`), with a draft state at `src/common/memory/memory_library_store.h:47`.

**What is missing**
- A memory search API. Recall scoring is private (`RecallTerms/RecallScore`, `src/app/memory_service.cpp:1761-1783`).
- Workspace-scoped agent definitions.
- A check that a defined agent is never broader than the chat that created it.
- An approval-card kind with a content preview.

**First slice: read-only `memory_search`**
- Acceptance criteria:
  - Results come only from the calling chat's resolved global and workspace memory scopes, ranked by the existing recall scoring, which becomes public.
  - The tool is in the readOnlyHint list and `ReadOnlyTool()`, so it is available in Plan and to read-only callers.
  - Memory text never goes to the diagnostic log.
  - The response stays within the tool response size limit.
  - Tests: the readOnlyHint assertion, scope isolation (another workspace's memory is never returned), and truncation.
  - Docs: `docs/providers/uam-tools.md` and `docs/memory/memory.md` are updated.
- Files: `src/app/uam_control_service.cpp`, `src/app/memory_service.{h,cpp}`, `tests/core_agent_control_tests.cpp`, `tests/interaction_performance_tests.cpp`, the docs above.
- Risk: low.
- Product-owner decision: none for this slice. Before `memory_save`, `skill_create` and `agent_define`:
  - "Does a skill an agent creates become usable by that agent straight away (added to its allowed skill ids)?"
  - "`SaveGlobalAgent` only writes global definitions. Should `agent_define` default to workspace scope as the issue says, which means adding workspace-scoped definitions?"
  - "How should 'never broader than the creating chat' be checked: tool set, workspace access, or both?"
- Later order: `memory_save` (card using `IsCardMethod` + `QueueApproval` + a new pending kind; hidden in `UnusableTools` for read-only callers, reviewers and side chats), then `skill_create`/`skill_update`, then `agent_define`.
- Dependencies: none.
- Size: **S**.

## #964 Workspace and git tools

**Issue:** add `worktree_status` (read-only, replacing the reviewer's shell workaround), `commit_propose` (card) and `pr_create`/`pr_comment` (card, using the user's `gh` sign-in so the agent never sees a token). Reuse the hardened git builder, respect read ceilings, and never expose credentials. The issue has no comments.

**What exists**
- Hardened git builder: `uam::vcs::HardenedGitArgv`/`HardenedGitShellCommandInDirectory` (`src/common/vcs/git_command.h:40,115-121`).
- `GitWorktreeService::Status` (`src/app/git_worktree_service.h:158`, struct at :31) returns branch, base and dirty flags.
- `VcsCommitService::Status/Diff/Commit` (`src/app/vcs_commit_service.h:173-179`; `Status` at :177, `Diff` at :178) already covers Git and SVN, and is a ready backend for both `worktree_status` and `commit_propose`.
- The goal reviewer is told to use shell git in its prompt text (`src/app/goal_service.cpp:1008`).
- The only `gh` code is `PrWatchService::RunGh` (`src/app/pr_watch_service.cpp:32-38`). It is read-only, uses `gh api`, has a 30 s timeout, and is local to that file. Agents are blocked from running `gh` by the network-command list (`src/common/security/command_safety.h:158`).

**What is missing**
- Any tool exposing status or diff.
- A card that previews a file list.
- A `gh` runner that can write.

**First slice: read-only `worktree_status`**
- Acceptance criteria:
  - Returns the branch, the changed files and a diff summary for the calling chat's own workspace or worktree, through `VcsCommitService::Status` (Git and SVN).
  - Read-only and hinted.
  - Output is truncated with a marker under the 64 KiB response limit (`uam_control_service.cpp:2072`).
  - The goal reviewer prompt at `goal_service.cpp:1008` points to the tool instead of shell git. This only works if the reviewer is allowed this one tool; today it gets none, so that is a deliberate change.
  - Tests: a Git workspace, an SVN workspace, truncation, and the readOnlyHint list.
- Files: `uam_control_service.cpp`, `goal_service.cpp`, `tests/core_agent_control_tests.cpp`, `tests/interaction_performance_tests.cpp`, `docs/providers/uam-tools.md`, `docs/chat/goals.md`.
- Risk: low to medium. SSH-host workspaces go through the remote path in `VcsCommitService`. Diff size is a second risk.
- Product-owner decision: none for the tool. If the reviewer prompt changes: "May goal reviewers receive the single read-only `worktree_status` tool?"
- Later order:
  - `commit_propose`: card calls `VcsCommitService::Commit(message, files)`, after a preview-card UI design.
  - `pr_create`/`pr_comment`: merge with #514's `pr_open`.
- Dependencies: none for this slice. The PR tools are shared with #514.
- Size: **S**.

## #965 Background tasks, notifications and scheduling

**Issue:** add `task_run`/`task_status` (background command with progress and a completion wake), `notify` (desktop or phone) and `schedule` (follow up later). Resources are bounded, commands outside the chat's safety tier need approval, and tasks are owned by the chat and cancelled with it. The issue has no comments.

**What exists**
- No wall-clock scheduler. `src/app/agent_run_scheduler.h` handles managed sub-agent runs only.
- Closest template: PR watch. It keeps a saved `pr-watches.json`, runs a poll worker, and wakes the chat with `SendWake` → `SendAcpPrompt` (`src/app/pr_watch_service.cpp:40-43`, `acp_session_runtime.h:44`).
- Periodic hook: `Application::PollTick()` (`src/app/application.cpp:430`), rescheduled with `CefPostDelayedTask` (:653).
- Phone notifications come from the browser reacting to a chat's `attentionKind` (`UI-V2/src/components/companion/useCompanionAlerts.ts:41-55`). Bell rules are in `docs/ui/notifications.md`. There is no server-side push.

**What is missing**
- A timer service.
- A background process supervisor.
- A notify path an agent can trigger.

**First slice: `schedule` (a one-shot wake of the calling chat)**
- Acceptance criteria:
  - An agent can ask to be woken after N minutes (between 1 minute and 24 hours) with a short note.
  - The timer is saved and survives an app restart; a timer whose time has passed fires once after restart.
  - Stop, Done or deleting the chat cancels it.
  - Limit of a few pending timers per chat.
  - Hidden for sub-agents, side chats and goal reviewers.
  - Delivery reuses the PR-watch wake path.
  - Tests with an injected clock cover firing, cancellation, restart and the hiding rules.
- Files: a new small wake service, or `pr_watch_service` generalised into one; `uam_control_service.cpp`; `application.cpp`; tests; `docs/providers/uam-tools.md`.
- Risk: low to medium.
- Product-owner decision: none for `schedule`. Before `notify`: "Should `notify` simply raise the chat's attention flag (respecting bell rules), or be a separate push that can bypass them?" `task_run` needs a process-supervision service plus safety-tier approval, and is L.
- Dependencies: the timer code can be shared with #518.
- Size: **S–M**.

## #966 Messages and hand-offs between chats

**Issue:** add `chat_message` (a note to another chat the user approved) and `handoff` (pass the task to another chat, provider or model with a summary). Cross-chat actions need approval, receivers keep their own ceilings, and nothing escalates through messages. The issue has no comments.

**What exists**
- `created_by_chat_id` records which chat started another; it is provenance only and grants no authority.
- The only ways to put text into another chat are `chat_create`'s first prompt and PR-watch wakes.

**What is missing**
- Any messaging tool, and any hand-off summary logic.

**First slice: `chat_message` behind an approval card**
- Acceptance criteria:
  - Targets are limited to chats the caller started, or the chat that started the caller.
  - Each message needs user approval, then arrives in the target as a wake prompt that visibly names the sender chat.
  - The receiver keeps its own permission modes and tools.
  - Refused for read-only callers, side chats and goal reviewers.
  - Tests:
    - targeting an unrelated chat is refused;
    - permissions are unchanged after delivery;
    - denying the card delivers nothing;
    - the sender's name appears in the delivered text.
- Files: `uam_control_service.cpp`, the wake path in `acp_session_runtime`, tests, `docs/providers/uam-tools.md`.
- Weakness to design for: text from one chat becomes another chat's input, so a compromised or confused chat could try to steer a more privileged one. Fix: always label the sender, keep the receiver's own ceilings, require approval per message, and limit targets to related chats.
- Risk: medium.
- Product-owner decision: "Is limiting targets to parent and child chats acceptable for the first release? For `handoff`, who writes the summary, and may a hand-off change provider or model?"
- Dependencies: none. Do not run in parallel with other approval-card tools.
- Size: **M**.

## #967 Usage and diagnostic insight for agents

**Issue:** add `usage_report` (the chat's token and cost use against the user's limits) and `log_tail` (read-only diagnostic log for the calling chat). Only the calling chat's own data; never other chats' content or secrets. The issue has no comments.

**What exists**
- Per-message `tokens_input`, `tokens_output` and `estimated_cost_usd` (`src/common/models/app_models.h:376-378`).
- Account limits from `src/common/usage/account_allowance_service.h:23`.
- `usage_ledger.h`, keyed by provider session rather than chat.
- The diagnostic log is free text from a single `Write(string)` (`src/common/utils/diagnostic_log.h:23`) with no chat id field.

**What is missing**
- Any agent-facing usage tool.
- Structured chat ids in log lines. Filtering by matching text like "chat <id>" would be unreliable, because some lines name two chats.

**First slice: read-only `usage_report`**
- Acceptance criteria:
  - Returns the calling chat's total tokens and estimated cost, plus a summary of the account allowance cache.
  - Cost is labelled as an estimate.
  - Only the calling chat's data.
  - Read-only and hinted.
  - Tests: correct totals, other chats excluded, and the readOnlyHint list.
- Files: `uam_control_service.cpp`, `tests/core_agent_control_tests.cpp`, `tests/interaction_performance_tests.cpp`, `docs/providers/uam-tools.md`.
- Risk: low.
- Product-owner decision: none.
- Why `log_tail` is deferred: without a structured chat id, a per-chat filter could leak other chats' lines. The fix is to add a chat id field to log records first, then filter on that field only.
- Dependencies: none.
- Size: **S**.

## #518 Scheduled and webhook automations

**Issue acceptance:**
- An automation is a stored automation-API request with the same tokens, trust levels, git permission, worktree isolation and budget.
- Webhooks insert values from the request body and support optional signature checks.
- Settings can list, pause, edit and delete automations, showing last and next run.
- Missed runs are skipped or run once (chosen per automation), and nothing runs twice.

**Owner comment (2026-11-13):** the automation API for remote chat requests shipped in V5.1 (#821). Still to do: scheduled runs and webhook triggers.

**What exists**
- `src/app/automation_api_service.{h,cpp}` (430 lines) handles `POST /automation/chats` and `GET /automation/chats/<id>`.
  - `Handle()` (:295) checks the token hash and expiry (:309), then calls `CreateChat()` (:201-250).
  - `CreateChat()` enforces the workspace list, git rank, the 16-open-chats limit, worktree isolation (:230), provider availability, and refusal of Antigravity (:233).
  - `BeginLaunch`/`AbandonLaunch` are at :389/:418.
  - Tokens are kept only as SHA-256 hashes in `data_root/automation_tokens.json` (:64), expiring after 30 days, 90 days or 1 year.
- HTTP path: requests are routed inside the phone companion server (`src/cef/companion_server.cpp:717-719`; `HandleAutomationRequest` :790-826, Bearer auth, refuses any `Origin`, 256 KiB body), then sent to the UI thread and handled by `src/cef/automation_handlers.cpp:60` (`HandleAutomationHttpRequest` → `LaunchAutomationChat`). The API is reachable only while Phone access is on.
- "Budget" today is `maxIterations` (1–200), the goal-loop cap. A token budget was never built (`docs/documentation-review.md:244`).
- No wall-clock scheduler; `Application::PollTick()` (`application.cpp:430`) is the only periodic hook.
- Docs:
  - `docs/automation-api.md` says "Status is polling only: UAM makes no outbound callbacks or webhooks." That excludes outbound calls only, so inbound webhooks do not contradict it.
  - `docs/settings/automation.md` covers only the token list.
- Tests: `tests/automation_api_tests.cpp` (29 cases), `tests/automation_api_integration.py`, `UI-V2/src/components/settings/AutomationSettings.test.tsx`.

**What is missing**
- Stored schedules, a timer, webhook routes, value insertion, signature checks, a missed-run policy, and the Settings UI for all of these.

**First slice: scheduled automations only**
- Acceptance criteria:
  - A new `AutomationScheduleService` stores records in `data_root/automation_schedules.json`. Each record holds:
    - the token reference (id or hash) and the same body as `POST /automation/chats`;
    - the trigger: an interval, or a time of day on chosen weekdays;
    - `paused` and `missed_policy` (skip or run_once);
    - `last_run_at` and `next_run_at`.
  - `PollTick` asks for due runs. Each run goes through `Handle()` with the stored token, so trust, git ceiling, worktree isolation, the iteration cap, revocation and expiry all apply unchanged.
  - `last_run_at` is saved before launching, which guarantees a run never repeats.
  - Settings (`AutomationSettings.tsx`) lists schedules and can pause, edit and delete them, showing last and next run.
  - Native tests with an injected clock:
    - an interval run and a weekday-time run;
    - a paused schedule does not run;
    - after a simulated restart with 3 missed slots, skip runs 0 times and run_once runs exactly once;
    - a crash between saving and launching does not cause a second run;
    - a revoked or expired token records a failure and creates no chat.
  - A Vitest covers the Settings list.
  - Docs: `automation-api.md`, `settings/automation.md` and `documentation-review.md` are updated.
- Files: new `src/app/automation_schedule_service.{h,cpp}`, `src/app/application.cpp`, `src/cef/automation_handlers.cpp` (plus query handler registration), `UI-V2/src/components/settings/AutomationSettings.tsx`, new `tests/automation_schedule_tests.cpp`, `CMakeLists.txt`.
- Risk: medium.
  - Time zone and daylight-saving rules for time-of-day triggers.
  - The 16-open-chats limit fills quickly.
  - The Settings change needs Claude review and David's approval.
- Product-owner decisions needed first:
  1. "Should schedules run while Phone access is off? A schedule needs no listener, but today the automation API is only on with Phone access."
  2. "An automation stops when its token expires (one year at most). Is that acceptable, or should schedules use a non-expiring internal credential?"
  3. "Is starting a new chat enough for the first release, or is 'send a message to an existing chat' required?"
- Second slice: webhooks (M).
  - A `POST /automation/hooks/<id>` route on the companion server, with values from the request body inserted into the stored prompt.
  - Optional signature check (HMAC). The weakness here is secret storage: an HMAC check needs the signing secret in a recoverable form, which breaks the current hashes-only rule. Fix options are the OS keychain, or an explicitly agreed encrypted-at-rest file.
  - Inserted body values are untrusted input. They should be clearly marked as quoted data in the prompt and limited in size.
  - Question: "May UAM store webhook signing secrets in a recoverable form (keychain where available)?"
- Dependencies: none hard. Automation chats already pass through the compute-pool gate (#524).
- Size: **M**.

## #514 PR, branch and GitHub workflows

**Issue acceptance:**
- Design agreed with David before any GUI implementation.
- Works with the existing workspace, worktree and branch controls; no hidden git or GitHub writes.
- Auto-settling can be turned off and never hides a chat with pending work.

**Owner comment (2026-11-16):** #907 shipped PR watching (linked PR rows, check state, waking on CI, review or conflict changes) and shows the chat's branch. Still missing: opening or updating a PR from the chat, auto-settling with an off switch, and design sign-off.

**What exists**
- `src/app/pr_watch_service.{h,cpp}` and `src/common/pr_watch/pr_watch_rules.*`.
- `RunGh` (`pr_watch_service.cpp:32-38`): local to that file, read-only, 30 s timeout.
- Docs: `docs/chat/pull-request-watch.md`, marked "Needs verification"; interim choices at `docs/documentation-review.md:319+`.
- Agents are blocked from running `gh` (`src/common/security/command_safety.h:158`).
- Settling: only the sidebar's inactivity drop `ACTIVE_CHAT_AUTO_DONE_MS` (3 days, `UI-V2/src/components/sidebar/chatSearch.ts:79-83`). It is already under an open decision (`docs/documentation-review.md:266`, "Active Chats inactivity settling", #629), which asks that chats leave Active only through Done, Clear all or Snooze.

**What is missing**
- Any `gh pr create`/`edit` code.
- Auto-settling with an off switch.
- The GUI design.

**First slice: `pr_open`, a UAM tool behind an approval card, with no new GUI (shared with #964's `pr_create`)**
- Acceptance criteria:
  - With approval, the tool runs `gh pr create --head <chat branch>`. If `gh pr list --head` finds an existing PR for the branch, it runs `gh pr edit` with the title and body instead.
  - It then links and watches the PR, reusing the existing PR row.
  - It never pushes; the branch must already be on the remote.
  - It runs with the user's existing `gh` sign-in, and no token is ever returned to the model.
  - Refused in sub-agent and side chats.
  - Tests with a stand-in `gh` executable check the command line, the create-versus-update choice, refusal without approval, and automatic watching.
- Files: `pr_watch_service.cpp` (a general `gh` runner), `uam_control_service.cpp`, tests, `docs/chat/pull-request-watch.md`, `docs/providers/uam-tools.md`.
- Risk: medium. It writes to GitHub; the "no hidden writes" rule is met by approving each write.
- Product-owner decisions needed first:
  - "Before the PR GUI design, may agents open or update a PR through an approval-gated tool that then watches it?"
  - "Should auto-settling wait for your #629 Active Chats decision?"
- Dependencies: #964 (same slice); auto-settling depends on #629.
- Size: **M**.

## #520 Branching for every VCS

**Issue acceptance:**
- SVN branches are created with `svn copy` into the branches location and switched with `svn switch`.
- Trunk, branches and tags locations are set per repository in Settings, defaulting to the standard layout.
- Composer, new-chat selector and sidebar behave the same for Git and SVN.
- Anything unsupported is shown greyed out with the reason.
- Default selection follows recency.
- Real-SVN tests run against a local `file://` repository; Git tests are unchanged.

**Owner comment (2026-11-13):** switching to existing SVN branches shipped in V5.1 (#914). Still to do: creating and merging branches in-app for every VCS.

**What exists**
- #519 (the VCS interface) is **open**. Landed:
  - `FindNearestVcsRoot` (#795);
  - `VcsProvider` (#805) for status, diff and commit with declared capabilities, later adding History, Update, RevisionDiff and Origin (`src/app/vcs_provider.h:28-63`, `src/common/vcs/vcs_provider_declaration.h`; capability enum at :76-91).
- The interface has no branch methods; `docs/architecture/vcs-boundaries.md:5` says the other capabilities "are declared but not yet read".
- Capability lists:
  - `kGitCapabilities` (`vcs_provider_declaration.h:99-101`) includes BranchCreate.
  - `kSvnCapabilities` (:104-107) has BranchList and BranchSwitch but **not BranchCreate**.
- SVN branch service: `src/app/svn_branch_service.{h,cpp}`.
  - `DiscoverSvnBranches` (:253) and `SwitchSvnBranch` (:258).
  - The `svn switch` command (:281) is pinned to a revision with an ancestry check.
  - Commands run through an injectable `SvnBranchRunner`.
- Branch handlers in `src/cef/chat_branch_handlers.cpp`:
  - list :168-199, switch :201+;
  - SVN create is refused with "SVN selection does not create repository folders." (:214, again at :331).
- Tests:
  - `tests/svn_branch_service_tests.cpp` (fake runner).
  - `tests/svn_branch_integration.py` (real `file://` repository), registered in `tests/svn-branch/CMakeLists.txt` only when `svn`/`svnadmin` exist. It is not on hosted CI; no workflow installs subversion, and `svn` is not installed in this environment.
- SVN layout settings: `src/common/config/svn_repository_layout.h`; saved at `settings_store.cpp:73,706,1042`; validated at `settings_handlers.cpp:1587-1643`; edited in `UI-V2/src/components/settings/VcsSettingsSection.tsx`. Layouts are display-only: only `VcsHistory.tsx` reads them, and `docs/settings/version-control/svn.md` says "nothing is inferred for an action".
- Branch pickers:
  - Composer: `UI-V2/src/components/chat/ChatBranchPicker.tsx` (Git/SVN modes).
  - New chat: `UI-V2/src/components/sidebar/NewChatBranchPicker.tsx:20`, which returns an empty list for SVN.
  - Recency ranking: `UI-V2/src/utils/branchRecency.ts`.
  - Owning doc: `docs/chat/composer/workspace-actions.md`.
- `VcsType::Git` is still hard-coded at `src/cef/vcs_handlers.cpp:903`.
- The UI hides the picker for unsupported systems instead of greying it out.
- SVN branch selection decision: `docs/documentation-review.md:5-7`.

**What is missing**
- SVN branch creation, merge for every VCS, layout-driven actions, the SVN new-chat picker, greyed-out reasons, and branch methods on the VCS interface.

**First slice: SVN branch creation (native code and bridge only)**
- Acceptance criteria:
  - `CreateSvnBranch` runs a server-side copy of the current URL at a pinned revision to `^/<branches>/<name>`, with a message.
  - The branches location is read natively from `svn_repository_layouts`, matched by root URL or UUID, defaulting to `branches`.
  - Branch names are validated; creation is refused if the target exists.
  - `kSvnCapabilities` gains BranchCreate, and the create refusal at `chat_branch_handlers.cpp:214/331` is removed.
  - Optionally, the chat switches to the new branch through the existing guarded switch.
  - Tests: the fake-runner command line; default and overridden layouts; the existing-target refusal; create and switch added to `svn_branch_integration.py` against `file://`. Git tests unchanged.
  - Docs: the "display only" sentence in `svn.md` and `workspace-actions.md` are updated.
- Out of scope: merge, SVN new-chat parity, greying out (all need UI).
- Files: `svn_branch_service.{h,cpp}`, `vcs_provider_declaration.h`, `chat_branch_handlers.cpp`, `tests/svn_branch_service_tests.cpp`, `tests/svn_branch_integration.py`, the docs above.
- Risk: medium.
  - Creating a branch is a real server commit.
  - The layout setting starts driving actions.
  - The session must install `subversion` to run the real test.
- Product-owner decision needed first: "Should a new SVN branch copy from the working copy's current URL and revision, or from trunk HEAD? Is a commit message written by UAM OK?"
- Dependencies: the #519 remainder (ideally a branch method on `VcsProvider`); it can land in `SvnBranchService` first.
- Size: **M**.

## #521 Working with SVN through Git

**Issue acceptance:**
- A managed Git mirror of an SVN checkout that respects `svn:ignore`.
- New Git branches can be published as SVN branches.
- Git commits are published as SVN commits in order, with their messages, clear conflict and out-of-date errors, and nothing lost on failure.
- Pulling SVN changes is explicit and safe while chats run.
- Agents see an ordinary Git repository.
- `file://` tests cover branch publish, a conflict and ignore handling.

**Owner comment (2026-11-13):** not started.

**What exists**
- SVN-to-Git snapshot isolation in `src/app/git_worktree_service.cpp:1649` (`is_svn_workspace`) and `:1810` (`ManagedSnapshotDisabledReason`); doc at `docs/settings/version-control/local-snapshots.md`.
- Worktrees are not behind the VCS interface (`git_worktree_service.h:153-174`).

**What is missing**
- Everything in the acceptance list.

**First slice: a design note and throwaway spike, not product code**
- It should decide:
  - `git svn` (needs Perl SVN bindings, often missing on Windows and macOS) or a mirror UAM owns, built on the managed snapshot;
  - how `svn:ignore` maps to Git excludes;
  - how commits are replayed and how that recovers on conflicts and out-of-date errors;
  - the pull model while chats run.
- It ends with a throwaway prototype script against a local `file://` repository and its results.
- Risk: low for the spike. High for the feature, because publishing could lose or reorder commits.
- Product-owner decision needed first: "Is depending on `git svn` acceptable, or must UAM own the mirror? Is pulling SVN changes into the mirror always explicit and one-way?"
- Dependencies: #519 (worktree, isolation and branch methods), #520 (SVN branch creation).
- Size: **S** for the spike; **L+** for the feature.

## #522 Extension recipes (custom AI providers and VCS)

**Issue acceptance:**
- Extensions run as separate processes speaking a protocol (decided 2026-10-25: no DLL plugins).
- Custom AI providers use generic ACP; custom VCS uses JSON over stdio shaped like ACP, with declared action buttons.
- Each recipe ships a template, a docs page and a conformance test.
- A sample read-only VCS adapter passes and shows its button.
- A sample custom ACP agent works in Chat.
- A crashing or misbehaving adapter cannot crash or hang UAM.

**Owner comment (2026-11-13):** not started; depends on #519 and #523.

**What exists**
- Nothing for adapters.
- The capability model to mirror: `VcsProviderDeclaration` (`src/common/vcs/vcs_provider_declaration.h`).
- No "open in" actions exist anywhere.

**First slice: a protocol spec plus a standalone conformance harness, no UAM integration**
- Acceptance criteria:
  - A documented JSON-RPC-over-stdio protocol with a handshake that declares capabilities (mirroring `VcsProviderDeclaration`) and actions.
  - A Python conformance runner checks an adapter for crash handling, timeouts, oversized output and capability honesty (declared operations actually work; undeclared ones are refused).
  - A sample read-only folder adapter passes.
- Files: new `docs/` page, `tools/` or `tests/` conformance runner, sample adapter.
- Risk: low for this slice.
- Weakness to design for: an adapter is an external executable UAM launches, and an action button could run commands. Fix: explicit registration and trust by the user, a minimal environment, size and time limits on all output, and buttons that only open URLs or files unless David agrees otherwise.
- Product-owner decision needed first: "Where are adapters registered, and who decides an adapter executable is trusted? May action buttons only open URLs or files, or may they run commands?"
- Dependencies: #519 (branch, worktree and "open in" methods) and #523 before wiring into UAM.
- Size: **S–M**.

## #523 Generic ACP agents

**Issue acceptance:**
- A "Custom ACP agent" provider (command, arguments, environment, optional icon).
- Capabilities read from ACP `initialize`.
- Chat, streaming, permissions, cancellation, and model and mode selection work when reported; unreported features (usage, history import, terminal view, compaction) degrade and show as unavailable.
- Runs locally or on SSH hosts.
- Labelled "best effort" in Settings and in a seventh checklist column.
- Tested with a synthetic agent and one real third-party agent.

**Owner comment (2026-11-13):** not started.

**What exists**
- The provider set is fixed at compile time:
  - `src/common/provider/provider_ids.h:16-44` (six flag-gated ids in `kAllCliProviderIds`);
  - `src/common/provider/runtime/provider_runtime_registry.cpp:88-125` (an `if` chain; unknown ids fall back to `UnsupportedProviderRuntime`, :27-78);
  - `CMakeLists.txt:149` rejects other provider flags;
  - `ProviderProfile` (`src/common/provider/provider_profile.h`) exists, but `src/app/application.cpp:890-891` loads only `BuiltInProfiles()`, noting this holds "unless profiles later become editable".
- ACP providers:
  - Gemini: `gemini --acp`, `gemini_cli_provider_runtime.cpp:591`.
  - OpenCode: `opencode-acp`, `opencode_cli_provider_runtime.h:61`.
  - Copilot: `copilot --acp --stdio`, `acp_session_lifecycle.cpp:3034`.
  - Codex (app-server), Claude (stream-json) and Antigravity (own stream-json) are not ACP; protocol ids are in `provider_profile_constants.h:16-23`.
- Shared ACP defaults in `src/common/provider/runtime/provider_acp_strategy.cpp`:
  - `OnAcpBuildInitialize` (:22-31) advertises `clientCapabilities.session.compaction`.
  - `OnAcpInitializeResult` (:33-60) already reads agentInfo, `promptCapabilities.image`, `loadSession`, `sessionCapabilities.resume` and MCP http/sse.
  - Default prompt, cancel and setup builders are at :62-120+.
  - `available_commands_update` is parsed in `acp_session_update_handler.cpp:278-289`.
- Tests inject JSON-RPC lines through `ProcessAcpLineForTests` (`acp_session_runtime.h:218`; `tests/acp_session_tests.cpp`, e.g. `GenericAcpPermissionReviewPreservesNonObjectJsonInput` at :167). No synthetic agent process exists.
- Settings: `UI-V2/src/components/settings/CentralProviderSettings.tsx`.
- Docs: `docs/providers/providers.md` has one checklist page per provider (list at :46-51), not table columns. `docs/architecture/provider-boundaries.md:5` says "six adapters".

**What is missing**
- A data-driven provider entry, a generic runtime, configuration storage, a synthetic agent, the seventh checklist page, SSH support and the "best effort" labelling.

**First slice: one "Custom ACP agent", native and local only**
- Acceptance criteria:
  - A `kCustomAcp` id behind a new `UAM_ENABLE_RUNTIME_CUSTOM_ACP` flag, with a registry branch and a `CustomAcpProviderRuntime` built on the shared ACP defaults.
  - One saved configuration: command, arguments, environment and display name.
  - Features follow strictly what `initialize` and `session/new` report (image prompts, load and resume, models and modes). Usage, history import, terminal view and compaction show as unavailable, with no hidden fallbacks.
  - A "Best effort" label in Settings and in Chat's provider picker.
  - A scripted Python ACP agent fixture runs as a real stdio process. An integration test covers initialize, a new session, a streamed prompt, a permission round-trip and cancel.
  - A bad command gives a clear launch error.
  - A new checklist page `docs/providers/custom-acp/custom-acp.md`; `providers.md` and `provider-boundaries.md` updated; `check_docs.py` passes.
  - Deferred: SSH, several custom agents, an icon, and the real third-party agent test.
- Files: `provider_ids.h`, `provider_runtime_registry.cpp`, `CMakeLists.txt`, `provider_profile.{h,cpp}`, settings persistence and serializer, `CentralProviderSettings.tsx`, new `src/common/provider/custom_acp/`, a new `tests/fixtures/` agent script, `docs/providers/*`, `docs/architecture/provider-boundaries.md`.
- Risk: medium to high.
  - About 25 `.cpp` files compare against the Gemini id alone; history adapters, the version policy and the excluded-id sentinel assume a known provider.
  - A command line the user can edit widens what UAM launches. Fix: launch with an argument array, never a shell string; keep the configuration in user settings only (never from repository or chat content); show the exact command before first launch.
- Product-owner decisions needed first:
  - "Is one custom agent enough to start?"
  - "Should its configuration live in app settings or in a per-agent profile file?"
  - "Is a seventh checklist page an acceptable reading of 'seventh column'?"
  - "Is SSH a later slice?"
  - "Which third-party agent is the reference test?"
  - "Should the build flag be on by default?"
- Dependencies: none. #522 depends on this.
- Size: **M–L** for the slice; **XL** for the whole issue.

## #524 Compute pools

**Issue acceptance:**
- With a limit of 2, a third turn waits showing "Waiting for <pool> slot (2/2 in use)" and starts by itself, without losing or reordering input.
- Stop removes a waiting turn.
- Pools are managed in Settings with live usage.
- A chat in several pools runs only when all have room.
- Native tests cover queueing, release, failure, cancel and restart recovery.

**Owner comment (2026-11-16):** done in V5.1 (#812): queueing, auto start, Stop, multi-pool admission, the Settings page with live usage, and tests. Still missing:
- a pool picker for a new chat;
- machine-wide pools;
- restart recovery and its test;
- "in use" in the status text;
- CLI input, sub-agent results, compaction and native steering counting toward a pool but never waiting.

**What exists**
- Model, validation and serialization in `src/common/config/compute_pools.h`.
- Gate in `src/common/runtime/acp/compute_pool_gate.{h,cpp}`:
  - occupancy is derived live (`Occupies` :43, skipping `agent_wait`);
  - waiting order uses tickets;
  - `AdmitTurnToComputePools` (:92) is called once, at `acp_session_lifecycle.cpp:4379`.
- Structs: `app_models.h:1089-1100`, settings at :1193; session state at `app_state.h:942`.
- Status text at `UI-V2/src/components/views/ChatView.tsx:3794` reads `Waiting for {poolName} slot ({inUse}/{limit})`, without "in use".
- Settings: `ComputePoolsSettings.tsx` (with test).
- `docs/settings/compute-pools.md`:
  - :5 — a held turn whose provider restarts rejoins at the back.
  - :11 — the picker, machine-wide pools and restoring a wait across an app restart are deferred; after restart the message stays in its chat for Retry. CLI View input is "not held". Sub-agent results, compaction and native steering bypass the gate (only the queued-prompt path calls it) but count toward use.
- `tests/compute_pool_tests.cpp` has 18 tests, none for restart recovery or the paths that count but never wait.

**Remaining items, ranked by value against size**

| Rank | Item | Value | Size | Notes |
|---|---|---|---|---|
| 1 | "in use" in the status text | High (literal acceptance wording) | XS | `ChatView.tsx:3794`, its test, docs wording |
| 2 | Tests proving compaction, sub-agent results and native steering count but never wait | High | S | Behaviour likely already correct; tests missing |
| 3 | Restart recovery and its test | Acceptance item | S (test today's behaviour) / M (auto-rejoin) | Needs a decision |
| 4 | CLI View input counting | Medium | M | A terminal has no turn boundary |
| 5 | Pool picker for a new chat | Medium | M | Chat field, new-chat UI, gate honours an explicit pool |
| 6 | Machine-wide pools | Medium | M–L | Needs a host dimension and a "whole machine" member kind |

**First slice: ranks 1–3 (with rank 3 as a test of today's behaviour)**
- Acceptance criteria:
  - The status reads "Waiting for X slot (2/2 in use)".
  - Native tests show that while a pool is full, compaction, a delivered sub-agent result and a native steer are each let through and raise the in-use count.
  - A restart test shows the held message stays in its chat and is checked again as a new turn, never sent twice.
  - `docs/settings/compute-pools.md` is updated.
- Files: `ChatView.tsx` and its test, `tests/compute_pool_tests.cpp`, `docs/settings/compute-pools.md`.
- Risk: low. The status text is a UI change, so it needs Claude review and David's approval.
- Product-owner decision: "After a restart, should a turn that was waiting for a pool slot rejoin the queue automatically, or stay in its chat for Retry as now?" If it should rejoin, that is a separate M slice.
- Dependencies: none.
- Size: **S**.

## #363 Decision model for review and gate decisions

**Issue scope:**
- One decision interface with three shapes: Choice, Score and Probability, each with a reason.
- Pluggable backends: hosted Jev, local Laya, or a chat-provider fallback, chosen in Settings.
- First uses: turn review, goal "done" and the attention flag.
- Each decision records its backend.
- Done when: those decisions go through the interface and show backend, result and reason; latency and cost are measured against the text-model path; tests catch a filter or fallback being reported as a model decision.

**Owner comment (2026-11-13):** #789 records who decided; the decision model itself is not built.

**What exists (#789)**
- Decision source constants `model:<id>`, `filter`, `fallback` and `unknown` (`src/common/models/app_models.h:461-467`).
- Goals carry `last_decision_source` (:485); chats carry `attention_source` (:797).
- `GoalService::ReviewDecision` (`src/app/goal_service.h:21`); `RecordBlocker` requires a source (:131).
- `ParseReviewDecision` (`goal_service.cpp:1094`) is used by `acp_goal_loop.cpp:484` (text-model JSON review with one repair attempt).
- Attention is always `filter` (`chat_domain_service.cpp:1207`); saved in `chat_repository.cpp:2282`.
- UI: `GoalBanner.tsx` shows Last decision and Last attention flag.
- Docs: `docs/chat/goals.md:30-34` ("Decision source", with an implementation gap note).
- Tests: `goal_tests.cpp:4373,4413` (decision source), parse tests at :2304-2367, `GoalBanner.test.tsx`.
- The permission "AI Review" (`command_safety.h:60`) is a separate feature.

**What is missing**
- The decision interface, any Jev or Laya backend, a Settings backend picker, latency and cost measurement, and a non-filter attention decision.

**First slice: backend-agnostic interface with the chat-provider fallback only**
- Acceptance criteria:
  - A `DecisionService` with `Choice(options)`, `Score(levels)` and `Probability(statement)`, each returning the result, reason or evidence, a structured backend id, latency in ms, and usage.
  - The only backend wraps the existing reviewer-model path.
  - The goal "done" decision goes through it; the structured backend id is saved per decision alongside the existing `last_decision_source` strings, which stay compatible.
  - Latency appears in goal diagnostics.
  - A test proves a filter or fallback path can never be recorded as `model:`.
  - `docs/chat/goals.md` is updated.
- Files: new `src/app/decision_service.{h,cpp}`, `acp_goal_loop.cpp`, `goal_service.{h,cpp}`, `app_models.h`, new `tests/decision_service_tests.cpp`, `docs/chat/goals.md`.
- Risk: medium, because it touches the goal loop.
- Product-owner decisions:
  - "Is 'turn review' the goal-loop review, or a separate per-turn review that doesn't exist yet?"
  - "Do you have Jev early-access credentials, and should sending goal or transcript text to a hosted service be opt-in only?"
  - Jev blocks only the hosted backend, not this slice. Laya also needs a packaging decision.
- Dependencies: none. The Settings backend picker should wait until a second backend exists.
- Size: **M**.

## #512 Manual compaction for Gemini and Antigravity (re-checked 2026-11-23)

**Issue acceptance:**
- Track each provider's upstream API; when it ships, Chat's compact action starts native compaction, shows progress and records the summary.
- Any UAM fallback is agreed with David and never loses or rewrites history.
- Until then the rows stay Partial with a reason.

**Owner comment (2026-11-13):** blocked on provider APIs.

**What exists**
- Manual compaction is a runtime hook pair, `OnAcpBuildCompact`/`SupportsAcpCompaction` (`src/common/provider/provider_runtime.h:484-487`).
  - Called from `acp_session_runtime.cpp:3703,3770`.
  - Gated in the UI at `state_serializer.cpp:1792`.
- Implemented for:
  - Codex: `thread/compact/start`, `codex_cli_provider_runtime.cpp:534`;
  - Claude: `claude_cli_provider_runtime.cpp:408`;
  - Copilot: `session.history.compact`, `copilot_native_protocol.cpp:1511`;
  - OpenCode: its own native HTTP route.
- Gemini and Antigravity implement neither hook.
- Gemini launches with `--acp`; minimum version 0.55.1 (verified 2026-08-27); only 0.55.1 and 0.64.0 qualify for native binding (`gemini_cli_provider_runtime.cpp:284-338`).
- Rows marked Partial: `docs/providers/gemini/gemini.md:22`, `docs/providers/antigravity/antigravity.md:22`.

| Open decision (`docs/documentation-review.md:136`) | Decision required |
|---|---|
| Gemini and Antigravity structured manual compaction lack a safe exposed control | Retain native automatic/TUI controls or design an agreed UAM equivalent without sending command text as an ordinary model prompt. |

**Gemini: still blocked**
- Latest on npm: 0.70.0 (2026-11-19) and 0.71.0-nightly.20261123.
- In both packages, the ACP `CommandHandler` registers only Memory, Extensions, Init, Restore, About and Help. Confirmed directly in the 0.70.0 bundle at `CommandHandler.createRegistry`.
- `/compress` is not offered over ACP, and the ACP prompt loop handles no `ChatCompressed` event.
- Release notes for 0.68–0.71 mention ACP fixes but nothing about compression (https://github.com/google-gemini/gemini-cli/releases).
- Sending `/compress` as prompt text would be ordinary model input, which the open decision rules out.

**Antigravity: possibly partly unblocked (third-party evidence only)**
- An issue on another project (https://github.com/BytePioneer-AI/codex-host/issues/746, opened 2026-11-05, closed 2026-11-07) reports, for agy 1.4.12:
  - `/compact` sent through `--input-format stream-json` is expanded as a slash command (the log says `expanded slash command "compact"`);
  - it emits a `step_update` with `step_type: "checkpoint"` (state DONE, with a duration), then a `result` with status SUCCESS;
  - there is no summary text, and 1.2.5 lacked the feature.
- UAM's adapter ignores checkpoint steps; it handles only user input, agent responses and tool steps (`antigravity_cli_provider_runtime.cpp:577-654`).
- The official headless docs (https://antigravity.google/docs/cli/headless/) were not confirmed to mention `/compact`.

**What is missing**
- Gemini: an upstream control.
- Antigravity: first-party confirmation, checkpoint handling, and a source for the summary.

**First slice (S)**
- Gemini: record the re-check (0.70.0 and the 2026-11-23 nightly) in the row's reason.
- Antigravity: a check run on agy 1.4.x or later:
  - confirm `/compact` over stream-json is a native control and not model text;
  - capture the checkpoint step;
  - check whether a summary can be read from native history (steps table, `antigravity_native_history.cpp:283,627`).
- If a summary is available, implement `OnAcpBuildCompact` plus checkpoint handling (M). If not, record "native trigger, no summary".
- Risk: low.
- Product-owner decision: "If Antigravity gives a completion signal but no summary, is that enough to meet the 'records the summary' criterion?"
- Dependencies: none.
- Size: **S**, or **M** for the Antigravity implementation.

---

## Summary

| Issue | First slice | Size | Decision needed before starting? | Depends on |
|---|---|---|---|---|
| #963 | Read-only `memory_search` | S | No (yes for later tools) | — |
| #964 | Read-only `worktree_status` | S | No (only if reviewer gets the tool) | — |
| #965 | `schedule` self-wake | S–M | No (yes for `notify`) | Can share timer with #518 |
| #966 | `chat_message` behind approval | M | Yes | — |
| #967 | Read-only `usage_report` | S | No | — |
| #518 | Scheduled automations (no webhooks) | M | Yes (3 questions) | — |
| #514 | `pr_open` behind approval, no GUI | M | Yes | #964; settling on #629 |
| #520 | SVN branch creation (native) | M | Yes | #519 remainder |
| #521 | Design note and spike | S | Yes | #519, #520 |
| #522 | Protocol spec and conformance harness | S–M | Yes | #519, #523 for integration |
| #523 | One custom ACP agent, local only | M–L | Yes | — |
| #524 | "in use" text, never-wait tests, restart test | S | No (yes for auto-rejoin) | — |
| #363 | Decision interface with chat-provider fallback | M | Partly (scope of "turn review") | — |
| #512 | Gemini re-check recorded; Antigravity check run | S | Only if no summary | — |

## Recommended order

1. **Start in parallel now (no decision needed, no shared files):**
   - #524 slice.
   - #512 re-check.
   - #363 slice.
   - One agent working through #967 `usage_report`, then #964 `worktree_status`, then #963 `memory_search`. They share `uam_control_service.cpp` and the readOnlyHint test.
2. **After David answers the questions above:** #518 scheduled automations, #520 SVN branch creation, #965 `schedule`.
3. **Next:** #523 single custom ACP agent; #966 `chat_message`; the combined #514/#964 `pr_open` once agreed.
4. **Design first:** the #522 spec and harness (after #523), and the #521 spike (after the #519 remainder lands).
