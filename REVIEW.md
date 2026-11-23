# UAM V5.2 review R1

I reviewed all four branches against `origin/uam/v5.2-sync-main`, read-only (nothing edited, committed or pushed). One needs rework (#526), two need fixes first (#1029, #278) and one can go in with nits (#984).

What I could run here: the docs check passes on all four; `tsc -b` passes on #1029 and #278; the full ChatView Vitest suite passes on both (501/501 on #1029, 499/499 on #278); and #1029 and #278 merge together cleanly. Native code could not be built here. For #984 and #526 I compiled only the new helper or header with g++ (C++20).

## v5/fix-1029-composer-base-branch (#1029)

1. **should-fix**, `src/app/git_worktree_service.cpp:245`
   - **Problem:** the refusal "Local copy is on X, not Y." no longer tells the user what to do. It also fires for chats with no starting-branch override, when the mismatch comes from the default base branch in Git settings. Those chats still show the checked-out branch in the composer (as acceptance criterion 3 requires), so the line gives no clue to the cause or the fix. `docs/documentation-review.md:299` was also changed to carve this message out of the "each names its cause and an action" agreement, rather than keeping the agreement.
   - **Fix:** keep one line that includes the action, for example "Local copy is on main, not build/x. Check out build/x or turn off Pull before first chat." Revert the documentation-review exception, and update the test string at `tests/git_worktree_pull_tests.cpp:187`.
2. **nit**, `UI-V2/src/components/views/ChatView.tsx:4194`
   - **Problem:** the outer `composerBranch && …` check hides the chosen base whenever the branch read is pending or failed, because a pending chat has an empty `workspaceBranchName`.
   - **Fix:** guard with `(composerBranch || composerChosenBase)` and fall back to the chosen base for the label.

**Not verified:** no visual check of the tooltip or the two themes was possible here. The required Claude review and David's sign-off for a UI change are still outstanding.

**Verdict:** approve after fixes. **Suggestions:** 2.

## v5/fix-278-completed-fallback-duplicate (#278)

1. **should-fix**, `UI-V2/src/components/views/ChatView.tsx:1255-1260`
   - **Problem:** only the completed path (`!acp.processing`) was widened. `liveTurnOwnsLatestAssistant` still requires `latestAssistantMessageIndex > latestUserMessageIndex`. So a turn that is still running, with a stale anchor and a steer or follow-up row after its answer, still renders the answer twice, and the second card holds the next tool call. That is exactly the 9 Aug Codex regression in the issue, so the reported case is probably still unfixed. The commit says "Refs", not "Fixes".
   - **Fix:** apply the same alternative condition to `liveTurnOwnsLatestAssistant`: the latest assistant row comes after the turn's own user row, that row is a user row, and it is at or after `earliestRenderedMessageIndex`. Add a test with `processing: true`, `turnAssistantMessageIndex: 5` and a follow-up row, and update the processing sentence in `docs/chat/messages.md`.
2. **nit**, `ChatView.tsx:1272-1273`
   - **Problem:** the new clause does not require `latestAssistantMessageIndex >= earliestRenderedMessageIndex`. If the matching row is on an unloaded older page, the trailing card is suppressed and the answer is not visible.
   - **Fix:** add that bound.

**Verdict:** approve after fixes. **Suggestions:** 2.

## v5/fix-984-cef-remote-debugging (#984)

The new helper compiles under g++ (C++20), and a driver mirroring the new test's assertions passes. From memory of the CEF API, `Reset`, `GetSwitches`, `GetArguments`, `AppendSwitch(WithValue)` and `AppendArgument` exist with these signatures, but the CEF headers are not in this checkout, so I could not confirm them. The command line is rebuilt only when a switch is dropped, so normal launches are untouched. The filter does not relax any permission, and `tests/html_view_isolation_browser_test.py` launches Chrome rather than UAM, so it is unaffected.

1. **nit**, `src/cef/uam_cef_app.cpp:409`
   - **Problem:** in a test run, where only `remote-debugging-address` is dropped, the log still says "outside an explicit test run".
   - **Fix:** use a separate message for that case, for example "Ignored --remote-debugging-address; DevTools stays on 127.0.0.1."
2. **nit**, `docs/security-enterprise.md`
   - **Problem:** only the reference page was updated, not the agreement page.
   - **Fix:** add one clause saying a normal launch exposes no DevTools port.

**Not verified:** acceptance still needs a manual run on macOS and Windows: no port opens on a normal launch, the port opens on 127.0.0.1 with `UAM_TEST_ALLOW_REMOTE_DEBUGGING=1`, and `scripts/perf_suite.py driven` passes.

**Verdict:** approve. **Suggestions:** 2.

## v5/fix-526-cold-admission-budgets (#526)

1. **blocker**, `tests/poll_budget_clock.h:37-51`, used throughout `tests/async_prompt_durability_tests.cpp`
   - **Problem:** with `GITHUB_ACTIONS=true`, the per-input and per-poll budgets are timed in the poll thread's own CPU time. A blocked thread uses almost none, so lock waits, `future.get()`/join, fsync and disk waits on the poll thread become invisible. These are the regressions the "KeepsInputAndPollBudgets" and "DurableWritesOffPoll" cases exist to catch. I confirmed it: a 300 ms `sleep_for` measured with `PollBudgetTimer` is charged 0 ms, and `Within(100ms)` passes.
   - The durable-write counter is a backstop only where `slow_durable_writes` is armed, which is 2 of about 40 call sites (line 1857; armed only at 2637 and 2643).
   - This is a budget relaxation the issue rules out ("without budget changes"). `docs/engineering/testing.md` was rewritten to allow it without David's decision.
   - The root cause in the issue's evidence, background save threads starving the poll thread, is not addressed.
   - **Fix:** keep wall-clock budgets and fix the starvation first, by checking that the chat-save and setup workers use the existing `background_thread_priority` helper.
   - **If CPU time must stay:** also keep a wall-clock ceiling on each poll, arm the durable-write counter in every variation, and get David's approval for the testing-agreement change.
2. **should-fix**, `tests/poll_budget_clock.h:28-32` and `docs/engineering/testing.md`
   - **Problem:** the docs say acceptance runs stay on wall time. But release-candidate tag runs, and every other Actions run, also set `GITHUB_ACTIONS=true`, so release gating gets the CPU clock too.
   - **Fix:** gate on a dedicated variable set only in the hosted-runner test step, and document that variable.
3. **should-fix**, acceptance
   - **Problem:** no hosted CI run on Windows, macOS Intel or macOS ARM is cited. The issue's Windows failures were the wall-clock `completed` completion deadline (`:2233`), which this branch leaves unchanged, so the 27-case Windows failure is not shown to be fixed.
   - **Fix:** cite passing runs on all three runners, or address the completion-deadline failures.

The quarantine file has no entries for this family, so nothing needed removing there.

**Verdict:** reject; rework. **Suggestions:** 3.
