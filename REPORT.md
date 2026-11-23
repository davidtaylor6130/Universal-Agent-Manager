# Report: #526 cold-configuration admission budgets

**Work branch:** `v5/fix-526-cold-admission-budgets`, pushed.
**Final SHA:** `641646b6`. The branch has four commits on `6f235eb9`. Since then, `origin/uam/v5.2-sync-main` has moved to `c252f9a9`; I have not rebased onto it.
- `210ded64`: first attempt, which timed the poll budgets in CPU time on hosted runners. Rejected in review.
- `402229a9`: revert of `210ded64`.
- `243a8424`: runs the chat-save workers below the poll thread and raises them at barriers.
- `641646b6`: re-check fixes, which raise every chat's save workers before the UI waits on chat saves.

Compared with the base, every wall-clock budget, completion deadline and line of `docs/engineering/testing.md` is unchanged.

## What changed
- **Cause (#526):** background chat-save workers (~26 MB saves, 6–8 per case) ran at the same priority as the UI poll thread. On the few shared cores of a hosted runner they starved it, so polls missed their 100 ms budget. The setup, agent-preparation and cold-history workers were already lowered (#647).
- **Why not simply lower them:** #647 deliberately kept chat-save workers at default priority, because force, shutdown and synchronous-save barriers make the UI wait on them, and a lowered worker gets no boost from that wait.
- **Fix:** the new `BarrierRaisableWorkerPriority` / `StartBarrierRaisableBackgroundThread` in `src/common/platform/background_thread_priority.*` keep both guarantees.
  - A save worker runs lowered until a thread about to wait on it calls `RaiseForBarrier`.
  - That returns the worker to the priority it started at: through a QoS override on macOS and the thread handle on Windows. Linux is best effort, because getting back to normal priority needs privilege there.
  - A raise that comes before the worker starts keeps it from lowering at all.
  - There is one priority object per worker, so a raise never carries over to later saves.
- **All seven chat-save workers** in `acp_session_runtime.cpp` start through `StartChatSaveWorker`.
- **Where the UI raises them:** before it blocks or spins on chat saves, it raises every chat's live save workers (`RaiseAllChatSaveWorkersForBarrier`). It raises all chats, not only the one being waited on, because publication holds the process-wide chat save lock. The places that raise are:
  - `FinishPendingQueuedAdmissionSave`, which covers synchronous saves and the force path;
  - a forced `FlushPendingChatSaves`, on entry;
  - `AgentRunScheduler::QuiesceInitialForShutdown`, after each flush;
  - forced empty-held and owned publications, for their own chat.
- **Tests:** the #647 source scan is renamed `ChatSaveWorkersRiseForBarriersAndStopWorkersKeepDefaultPriority`. It now requires all seven save workers to use the helper and each barrier to raise first. There are two new behavioural tests: `BarrierRaisableWorkerRunsLoweredUntilABarrierRaisesIt` and `BarrierRaisedBeforeStartKeepsWorkerAtDefaultPriority`.
- **Docs:** `docs/architecture/platform-boundaries.md` records the save-worker rule and corrects the Windows note: priority there is strict, with only a starvation boost.

## Review rounds
**Round 1** (independent review R1, #526 section only): 3 suggested, 2 implemented, 1 declined.
1. Blocker (the CPU-time clock relaxed the budgets and hid lock and disk waits): **implemented.** I reverted it and fixed the root cause, as directed by the product owner.
2. Should-fix (gating on `GITHUB_ACTIONS` also covered release-candidate runs): **implemented** by removal. The gating went with the revert.
3. Should-fix (cite passing hosted runs on Windows, macOS Intel and macOS ARM): **declined.** This session can't dispatch CI, and Linux can't build `uam_core_tests`.

**Round 2** (fresh-context Opus re-check of the rework): 6 suggested (0 blockers, 3 should-fix, 3 nits), 4 implemented, 2 declined.
- Implemented:
  - A synchronous save or force flush could block on another chat's lowered publisher through the global save lock. It now raises all chats' workers.
  - The shutdown quiesce loop never raised. It now raises after each flush.
  - The docs overstated the guarantee. They are corrected.
  - A started worker could go unrecorded if recording it ran out of memory. The record is now reserved before the thread starts.
- Declined:
  - **macOS no-QoS fallback (nit):** a starting class of `UNSPECIFIED` is raised to `QOS_CLASS_DEFAULT`, and on Windows a `GetThreadPriority` failure is passed through unchecked. Both are harmless: the first is a raise to the normal default, the second a failing no-op.
  - **Test strength (nit):** the macOS raise cannot be seen through the requested QoS class, so it is left to code review. The scan stays name-based, but now also asserts every barrier raise.

## Issue previews
None.

## PR preview
**Title:** `fix(chat): run chat-save workers below the poll thread and raise them at barriers`

On hosted runners the cold-configuration admission cases miss their wall-clock poll budgets because background chat-save workers (~26 MB saves) run at the poll thread's priority and starve it.
- **New helper:** `BarrierRaisableWorkerPriority` runs a worker below the UI until a thread about to wait on it raises it back to its starting priority. That is a QoS override on macOS, the thread handle on Windows, and best effort on Linux.
- **Save workers:** all seven chat-save workers use it.
- **Raises:** before the UI blocks or spins on chat saves, it raises every chat's live save workers, because publication holds the process-wide save lock. That covers synchronous saves, the force flush, the shutdown quiesce loop and forced publications, so #647's guarantee holds.
- **Unchanged:** wall-clock budgets and deadlines. The earlier CPU-time clock is reverted.
- **Tests and docs:** the #647 source scan is updated, two behavioural tests are added, and `platform-boundaries.md` is corrected.

**Proof:**
- **Priority tests:** the real `tests/background_thread_priority_tests.cpp` cases (4), built with a stand-in test harness on Linux, pass 20 of 20 runs.
- **Mutation check:** the scan fails against the original runtime, and fails when any one barrier raise is removed (forced flush, shutdown loop, forced empty-held publication).
- **Compile check:** g++ 13 and clang++ 18 syntax checks are clean for `acp_session_runtime.cpp`, `acp_session_lifecycle.cpp`, `agent_run_scheduler.cpp`, the priority source and the test. These used stubbed CEF headers.
- **Starvation probe** (Linux, one core, 4 CPU-bound workers, 3 runs): worst wall-clock poll was 18–21 ms with default-priority workers and 4–9 ms with lowered ones.
- `python3 scripts/check_docs.py` passes.
- **Not run:**
  - macOS and Windows builds, including the `pthread_override_qos_class_*` and `OpenThread`/`SetThreadPriority` paths;
  - `uam_core_tests` and the cold-admission cases;
  - hosted CI on Windows, macOS Intel and macOS ARM, which the issue's acceptance needs.

**Review:** Reviewed by 2 agents over 2 iterations.
- Agent 1: claude-opus-5-5. Suggested 3, implemented 2. Declined: hosted-runner proof, which this session can't produce.
- Agent 2: claude-opus-5-5. Suggested 6, implemented 4. Declined two nits: the macOS no-QoS fallback, and macOS raise test coverage.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

Refs #526

## Screens needing Before/After capture
None. No UI changed.
