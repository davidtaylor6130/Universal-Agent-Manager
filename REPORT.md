# Goal-loop fixes: v5/fix-goal-loop-unbounded-resume

**Branch:** `v5/fix-goal-loop-unbounded-resume`, final SHA `31898ec402ec4fbde0a70ec905787724715ea33d` (3 commits on `uam/v5.2-sync-main` `6f235eb9`; merges cleanly into `origin/uam/v5.2-performance` `64671114`, so no merge was made).

## Changes
1. **Unbounded stalled-loop auto-resume.** The old session counter was reset by every prompt start and turn completion, and the cap only counted reviewed iterations. Each goal now saves `unreviewedResumeCount`; only an applied review or Restart clears it. Unreviewed resumes count toward the iteration cap, and 3 in a row block the goal with "Goal stopped: 3 automatic resumes in a row produced no goal review." An ordinary Resume keeps the count. Automation `iterationsUsed` reports the same count the cap checks.
2. **Decision-shaped worker output skipped review.** Removed the worker-reply `ParseReviewDecision` skip; only review turns supply decisions.
3. **"stop"/"yes" meant complete.** Both are now invalid, so the one repair turn runs; "done"/"passed" still mean complete.
4. **Docs:** `docs/chat/goals.md` and `docs/automation-api.md`. Provider checklist rows unchanged (shared UAM behaviour).

## Review rounds
- **Round 1** (independent claude-opus-5-5, fresh context): 7 suggested, 6 implemented.
  - Implemented: obsolete half of `AcpGoalSchedulerSkipsMissingUserIndex...` (blocker); stale `app_state.h` comment; automation `iterationsUsed`; pinned Resume/Restart semantics with tests and docs; Antigravity check comment; watchdog test rename.
  - Declined: skipped tool-only turns are not charged to the goal budget. Pre-existing and separate from this fix; proposed as a follow-up issue below.
- **Round 2** (fresh-context claude-opus-5-5 re-check): 0 blockers, 0 should-fix, 3 nits; 2 implemented.
  - Implemented: Antigravity comment wording; goals.md "after that stop" wording.
  - Declined: a new automation-API test for `iterationsUsed`. The one-line sum is unchanged in shape; tests cannot be run on Linux to validate a new fixture.

## Tests
New: `AcpGoalWatchdogStopsUnreviewedResumesAcrossTurnCompletionsAndSkippedReviews`, `AcpGoalUnreviewedResumesCountTowardIterationCapUntilAReviewApplies`, `AcpGoalWorkerOutputShapedLikeReviewDecisionIsStillReviewed`, `GoalServiceParseReviewDecisionTreatsStopAndYesAsInvalid`. Updated: the Resume, Restart, watchdog and scheduler-guard tests.

## Unverified
- No native build or test run: CMake refuses Linux. Checked only with `g++ -std=c++20 -fsyntax-only` on every changed file, using stub CEF headers (CEF CDN blocked).
- The watchdog test sets the user-turn start state directly rather than going through `DrainNextQueuedAcpUserPrompt`.

## Screens needing Before/After
None. No React/UI change; the only visible difference is the goal window's blocker text after the new stop.

## Issue preview
**Title:** `bug(goals): stalled-loop auto-resume runs worker turns without limit`

Stalled-loop auto-resume can keep starting worker turns with no review and no limit; with the default token budget of 0 that means unlimited spend. Found by code inspection on `uam/v5.2-sync-main`.

**Reproduction:** Start a goal whose worker turn skips review (tool-only turn with no assistant text, or decision-shaped JSON text). Wait 5 s idle.
**Expected:** after a few unreviewed resumes the goal stops with a visible reason; resumes count toward the iteration cap.
**Actual:** the resume counter resets on every prompt start and turn completion, and `loop_count` only rises on an applied review, so neither limit trips. Related: the review parser maps "stop" and "yes" to complete.

**Evidence:** code inspection only. **Reproduction status:** no runtime reproduction attempted.

**Acceptance criteria:**
- [ ] Three automatic resumes with no review stop the goal with a one-line reason.
- [ ] Unreviewed resumes count toward the iteration cap.
- [ ] Worker output shaped like a decision is still reviewed.
- [ ] "stop" and "yes" never mark a goal complete.

Related to #454.

**Review:** self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

### Follow-up issue preview
**Title:** `bug(goals): worker turns that skip review are not charged to the goal budget`

`ScheduleGoalReviewAfterSuccessfulTurn` returns on `skip_schedule_empty_turn_text` / `skip_schedule_missing_turn_indexes` before `RecordTurnCompletion`, so tool-only worker turns never count toward the token budget. Found in code review; not reproduced at runtime. Acceptance: a skipped worker turn charges its reported usage, or the labelled estimate, exactly once. Related to #448, #905.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## PR preview
**Title:** `fix(goals): bound stalled-loop auto-resume and only take decisions from review turns`

Goal loops could auto-resume forever when worker turns skipped review. Each goal now saves a count of automatic resumes in a row that only an applied review or Restart clears. That count is part of the iteration cap and of the automation `iterationsUsed`. After 3 resumes with no review the goal blocks with a one-line reason. Worker replies that look like decisions are now reviewed, and "stop"/"yes" review replies go to the repair turn instead of completing the goal. Goals and automation docs are updated.

**Proof:** `python3 scripts/check_docs.py` passed. `g++ -std=c++20 -fsyntax-only` with stub CEF headers passed on every changed source file and `tests/goal_tests.cpp`. Native tests were not built or run, because CMake refuses Linux. They need a macOS or Windows run.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 10 suggested, 8 implemented (declined: pre-existing budget gap for skipped turns, filed as a follow-up; automation `iterationsUsed` test not added without a runnable build).

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

Related to #454, #448, #905, #917.
