# #524 compute pools, first slice — report

**Final SHA:** `4d330d456a8b257fdc07e9f80c07e7e89045c403` on `v5/feat-524-compute-pool-slice` (one commit on `origin/uam/v5.2-sync-main`, pushed). No PR, issue or comment was created.

## What changed
- **Status text:** a held turn now reads "Waiting for <pool> slot (2/2 in use)" (`ChatView.tsx` and its test).
- **New native tests in `tests/compute_pool_continuation_tests.cpp`** (registered in `CMakeLists.txt`). This is a new file because other branches edit `compute_pool_tests.cpp`.
  - While a one-slot pool is full and an older turn waits, manual compaction is written at once, gets no ticket and raises use to 2/1. The waiter starts only after compaction ends.
  - A native Codex steer is written at once and keeps its turn's slot, while the waiter stays held.
  - Restart test, along the route a user actually takes:
    1. A turn is held at the gate, then the app quits through the real exit stop (`FastStopAcpSessionsForExit`).
    2. A fresh app loads the saved chat, runs restore and ordinary polls, and the row stays unsent with nothing waiting.
    3. Retry is pressed with no provider running. It stages the same row, and the simulated relaunch starts it as a new turn.
    4. That turn is held at the gate again, then sent exactly once with one user row.
- **New native test in `tests/uam_control_transport_tests.cpp`**, next to its fixture: a delivered sub-agent result starts on a full pool without waiting and raises use to 2/1.
- **Docs:**
  - `docs/settings/compute-pools.md` updated.
  - The `compute-pools` row on all six provider pages now says what each provider actually does: native steer is Codex and Copilot only; structured compaction is Codex, Claude and Copilot only; same-session result delivery is Codex only.
  - `docs/reference/source-map.md` links the new tests.
  - `docs/documentation-review.md` gains the open decision on rejoining the line after a restart.
  - `python3 scripts/check_docs.py` passes.

## Review rounds
- **Round 1 (independent fresh-context claude-opus-5-5): 8 suggested (0 blocker, 3 should-fix, 5 nits), 6 implemented.**
  - Fixed the provider rows that overclaimed.
  - The restart test now goes through the real exit stop.
  - Retry is now tested with no provider running.
  - Restore and polls now run before the "not restored" checks.
  - Corrected the test header comment.
  - Added the sub-agent test to the source map.
  - Declined: rewording a comment in `src/common/state/app_state.h:938`. Both parallel branches (host admission, load signal) edit `app_state.h` beside that line, and it is a comment only.
  - Declined: updating `V5.1 Improvement Change log.md`. It is a historical V5.1 record.
- **Round 2 (one fresh-context claude-opus-5-5 re-check): 7 suggested (0 blocker, 1 should-fix, 6 nits), 6 implemented.**
  - The restart wording is now limited to local chats. The docs and the open decision record that a held SSH turn is already kept as prepared remote input and sends itself after a restart. This is existing behaviour, not changed here.
  - The waiter is re-ticked before the use counts, avoiding a race with the 5 s stale-waiter window.
  - Native steer is described as keeping its slot rather than adding use.
  - Corrected the Codex row wording.
  - "n" is now documented as running or promised slots.
  - The restart sentence is qualified for input queued behind the held turn.
  - Declined: driving the relaunch through `PollAllAcpSessions`. That launch path is already covered by `AcpTimeoutStoppedTurnRestartsItsOutputLessDeliveredPrompt`, and this test's claim starts at session setup.

## Overlap with parallel branches
Some overlap was unavoidable:
- `UI-V2/src/components/views/ChatView.tsx:3794`: `v5/feat-host-admission-policy` rewrites the same status line, adding an optional `message` and Run anyway. The merge is a one-line conflict; keep their `message ??` fallback and use `(n/m in use)` in the template.
- The `compute-pools` row on all six provider pages and `docs/settings/compute-pools.md` are also edited by host admission, and line 11 by `v5/feat-routing-load-signal`. Expect text conflicts.
- `docs/reference/source-map.md` line 21: host admission adds the line right after it, so a conflict is likely.
- No other overlap:
  - `CMakeLists.txt` and `docs/documentation-review.md` changes sit in separate hunks.
  - The new test file avoids `tests/compute_pool_tests.cpp`.
  - No `src/` file changed.

## Open questions for David
1. After an app restart, should a turn that was waiting for a pool slot rejoin the line by itself, or stay in its chat for Retry as it does now? If it should rejoin, does it go behind or ahead of turns sent since? Today local chats wait for Retry, while SSH chats already rejoin by themselves; should those two match? Rejoining for local chats would be a separate, medium-sized slice.
2. "(n/m in use)" counts slots promised to older waiters as in use, so a chat can show 2/2 while the Settings page shows 1 in use. Keep the label, or show only running turns?

## New issue previews
None. The open questions belong to #524 and are recorded in `docs/documentation-review.md`.

## PR preview (not opened)
**Title:** `test(compute-pools): show "in use" and cover continuations and restart`

```markdown
A turn held for a compute pool slot now shows "Waiting for <pool> slot (2/2 in use)". New native tests show that manual compaction, a delivered sub-agent result and a native steer start on a full pool without waiting and count towards use (a steer keeps its turn's slot). A turn held across an app restart stays unsent in its chat; Retry checks it at the gate again as a new turn and sends it once. Automatic rejoin after a restart is not built; it is recorded as an open decision. Docs, the six provider capability rows and the source map are updated.

<Before/After: chat with a turn held for a compute pool slot>

**Proof:**
- `npx vitest run`: 239 files, 4215 tests passed.
- `npx tsc --noEmit -p .`: passed.
- `npm run build`: passed.
- `python3 scripts/check_docs.py`: passed.
- New native tests: syntax and type check only (g++ `-fsyntax-only` against the real headers with stub CEF headers). They were not built or run: the desktop and test build refuses Linux, and no macOS or Windows machine was available.
- Runner build: not needed (no `src/common` or `src/remote` change); an attempted configure failed on a blocked dependency download.

**Review:** 1 independent reviewer, 2 iterations (fresh context each).

- Independent claude-opus-5-5 (round 1): 8 suggested; 6 implemented.
- Independent claude-opus-5-5 (re-check): 7 suggested; 6 implemented.

Declined:
- Rewording a comment in `app_state.h`: parallel branches edit beside it.
- Updating the V5.1 change log: it is a historical record.
- Driving the relaunch through a poll: existing tests cover that path.

UI change: still needs Claude review and David's approval under AGENTS.md.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

Related to #524.
```

## Screens needing Before/After capture
- Chat view with a turn held for a compute pool slot: the status line changes from "Waiting for InferDeck slot (2/2)" to "Waiting for InferDeck slot (2/2 in use)".
