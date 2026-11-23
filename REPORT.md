# Accessibility fixes C, D and E: report

**Branch:** `v5/fix-a11y-dialogs-forced-colours`, based on `origin/uam/v5.2-sync-main` (`c252f9a9`)
**Final SHA:** `4f4b50f8416c0fb9d56ae6d253b2909c1e59905c` (pushed)

Commits:
- `b5fe05d6`: the fix
- `7f6da2d4`: round 1 review fixes
- `4f4b50f8`: round 2 re-check fixes

No PR, issue, comment or label was created. No native code changed, so the runner build was not needed. No live token tests were run.

I re-checked each audit item before fixing it, and all were real:
- **C:** forced colours turn fills into Canvas, so the Switch thumb, the underline Tabs indicator and the rail marks vanished. I confirmed this in Chromium's forced-colours emulation.
- **D:** New chat and the remote directory browser each built their own overlay, and New chat did not restore focus on close. Overlay had no semantics or focus handling. Chat name had only a placeholder for a visible label. The Remote workspace path `<label>` and the companion's Theme `<label>` were not linked to their fields.
- **E:** SelectionGrid's `aria-label` hid its details, status and issues. The changed-files tree's `aria-label` hid the change letter and the counts. Queue reorder used a fixed 180 ms, and the exit used a fixed 170 ms timer.

## Review rounds

**Round 1.** Independent `claude-opus-5-5`, fresh context, given the issue, the diff and the docs: **8 suggested, 8 implemented** (0 blockers, 3 should-fix, 5 nits).
- StrictMode focus restore pulled focus out of labelled dialogs.
- The driven performance script finds New chat by `aria-label`.
- A New chat backdrop test step had become a no-op.
- Disabled Switch under forced colours.
- Changed-files folder rows lost their path.
- Hover on a chosen but unavailable card.
- No focus target when New chat is opened from a row menu item.
- "Remote runner is uninstalled" wording.

**Round 2.** One fresh-context re-check by independent `claude-opus-5-5`. It confirmed all 8 fixed, then raised **4 suggestions; 3 implemented**:
- Implemented: the phone New chat and remote directory browser fields lost their 16px companion styling once portalled.
- Implemented: the row lookup skips hidden and inert rows, through a shared `focusableRowWith` helper.
- Implemented: a footer wording test.
- **Declined:** dropping New chat's extra `aria-label`. `scripts/perf_driven.py` needs it, and that script is outside this task's file list. I documented that `labelledBy` wins instead, and filed issue preview 1.
- **Partly declined:** making FolderTree use the new helper. FolderTree is outside the file list; see issue preview 2.

The third commit came after the single allowed re-check, so it was not independently re-reviewed. Its changes are small and covered by tests.

## Issue previews (not filed)

### Preview 1

**Title:** `refactor(perf): find the New chat dialog by its visible title in the driven suite`

`scripts/perf_driven.py:183-184` finds New chat with `[role="dialog"][aria-label="New chat"]`. The dialog is now named by its visible heading through `aria-labelledby`, on the shared Overlay. It keeps a redundant `aria-label="New chat"` only so the script still matches.

**Desired outcome:** the script finds the dialog by the text of its `aria-labelledby` title. `NewChatModal.tsx` then drops `label="New chat"`.

**Evidence:** the round 1 review found that switching to `labelledBy` alone made `__perf.create()` time out. Reproduction status: inferred from the selector. The driven suite was not run.

**Acceptance criteria:**
- [ ] The driven suite creates a chat with New chat named only by `aria-labelledby`.

**Review:** raised by an independent review; not triaged.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

### Preview 2

**Title:** `refactor(ui): move the phone dialog field rule into companion.css and share the row-focus helper`

Two pieces of this branch were written outside their natural home, because the task restricted which files could change:
- **Field rule:** `index.css` repeats the companion's `.uam-companion input` rule as `.uam-companion-overlay input`. Portalled phone dialogs need it to keep their 16px fields.
- **Row helper:** `FolderTree.tsx`'s `rowFocusTarget` duplicates the new `utils/modalFocus.ts` `focusableRowWith`, and does not skip hidden or inert rows.

**Desired outcome:**
- `companion.css` owns one `:is(.uam-companion, .uam-companion-overlay) input` rule.
- FolderTree uses `focusableRowWith`.

**Acceptance criteria:**
- [ ] One field rule.
- [ ] One row lookup.
- [ ] Phone New chat fields stay at 16px.

**Review:** raised by an independent review; not triaged.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## PR preview (not opened)

**Base:** `uam/v5.2-sync-main`
**Head:** `v5/fix-a11y-dialogs-forced-colours`
**Title:** `fix(ui): restore dialog focus, label fields and keep state visible in forced colours`

**Dialogs.** The Overlay primitive now provides dialog semantics and focus handling, so every dialog built on it gets them.
- With `label` or `labelledBy`, the full-screen layer becomes the modal dialog. It gets the role, `aria-modal`, its name and an optional description, and takes focus when it opens.
- Every Overlay returns focus to the control that opened it, or to `returnFocusTo` if that control is gone. This only happens when focus would otherwise be lost, after the owner's own restore, and never out of another open modal.
- New chat and the remote directory browser now use Overlay instead of their own layers, so closing New chat restores focus.
- The browser and SSH routing steps return focus to Browse or More options.
- Opened from a row menu item, New chat returns focus to that workspace's sidebar row.

**Labels.** Chat name, Remote workspace path, Reasoning effort and the phone browser's Theme menu now have visible labels linked to their fields.

**Forced colours.** The Switch thumb and track, the Tabs indicator, the chosen choice card and the message rail marks now use system colours, so they stay visible.

**Screen readers.**
- Choice cards describe their detail, status and why they cannot be chosen.
- An option that cannot be chosen for a stated reason stays focusable and announces why.
- Changed-files rows describe their change type and line counts in words.
- An unavailable computer is described in words, for example "The remote runner is not set up", never by its raw status.

**Motion.** Queue reorder and exit timing follow the Animation speed setting.

Visual evidence: none attached. In-app before/after captures still need to be taken; see the list below.

**Proof:**
- `npm ci`.
- `npx vitest run`: 242 files, 4248 tests passed.
- `npx tsc --noEmit -p .` and `-p tsconfig.app.json`: exit 0. The first one checks nothing because the root `tsconfig.json` lists no files.
- `npm run build`: passed.
- `python3 scripts/check_docs.py`: passed.
- I removed each of these fixes and confirmed its test then failed:
  - Overlay's deferred restore: the test where the owner's chosen target wins, and the existing ChatView goals test.
  - The StrictMode open check.
- The round 2 reviewer did the same for the remaining round 1 fixes:
  - The backdrop tests.
  - The row-menu fallback.
  - The runner wording.
  - The folder path.
  - Disabled forced colours.
- Forced colours: checked visually in Chromium (Playwright `forcedColors: 'active'`, dark and light) on a temporary harness, not committed. It covered the Switch (plain, Settings, disabled), both Tabs variants, SelectionGrid and the message rail. Jsdom cannot evaluate forced colours, so CSS-rule tests guard these instead.
- Not run: the native build (no native changes), the driven performance suite, and any check in the desktop app or on a phone.

**Review:** Reviewed by 2 agents over 2 iterations.
- Agent 1: claude-opus-5-5. Suggested 8; implemented 8.
- Agent 2: claude-opus-5-5 (re-check). Suggested 4; implemented 3. Declined: removing New chat's extra `aria-label`, which `scripts/perf_driven.py` relies on and which is outside scope (follow-up proposed). The FolderTree half of the helper reuse is also left for a follow-up.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code harness), on behalf of David Taylor.

## Screens needing Before/After capture

**Desktop New chat dialog**
- Visible "Chat name" label.
- "New chat" placeholder.
- Title is now a heading.
- The shared Overlay backdrop: dim plus a 3px blur, previously a flat 48% black.
- Runs on cards with an unavailable SSH computer: it stays focusable, its tooltip has the new wording, and the footer says "The computer is offline…" or "The remote runner is not set up…".
- Focus after Cancel or Escape: on the opener, or on the workspace row after a row menu.

**Remote directory browser** (New chat Browse, sidebar new workspace, chat recovery)
- The backdrop is now the shared dim plus blur, previously a flat 56% black.
- After Cancel, focus returns to Browse.

**Phone browser**
- New chat dialog: fields stay at 16px.
- Settings → Theme row: now a label-and-menu row, like the Input alerts row.

**Forced colours (Windows contrast theme, dark and light)**
- Switch: plain, Settings, on, off and disabled.
- Tabs: underline and pill.
- Choice cards: the chosen one, with its Highlight border.
- Message navigation rail: marks, current mark and the neighbours on focus or hover.

**Queued prompts**
- Reorder animation at Default, Slow, Fast and Off. This needs a recording.
