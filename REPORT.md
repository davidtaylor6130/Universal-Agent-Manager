# Routing fairness and spawn priority: final report

**Work branch:** `v5/fix-routing-fairness-qos` at `bc5c1542d82604c652e54ffad3a1d89d49aa43fe` (3 commits on `uam/v5.2-sync-main`: 69f55cc9, 1e17732a, bc5c1542). No PR or issues created.

## Review rounds

1 independent reviewer (fresh-context claude-opus-5-5 sub-agent), 2 iterations.

- **Round 1** (full diff, 69f55cc9): 10 suggested (1 blocker, 4 should-fix, 5 nits), 10 implemented, one of them only in part.
  - Blocker: macOS needs `<pthread/spawn.h>` for `posix_spawnattr_set_qos_class_np`. Fixed.
  - Should-fix: the repository launch check before a user's turn was lowered. It now runs at the chat's class.
  - Should-fix: agents queued behind the four running agents kept a routing ticket for up to 5 s. They now withdraw it on the same poll.
  - Should-fix: the sub-agent docs overstated FIFO across the @mention queue and the ordinary queue. Reworded.
  - Should-fix: the capability rows claimed Linux verification without CI evidence. All platforms are now marked as needing verification.
  - Nits done: waiters hold slots only on enabled, ready computers; corrected `QueuedChatIds` comment; niceness test reads `/proc` on Linux; source detail moved from platform boundaries to the execution reference; macOS nice 10 mentioned.
  - Nit done in part (test gaps): added the `PollExecutionRouting` deleted-chat test, plus a scheduler-level withdrawal assertion in round 2. Not added, because they need the macOS/Windows test build to be worth anything: a cap test for @mention runtimes, a macOS QoS-clamp assertion, and tests for how the ACP launch path applies the class.
- **Round 2** (re-check of the round-1 fixes, 1e17732a): 5 suggested (1 blocker, 4 nits), 5 implemented.
  - Blocker: the new routing test came before the include that declares `PollExecutionRouting`. Moved.
  - Nits: clear the 1 s retry gate when a ticket is withdrawn; scheduler-level withdrawal test; reworded the queue-order sentence; concurrency doc now says the repository check before a chat's launch keeps the chat's priority.
  - Round 2 confirmed 9 of the 10 round-1 findings fixed; the tenth was this blocker. Nobody re-reviewed the round-2 fixes, because the brief allowed one re-check. They are syntax-checked.

## Issue previews

### 1. `bug(routing): newly arriving chat can take a freed slot before an older waiter`

Automatic routing admission has no queue order. Waiting chats each retry on their own 1 s timer without a ticket (`execution_routing_integration.cpp` ~447), so a chat arriving just after a slot frees can take it before one that has waited longer. A busy computer can starve a waiting chat. Compute pools already use FIFO tickets (`compute_pool_gate.cpp` ~111-115). Reported in the routing and load review.

**Reproduction:** one computer with one slot, slot taken. Chat A asks and waits. The slot frees. Chat B asks before A's next retry.
**Expected:** A gets the slot and B waits.
**Actual:** B gets the slot and A keeps waiting.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** reproduced with `ExecutionRoutingService` directly. Old code: the newest arrival is Assigned while A is still Waiting on its retry.
**Reproduction status:** reproduced in a standalone build of the routing service on Linux.

**Acceptance criteria:**

- [ ] A freed slot goes to the longest-waiting chat that can use that computer.
- [ ] A waiter that fits on another computer does not leave a second one idle.
- [ ] A waiter that stops asking ages out and does not block a computer.

**Review:** self-triage only; no independent review of the report.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

### 2. `bug(subagents): one parent's agents can take every running slot`

`kMaxDirectChildren` (4) equals `kMaxRunningAgents` (4) in `agent_run_scheduler.cpp` ~277-279. One parent can therefore fill all running slots while another parent's agents wait. Reported in the routing and load review.

**Reproduction:** one chat delegates four agents, then a second chat delegates one.
**Expected:** the second chat's agent gets a slot within a fair share.
**Actual:** it waits until one of the first chat's agents finishes.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** found by reading the code; the constants allow it.
**Reproduction status:** not run at runtime.

**Acceptance criteria:**

- [ ] One parent's agents hold at most half the running slots (2 of 4).
- [ ] The rest stay queued in request order, and agents held back keep their place.

**Review:** self-triage only; no independent review of the report.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

### 3. `bug(platform): agent and background provider processes spawn at normal priority`

Provider child processes are spawned with no QoS or niceness (`platform_services_macos_impl_internal.h` ~220-224). #647 lowered only UAM's own threads. Many sub-agents or a background sweep therefore compete equally with the turn the user is waiting on. Reported in the routing and load review.

**Expected:** agent and background children run below user turns, on remote computers too.
**Actual:** every child starts at default priority.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9; macOS, Windows and the Linux helper.

**Evidence:** on the Linux helper, a spawned child reported nice 0 regardless of which kind of work started it. On macOS and Windows this is from reading the code only.
**Reproduction status:** reproduced on Linux only.

**Acceptance criteria:**

- [ ] Agent and background children start at utility QoS on macOS, nice 10 on Linux and below-normal on Windows.
- [ ] User turns, CLI view, the repository check before a chat's launch, and installs the user starts keep normal priority.
- [ ] Remote computers apply the same class, and older helpers keep working.

**Review:** self-triage only; no independent review of the report.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**Title:** `fix(scheduling): queue routing admission, share agent slots and lower agent spawn priority`

Routing admission now serves waiting automatic chats in order per computer. A freed slot goes to the longest-waiting chat that can use it, before newer arrivals. A waiter holds slots only on enabled, ready computers, stops holding one after 5 s of silence and loses its ticket after 60 s. One parent's agents may hold at most 2 of the 4 running slots; agents held back keep their queue position and give up any routing ticket until they are offered a slot again. Agent work (sub-agents, goal iteration transcripts, automation chats) and background work (model discovery, memory workers, history import, version checks, repository discovery) start their processes at lowered priority: utility QoS and nice 10 on macOS, below-normal on Windows, nice 10 on Linux. Remote computers get the same class through an optional `processClass` field that older helpers ignore. User turns, CLI view, the repository check before a chat's launch, and installs the user starts keep normal priority. Docs and the six `concurrency` capability rows are updated.

Related to: the three issues above, once filed.

**Proof:**

- The Linux helper (`uam_runner`) builds at bc5c1542. The sqlite download is blocked in this sandbox, so I supplied the same amalgamation from npm.
- **Routing:** 12 of 12 service-level cases from `tests/execution_routing_tests.cpp` pass in a standalone harness. A repro against the old code shows the newcomer taking the freed slot; with the change it waits and the older chat is admitted.
- **Spawn priority:** 5 of the 6 cases in `tests/child_process_priority_tests.cpp` run on Linux against the real helper process service. They pass, and children measure nice 10. With the old Linux spawn code, the 2 spawn cases fail.
- **Compile:** g++ and clang++ syntax-only checks are clean for the changed app, helper and test sources, with CEF headers stubbed. That includes the new scheduler test, the `PollExecutionRouting` test and the classification test.
- `python3 scripts/check_docs.py` passed.
- **Not run:**
  - macOS and Windows builds, including the macOS QoS header change and the Windows creation flags.
  - `uam_core_tests`.
  - The macOS connected handler tests, two of which were edited to use two parents.
  - Real SSH helper checks.
  - There is no UI change, so no npm checks were run.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: round 1 suggested 10, implemented 10 (one only in part: three test-gap suggestions that need the macOS/Windows test build were not added); round 2 suggested 5, implemented 5.

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

None: no UI or visible text changed.
