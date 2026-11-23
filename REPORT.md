# #961 startup runtimes stage: report

**Work branch:** `v5/perf-961-startup-runtimes-stage`, head **`5c12590e`**, based on `origin/uam/v5.2-sync-main`. Commits:

- `4f5eeb62`: the fix
- `23e89b21`: fixes from review round 1
- `5c12590e`: fixes from review round 2

Nothing has been built or run natively: CMake refuses Linux and the CEF headers are absent here. No PR, issue comment, label or merge.

## What changed
1. **Startup model discovery (the likely main cause).**
   - Before: the tick that applies the startup chat list runs `FinishStartupState`, and the poll timed it inside "runtimes". In it, `StartupModelDiscoveryService::CollectScopes` checked each candidate scope against every chat and canonicalised both paths on each comparison. That is O(scopes × chats) file-system calls, worst for worktree chats and cached scopes of removed workspaces. `Poll` repeated it every tick for waiting scopes.
   - Now: each distinct host and path is resolved once per pass, and a scope check is a set lookup. Results are unchanged.
   - `Poll` tests registration only right before a launch, or before a scope's first version check. A scope that passed once does not repeat the test while its check is refused.
2. **Side-chat cleanup.**
   - Before: startup flags every leftover side chat, and the next poll removed all of them in one tick. Each removal is a full deletion transaction.
   - Now: at most one removal per poll. The latest request goes first, so Dismiss side chat still removes the dismissed chat at once unless a session still has to stop for it.
3. **Timing.**
   - The PollTick line has a `startup` stage (work done before the stages), so `runtimes` covers only the runtime polls. There is also a `side_chats` sub-stage of `runtimes`.
   - A new line, `startup held bridge requests answered in N ms`, times the replayed requests, including the UI's first full state.

Nothing moved to a background thread. The UI-thread work became proportional to distinct workspaces instead of scopes × chats, and side-chat removal is limited to one per tick.

## Tests (added, not run)
- **`PerformanceStartupModelDiscoveryOnLargeInstall`.** Fixture: 2,400 chats, 300 worktrees and 20 stale cached scopes.
  - Correct scopes: one per registered workspace, none for removed ones.
  - Budgets: 250 ms for the one-off collection and 40 ms for a waiting poll. The case is listed in `tests/ci_serial.txt`.
  - A waiting scope survives a poll with its chat missing and still launches.
- **`TemporarySideChatCleanupRemovesOneSideChatPerPoll`:** one removal per poll, latest request first.
- **Ran:**
  - `python3 scripts/check_docs.py`: passed.
  - `python3 tests/perf_suite_test.py`: passed.
  - Stub compiles of the new lookup class and the rotate logic: passed. 1,000 chats needed 103 identity calls instead of one per scope × chat.

## Unverified
- Native compile and the new and existing native tests.
- Before/after performance figures, and a real-install confirmation that discovery accounts for the 21–66 s. The new `startup`, `side_chats` and held-requests lines will show it.
- Remaining UI-thread work I found but did not change:
  - the replayed first-state request (in files other agents own);
  - result delivery for interrupted agent runs;
  - synchronous side-chat and goal saves in `FinishStartupState`;
  - O(pending × chats) lookups in `FlushPendingChatSaves`.

## Review rounds
- **Round 1, independent claude-opus-5-5 (fresh context): 7 suggested (0 blocker, 3 should-fix, 4 nit), 7 implemented.** Two optional parts were declined:
  - Counting side-chat cleanup as pending work for a 250 ms cadence: it adds a scan of every chat to the cadence check, and the drain is background housekeeping (about one side chat per second when idle).
  - A `chat_saves` sub-stage: `side_chats` was added; the rest of `runtimes` can be split later if field logs point there.
- **Round 2, independent claude-opus-5-5 re-check (fresh context): 4 suggested (0 blocker, 1 should-fix, 3 nit), 4 implemented.**

## Issue previews
None. No new issue was filed or drafted.

## Screens needing Before/After capture
None. This is native timing and cleanup ordering, with no visual change.

## PR preview
**perf(startup): resolve each discovery workspace once and spread side-chat cleanup**

On large installs, the poll tick that applies the startup chat list held the UI thread for 21–66 s. Startup model discovery compared every candidate scope with every chat and canonicalised both paths on each comparison. That ran inside the tick's "runtimes" timing, and `Poll` repeated it for waiting scopes.

Discovery now resolves each distinct workspace once per pass and checks scopes against one set of registered workspaces, with unchanged results. Waiting scopes skip the check until they launch or start their first version check. Side-chat cleanup removes one side chat per poll, with the latest request (Dismiss) first. The PollTick line reports the startup apply as a `startup` stage and side-chat cleanup as `side_chats`, and the held bridge requests log their own duration. Related to #961.

**Proof:**
- Passed: `python3 scripts/check_docs.py`, `python3 tests/perf_suite_test.py`, and stub compiles of the new lookup class (1,000 chats, 103 identity calls instead of one per scope × chat) and the rotate logic.
- Not run: the native build and tests, because CMake refuses Linux. That includes `PerformanceStartupModelDiscoveryOnLargeInstall` (250 ms collection, 40 ms waiting poll) and `TemporarySideChatCleanupRemovesOneSideChatPerPoll`.
- Not measured: before/after performance-suite figures.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 11 suggested, 11 implemented (round 1: 7/7, round 2: 4/4; two optional parts declined as noted). Claude review of the final head and David's approval are pending.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
