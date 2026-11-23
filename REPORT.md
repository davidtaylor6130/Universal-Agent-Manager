# Host admission policy: agent report

**Work branch:** `v5/feat-host-admission-policy`, final SHA `c311d47201b5d63a2a1ddb0a464cb5981b0015c8` (pushed). No PR, issue or comment was created.

## What was built

Commits on top of `origin/uam/v5.2-sync-main`:

| SHA | Commit |
|---|---|
| a0901e71 | feat(routing): add the host admission policy, ledger and limits settings |
| 7ebdad88 | feat(routing): send every process start through host admission |
| d337e6dd | feat(routing): move unstarted work between remote hosts and skip overloaded ones |
| 1cc38462 | feat(routing): address independent review of host admission |
| c311d472 | feat(routing): address the re-check of the host admission fixes |

**HostAdmission core** (`src/common/runtime/host_admission.{h,cpp}`, no app dependencies):
- **Ledger:** a per-computer ledger of live work by class (interactive, agent, background).
- **Limits:** a slot cap shared by user turns and agents; a separate background budget (default 2, range 1–4).
- **Queue:** first come, first served tickets per computer and class. User turns go before agents, and an agent that has waited 2 minutes is served like a user turn. A waiter that stops asking loses its place after 5 s and its ticket after 60 s; these timings match the fairness branch's queue.
- **Health:** Ready, Degraded, Overloaded or Unreachable. Each state has its own hold after the load drops: 10 s, doubling up to 5 minutes if the state returns within 5 minutes.
- **Run anyway:** offered for a user's own turns only.
- **Wait messages:** always one line, for example "Waiting for host A (8/8)" or "Waiting for this computer: busy (CPU 80%)".
- **Load signal:** the `LoadSample` interface is based on the load-signal branch's `platform::HostLoad` (`LoadSampleFromFigures`). Unknown load means slot counts only, never idle.

**The gate is asked by every start:**
- **Chat turns:** every chat provider launch (queued turn, direct send, retry, model list) is checked before the process starts. An admitted start reserves its slot for up to 30 s. The turn is checked again at the prompt gate: the host is checked first, then compute pools, and the host admits only after the pools pass. This covers explicit and automatic chats, sub-agents, goal iterations and automation chats.
- **CLI View:** a launch waits with the same line and Run anyway, and retries about once a second.
- **Background lanes:**
  - Model discovery: ephemeral ACP, Antigravity native, and startup scopes. A held request keeps its place and starts by itself.
  - Memory workers.
  - Startup history scan and its retries.
  - Repository inventory read.
  - Queued version-check sweeps, and the background version checks started by routing and by startup discovery.

**Routing** (decision 2):
- Unstarted automatic work moves to another eligible remote computer with a matching workspace when its own computer is full, down or overloaded. "Full" means either routing's own count or host admission reporting every slot taken.
- It never moves to the local computer. Started work stays pinned.
- New work is not placed on an Overloaded or full computer.
- A chat that cannot move stays assigned and waits in its computer's host admission queue.

**Local computer** (decision 3): it has its own limit, defaulting to processors minus two (4 if the processor count is unknown). It stays outside automatic routing, and the refusal of a local routing slot policy is kept.

**Settings › Compute Pools › Host limits:**
- Fields: this computer's limit, the remote default (8), the background budget and the soft/hard thresholds. Invalid values are blocked, and a failed save shows beside the form.
- A Computers list shows each computer's running, background and waiting counts and its health.

**Docs** (in the same commits):
- Compute Pools: new Host limits section.
- Routing: the rule now distinguishes started from unstarted work.
- Also updated: concurrency, the six provider `compute-pools` capability rows, the settings index and the source map.
- Documentation review: the 2026-11-23 decision is quoted as given. The reading of "no saved assignment" and the interim choices are recorded as open items. `python3 scripts/check_docs.py` passes.

## Review rounds

| Round | Reviewer | Suggested | Implemented |
|---|---|---|---|
| 1, independent fresh context | claude-opus-5-5 | 17 (3 blocker, 10 should-fix, 4 nit) | 16 |
| 2, fresh-context re-check of round-1 fixes | claude-opus-5-5 | 7 (0 blocker, 4 should-fix, 3 nit) | 7 |

**Round 1, declined (1):** #12, merge hazards with the sibling branches. There will be conflicts in `execution_routing_service.{h,cpp}` and `CMakeLists.txt` with both branches, and with the load-signal branch in the UI store files, `state_serializer.cpp` and `source-map.md`. These cannot be resolved here without taking over unmerged code from the other branches. Merge notes:
- **Fairness branch:** a moved chat should also join its `wait_in_queue`, and `SelectAutomaticHost` should return the eligible list.
- **Load-signal branch:** add an Overloaded skip reason, and set `app.host_load_sample` from `host_load::Sampler` through `LoadSampleFromFigures`.

**Round 1, implemented only in part:**
- **#10:** health now refreshes once a second, and the load fields say they apply once readings exist. The sampler itself lives on the other branch.
- **#13:** added tests for Overloaded falling back to Degraded, for reservation expiry, and an integration test for moving a chat with a pending reservation. There are no native tests yet for the cold-start gate, the CLI View retry or the two CEF handlers.
- **#16:** remote history-scan retries still count against this computer's budget, because the scan is driven from here. This is recorded as an open interim choice.

The round-2 re-check confirmed all earlier fixes and found no compile errors. All seven of its items were implemented. Only one re-check was run, as instructed.

## Proof (only what was run, on Linux)

- **Policy and routing tests:** `tests/host_admission_tests.cpp` (14 cases) and the pure section of `tests/execution_routing_tests.cpp` (12 cases, including 3 new re-placement cases). Both were built with g++ 13 against `host_admission.cpp` and `execution_routing_service.cpp` using a small stand-in harness, under ASan/UBSan. All pass at `c311d472`.
- **Native syntax checks:** changed native files were syntax-checked with stub CEF headers: routing integration, host admission integration, compute pool gate, ACP lifecycle and runtime, terminal polling, memory, model discovery, settings store, repository match, history import, CLI compatibility and model catalogue. No errors.
- **Native test files:** the 4 changed test files (`execution_routing`, `compute_pool`, `model_discovery_startup`, `memory`) were checked the same way, after confirming the compiler reaches test bodies. No errors inside the test files.
- **CEF handlers:** the changed hunks were checked with stub headers. The only errors are the stub's missing `Callback` type, which unchanged handlers share.
- **Frontend:** `tsc -b` passes. The full `npm run test:unit` run passed (232 files, 4,184 tests) on the pre-review commits. After the review fixes, the focused suites pass (settings, ChatView, CLIView, reconcile).
- **Not run:**
  - The desktop build, which refuses Linux.
  - `uam_core_tests` and `ctest`, so the new native integration cases in `compute_pool_tests`, `model_discovery_startup_tests`, `memory_tests` and the routing integration section were never executed.
  - `npm run build`, the performance suites and any real SSH host.

Native acceptance on macOS/Windows is pending.

## Screens needing Before/After capture

1. A chat held at its computer's limit: the one-line wait with Run anyway, as an explicit local chat.
2. An agent chat held at the limit: the wait line with no Run anyway.
3. CLI View held at the limit: the info notice with Run anyway.
4. Settings › Compute Pools: the new Host limits section, the Computers list, and a validation/save error.

## Issue preview

**Title:** `feature(routing): admit every process start against per-host limits so routing never overloads a machine`

Automatic routing and explicit chats can overload a computer. Explicit chats are never checked against a slot limit. Background lanes (model discovery, memory workers, sub-agents) each have their own global caps and ignore load. Occupancy counts chats only. An assigned chat waits indefinitely on a down host. Health is only Ready or error. The product owner reported logs showing 99% of 1,210 process launches over 3 days were background model discovery (1,034 on the Mac), peaking at 60 a minute.

**Desired outcome** (decided by David, 2026-11-23):
- Explicit chats queue at their host's limit with one line, such as "Waiting for host A (8/8)", and Run anyway.
- Unstarted work may move between eligible remote hosts with a matching workspace when its host is full, down or overloaded. It never moves silently onto the local Mac, and started work stays pinned.
- The local Mac has its own limit (cores minus 2, configurable) and is not an automatic candidate.
- Priority is user turns, then agents, then background. Background waits over a soft limit. Over a hard limit, new agent and background work waits, and user turns wait with Run anyway.

**Environment:** `uam/v5.2-sync-main` (V5.2 performance branch).

**Evidence:**
- Code locations named in the request were verified: `acp_session_lifecycle.cpp` ~2310, `terminal_launch.h` ~124, `execution_routing_service.cpp` ~16 and ~74-104, `execution_routing_integration.cpp` ~366, `compute_pool_gate.cpp` ~51, `model_discovery_service.cpp` ~25, `memory_service.cpp` ~104, `agent_run_scheduler.cpp` ~277, `settings_store.cpp` ~1343.
- The launch counts are user-reported; reproduction was not attempted.

**Acceptance criteria:**
- [ ] Every process start asks one host admission gate.
- [ ] Explicit chats queue at the limit with Run anyway.
- [ ] Background work waits under load and starts when load drops.
- [ ] Waiters are served FIFO per host and class, with aging.
- [ ] Unstarted work moves only between remote hosts.
- [ ] The Mac's local limit applies.
- [ ] Unreachable and Degraded/Overloaded hosts back off.
- [ ] Settings expose the limits and thresholds.
- [ ] Every wait is one plain line.

**Related:** #524 (compute pools), #846.

**Review:** Drafted from the product owner's request; no independent review of the report itself.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview

**Title:** `feat(routing): host admission gate for every process start, with per-host limits and re-placement`

Every provider and helper process start now asks one host admission gate for its computer, so routing and explicit choices no longer overload a machine:
- **Classes and limits:** user turns and agents share a per-computer slot cap. This computer defaults to processors minus two; remote computers use their routing slot count, or 8. Background work has its own budget of 2.
- **Waits:** queued work is served first come, first served with agent aging. Held starts show one line, such as "Waiting for host A (8/8)"; a user's own turn also offers Run anyway.
- **Health:** Ready, Degraded, Overloaded or Unreachable, with a back-off. Background work waits over the soft limit; new agent work waits over the hard limit.
- **Routing:** unstarted automatic work moves between remote computers when its own is full, down or overloaded. It never moves to this computer, and started work stays put.
- **Settings:** Settings › Compute Pools gains Host limits.
- **Load readings:** the live load reading arrives with the separate load-signal branch. Until then, load is unknown and only slots and budgets apply.

Related to #524. Related to #846.

**Screenshots:** Before/After needed for the chat wait line with Run anyway, the agent wait line, the CLI View wait notice, and Settings › Compute Pools › Host limits (not captured; the desktop app cannot run here).

**Proof:**
- **Ran and passed (Linux):**
  - `host_admission_tests` (14) and the pure `execution_routing_tests` cases (12), with g++ 13 under ASan/UBSan.
  - Syntax checks with stub CEF headers on the changed native files and the 4 changed native test files.
  - Frontend: `tsc -b` and the full `npm run test:unit` (232 files, 4,184 tests) before the review fixes; the focused settings, ChatView, CLIView and reconcile suites after them.
  - `scripts/check_docs.py`.
- **Not run:** the desktop build, `uam_core_tests`/`ctest` (the new native integration cases were never executed), `npm run build`, the performance suites, and any real SSH host.
- **Pending:** native verification on macOS/Windows.

**Review:** Reviewed by 2 agents over 2 iterations: 1 independent reviewer, then a fresh-context re-check.

- Agent 1: claude-opus-5-5 (independent, fresh context) — suggested 17; implemented 16. Declined: sibling-branch merge conflicts, deferred to merge time with notes. Partly implemented: load sampler (lives on the other branch), extra native tests, remote history-retry budget.
- Agent 2: claude-opus-5-5 (fresh-context re-check of the fixes) — suggested 7; implemented 7.
- Still required: Claude review of the React changes, and David's approval. Both are pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
