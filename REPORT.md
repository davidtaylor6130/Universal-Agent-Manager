# #363 decision interface (first slice): report

The goal review now answers through the decision interface. David's 2026-11-23 decisions are recorded, and the work is pushed to `v5/feat-363-decision-interface`. No PR, issue or comment was created.

**Final SHA:** `0dec7eaf4a8a55e1aa25b941778872759510ac42`. That is 5 commits on top of `origin/uam/v5.2-sync-main`: `d52ed520`, `2949e512`, `27e53b79`, `6ce9d77f` and `0dec7eaf`.

## What changed

- **Decision interface:** new `src/app/decision_service.{h,cpp}`.
  - It answers Choice, Score and Probability requests.
  - Each answer carries a reason and evidence, who decided (a model through a backend, a UAM rule, or a fallback), latency in ms and usage.
  - A `DecisionBackend` interface has one backend: the chat provider.
- **Who decided:**
  - The backend always reads the answer from its own reply text; a caller's check can only reject it.
  - Only `DecisionService::FromReply` can attribute a decision to a model. Rule and fallback decisions cannot name one, and a bare `model:` string is recorded as `fallback`.
  - The attribution is only as true as the reply text the caller passes in. The header says so; the types do not enforce it.
- **Goal review:**
  - It is now a Choice of continue, blocked or complete, read from the `decision` key. `done`, `passed`, `stop` and `yes` still mean complete; the parser and the interface share one alias list.
  - A reply with neither a reason nor evidence is no longer a model decision. It gets the single repair turn and then falls back. This is the only behaviour change: a comparison against the base parser over 37,800 generated replies found it in 520 replies and no other difference.
  - Goals save `lastDecisionBackend` (`chat_provider`) beside `lastDecisionSource`, which keeps working as before. A saved backend next to any non-model source is dropped on save and load.
  - Every write of the decision source goes through `RecordDecision` or `ClearDecision`.
- **Diagnostics:**
  - Each review decision writes a `goal_review_decision` diagnostic with the answer, backend, latency and usage. Latency and usage cover the repair turn, and usage is unknown when any turn's count is missing, including after a recovered repair.
  - Goal-loop diagnostics now record only the size of prompts and replies, not the first 160 characters of them, which the logging rules forbid. Failures keep their cause as one short message.
- **Decisions recorded** in `docs/documentation-review.md` ("Decision model", Resolved by David, 2026-11-23):
  - #363's "turn review" is the goal-loop review, which already uses the interface; the issue does not ask for a separate per-turn review.
  - A hosted decision backend such as Jev may receive goal or transcript text only after the user opts in. No hosted backend exists yet, so nothing is sent today.
- **Docs:** `docs/chat/goals.md` now carries both decisions as agreements, and its gap note no longer lists the turn review. `docs/reference/source-map.md` is updated. The six provider `goals` rows stay Partial.
- **UI:** none.
- **Permission-rules branch:** no permission AI Review files are touched. `git merge-tree` against `origin/v5/feat-ai-review-command-rules` merges cleanly.

## Review rounds

All three reviewers were fresh-context Opus agents (`claude-opus-5-5`).

| Round | Reviewer and scope | Suggested | Implemented |
|---|---|---|---|
| 1 | Agent 1: the initial slice | 11 (0 blocker, 6 should-fix, 5 nit) | 11 |
| 2 | Agent 2: re-check of the round-1 fixes | 5 (0 blocker, 2 should-fix, 3 nit) | 5 |
| 3 | Agent 3: the round-2 fixes and the recorded decisions | 9 (0 blocker, 1 should-fix, 8 nit) | 9 |

None were declined. Two notes:
- **Round 3:** I ran no further re-check after it. Its fixes were tests, comments, docs wording, the repair-prompt wording and one shared alias list. The alias change was checked against the base parser over the same 37,800 replies, with no new difference.
- **Round 1:** while fixing nit 10, I also fixed a related problem: request text could close the `</decision_request>` marker in the prompt, because JSON does not escape `<`.

## Open questions

1. Are hosted Jev early-access credentials available? This blocks only the hosted backend.
2. How would a local Laya backend be packaged and shipped?
3. Should Permission AI Review (getting allow/ask/deny rules on `v5/feat-ai-review-command-rules`) later answer through this interface, or stay a separate reviewer?
4. Where should the backend and the reason show for the user: the goal window's Last decision row, diagnostics only, or both? The goal window is unchanged. The Settings backend picker waits until a second backend exists.

Still open under #363 after this slice:
- the attention flag going through the interface;
- latency and cost measured against the text-model path;
- hosted (opt-in) and local backends.

## New issue preview (not filed)

**Title:** `bug(goals): a goal review can be repaired twice after a provider restart`

A goal review gets one JSON repair turn. Two session restart paths restore most of the in-flight review state but not the repair count, so a review can be repaired a second time after a restart. The round-2 reviewer found this while reading the code for #363; it predates that work. With #363, the second repair keeps the cost of every earlier turn; a test covers that.

**Reproduction:** no exact steps are recorded. From reading the code:
1. A goal review's first reply is invalid, so UAM queues the repair turn.
2. The provider process restarts before the repair settles, through `StartAcpProcessForChat` during remote recovery or the startup-crash auto-restart.
3. `ResetAcpRuntimeState` sets `goal_review_repair_attempts` to 0, and neither restart path restores it.

**Expected:** the restored review keeps its repair count, so an invalid repair reply blocks the goal.

**Actual (from the code):** the repair count is 0 after the restart. An invalid repair reply then gets a second repair turn, which spends extra reviewer tokens.

**Environment:** `origin/uam/v5.2-sync-main` at `c252f9a9`, all six providers' UAM-managed goals.

**Evidence:**
- Observed in code: `src/common/runtime/acp/acp_session_lifecycle.cpp` (`ResetAcpRuntimeState`, the `StartAcpProcessForChat` save and restore) and `src/common/runtime/acp/acp_session_runtime.cpp` (crash restart save and restore).
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
UAM's goal review recorded who decided as a plain string, with no shared interface. This adds the first slice of #363: one decision interface (Choice, Score and Probability, each with a reason or evidence, attribution, latency and usage), with the chat provider as the only backend. The UAM-managed goal review, which #363 calls the turn review, now answers through it as a Choice of continue, blocked or complete.

- **Attribution:** the backend reads the answer from its reply. Only an answer it reads and checks is attributed to a model. Goals save the backend id (`chat_provider`) beside the existing decision source, which keeps working as before. A filter, a fallback or a bare `model:` string can never carry a backend.
- **Behaviour change:** a review reply with neither a reason nor evidence is now repaired and then falls back, instead of being accepted.
- **Diagnostics:** each review decision writes a `goal_review_decision` goal-loop diagnostic with the answer, backend, latency and usage, including the JSON repair turn. Goal-loop diagnostics now record the size of prompts and replies instead of a text prefix of them, and failures keep a short cause.
- **Decisions:** David's 2026-11-23 decisions are recorded in `docs/documentation-review.md`. The turn review is the goal-loop review, and a hosted backend may receive goal or transcript text only after the user opts in.
- **Not included:** no UI change. Hosted and local backends, the Settings picker, comparative latency and cost measurement, and the attention flag stay open.

Related to #363.

**Proof:**
- `python3 scripts/check_docs.py` passes (203 pages, 1517 local links, 6 providers × 32 capabilities).
- `g++ 13` and `clang++ 18` `-fsyntax-only` are clean on every changed C++ file. CEF headers were stubbed, because the native desktop build refuses Linux and the CEF and SQLite downloads are blocked here.
- In a scratch harness, these pass: the 3 pure DecisionService tests and the 5 existing `ParseReviewDecision` tests.
- A comparison of the base and new review parsing over 37,800 generated replies differs only for the 520 replies with neither a reason nor evidence.
- Not run:
  - `uam_core_tests`, including the new goal-loop, recovery and diagnostic cases;
  - the native build;
  - the runner build (its configure step needs the blocked SQLite download; no runner source changed);
  - any live provider.
- macOS and Windows CI still need to run.

**Review:** Reviewed by 3 agents over 3 iterations.

- Agent 1: claude-opus-5-5 — suggested 11; implemented 11.
- Agent 2: claude-opus-5-5 — suggested 5; implemented 5.
- Agent 3: claude-opus-5-5 — suggested 9; implemented 9. Its fixes (tests, comments, docs wording, one shared alias list checked by the 37,800-reply comparison) were not re-reviewed.

**Created with:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
```

**Screens needing Before/After capture:** none. The change has no UI or visible interaction change; the goal window's Last decision row is unchanged.
