# Report: #278, completed fallback duplicates an already-persisted assistant answer

**Work branch:** `v5/fix-278-completed-fallback-duplicate` (base `uam/v5.2-sync-main` at `6f235eb9`)
**Final SHA:** `87a327b39b519e1856cabea837821ea31385fafe`
**Commits:** `cee5eed3` (finished turn), `cd9da7b7` (running turn and rendered-window bound), `87a327b3` (running turn only binds past priority steers)

## Finding

The issue was still present on the base for all six providers, because it sits in the shared React path (`UI-V2/src/components/views/ChatView.tsx`). PR #803 had fixed a stale anchor only when the matching assistant row was the last row after the latest user row. A user row after the answer (a steer, or a queued or prepared follow-up) defeated both checks when the turn's recorded assistant index was stale:
- **Running turn:** the answer showed twice, with the next tool call in the second card. This is the Codex regression reported on 9 August.
- **Finished turn:** the whole timeline was repeated as a trailing card below the follow-up row. Base tests asserted this duplicate.

The fix binds to the latest shown assistant row when it holds exactly the turn's answer text and follows the turn's own recorded user row. While the turn is running, every later user row must be a priority steer. Otherwise an ordinary row could be the running turn's own prompt, with the stale index left on an older turn whose answer reads the same. `docs/chat/messages.md` is updated.

## Review rounds

| Round | Reviewer | Suggested | Implemented | Declined |
|---|---|---|---|---|
| 1 | Independent fresh-context claude-opus-5-5 (`agent-reports/review-R1`, #278 section) | 2 (1 should-fix, 1 nit) | 2 | 0 |
| 2 | Fresh-context claude-opus-5-5 re-check of `cd9da7b7` | 3 (1 should-fix, 2 nits) | 2 (the should-fix only on the running path) | see below |

Round 1 had no blockers, and both its findings are on the branch: the running-turn fix with a `processing: true` test, and the rendered-window bound.

**Declined or partly declined in round 2:**
- **Should-fix 1, finished-turn half:** not applied. Queued and prepared follow-up rows carry no marker that tells them apart from a prompt. Applying it would bring back the finished-turn duplicate this issue is about. The remaining case needs a stale index on an older turn with identical answer text and an unsaved answer. No text is lost there, and the answer shows at the older position.
- **Nit 2:** the running-turn hidden-row case also passes on the parent. I kept it as a guard against future changes; the finished-turn case fails on the parent.

## Issue previews

None.

## PR preview (`v5/fix-278-completed-fallback-duplicate` into `uam/v5.2-sync-main`)

**`fix(chat): render a turn's answer once when a follow-up row follows it`**

A turn's timeline could repeat an answer already in the transcript when the turn's assistant index was stale and a user row followed the answer:
- **Running turn:** the answer showed twice, with the next tool call in the second card. This is the Codex regression reported on 9 August.
- **Finished turn:** the whole timeline was repeated as a trailing card.

ChatView now binds to the latest shown assistant row when it holds exactly the turn's answer text and follows the turn's own user row:
- **Running turn:** only priority steers may follow that row. An ordinary row may be the running turn's own prompt, so the separate card stays.
- **Finished turn:** queued or prepared follow-up rows may follow it.
- **Hidden row:** a matching row hidden behind Show earlier messages leaves the card in place, so the answer stays visible.
- **Unchanged:** a pending question or permission still shows its card, as does a turn with no resolvable user row or a row with different text.

`docs/chat/messages.md` is updated.

**Proof:**
- **New tests:** ChatView cases cover a finished turn (stale index on a user row, index past the end, no recorded row), a running turn with a steer, a running turn with an ordinary follow-up row (separate card kept), a matching row outside the rendered window (running and finished) and a pending permission.
- **Fail-first:** the finished-turn cases fail on the base. The steer case and the finished hidden-row case fail on `cee5eed3`. The ordinary-row case fails on `cd9da7b7`.
- **Results:** ChatView 503/503. Frontend unit tests 4188/4188 (232 files), performance tests 33/33 (7 files). `npm run build` (tsc and vite) passes. `python3 scripts/check_docs.py` passes.
- **Not run:**
  - Native tests: CMake refuses to configure on Linux, and the change is frontend-only.
  - No live provider chat and no visual check.

**Review:** Reviewed by 2 agents over 2 iterations.
- Agent 1: claude-opus-5-5 (independent): suggested 2; implemented 2.
- Agent 2: claude-opus-5-5 (re-check): suggested 3; implemented 2. Declined: the finished-turn half of the stale-older-turn guard, because follow-up rows have no marker; and a nit whose test is kept as a guard.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

Refs #278

## Screens needing Before/After capture

1. **Running turn with a steer:** a Codex chat whose turn is still running after a steer row, with a stale anchor. Before: the answer appears in two consecutive Codex CLI cards, and the second also holds the next tool call. After: one card with the answer and the tool, above the steer row.
2. **Finished turn with a follow-up row:** a finished turn followed by a queued or prepared user row (for example after a failed reconnect). Before: the answer row, the follow-up row, then a trailing card repeating the answer. After: the answer once, above the follow-up row.
