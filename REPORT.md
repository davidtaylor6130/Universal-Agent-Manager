# Sidebar accessibility fixes (task B): report

**Branch:** `v5/fix-a11y-sidebar-rows`, pushed. It is based on `origin/uam/v5.2-sync-main` (`c252f9a9`).
**Final SHA:** `99608a486ff44e4e7611dced0a4508f99bcd60fa`

I re-checked each audit item against the base before fixing it, and all four were present. No PR, issue, comment or label was created.

## Review rounds

- **Round 1, fresh-context Opus reviewer (`claude-opus-5-5`):** 7 suggested, 7 implemented (0 blockers, 2 should-fix, 5 nits).
  1. **Announcements:** the Active announcement used each root chat's own status instead of its branch family's. It also announced chats that had not been listed before in a burst. Fixed.
  2. **Inline pin:** it came before the row in the Tab order and had the same name as the row's pin chip, and a doc sentence overclaimed. The pin is now `tabIndex=-1` and the docs are reworded. Fixed.
  3. **Docs claimed "failed" is reported:** provider error attention never reaches sidebar rows. The docs are reworded and the gap is recorded under "Known gaps retained" in `docs/documentation-review.md`. Fixed.
  4. **Comment overclaimed chip positions:** corrected.
  5. **State paint:** the framed workspace tile gained a hover fill, and the clear-search X painted a full 24px red box on focus. Fixed.
  6. **Rename field:** right-clicking it opened the row menu. Fixed.
  7. **CSS test:** it matched exact source formatting. Loosened.
- **Round 2, one fresh-context Opus re-check (`claude-opus-5-5`):** 6 suggested, 6 implemented (0 blockers, 1 should-fix, 5 nits).
  1. **Held automation chats:** a new chat held for automation approval was never announced. It is now announced once the sidebar has loaded.
  2. **Pull request watch:** the announcer ignored it. Watched chats now speak as their rows do, and watch states are not announced.
  3. **Announcer mismatches:** while searching, branch rows weren't covered, and ACP `starting` differed from the row. The announcer now uses `displayedChatStatus` on Active rows, exactly as the row does.
  4. **Phone companion:** it lost its keyboard unpin. The inline pin stays a Tab stop there.
  5. **Stale code comment:** fixed.
  6. **Framed workspace tile:** its icon changed colour on hover. Fixed.
  - **Further fix found while making these changes:** clearing a search re-announced Active rows that the search had hidden. Their last status is now kept while searching, and a regression test covers it. That test fails with the fix disabled.
- **Not reviewed again:** the round-2 fixes. The rule allows one re-check. They are covered by new tests and the full gates.
- **Declined:** none.

## Issue previews (not filed)

### 1. `bug(sidebar): section headers and some sidebar rows are still under 24px targets`

Some sidebar controls are still under the 24 by 24px minimum that `docs/ui/accessibility.md` now states (WCAG 2.5.8). This branch converts the icon controls only.

**Expected:** every sidebar pointer target is at least 24 by 24 CSS px, or has an equivalent control that is.
**Actual:** measured in Chromium on the branch, using an isolated render of `FolderTree`:
- Section toggles (All chats, Active, Pinned, Snoozed) are 272×18.
- The collection name button is 180×20.
- `.uam-subagent-toggle` has `min-height: 22px` in `sidebar.css`.

**Environment:** `v5/fix-a11y-sidebar-rows` at `99608a48`.

**Evidence:** these are bounding-box measurements, not a user report.
**Reproduction status:** observed in the isolated render.

**Acceptance criteria:**
- [ ] The listed controls reach a 24px hit area without changing row height, for example with `.uam-sb-target`.

**Review:** self-triage only.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

### 2. `bug(sidebar): provider failures never show as Failed in sidebar rows`

`docs/sidebar/active/activity-status.md` says status distinguishes failure, but no sidebar row or announcement shows a failed chat. This is now recorded under "Known gaps retained" in `docs/documentation-review.md`.

**Expected:** a chat whose provider reports an error shows a failure state in the sidebar. The row's spoken status already reads "failed" for that state.
**Actual:** `acpRuntimeStatus` in `FolderTree.tsx` and `displayedChatStatus` in `chatSearch.ts` both drop `attentionKind === 'error'`. The Active layout's "Failed" label is therefore unreachable.

**Evidence:** source reading only.
**Reproduction status:** a live provider error was not attempted.

**Acceptance criteria:**
- [ ] Decide whether provider errors belong in the sidebar. If they do, rows show and announce them.

**Review:** this was raised by the independent reviewer in round 1. The report itself was not independently reviewed.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `fix(sidebar): one interactive element per chat row with named 24px actions`

Sidebar chat rows nested their action buttons inside the row's `role="button"`. The row's name also hid its status, every Active row was its own live region, and several icon controls were under 24px with shared names such as "More actions". This PR fixes all four:

- **One interactive element per row.** Each chat row now has a single "Open chat" element. Its pin, wake, snooze, done and more-actions buttons sit beside it inside the visual row, which draws the focus ring and keeps click, shift-click, double-click and right-click.
- **Status in the name.** The row's name carries its status: running; waiting, with the reason; waiting for approval; watching pull request; or done, unread. It also says when the chat is snoozed or selected for bulk actions.
- **One announcement region.** No row is a live region any more. The sidebar's existing polite region announces Active chats that start waiting or finish, working out each status exactly as the row's name does.
- **24px targets.** Sidebar icon controls are IconButtons with a 24px hit area set in `sidebar.css`, and their painted tiles keep their earlier sizes. These are the row pin and more-actions chips, the inline unpin, the workspace, collection and Unsorted actions, the collection chevron, the desktop-app icon button and clear search. Small text actions get a 24px hit layer.
- **Named actions.** Every row action names its item, for example "More actions for Build fixes" or "Actions for Project", and the tooltips stay short.
- **Docs.** Updated pages: `docs/ui/accessibility.md`, `docs/ui/primitives.md`, `docs/sidebar/active/activity-status.md` and `docs/sidebar/management.md`. A known-gap row was added to `docs/documentation-review.md`.

**Visual evidence:** before/after captures from the actual app are still needed; see the list below. I compared base and branch locally, rendering the sidebar components in Chromium outside the app:
- At rest, the pixels are identical apart from animated spinners.
- On hover, the row's pin and more-actions tiles are now 4px apart instead of 2px, and the pin tile and wake button sit 2px further left.
- Collection chevrons and actions, the Unsorted trigger and the desktop-app icon gain a hover tile. The desktop-app pencil is grey instead of white.
- The clear-search X uses the shared red close state on hover and focus.

**Proof:**
- `npm ci`, `npx vitest run` (240 files, 4224 tests), `npx tsc --noEmit -p .`, `npm run build` and `python3 scripts/check_docs.py` all pass at `99608a48`.
- New Vitest coverage:
  - status in row names for both layouts;
  - no interactive descendants in the row element;
  - unique, item-named actions;
  - IconButton plus the 24px rules;
  - a single polite region: announces waiting, done and held automation, stays silent for branch-only running, running starts, newly listed chats, pull request watches and a cleared search;
  - shift-click from the visual row;
  - the 24px clear-search button.
- Existing tests that depended on the old nesting were retargeted to the visual row and keep their original intent.
- In Chromium, the converted controls measured 24×24.
- **Not run:** the native and runner builds (no native, `src/common` or `src/remote` changes), the CEF app on macOS or Windows, screen-reader passes, and forced-colors captures.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 13 suggested, 13 implemented.

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

1. **All Chats and Pinned compact rows at rest:** a pinned chat (inline pin), a snoozed chat ("until …"), and running, attention and done icons. These are expected to be identical.
2. **Compact chat row hovered, and with keyboard focus inside:** the Unsnooze, pin and more-actions chips, whose spacing changed.
3. **Keyboard focus ring on a chat row:** the ring is now drawn by the row container around the whole row.
4. **Active Chats row hovered:** the Done and Snooze overlay over the status, plus a Working row.
5. **Workspace header hovered:** the framed actions tile, in default theme and in forced-colors mode.
6. **Collection header:** the chevron and actions at rest and hovered, and the desktop-app reference row's change-icon pencil (colour changed).
7. **Unsorted header actions hovered.**
8. **Search field with text:** the clear X at rest, hovered and keyboard-focused.
9. **Chat filter menu with active filters:** the Clear pill.
10. **Phone companion sidebar:** a pinned chat row.
