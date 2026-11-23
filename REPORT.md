Work branch `v5/fix-automation-limit-inheritance` is pushed. Final SHA: `ce28dcfec457f73d0dbde319628766ab6a87abad`.

No test has run yet. On Linux I could only check that the code compiles (syntax only); the test suite still needs to run on macOS or Windows.

**Review rounds**
- **Round 1:** a fresh-context Opus reviewer looked at the full diff. 6 suggested (0 blockers, 4 should-fix, 2 nits). 5 implemented in full, 1 in part.
  - **Implemented:**
    - A launch that failed after the send was accepted no longer holds a place under the token.
    - A chat started by an automation chat counts as open for as long as it exists. Before, it stopped counting when idle, yet a pull-request watch can wake it without checking the token.
    - Retrying an identical `chat_create` returns the chat already started, even when the token is full.
    - The goals docs no longer overclaim: they now say the cap does not bound provider-managed goals or older side-chat goals.
    - `uam-tools.md` links to the automation API page instead of restating its rules.
  - **Partly implemented (the tests nit):** I added the expired-token, `goal_create`-with-approval, failed-launch and branch-running-turn tests. I did not add a goal-loop-level test of the iteration cap stopping a goal: it needs a full goal-loop fixture, and that path is a one-line call to the `IterationLimit(owner, …)` overload, which is unit-tested.
- **Round 2:** a fresh-context Opus re-check of the round-1 fixes. 4 suggested (0 blockers, 2 should-fix, 2 nits). All 4 implemented.
  - The full-token messages now say that started chats count until they are deleted.
  - A test covers the approval path that Default-mode automation chats use.
  - A failed launch still counts while a turn is running, waiting for the user or running in CLI View.
  - The docs say branches of a started chat also count for as long as they exist.

**What changed**
- **`chat_create` from an automation chat:**
  - The new chat keeps the token, the iteration cap and the Git permission, but not the approval hold. Its first prompt states the Git rule.
  - It counts against the token's 16 open chats.
  - The start is refused when the token is revoked, expired, unreadable or full, and for a provider that answers its own permission prompts (Antigravity).
- **Branches of an automation chat:** they keep the same limits.
- **Goals:** every goal created in an automation chat or one of its side chats gets the chat's cap and Git rule. This includes goals from `goal_create`, whether run straight away or after approval.
- **Status:** each token still reads only the chats it created itself.
- **Docs:** updated, and `check_docs.py` passes.

**Open questions for David**
- Should started chats count until they are deleted?
- Should branches and side-chat goals be covered?
- Should the Git rule go in a started chat's first prompt?
- Should the token's status read stay limited to the chats it created?
- Two gaps remain open:
  - A pull-request watch can still wake an idle API chat or branch outside the token limit; this predates this change.
  - Provider-managed goals are not bounded by UAM's iteration cap.
- I couldn't find the 2026-11-11 decision in the docs on this branch.

**Screens needing Before/After capture:** none. There is no UI change.

---

**Issue preview 1**

```
bug(automation): chats and goals started from an automation chat lose its limits

A chat that an automation-API chat starts with `chat_create`, or a branch of one, was not linked to the automation token. It had no iteration cap or Git policy, did not count toward the token's 16-open-chat limit, and was not covered by the automation-chat secret refusal. A goal the model created with `goal_create` in an automation chat used only the Settings iteration cap and carried no Git policy. Issue #517 requires chats started by a chat to use the automation trust and Git model.

**Expected:** chats and goals started from an automation chat keep its iteration cap, Git permission and token limits, and are never more permissive than it.
**Actual:** `chat_create` copied only the approval mode, safety tier, UAM Control setting and agent; branches copied none of the automation fields; `goal_create` set no cap.
**Environment:** uam/v5.2-sync-main at 6f235eb9.
**Evidence:** source inspection of `uam_control_service.cpp` (chat_create copy, GoalCreate), `automation_api_service.cpp` (cap and policy set only on the launch goal), `goal_service.cpp` (per-goal cap) and `chat_domain_service.cpp` (branch copy).
**Reproduction status:** established from source; not reproduced in a running app.

**Acceptance criteria:**
- [ ] Chats started or branched from an automation chat keep its token, iteration cap and Git permission.
- [ ] They count toward the token's open-chat limit, and chat_create is refused when the token is full, revoked or expired.
- [ ] Every goal in an automation chat carries the chat's cap and Git policy.
- [ ] No existing restriction is relaxed.

**Review:** not reviewed.
**Prepared by:** Claude Opus 5.5 in Claude Code on the web.
```

**Issue preview 2 (the remaining gap)**

```
bug(automation): pull request watch wakes are not checked against the automation token

An automation-API chat, or a branch of one, counts toward its token's 16 open chats only while it waits for approval, has an active goal or has a turn queued or running. Once idle it can still watch a pull request, and a watch wake sends it a new prompt without checking the token, so watched chats can run beyond the token's open-chat limit. Chats started from automation chats are not affected, because they count for as long as they exist.

**Expected:** an automatic wake of a chat under an automation token respects the token's open-chat limit and validity (revoked or expired tokens).
**Actual:** `pr_watch_service.cpp` sends the wake prompt with no token check.
**Environment:** v5/fix-automation-limit-inheritance at ce28dcfe.
**Evidence:** source inspection of `pr_watch_service.cpp` and `ChatIsOpen` in `automation_api_service.cpp`.
**Reproduction status:** established from source; not reproduced in a running app.

**Acceptance criteria:**
- [ ] A watch wake of a chat under an automation token is held or refused while the token is full, revoked or expired.
- [ ] Other automatic wakes (stalled-goal resume, managed-parent continuation) are checked the same way, or the gap is recorded.

**Review:** not reviewed.
**Prepared by:** Claude Opus 5.5 in Claude Code on the web.
```

**PR preview** (no PR opened)

```
fix(automation): keep automation limits on chats and goals started from automation chats

Chats an automation chat started with `chat_create`, and branches of one, lost the automation token link, so they had no iteration cap or Git policy, escaped the token's open-chat limit and could request secrets. Goals the model created in an automation chat used only the Settings cap and carried no Git policy.

Now `chat_create` from an automation chat copies the token, cap and Git permission (never the approval hold) and states the Git policy in the first prompt. A new start is refused when the token is revoked, expired, unreadable or has 16 open chats, or for a provider that answers its own permission prompts; a retry with identical arguments still returns the chat it started. Started chats, and branches of them, count toward the token for as long as they exist; other chats under the token count while waiting, with an active goal, or with a turn queued or running, and a launch that failed after acceptance waits for the user instead. Branches of an automation chat keep its limits. Every goal created in an automation chat or its side chats gets the chat's cap and Git policy, and the iteration check applies the chat's cap to its goals saved without one. Token status stays limited to chats the token created. Docs and open decisions are updated.

Related to #517

**Proof:**
- `g++ -std=c++20 -fsyntax-only` (Linux, stand-in CEF headers) on the 6 changed sources and `tests/uam_control_chat_create_tests.cpp` at ce28dcfe: no errors.
- `python3 scripts/check_docs.py`: passed.
- Not run: `uam_core_tests` build and the 8 new cases, ctest, macOS and Windows builds.

**Review:** 1 independent reviewer, 2 iterations. Independent Claude Opus 5.5: 6 suggested, 5 implemented (the 6th, a goal-loop-level cap test, partly). Fresh-context Claude Opus 5.5 re-check: 4 suggested, 4 implemented.

**Created with:** Claude Opus 5.5 in Claude Code on the web.
```
