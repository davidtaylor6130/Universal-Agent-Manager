# De-slop findings 3 and 4 (UI-V2): elapsed time and attachment staging

**Branch:** `v5/refactor-deslop-ui-elapsed-staging`, based on `origin/uam/v5.2-sync-main` at `c252f9a9`
**Final SHA:** `210c17be6eab5bfab6ecf4d156a8a5b3135dbf0b` (3 commits: `5e7ede6e`, `5dc117e1`, `210c17be`)

## Re-checking the scout findings

I checked both findings against the code before changing anything. Neither had to be dropped. One correction to finding 4:

- **Finding 3 holds as written.** I read all six formatters and all six tickers.
- **Finding 4 holds, but the path leak is smaller than stated.** `stageDirectoryPaths` was missing both guards, and a companion `text/uri-list` drop did call `stageChatAttachments` with paths. The second reviewer found that native `src/common/companion/protocol.h:441-443` already rejects any companion item that carries a `path` or `id` with a 403. So on the old code the user got a confusing failed chip, but no path reached the desktop filesystem.

## What changed

- **One elapsed-time format.** A new `UI-V2/src/utils/elapsed.ts` `formatElapsed` replaces all six formatters:
  - the sidebar row;
  - the live "Working" header;
  - the finished "Worked" header;
  - the status strip;
  - native subagent rows;
  - managed subagent rows.

  The text is `45s`, `1m 5s`, `2m 0s` (the zero is kept), then `1h 2m`. It is unpadded and rounded to the nearest second. "1h 2m" was the majority hour format. Rounding keeps the existing ChatView comment that relies on `formatWorkedDuration` rounding true, without editing outside the allowed region.

  The sidebar's one-line working rows keep a shorter form: from one minute on they show minutes only (`1m`), with the same units and hour format. So at 3725 s the sidebar and the chat both read "1h 2m". Before, the chat read "Working 62m 5s" and the status strip "1h 02m".
- **One live clock.** A new `UI-V2/src/hooks/useNow.ts` `useNow` replaces all six 1-second tickers. While a clock is running, every render reads the current time. The interval only triggers re-renders. Ticking stops while the window is hidden and catches up when the window is shown again. The sidebar, the live header and the status strip used to keep ticking while hidden.
- **ChatView staging.** The edit stays inside `stageFiles` and `stageDirectoryPaths`, as you asked. Both functions now share four helpers: `canStageAttachments`, `beginStagingAttachments`, `applyStagedAttachments` and `failStagedAttachments`. Folder staging gains the two guards it was missing:
  - If every chip of a request was removed before it failed, the failure is not reported.
  - A drop on the browser companion is refused with "Add attachments from the desktop app.", sent through the shared notification, and no paths are sent.

  `onComposerDrop` is unchanged.
- **Docs.**
  - `docs/ui/visual-style.md`: a new "Elapsed time" section with the format, the sidebar's shorter form, the hidden-window rule, and the exceptions (the dictation recording timer, countdowns and ages).
  - `docs/reference/source-map.md`: a row for the two helpers.
  - `docs/chat/composer/attachments.md`: the removed-chip rule and the no-path rule, plus a note on the companion refusal.
  - `docs/documentation-review.md`: a known-gap row for the browser companion refusing all attachments.
  - No provider capability rows changed.
- **Size.** Production source is 144 lines added and 144 removed (net zero), because the shared helpers replace the copies. The scout estimated about 80 lines removed; that did not happen.

## Review rounds

1. **Round 1** (independent `claude-opus-5-5`, fresh context): **9 suggested, 9 implemented** (0 blockers, 2 should-fix, 7 nits).
   - The main finding was that the first `useNow` returned the time from its last tick. A row that became active after an idle period, or a run that started between ticks, could briefly show "Timing unavailable". This is fixed and covered by a regression test that fails on the round-1 hook.
   - The second should-fix: my first draft of the attachments doc wrote the companion's existing refusal of all attachments into the product agreement. It is now recorded as a known gap instead.
   - Nit 9 was that `tsc -p .` checks nothing; I now run `tsc -p tsconfig.app.json`.
2. **Round 2** (fresh-context `claude-opus-5-5` re-check, run because the round-1 fixes were material): **6 suggested, 5 implemented** (0 blockers, 0 should-fix, 6 nits).
   - **Declined:** nit 6, to open a tracking issue and cite it in the known-gap row. This run may not file issues; the preview is below.

## New issue preview (not filed)

**Title:** `question(companion): should the browser companion composer stage byte attachments`

The browser companion composer refuses every attachment with "Add attachments from the desktop app.". The companion protocol advertises byte attachment staging (`byteAttachmentStaging`), and the desktop app accepts bytes without paths. The attachments agreement says its rules apply to desktop, browser and native-phone callers. It is undecided whether the browser companion should stage bytes or keep refusing them. The gap is recorded in `docs/documentation-review.md` under "Browser companion attachments".

**Desired outcome:** David decides either way. Then either the browser companion stages byte attachments within the 25 MB limit, or the attachments agreement states the refusal.

**Evidence:**
- `ChatView.tsx` `canStageAttachments` refuses all companion staging.
- `src/common/companion/protocol.h` advertises byte staging and rejects any item that carries a path or id.
- I observed both in the code at `210c17be`. I did not reproduce this on a phone.

**Review:** this gap was raised by an independent `claude-opus-5-5` review of the de-slop branch.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `refactor(ui): share one elapsed-time format and harden attachment staging`

The same elapsed time read differently across the app. One running turn showed "1h 2m" in the sidebar, "Working 62m 5s" in the chat and "1h 02m" in the status strip. The live header floored and the finished header rounded. Three of the six 1-second clocks kept re-rendering while the window was hidden.

All six surfaces now use one format: `45s`, `1m 5s`, `1h 2m`, rounded to the nearest second. The sidebar's working rows drop seconds from one minute on. All six share one live clock that stops while the window is hidden.

ChatView's file and folder staging now share their success and failure handling. Folder staging gains two guards: it no longer reports a failure for chips the user already removed, and a browser companion drop sends no paths to the desktop app. The format and rules are documented in `docs/ui/visual-style.md` and `docs/chat/composer/attachments.md`. The companion's refusal of all attachments is recorded as an open gap.

**Visual evidence:** still needed. Capture before and after, in a dark and a light theme:

- A sidebar Active Chats working row, at under 1 minute, 1–59 minutes and over 1 hour.
- A chat turn header while working ("Working 1m 5s") and after the turn ("Worked 1m 5s"), with the same turn over 1 hour.
- The status strip above the composer after more than 1 minute (it was "1m 05s", now "1m 5s").
- A subagent group row and a managed-subagent panel row with a running and a finished child.
- A desktop folder chip removed while staging, then failing: no error notice should appear.
- A folder dropped on the browser companion: one notice "Add attachments from the desktop app." and no chip.

**Proof:**

| Check (at `210c17be` unless stated) | Result |
|---|---|
| `npx vitest run` (UI-V2), full suite | 242 files, 4237 tests passed |
| 7 new component tests on the old source (source changes stashed, tests kept) | All 7 failed: "Working 62m 5s", "1m 05s", "Working 59s", ticking while hidden, the removed folder chip reporting an error, the companion drop calling `stageChatAttachments` |
| The same tests on the new source | All passed |
| New `useNow` stale-time test on the round-1 hook | Failed; passes now |
| `npx tsc --noEmit -p tsconfig.app.json` | Passed |
| `npm run build` | Passed |
| `python3 scripts/check_docs.py` | Passed (203 pages, 1517 links, 6 providers × 32 capabilities) |

- **Native build:** not run. No native code changed: every changed file is under `UI-V2/` or `docs/`.
- **Not done:** no visual or theme check, no live desktop or companion run, no macOS or Windows check.
- **Still pending:** the Claude review that AGENTS.md requires for UI changes, and David's approval.

**Review:** 2 independent reviewers (fresh contexts), 2 iterations.

- Independent `claude-opus-5-5` (round 1): 9 suggested, 9 implemented.
- Independent `claude-opus-5-5` (round 2 re-check): 6 suggested, 5 implemented. Declined: opening a tracking issue for the companion gap, because this run may not file issues.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
