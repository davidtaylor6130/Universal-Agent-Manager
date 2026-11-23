# #363 decision interface (first slice): report

The decision interface is built. The goal review now goes through it, and the work is pushed to `v5/feat-363-decision-interface`. No PR, issue or comment was created.

**Final SHA:** `27e53b79548e3546207dbe8e04ba0fb90f0a25d0` (3 commits on `origin/uam/v5.2-sync-main`: `d52ed520`, `2949e512`, `27e53b79`).

## What changed

- **Decision interface:** new `src/app/decision_service.{h,cpp}`.
  - It has three request shapes: Choice, Score and Probability.
  - Each result carries a reason and evidence, an attribution (model through a backend, filter, or fallback), latency in ms and usage.
  - A `DecisionBackend` interface (id, prompt, reply reader) has one backend: the chat provider.
- **Attribution:**
  - The backend always reads the answer from its own reply text. A caller's check can only reject that answer.
  - Only the model through a backend can produce a `model:` attribution; the filter and fallback factories cannot name a model.
  - A bare `model:` string passed to `RecordBlocker` is recorded as `fallback`.
- **Goal review:**
  - The UAM-managed review is now a Choice of continue, blocked or complete.
  - Goals save `lastDecisionBackend` (`chat_provider`) beside the existing `lastDecisionSource`, which keeps working as before.
  - A backend saved beside any source other than a model is dropped on save and load.
  - Every remaining direct write of the decision source now goes through `RecordDecision` or `ClearDecision`.
- **Diagnostics:**
  - Each review decision writes a `goal_review_decision` goal-loop diagnostic with the answer, source, backend, latency and usage.
  - Latency and usage both cover the single JSON repair turn.
  - Goal-loop diagnostics now record the size of prompts and replies instead of a 160-character text prefix, which the logging rules forbid. Error causes are kept as one short message.
- **Docs:**
  - `docs/chat/goals.md`: the decision source section and the implementation gap.
  - `docs/documentation-review.md`: a new "Decision model" section with the open questions and an interim interpretation.
  - `docs/reference/source-map.md`.
  - The six provider `goals` rows stay Partial; the interface is shared UAM delivery owned by the goals page.
- **No UI change.**
- **Permission-rules work:** none of the permission AI Review files are touched. `git merge-tree` against `origin/v5/feat-ai-review-command-rules` merges cleanly; the shared files (`app_models.h`, `app_state.h`, `CMakeLists.txt`, `source-map.md`) change in separate places.

## Review rounds

Both reviewers were fresh-context Opus agents (`claude-opus-5-5`).

| Round | Reviewer | Suggested | Implemented |
|---|---|---|---|
| 1 | Agent 1, initial review | 11 (0 blocker, 6 should-fix, 5 nit) | 11 |
| 2 | Agent 2, one re-check | 5 (0 blocker, 2 should-fix, 3 nit) | 5 |

None were declined. Two notes on scope:
- **Round-1 nit 10:** while fixing it I also found and fixed a related problem. Request text could close the `</decision_request>` prompt marker, because JSON does not escape `<`.
- **Round-2 finding 1:** its fix makes the backend read the answer, but the model attribution is still only as true as the reply text the caller passes. That limit is stated in the header rather than enforced by types.

The round-2 fixes have not had a further independent review, because the brief allows one re-check.

## Open questions for David

These are left out on purpose; none of them is guessed in the code.
1. Is the issue's "turn review" the goal-loop review, which now uses the interface, or a separate per-turn review that does not exist yet?
2. Are hosted Jev early-access credentials available, and should sending goal or transcript text to a hosted decision service be opt-in only? This blocks only the hosted backend.
3. How would a local Laya backend be packaged and shipped?
4. Should Permission AI Review (getting allow/ask/deny rules on `v5/feat-ai-review-command-rules`) later answer through this interface, or stay a separate reviewer?
5. Where should the backend and the reason appear for the user: the goal window's Last decision row, diagnostics only, or both? The goal window is unchanged. The Settings backend picker waits for a second backend.

Still open under #363 after this slice:
- the attention flag and turn review going through the interface;
- latency and cost measured against the text-model path;
- hosted and local backends.

## New issue preview (not filed)

**Title:** `bug(goals): a goal review can be repaired twice after a provider restart`

A goal review gets one JSON repair turn. Two session restart paths restore most of the in-flight review state but not the repair count, so a review can be repaired a second time after a restart. The re-check reviewer found this while reading the code for #363. It predates that work.

**Reproduction:** no exact steps are recorded. From reading the code:
1. A goal review's first reply is invalid, so UAM queues the repair turn.
2. The provider process restarts before the repair settles, through `StartAcpProcessForChat` during remote recovery or the startup-crash auto-restart.
3. `ResetAcpRuntimeState` sets `goal_review_repair_attempts` to 0, and neither restart path restores it.

**Expected:** the restored review keeps its repair count, so an invalid repair reply blocks the goal.

**Actual (from the code):** the repair count is 0 after the restart. An invalid repair reply then gets a second repair turn, which spends extra reviewer tokens.

**Environment:** `origin/uam/v5.2-sync-main` at `c252f9a9`, all six providers' UAM-managed goals.

**Evidence:**
- Observed in code: `src/common/runtime/acp/acp_session_lifecycle.cpp` (reset in `ResetAcpRuntimeState`, save and restore in `StartAcpProcessForChat`) and `src/common/runtime/acp/acp_session_runtime.cpp` (crash restart save and restore).
- Runtime impact is a hypothesis.

**Reproduction status:** not attempted, because no native build is possible on Linux.

**Acceptance criteria:**
- [ ] Both restart paths restore `goal_review_repair_attempts` with the rest of the review state.
- [ ] A test covers an invalid repair reply after a restart: the goal blocks without a second repair turn.

**Review:** found by an independent Opus re-check of the #363 slice; not separately reviewed as a report.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `feat(goals): route the goal review through a decision interface`

**Base / head:** `uam/v5.2-sync-main` ← `v5/feat-363-decision-interface`

```markdown
UAM's goal review recorded who decided as a plain string, with no shared interface. This adds the first slice of #363: one decision interface (Choice, Score and Probability, each with a reason or evidence, attribution, latency and usage), with the chat provider as the only backend. The UAM-managed goal review now answers through it as a Choice of continue, blocked or complete.

- **Attribution:** only a reply in which the backend reads a valid answer is attributed to a model. Goals save the backend id (`chat_provider`) beside the existing decision source, which keeps working as before. A filter, a fallback or a bare `model:` string can never carry a backend.
- **Diagnostics:** each review decision writes a `goal_review_decision` goal-loop diagnostic with the answer, backend, latency and usage, including the JSON repair turn. Goal-loop diagnostics now record the size of prompts and replies instead of a text prefix of them.
- **Not included:** no UI change. Hosted and local backends, the Settings backend picker, comparative latency and cost measurement, and the attention and turn-review uses stay open; see the "Decision model" open decisions in `docs/documentation-review.md`.

Related to #363.

**Proof:**
- `python3 scripts/check_docs.py` passes (203 pages, 1516 local links, 6 providers × 32 capabilities).
- `g++ 13` and `clang++ 18` `-fsyntax-only` are clean on every changed C++ file. CEF headers were stubbed, because the native desktop build refuses Linux and the CEF and SQLite downloads are blocked here. No warnings come from the changed files.
- In a scratch harness linking `decision_service.cpp`, `goal_service.cpp` and `app_models.cpp`, these pass: three new DecisionService tests and the five existing `ParseReviewDecision` tests (to cover the shared-parser refactor).
- Not run:
  - `uam_core_tests`, so not the new `GoalServiceRecordsDecisionBackendOnlyForModelDecisions`, `GoalReviewDecisionDiagnosticReportsBackendLatencyAndUsage` or `AcpGoalReviewRepairKeepsOneDecisionTimeAndCost`, nor the extended decision-source tests;
  - the full native build;
  - the runner build (its configure step needs the blocked SQLite download; this change does not touch runner sources);
  - any live provider run.
- macOS and Windows CI still need to run.

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 — suggested 11; implemented 11.
- Agent 2: claude-opus-5-5 (re-check) — suggested 5; implemented 5. The round-2 fixes have not been independently re-reviewed.

**Created with:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
```

**Screens needing Before/After capture:** none. The change has no UI or visible interaction change; the goal window's Last decision row is unchanged.
