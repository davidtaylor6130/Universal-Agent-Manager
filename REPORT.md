# #808 Shared motion and loading responsiveness: report

Final SHA: `d4373e27b66a55abdcd2a7582980f1fe5669e3da` on `v5/feat-808-motion-loading`, pushed. Base: `origin/uam/v5.2-sync-main`.

All five loading placeholders now share one skeleton that animates transform only:
- the sidebar list shown at startup;
- icon placeholders (the inline 16 px fallback and the icon library grid);
- the Settings loading rows;
- the chat history skeleton;
- the subagent history skeleton.

Before, each had its own background-position shimmer. Nothing changed in the typing or streaming path. No before/after paint or frame figures were measured, so no measured performance improvement is claimed.

## Commits

- `57936fef` feat(ui): one compositor-only shared loading skeleton (#808)
- `30e76994` fix(ui): keep transcript skeleton lines separate and narrow motion claims (#808)
- `d4373e27` test(ui): require every shared skeleton to be a leaf element (#808)

## What changed

- **Shared class.** `UI-V2/src/index.css` adds `uam-skeleton`:
  - The block is a still `--surface-up` fill.
  - Its `::after` highlight band animates `transform` only, at `calc(1400ms * var(--motion-scale, 1))`.
  - With Motion Off or reduced motion the block stays still.
- **Feature classes.** These now only set size and corner rounding: `.uam-icon-skeleton`, `.uam-set-skeleton`, and `.conversation-skeleton__bubble` / `__line`. The three old shimmer keyframes are removed.
- **Components.** `Sidebar.tsx`, `CustomIcon.tsx`, `SettingsPrimitives.tsx`, `ChatView.tsx` and `SubagentTranscript.tsx` put `uam-skeleton` on leaf blocks only, never on the line containers.
- **Docs.**
  - `docs/ui/motion.md` describes the shared skeleton and which surfaces show a visible loading label:
    - visible label: transcript, subagent and Settings;
    - screen-reader-only label: the sidebar list and the icon grid;
    - no label: the inline icon fallback.
  - `docs/ui/primitives.md` lists `uam-skeleton` and names Usage's own opacity-pulse skeleton as the exception.

## Review rounds

- **Round 1 (independent fresh-context Opus review, `agent-reports/review-R3`, #808 section): 4 suggested (1 blocker, 1 should-fix, 2 nits), 4 implemented.**
  1. **Blocker, implemented.**
     - Problem: the skeleton class was also on the three `conversation-skeleton__text` line containers, which merged the lines into one block.
     - Fix: I removed it from those containers. The contract test now asserts containers never take the class, and the ChatView performance test asserts every skeleton has no child elements. Both tests fail if the class is put back.
  2. **Should-fix, implemented by rewording, as directed.**
     - Problem: the paint and composite improvement was claimed without being measured.
     - Fix: the cloud environment can't measure it, so the wording now states only what was shown: a transform-only animation, no script or React work per frame, and no remount while typing.
  3. **Nit, implemented.** `primitives.md` names Usage as the exception.
  4. **Nit, implemented.** `motion.md` now says which loading labels are visible and which are for screen readers only.
- **Round 2 (one fresh-context Opus sub-agent re-check, run because the round-1 blocker fix was material): 3 suggested (0 blockers, 0 should-fixes, 3 nits), 3 implemented.**
  1. **Contract test: every `uam-skeleton` must be a self-closing leaf.** The check is now generic: it covers any element in the five placeholder files that takes the class, whatever its other classes, not just ones whose class name contains the placeholder family. Putting the class on the Settings row wrapper as a test makes it fail.
  2. **`motion.md`:** the inline icon fallback is described as unlabelled.
  3. **Comment:** the stale "shimmer" wording is gone from the `conversation.css` comment.
- **Total: 7 suggested, 7 implemented, none declined.**

## Checks run (on `d4373e27`)

- `npx vitest run`: 239 files, 4,216 tests passed (includes the performance files).
- `npm run test:performance` (serial): 7 files, 34 tests passed.
- `npx tsc --noEmit -p .`: passed. `npm run build`: passed.
- `python3 scripts/check_docs.py`: passed (203 pages, 1510 local links, 6 providers × 32 capabilities).
- Headless Chromium spot checks, CSS only, not the app:
  - The sweep animates at Default and does not animate under Motion Off or reduced motion.
  - The built transcript skeleton renders four separate rounded lines with 8 px gaps and a transparent container.
- Not run:
  - the native build;
  - the desktop app or CEF;
  - before/after paint or frame measurements;
  - a visual check of the real screens.

## Issue previews

None. No issue comment was requested.

## PR preview (not opened)

**Title:** `feat(ui): one shared loading skeleton that animates transform only (#808)`

> The loading placeholders now share one `uam-skeleton` class:
> - the startup sidebar list;
> - the inline icon fallback and the icon library grid;
> - the Settings loading rows;
> - the chat history skeleton;
> - the subagent history skeleton.
>
> The block is a still `--surface-up` fill whose highlight band animates `transform` only, at the selected animation speed. It needs no script or React work per frame. With Motion Off or reduced motion it stays still.
>
> Each placeholder used to repaint its own background-position shimmer. Feature classes now only set size and corner rounding, and only leaf blocks take the shared class. Usage keeps its own opacity-pulse skeleton and 250 ms threshold, documented as the exception. `docs/ui/motion.md` and `docs/ui/primitives.md` are updated in the same commits.
>
> Related to #808
>
> **Proof:**
> - `npx vitest run`: 4,216 tests in 239 files passed. `npm run test:performance`: 34 tests in 7 files passed.
> - A new ChatView performance case:
>   - shows the history skeleton within the 250 ms interaction budget;
>   - keeps 10 keystrokes during a history read within 100 ms each;
>   - never remounts the placeholders.
> - The motion contract test checks that the sweep:
>   - animates transform only;
>   - follows the speed scale;
>   - stops under Motion Off and reduced motion;
>   - is used by every placeholder, on leaf elements only.
> - `npx tsc --noEmit -p .`, `npm run build` and `python3 scripts/check_docs.py` passed.
> - Headless Chromium on the CSS alone:
>   - the sweep animates at Default and is stopped under Motion Off and reduced motion;
>   - the transcript lines render separately.
> - No paint or frame figures were measured, and nothing was checked in the running desktop app.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 7 suggested, 7 implemented.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

Capture each in light and dark, with Default motion and with Motion Off or reduced motion:
- chat history loading skeleton ("Loading chat history…");
- subagent transcript loading skeleton;
- sidebar startup loading list;
- inline icon fallback and the icon library loading grid;
- Settings loading rows.

Pending: Claude review sign-off and David's acceptance.
