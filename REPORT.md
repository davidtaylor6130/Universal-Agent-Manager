# Accessibility fix A: chat announcements

The work branch `v5/fix-a11y-chat-announcements` is pushed. Final SHA: `7f585961379c78bef0f99137607f401f903fd90f`. It has three commits on `origin/uam/v5.2-sync-main`. No PR, issue or comment was created.

I re-checked all three audit findings in the code before fixing them:
1. **Running timer re-announced every second.** The `turn-starting` line was `role="status"` around `Working Ns`, and each `ChatStatusStrip` was `role="status"` around its ticking timer. Neither is a live region any more, and no timer is inside one.
2. **Nothing announced when a reply finished or failed.** Each open chat now has one persistent polite region, empty at first, outside the busy message list. It announces only state changes: "Reply started.", "Waiting for your permission." or "Waiting for your answer.", and one of "Reply finished.", "Reply stopped." or "Reply failed.". It also announces a status divider's title once when the divider opens.
3. **Delivery marker was a focusable live region.** It no longer has `tabIndex` or `role="status"`, and its explanation is read with the message. "Message not sent." is announced once, only when an in-flight message becomes definitely unsent; uncertain delivery is never announced.

**One trade-off needs your decision.** Sighted keyboard-only users can no longer open the delivery-explanation tooltip. I recorded this as an open decision in `docs/documentation-review.md`; it is not a silent change to the agreement.

There is no visual change, apart from removing focus-only CSS that had no effect.

## Review rounds

- **Round 1** (fresh-context claude-opus-5-5): 10 suggested (0 blocker, 4 should-fix, 6 nit), 8 implemented. The fixes:
  - A turn that ends with no reply text reads "Reply stopped.", not "Reply finished.".
  - No second outcome line when an alert or divider already announces it.
  - The region is keyed to the chat, kept out of copied text and cleared after 7 seconds.
  - Leftover focus CSS removed.
  - Comment corrected and tests added.
  - The keyboard-tooltip gap is recorded as an open decision.

  Declined:
  - #7, several panes announce without naming their chat: ChatView has no pane-count signal inside the allowed transcript scope. Documented in `accessibility.md`.
  - #8, the Radix tooltip's `aria-describedby` repeats the explanation when a screen-reader user hovers: fixing it means changing the shared Tooltip primitive, and it affects hover only.
- **Round 2** (one fresh-context claude-opus-5-5 re-check, because round 1's fixes were material): 4 suggested (0 blocker, 2 should-fix, 2 nit), 4 implemented.
  - Whether a reply exists now comes from the turn's own data, which arrives together with `processing=false`, so a reply whose row hasn't loaded yet still reads as finished.
  - The outcome line is skipped only when an error alert actually shows and appeared during the turn, so a dismissed or unchanged alert no longer hides a failure.
  - Goal or queued continuations read the previous turn as finished.
  - The new test now restores real timers.
- **Totals:** 14 suggested, 12 implemented.

## Issue previews (not filed)

1. **`bug(chat): delivery explanation cannot be opened from the keyboard`**
   The outgoing-message marker (Sending, Not sent, Delivery not confirmed) is no longer a Tab stop or a live region. Its explanation tooltip now opens only on hover, and Sending and Delivery-not-confirmed rows have nothing focusable.
   **Expected:** a keyboard-only user can read the explanation.
   **Evidence:** observed in code (`ConversationTurn.tsx`); recorded in `docs/documentation-review.md#delivery-explanation-from-the-keyboard`.
   **Acceptance criteria:**
   - [ ] The explanation is visible while keyboard focus is in the message footer or its recovery actions, or is shown as text.
   - [ ] The marker stays out of the Tab order and out of live regions.
   **Review:** not independently reviewed (report only).
   **Prepared by:** claude-opus-5-5 in Claude Code (cloud session), on behalf of David Taylor.

2. **`bug(chat): Cancel turn after reply text is announced as finished`**
   Cancel turn sets no `last_stop_reason`, and the interface only learns of the interruption once the reloaded reply row carries `interrupted`. A cancel after some reply text can therefore read "Reply finished.".
   **Expected:** "Reply stopped.".
   **Evidence:** reviewer code reading (the cancel path in `acp_session_runtime.cpp` around line 6707; native tracks `session.last_turn_outcome` but does not send it). Not reproduced live.
   **Acceptance criteria:**
   - [ ] The turn outcome reaches the UI binding.
   - [ ] The announcement uses it.
   **Prepared by:** claude-opus-5-5 in Claude Code (cloud session), on behalf of David Taylor.

3. **`bug(chat): unsent-message Retry/Remove failure bypasses the notification system`**
   `OutgoingRecoveryActions` shows its failure as an inline `role="alert"` line. The notifications agreement and your preference send every user-facing error through `reportAppFailure` as one plain line. This was there before this change and is outside this task's scope.
   **Acceptance criteria:**
   - [ ] The failure is reported once through the shared notification path; the inline line is removed or kept only as a persistent state.
   **Prepared by:** claude-opus-5-5 in Claude Code (cloud session), on behalf of David Taylor.

4. **`feature(chat): name the chat in announcements when several panes are open`**
   Each pane's region announces "Reply finished." without saying which chat it is.
   **Acceptance criteria:**
   - [ ] With several panes, the line names its chat, or only the focused pane announces.
   **Prepared by:** claude-opus-5-5 in Claude Code (cloud session), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `fix(chat): announce chat state changes once instead of ticking timers`

> The chat's working line and status dividers wrapped their elapsed timers in live regions, so screen readers re-read "Working 3s, Working 4s…" every second. Nothing announced when a reply finished or failed, and every outgoing message's delivery marker was its own Tab stop and live region.
>
> Each open chat now has one persistent polite announcement region, outside the message list and excluded from copied text. It announces state changes only:
> - a reply starting, or waiting for the user's permission or answer;
> - one outcome line: "Reply finished.", "Reply stopped." or "Reply failed.";
> - a status divider opening, by its title;
> - a message becoming definitely not sent: "Message not sent.".
>
> The state a chat opened in is not announced, and each line clears after 7 seconds. When an error alert that appeared during the turn, or the stopped divider, already carries the outcome, no second line is added. The delivery marker is no longer focusable or live, and its explanation is read with the message. Its keyboard-only route is recorded as an open decision. Docs updated: `docs/ui/accessibility.md`, `docs/ui/notifications.md`, `docs/chat/sending.md`, `docs/documentation-review.md`. No provider capability rows change: this is shared UI presentation.
>
> Visual evidence: no visual change is intended. Before/After captures are pending (screens listed below).
>
> **Proof:**
> - Checks run on the final revision, all passing:
>   - `npx tsc --noEmit -p .`
>   - `npx vitest run`: 239 files, 4228 tests
>   - `npm run build`
>   - `python3 scripts/check_docs.py`
> - Regression: against the base source, the 18 new or updated cases from the first commit fail; they pass with the fix.
> - Not run:
>   - The native runner build, because no native or `src/` code changed.
>   - A live screen-reader check (VoiceOver or NVDA).
>   - A GUI visual check (the desktop build refuses Linux).
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 14 suggested, 12 implemented. Declined:
> - Multi-pane chat naming: no pane signal within the allowed scope.
> - The tooltip's duplicate description on hover: needs a change to the shared Tooltip primitive.
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

Your skill forbids a "Generated with" footer, so I left out the session's default PR footer.

## Screens needing Before/After capture (dark and light themes)

1. The transcript while a turn starts: the "Working Ns" line, and the live turn header with its clock.
2. Status dividers: Refreshing history, Compacting context, Waiting on subagents, and the stopped divider ("Stopped after timeout" or "Response interrupted").
3. Outgoing message markers:
   - Sending
   - Not sent with Edit, Retry and Remove
   - Held (Not sent without Retry)
   - Delivery not confirmed
   - Not sent yet (automation)

   Capture each on hover (tooltip) and while tabbing through the row; the marker should no longer take focus.
4. The composer error notice after a failed reply, and the timeout stop notice (both unchanged).
