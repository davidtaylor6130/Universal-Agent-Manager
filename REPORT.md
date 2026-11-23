The review loop is done and all fixes are pushed. Final SHA of `v5/fix-uam-control-approvals-limits`: **`9daf4805dea16da1671352a02ee7004dfe2b8fd2`**. The new core tests have been syntax-checked only, not compiled or run: `uam_core_tests` needs CEF, which I couldn't download here.

**Commits**
- `f028e4d3`: the three fixes, with their docs.
- `b7bd0b37`: review round 1 fixes.
- `9daf4805`: review round 2 fixes.

## Review rounds

The reviewers were Opus sub-agents, each with a fresh context. I launched them with the `opus` model alias; the exact model ID was not recorded.

**Round 1 (full review of the branch): 5 suggested, 3 implemented.** It found 0 blockers, 1 should-fix and 4 nits.
1. **Should-fix, implemented.** A child run can outlive its finished parent, so the four-top-level-run cap alone did not stop one chat filling the shared 32-run queue. Delegation is now also refused once a chat has 20 live runs at any depth (four runs with four children each). The test was extended.
2. **Nit, implemented.** The rule that subagents cannot create goals was defined twice; the UAM tools page now links to the goals page.
3. **Nit, declined.** It suggested rewording the per-turn refusal. The current text matches `chat_create`'s existing refusal, and the CLI View per-connection limit is already documented.
4. **Nit, implemented.** The documentation-review wording about the existing children cap was corrected.
5. **Nit, declined.** It suggested a shared helper for the live-run count at four existing scheduler sites. Changing those sites is a refactor outside this fix; the new checks share one local helper.

**Round 2 (re-check of the round 1 fix): 3 suggested, 2 implemented.** It found 0 blockers, 0 should-fix and 3 nits, and confirmed the gap is closed.
1. **Nit, implemented.** The 20 limit is checked only when an agent delegates, so mentions and follow-ups can still reach 23. The docs now describe it as a delegation refusal and record that boundary. 23 is still under 32.
2. **Nit, implemented.** A test now covers a child delegated by a live parent being refused at 20.
3. **Nit, declined.** Resuming an interrupted run checks only the shared queue. This was already true before this branch, and only the user can start a resume. It is recorded in `docs/documentation-review.md` instead.

No compile errors were found in either round.

## Screens needing Before/After capture

No React code changed, but two screens behave differently and were not captured (no macOS or iOS here):
- **Phone, UAM Allow/Deny card.** Before: tapping Allow applied it immediately. After: the phone shows "Confirm this change on your computer" (iOS) or "Confirm this change on the desktop" (browser), and the card stays open.
- **Desktop, phone-confirmation prompt.** After: it lists the held request as "Allow Computer Use for this chat", "Allow the agent to start a new chat" or "Allow the agent to create a goal".

## Issue previews

````markdown
bug(companion): phone can allow agent Computer Use, chat and goal requests without desktop confirmation

Issue #937 requires phone changes that let an agent do more to wait for desktop confirmation. Answering Allow on a UAM Control approval card (Computer Use, starting a chat, creating a goal) was not on that list, so it applied straight from the phone.

**Expected:** an Allow from the phone waits for confirmation on the desktop; Deny and question answers apply directly.
**Actual:** the Allow applied from the phone.

**Evidence:** found by source review; `LooseningRequest` in `src/common/companion/desktop_confirmation.h` had no case for `resolveAcpUserInput`.
**Reproduction status:** not reproduced at runtime.

**Acceptance criteria:**
- [ ] An Allow from the phone on these three cards is held for the desktop.
- [ ] Deny and question answers still apply from the phone.
- [ ] Unit test and phone-access docs updated.

**Review:** self-review only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
````

````markdown
bug(uam-control): managed subagents can create goals on their parent chat

A managed subagent's UAM connection acts on its root chat, so `goal_create` from a subagent created and started a goal on the parent chat. Starting chats and watching pull requests already refuse subagents.

**Expected:** subagents cannot create goals, and the tool is not listed for them.
**Actual:** the goal was created on the parent chat (immediately in YOLO, otherwise after approval).

**Evidence:** source review of `src/app/uam_control_service.cpp`: the capability's chat is set to the root, and `GoalCreate` writes to it.
**Reproduction status:** not reproduced at runtime.

**Acceptance criteria:**
- [ ] `goal_create` is refused for subagents on every path, including an approval that is already waiting.
- [ ] Subagents' tool list omits `goal_create`, and `goal_get` reports it as disabled.

**Review:** self-review only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
````

````markdown
bug(uam-control): one chat can fill the shared delegation queue

`agent_delegate` needs no approval in any mode. Top-level runs were bounded only by the app-wide 32-run queue, so one chat could take every place and block other chats' agents.

**Expected:** each chat has its own limit on live agents and a per-turn limit, like `chat_create`.
**Actual:** only the global limit applied.

**Evidence:** source review of `AgentRunScheduler::Enqueue`, which checked direct children only when a parent run exists.
**Reproduction status:** not reproduced at runtime.

**Acceptance criteria:**
- [ ] Per-chat limits on live agents.
- [ ] A per-turn cap per caller.
- [ ] Refusals are one plain line; tests and UAM tools docs updated.

**Review:** self-review only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
````

## PR preview

````markdown
fix(uam-control): confirm phone approvals and bound subagent goals and delegation

Closes three UAM Control authorisation gaps.

- **Phone approvals:** an Allow from the phone on a Computer Use, chat-creation or goal-creation card now waits for desktop confirmation. Deny and question answers still apply from the phone.
- **Subagent goals:** managed subagents can no longer create goals, which landed on their parent chat. `goal_create` is refused on every path and hidden from them, and `goal_get` reports it as disabled.
- **Delegation limits:** a delegation is refused when the chat already has four top-level agents or 20 agents at any depth queued or running, or when the caller has started eight this turn. One chat therefore cannot fill the shared 32-run queue.

Docs updated: phone access, the companion protocol reference, UAM tools and goals. The per-turn figure of eight is recorded as an interim default in `docs/documentation-review.md`. Related to #937.

**Proof:**
- `uam_companion_protocol_tests`, built directly with g++ 13.3 on Linux: all cases pass. The new `DesktopConfirmationHoldsUamApprovalAllow` fails on the unfixed header and passes with the fix.
- `uam_runner` builds with the runner-only CMake configuration, using a local SQLite 3.54.1 because sqlite.org was blocked.
- `uam_control_service.cpp`, `agent_run_scheduler.cpp` and the changed core test files pass a g++ syntax check against stub CEF headers.
- `python3 scripts/check_docs.py` passes.
- Not run: `uam_core_tests`, including `UamControlRefusesGoalCreationFromManagedSubagents` and `DelegationLimitsLiveAgentsPerChatAndPerTurn`; full CTest; macOS and Windows builds; live phone and provider checks.

**Review:** 2 independent reviewers over 2 iterations.

- Independent Opus reviewer, full review (`opus` alias; exact model ID not recorded): 5 suggested, 3 implemented.
- Independent Opus re-check of the round 1 fix (`opus` alias; exact model ID not recorded): 3 suggested, 2 implemented.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
````
