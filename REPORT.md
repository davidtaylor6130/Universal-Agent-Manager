# #518 and #965 first slices: report

**Final SHA:** `9010b26ba9f2cf5f48dce8cbb4b446c565ab3b4f` on `v5/feat-518-965-schedules`. The base is `origin/uam/v5.2-sync-main` and the branch has 3 commits on top of it. No PR, issue, comment or label was created.

## What was built

A single timer service in the desktop app (`src/app/timer_service.cpp`), with pure schedule rules in `src/common/scheduling/schedule_rules.cpp`, runs from `PollTick`. Its timers are local, so they run whether or not Phone access is on.

- **#518 automation schedules (schedules only, no webhooks):**
  - A schedule is a stored `POST /automation/chats` body.
  - Triggers are an interval from 15 minutes to 7 days, or a local time on chosen weekdays.
  - Missed runs are either skipped or run once, chosen per schedule. A run counts as missed when it is noticed more than 5 minutes late.
  - The run is saved before its chat is created, so no slot runs twice.
  - Each schedule is its own non-expiring internal credential and never leaves the app. Its trust, workspace and Git access are set in Settings. Runs use the same `CreateChat` checks as an API request.
  - Each run starts a new chat.
  - Settings → Automation has a new **Schedules** section. It lists each schedule with its last and next run and can create, edit, pause and delete schedules. These actions are refused to the phone by its allow-list.
- **#965 `schedule` and `notify` tools:**
  - `schedule {minutes 1–1440, note}` wakes the calling chat through the pull request watch delivery path.
  - Follow-ups are saved in `chat_wakes.json` and survive a restart. Delivery is at most once.
  - Limits are 3 pending per chat and 200 in total.
  - Stop, Done and deleting the chat cancel pending follow-ups.
  - `notify {message}` raises the chat's attention flag, at most once a minute, with the read-only hint and no separate push.
  - Both tools are hidden from, and refused to, subagents, side chats and goal reviewers.
- **Errors and decisions:**
  - Every user-facing failure is one line through `reportAppFailure`/`reportAppNotice`, using a new `timers.notices` state slice. This slice is desktop-only and the phone state omits it.
  - David's 2026-11-23 decisions for #518 and #965 are recorded in `docs/documentation-review.md`, together with the interim choices that still need his decision.

## Review rounds

- **Round 1** (independent `claude-opus-5-5`, fresh context): 15 suggested (0 blockers, 6 should-fix, 9 nits), 14 implemented.
  - Declined: an exact skipped-run count after more than 1000 missed weekday runs. It is cosmetic.
  - On permission-card Stop (`stopAcpSession`) cancelling follow-ups: as with PR watch, I documented the behaviour instead of changing it.
- **Round 2** (one fresh-context `claude-opus-5-5` re-check, because the fixes were material): 7 suggested (0 blockers, 1 should-fix, 6 nits), 7 implemented. It confirmed 5 of the 6 round-1 should-fix items as fixed and item 1 (clock set back) as partly fixed. The remaining part was fixed in the third commit.
- **Total:** 22 suggested, 21 implemented.

## New issue previews

One follow-up issue seems worth filing. It has not been filed.

> **feature(chat): show and cancel pending scheduled follow-ups**
>
> An agent can set up to 3 follow-ups per chat with the UAM `schedule` tool (#965). The user can only see them as tool calls in the transcript. The only ways to cancel them are Stop on a running turn, marking the chat done, or deleting it. Once the agent has ended its turn, Stop is not available.
>
> **Desired outcome:** the chat shows its pending follow-ups with their due times, and the user can cancel one without marking the chat done. The phone shows the same list read-only.
>
> **Evidence:** `docs/chat/scheduled-follow-ups.md` lists this as a known gap on `9010b26b`. Nothing was reproduced in the app.
>
> **Acceptance criteria:**
> - [ ] Each pending follow-up shows in its chat with its due time.
> - [ ] The user can cancel a single follow-up, and the change is saved.
>
> **Review:** Self-triage only.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview

**Title:** `feat(automation): add scheduled automations and the schedule and notify tools`
**Base:** `uam/v5.2-sync-main` ← **Head:** `v5/feat-518-965-schedules`

```markdown
UAM can now run things on a clock. One timer service in the desktop app fires two kinds of local timer, whether or not Phone access is on:

- **Automation schedules (#518, first slice; webhooks come later).** A schedule is a stored automation-API chat request with an interval (15 minutes to 7 days) or a local time on chosen weekdays. Each run starts a new chat under the schedule's own non-expiring internal credential, through the same checks as an API request: workspace, Git access, iteration cap, worktree isolation, provider availability and the 16-open-chats limit. Runs missed while UAM was closed are skipped or run once, chosen per schedule. A run is saved before its chat is created, so nothing runs twice. Daylight-saving changes and clocks set back are handled. Settings → Automation gains a Schedules list with last and next run, plus create, edit, pause and delete.
- **`schedule` and `notify` UAM tools (#965).** `schedule` wakes the calling chat after 1–1440 minutes with the agent's note, through the pull request watch delivery path. Follow-ups survive a restart and are cancelled by Stop, Done or deleting the chat. `notify` raises the chat's attention flag under the user's own notification rules, with no separate push. Both are unavailable to subagents, side chats and goal reviewers.

Failures reach the notifications panel as one line each. Docs, provider capability rows and David's 2026-11-23 decisions are updated.

Related to #518 (webhooks remain). Related to #965 (`task_run`/`task_status` remain).

<Before/After captures needed; see the list below.>

**Proof:**
- Frontend: `npx vitest run` passed 241 files and 4222 tests. `npx tsc --noEmit -p .` was clean and `npm run build` succeeded.
- `python3 scripts/check_docs.py` passed.
- The runner build (`-DUAM_RUNNER_ONLY=ON`, target `uam_runner`) compiled and linked. SQLite was linked from the system library because the amalgamation download is blocked in this environment.
- `uam_companion_protocol_tests` was built and run on Linux and passed 32 of 32. This includes the refusal of the four new schedule actions to the phone.
- The schedule-rule tests in `tests/timer_service_tests.cpp` were built standalone and passed 5 of 5, both under UTC and under `TZ=Europe/London`. The system clock was also checked across the 2026 London daylight-saving changes.
- Every changed native file and test compiles with `-fsyntax-only` against CEF 8078 headers.
- **Not run:**
  - `uam_core_tests`, including the new timer-service and tool cases. The desktop build refuses Linux, so these cases were only traced by the reviewers.
  - The packaged app.
  - Any live provider.
  - The macOS and Windows builds.

**Review:** Reviewed by 1 independent reviewer over 2 iterations.

- Independent claude-opus-5-5: 22 suggested, 21 implemented. Declined: an exact skipped-run count after more than 1000 missed weekday runs, which is cosmetic.

The Settings UI change still needs Claude review and David's approval under AGENTS.md.

**Created with:** Claude Opus 5.5 (claude-opus-5-5) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
```

## Screens that need Before/After capture

1. Settings → Automation, Schedules section:
   - empty state;
   - a list with a running, a paused and a failed schedule, showing next and last run;
   - the "Schedules unavailable" state.
2. The New or Edit schedule dialog:
   - in interval mode;
   - in "On chosen days" mode, with the day chips and the time field;
   - Save disabled for an unchanged edit.
3. The Delete schedule confirmation.
4. The notification toast and its bell-panel entry for a schedule that did not start a chat, and for a dropped follow-up ("Chat “…”: …").
5. A waiting chat started by a schedule: the approval card reading "Started by <schedule name>".
6. An Active Chats row after `notify`. The visuals are unchanged, so this is an "After" capture only, to confirm.
