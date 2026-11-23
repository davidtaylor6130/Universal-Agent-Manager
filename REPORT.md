# #986 remaining poll-loop saves — delivery report

## Final SHA

`ab2f414597613d46adb80b21644177c39aa4eb53` on `v5/perf-986-remaining-saves` (one commit on `origin/uam/v5.2-sync-main` at `c252f9a9`), pushed. No PR, issue, comment or label was created.

## What changed

- **Mutating UAM-Control calls.** Some calls change nothing on the root chat except their own audit record. They now reply through the background chat writer, like read-only tools, and only after that record is durable. These are every call its tool refused, plus an accepted `agent_cancel`, `pr_watch` or `pr_unwatch`. A call takes this path only if the root's transcript revision stamp and its saved-field fingerprint are unchanged after the call. Every other accepted mutation (`agent_delegate`, `goal_create`, HTML views) keeps its synchronous save and its rollback. Calls on an allowlist, each checked not to write messages, take a rollback copy without the transcript. Every other call still copies the whole chat.
- **`UserBoundary`.** A continuation lease now keeps the user rows its digest was taken from. Each poll compares those rows in place instead of re-serialising and hashing them. Capture, comparison and the digest JSON share one field list, and the saved `origin_user_digest` bytes are unchanged.
- **Scheduler.** While an `@agent` request is prepared or unconfirmed, the per-poll check of the idle parent now reuses the last full comparison as long as both transcript stamps are unchanged. It still compares in full at least every 2 s. Every publication guard still compares in full.
- **Docs.** The `docs/architecture/app-state-persistence.md` agreement is updated. Provider capability rows are unchanged.

## Same-workload measurement (run here)

These figures come from a Linux harness that links the real `chat_repository.cpp`, model and page-index sources. Fixture: a 2,000-message, 12 MB root with 64 audit records. Each figure is the median [min–max] of 15 runs on one 4-core container, in milliseconds.

| Poll-thread work | Before | After |
|---|---|---|
| Mutating call: rollback copy of the root | 1.456 [1.339–2.799] | 0.0069 [0.0068–0.0118] |
| Audit-only mutating call: save on the poll (after: the two fingerprints that prove it; the background save itself is not included) | 88.258 [80.917–104.600] | 0.481 [0.441–0.637] |
| Lease poll: `UserBoundary` check (1,000 user rows) | 9.905 [9.709–12.161] | 0.149 [0.120–0.190] |
| Scheduler poll: prepared root transcript check | 0.978 [0.967–1.071] | 0.0001 [0.0001–0.0004] |

The row comparison agreed with the old digest comparison in 4,000 of 4,000 random edits, and the new digest is byte-identical to the old one.

## Review rounds

Both reviewers were fresh-context `claude-opus-5-5` agents. Each got the issue, the diff against `origin/uam/v5.2-sync-main` and the docs.

**Round 1: 7 suggested, 7 implemented (0 blockers, 5 should-fix, 2 nits).**
1. Rollback without the transcript was chosen by a denylist, so a future tool would silently get a partial rollback. Now an allowlist, with a whole copy for everything else, and the diagnostic names the method and request ID.
2. The digested fields were listed twice. Now a single source, and the test compares every edit against the real digest.
3. The lease check had no "before" figure. Added.
4. The rollback test also passed when no lease had been staged. It now requires a staged, revoked lease.
5. The accepted `agent_cancel` timing budget risked flaking on full-fsync. Now checked by the root storage revision instead.
6. Doc wording was too broad. Fixed.
7. A test name over-claimed. Renamed, and the exact-compare contract is now asserted. Partly done: no scheduler test hook pins that the guards pass `exact`, because those functions are file-local.

**Re-check: 7 suggested, 7 implemented (0 blockers, 3 should-fix, 4 nits).**
1. `GoalCreate`'s own undo still deep-copied the chat. It now uses the transcript-free copy. Added a refused-save rollback test for `goal_create` and a test that every allowlisted call leaves the transcript unwritten.
2. `html_view_preview` paid a whole-chat copy. Partly done: it is on the allowlist now, but its audit save stays synchronous. Deferring it would reorder its render-check reply.
3. No figures were cited. Partly done: the commit message cites the Linux harness figures. The macOS `[measure]` cases were not run, because no Mac was available.
4. `CopyWithoutMessages` duplicated `PendingSaveMetadata`. `PendingSaveMetadata` now forwards to it.
5. The per-poll row compare copied every row. It now compares in place with no copy (one field list drives capture, comparison and JSON).
6. Removed an unused test helper.
7. Reworded a "report-only" comment that over-claimed.

## New issue preview (not filed)

**Title:** `perf(control): accepted UAM-Control calls that change the chat still save it on the poll`

An accepted `agent_delegate`, `goal_create` or HTML view call from a provider's UAM-Control server still rewrites the whole root chat file on the poll thread before it replies. `goal_create` does this twice: `GoalService::CreateGoal` saves, and then so does the audit. Since #986, calls whose only change is their audit record use the background writer, but these calls changed the chat itself. Their rollback must be decided before the effect is observable, for example a started child run that the failure path cancels.

**Expected:** an accepted root-changing control call does not hold the poll for a whole-chat save. Rollback still covers exactly what the call changed, and the reply is still written only after the change is durable.
**Actual:** each such call costs one synchronous whole-chat save on the poll, and `goal_create` costs two. On the 12 MB fixture one save took 88 ms (median, Linux harness).

**Environment:** `v5/perf-986-remaining-saves` at `ab2f4145`.
**Evidence:** `src/app/uam_control_service.cpp` `HandleRequest` (the `AppendAuditAndSave` path), `src/app/goal_service.cpp` `CreateGoal`. Measured only for the general save cost; this exact path was not timed.
**Reproduction status:** not reproduced in the app. Inferred from code and the harness save timing.

**Acceptance criteria:**
- [ ] An accepted `agent_delegate`, `goal_create` or HTML view call does not block the poll for a whole-chat save.
- [ ] If the save fails, every field the call changed is rolled back before the error reply, and the external effects are undone as today (a child is cancelled, a view removed).
- [ ] Exactly-once result delivery is unchanged.
- [ ] The same workload is measured before and after on the same machine.

**Review:** drafted from the #986 follow-up review. The report itself was not reviewed separately.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `perf(chat): take the remaining #986 control saves and parent compares off the poll`

**Body:**

> Some mutating UAM-Control calls change nothing on the root chat except their own audit record. They now reply through the background chat writer, like read-only tools, and only once that record is durable. These are every refused call, plus an accepted `agent_cancel`, `pr_watch` or `pr_unwatch`. A call takes this path only if the root's transcript stamp and saved-field fingerprint are unchanged after it. Accepted calls that change the chat keep the synchronous save and full rollback. Allowlisted calls, checked not to write messages, take a rollback copy without the transcript. A delegating parent's continuation lease compares its recorded user rows in place on each poll instead of re-serialising and hashing them, and the saved digest bytes are unchanged. A prepared or unconfirmed `@agent` request reuses its last full parent comparison while the transcript stamps are unchanged, re-compares at least every 2 s, and always compares in full before publishing.
>
> Related to #986. Remaining: accepted calls that change the chat (`agent_delegate`, `goal_create`, HTML views) still save synchronously on the poll.
>
> **Proof:**
> - Same workload before and after, from a Linux harness linking the real chat repository code (2,000-message, 12 MB root; median of 15 runs; one 4-core container):
>   - rollback copy: 1.456 → 0.0069 ms
>   - audit-only mutating call on the poll: 88.3 → 0.48 ms (the background save is not included)
>   - lease boundary check: 9.9 → 0.149 ms
>   - prepared-root transcript check: 0.978 → 0.0001 ms
> - The row comparison matched the old digest comparison in 4,000 of 4,000 random edits, and the digest bytes are identical.
> - All changed native files and both test files passed syntax-only compiles with g++ 13.3 and clang 18.1 (CEF headers stubbed), with no errors outside the stubs.
> - `python3 scripts/check_docs.py` passed.
> - **Not run:** `uam_core_tests` and the desktop build, because CMake refuses Linux. The new cases have therefore never executed. They are `UamControlAuditOnlyMutationReplyFollowsDurableAuditOffPoll`, `UamControlDeferredAuditSaveKeepsRootResultDeliveryExactlyOnce`, `UamControlRootMutationRollbackRestoresSavedFieldsAndKeepsTranscript`, `UamControlGoalCreateRollbackRestoresGoalAndKeepsTranscript`, `UamControlTranscriptFreeRollbackCallsLeaveTheTranscriptUnwritten`, `ChatRollbackSnapshotKeepsTranscriptAndRestoresMetadata`, `PolledTranscriptMatchSeesStampedWritesAndRechecksInFull`, `ManagedParentUserBoundaryComparisonMatchesDigestRules` and `ManagedPollCheckMeasurements`.
> - **Not run:** the runner build, because the sqlite.org download is blocked by this environment's network policy.
> - **Not run:** macOS and Windows CI.
> - UI-V2 was not touched.
>
> **Review:** 2 independent reviewers, 2 iterations.
> - Independent claude-opus-5-5 (round 1): 7 suggested, 7 implemented (1 partly).
> - Independent claude-opus-5-5 (re-check): 7 suggested, 7 implemented (2 partly: `html_view_preview` keeps its synchronous audit save; figures are from Linux, not macOS).
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## Screens needing Before/After capture

None. This change touches no React/UI and no visible interaction.
