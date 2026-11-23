# UI-V2 accessibility and interaction audit (read-only)

- **Branch audited:** `uam/v5.2-sync-main` at `6f235eb9`.
- **Branch tip when published:** `c252f9a9`. The commits in between change no file under `UI-V2/`, so every file:line reference below still applies.
- **Standard:** the project's own agreements first (`AGENTS.md`, `CLAUDE.md`, `docs/ui/design-language.md`, `accessibility.md`, `interaction.md`, `primitives.md`, `motion.md`, `settings.md`), then WCAG 2.2 AA.
- **Scope:** this is an audit report only. It changes no application code.

All paths are relative to `UI-V2/src/` unless they start with `docs/`.

## Checks run

- **Tests:** `npm ci` then `npx vitest run` in `UI-V2`. Result: **239 test files and 4215 tests, all passed** (194 s).
- **Static lint:** `eslint-plugin-jsx-a11y` with the strict rule set, plus `control-has-associated-label`. It was installed and run against a copy of `src` in a temporary directory, not in the repository.
  - It reported 80 hits. Most are false positives: keyboard handling sits on a parent, or the hit is a backdrop or a drop zone.
  - The meaningful hits were checked by hand and appear below.
  - `control-has-associated-label` found **no** interactive control without an accessible name.
- **Forced colours:** the CSS was built with `vite build` into a temporary directory. The real Switch and Tabs classes were rendered in Playwright Chromium with `emulateMedia({ forcedColors: 'active' })`.
  - **Result:** the Switch thumb and the underline Tabs indicator both disappear, so on/off and the selected tab can't be told apart.
- **Not done:** the frontend refuses to run outside the native shell ("NATIVE SHELL REQUIRED"), so there are **no app screenshots**. Fixes marked "visual change" need app screenshots before acceptance.

## Findings, by user impact

### 1. The chat status line announces its timer to screen readers every second (high)

- **Evidence:**
  - `components/chat/StatusHelpers.tsx:69`: `ChatStatusStrip` renders `<div role="status">`.
  - The elapsed timer inside it (`:75`) re-renders every second (`useElapsedSeconds`, `:27–38`).
  - It is used for "Refreshing history", "Compacting context" and the stopped status (`components/views/ChatView.tsx:3903–3906`), and for the subagent wait line (`components/chat/SubagentWaitStatus.tsx:21`).
  - The turn-starting status (`components/views/ChatView.tsx:3801`) wraps `ConversationWork headerOnly`. Its `WorkingHeading` (`components/chat/ConversationWork.tsx:30–40`) ticks "Working Ns" every second.
- **What fails:** a polite live region whose text changes every second. Screen readers keep announcing the time, which breaks the "no announcement floods" rule (WCAG 4.1.3).
- **Who:** screen-reader users, whenever a turn starts or compaction, history refresh or a subagent wait is running.
- **Fix:** keep the live region on a stable, visually hidden text that holds only the title (for example "Compacting context"). Mark the ticking timer `aria-hidden`, or move it outside the live region.
- **Visual change:** none.

### 2. Sidebar chat rows nest buttons inside a button, and the row label hides status (high)

- **Evidence:**
  - `components/sidebar/SessionItem.tsx:633`: the row is a `div` with `role="button"`, `tabIndex={0}` and `aria-label={`Open chat ${sessionName}`}`. It closes at `:977`.
  - Inside it are these controls:
    - the Done button (`:719`) and the Snooze `IconButton` (`:736`);
    - the Unsnooze `IconButton` (`:904`), Pin (`:922`) and More actions (`:947`);
    - the rename `<input>` (`:824`).
- **What fails:**
  - It breaks the project rule "Interactive elements are never nested inside another interactive role" (`docs/ui/accessibility.md`).
  - The `aria-label` replaces the row's contents, so these are never announced: Working, Needs approval and similar statuses (`:701`, `:711`, `:879–891`); the workspace, branch, host and provider (`:757–772`).
- **Who:** screen-reader users, and voice-control users who address controls by name.
- **Fix:**
  - Make the name area a `RowButton` (or a plain `<button>`) that sits beside the actions group, not around it.
  - Remove the label that overrides the contents. Expose status and workspace through `aria-describedby`.
  - Keep the existing keyboard shortcuts (Enter/Space, arrows to reorder, F2, Delete, context menu) on the new button.
- **Visual change:** the layout can stay the same, but take screenshots because this is a restructure.

### 3. Every Active sidebar row is its own live region (high)

- **Evidence:** `components/sidebar/SessionItem.tsx:701` (`<span role="status">Working</span>`) and `:711` (`<span role="status">{activityStatus.label}</span>`).
- **What fails:** status changes in chats the user is not viewing are announced while they work elsewhere. With several chats running, announcements pile up. This goes against "activity in another chat never interrupts" (`docs/ui/interaction.md`, rule 11) and the notification-system principle in `design-language.md`.
- **Who:** screen-reader users with several active chats.
- **Fix:** remove `role="status"` from the row text. If background changes should be announced, send them once through the shared notification system.
- **Visual change:** none.

### 4. Screen-reader users are not told when a reply finishes (high/medium)

- **Evidence:**
  - The message list (`components/views/ChatView.tsx:3748`) has no `role="log"` and no completion announcement.
  - `components/chat/ConversationTurn.tsx:34` sets `data-streaming` but not `aria-busy`.
  - A search for announcement code in `components/` (outside settings) found none tied to the end of a turn.
- **What fails:** the only audible signal during a turn is the timer from finding 1, and nothing says when it ends (WCAG 4.1.3).
- **Who:** screen-reader users, who have to keep checking the transcript.
- **Fix:**
  - Add one polite, visually hidden region per chat pane, for the open chat only.
  - When processing ends, it announces once: for example "{Provider} replied", "Stopped" or "Needs your approval".
  - It never announces streamed text.
  - Set `aria-busy` on the streaming message.
- **Visual change:** none.
- **Decision for David:** confirm that a transcript-local announcement fits "chat status stays in the transcript".

### 5. In forced-colours mode the Switch thumb, the Tabs indicator and the rail marks disappear (high for high-contrast users)

- **Evidence (confirmed with Playwright forced-colours emulation on the built CSS):**
  - `index.css:1656` draws `.uam-switch__thumb` with `background: var(--text-3)`, and `:1659` gives the checked thumb `background: var(--bg)`. Forced colours replace backgrounds, so the thumb disappears.
  - Outside Settings the switch has no tick or cross icons (`index.css:1660`), so on and off look the same.
  - `index.css:1843` draws the underline `.uam-tabs__indicator` with `background: var(--accent)`. It disappears, and the selected tab's only other cue is text colour (`:1824`), which forced colours flatten.
  - The message navigation rail marks (`components/chat/MessageNavigationRail.tsx:204–215`) are also drawn with backgrounds only.
- **Who:** Windows High Contrast and forced-colours users (WCAG 1.4.11, 1.4.1).
- **Fix:** add an `@media (forced-colors: active)` block:
  - Switch thumb: `background: CanvasText`.
  - Checked track: `Highlight`, with the thumb in `HighlightText`.
  - `.uam-tabs__indicator` and the rail marks: `background: Highlight`.
  - Each with `forced-color-adjust: none`.
  - The SegmentedControl indicator already survives because it has a border (`index.css:1761–1771`).
- **Visual change:** only in forced-colours mode; normal themes don't change. Take screenshots under emulation.

### 6. Sidebar controls are smaller than 24px and bypass the IconButton primitive (medium)

- **Evidence (every one is marked "raw button kept"):**
  - `components/sidebar/SessionItem.tsx:926–929` (Pin) and `:952–954` (More actions): 20×20 with a 2px gap (`gap-0.5`).
  - `components/sidebar/FolderTree.tsx:2271`: Folder actions, 22×22.
  - `FolderTree.tsx:1267`: Unsorted actions, 22×22.
  - `FolderTree.tsx:1762`: collection collapse chevron, with no set size (about 14px).
  - `FolderTree.tsx:1797`: collection actions trigger, with no set size (about 14px).
  - `FolderTree.tsx:1869`: "Change icon" button. Its class `.uam-icon-button` has **no CSS rule anywhere**, so the target is about 12px.
  - `FolderTree.tsx:1104`: "Clear all", 10px text.
  - `components/sidebar/ChatSearchBar.tsx:128`: search clear button, 18×18.
- **What fails:**
  - WCAG 2.5.8 Target Size (Minimum): these are under 24×24 with no spacing exception.
  - The project's control heights of 26/32px (`docs/ui/interaction.md`, rule 7).
  - `docs/ui/primitives.md`: "Primitives never restyle a raw `<button>`".
- **Who:** pointer users with limited dexterity, tremor or touchpad input.
- **Fix:** use `IconButton size="sm"` (26px), or add a 24px size scoped in `components/sidebar/sidebar.css`. Give `.uam-icon-button` real styling or replace it with `IconButton`.
- **Visual change:** yes; needs screenshots.

### 7. The New chat dialog doesn't restore focus (medium)

- **Evidence:**
  - `components/sidebar/NewChatModal.tsx:97–104`: `requestClose` and the mount effect focus the name field but never save or restore the element that opened the dialog.
  - When the remote directory browser step returns (`:358–367`), the browser unmounts and focus drops to the page body. `RemoteDirectoryBrowser.tsx:63` only sets initial focus.
  - Both dialogs build their own overlay instead of using `Overlay`/`Presence`: `NewChatModal.tsx:371`, `RemoteDirectoryBrowser.tsx:81`.
  - The `RemoteDirectoryBrowser` dialog (`:89`) has no `tabIndex={-1}` as a fallback focus target.
- **What fails:** "Dialogs and menus must manage focus so … returning preserves the user's place" (`docs/ui/accessibility.md`); WCAG 2.4.3.
- **Who:** keyboard and screen-reader users.
- **Fix:**
  - Remember `document.activeElement` when the dialog opens and focus it again (if still connected) when it closes.
  - When the directory step returns, focus the Browse button.
  - Move both dialogs onto the shared `Overlay`.
- **Visual change:** none, unless switching to `Overlay` changes the backdrop dimming (`rgba(0,0,0,.48/.56)` compared with `uam-overlay--dim`). Check with a screenshot.

### 8. `SelectionGrid` hides status, details and why an option is disabled (medium)

- **Evidence:**
  - `components/shared/SelectionGrid.tsx:15`: `aria-label={option.label}` replaces `detail`, the tone `StatusDot` and the `issues` `StatusIndicator` (`:19–23`).
  - Options use native `disabled`, so the reason can't be reached by keyboard.
  - In New chat (`components/sidebar/NewChatModal.tsx:428–440`), unready SSH hosts are disabled with the reason "Remote runner is {status}. Open Settings to configure this computer.", which keyboard and screen-reader users never get.
  - The warning, info and neutral tones are plain coloured dots, so the state is shown by colour only.
- **Who:** screen-reader, keyboard and colour-blind users.
- **Fix:**
  - Remove the `aria-label` so the name comes from the contents.
  - Point `aria-describedby` at the issues text.
  - Use `aria-disabled` with a described reason, the way `IconButton`'s `disabledReason` works.
  - Give non-success tones an icon as well as a colour.
- **Visual change:** yes (the tone icon); needs screenshots.

### 9. The changed-files tree hides the change type and line counts (medium)

- **Evidence:** `components/chat/ChangedFilesTree.tsx:116`: on file rows, `aria-label={`Review changes to ${node.path}`}` replaces the M/A/D/R letter and the +N/−N counts rendered at `:96–105`.
- **Who:** screen-reader users reviewing a turn's changes.
- **Fix:** label the row "{path}, {added|modified|deleted|renamed}, {N} additions, {N} deletions", or drop the override and give the letter a visually hidden word.
- **Visual change:** none.

### 10. The `Overlay` primitive manages no dialog semantics or focus (medium, structural)

- **Evidence:** `components/ui/Overlay.tsx:57–80` renders a portal backdrop only. It sets no `role`, no `aria-modal`, no initial focus, no Escape handling and no focus restore.
- **Result:** about a dozen dialogs each build their own focus handling, with different definitions of what is focusable:
  - `utils/modalFocus.ts:1`;
  - `components/chat/useToolQuestionDialog.ts:22`;
  - `components/shared/CustomIcon.tsx:93`, which counts buttons and inputs only;
  - and separate handling in `ConfirmDialog`, `RepositoryDiffDialog`, `FilePreviewDialog`, `ExecutionRoutingDialog`, `StartupNativeImportChooser` and `GeminiChildBrowser`.
- **What fails:** "primitives own controls and presentation, so features never restyle equivalents" (`docs/ui/design-language.md`). Behaviour differs from dialog to dialog, and new dialogs (finding 7) miss steps.
- **Who:** keyboard users.
- **Fix:** add a shared dialog-focus hook to `Overlay`, or alongside it: remember the opener, set initial focus, handle Escape through `getTopmostModal`, and restore focus. Migrate dialogs gradually. The app-wide Tab trap (`App.tsx:34`) already covers trapping.
- **Visual change:** none.

### 11. New chat and companion form labels are incomplete (medium/low)

- **Evidence:**
  - `components/sidebar/NewChatModal.tsx:406`: Chat name has only `aria-label` and a placeholder, with no visible label. `docs/ui/settings.md` says "Labels sit above wide text inputs".
  - `NewChatModal.tsx:482`: the visible `<label>` "Remote workspace path" isn't linked to its input. A duplicate `aria-label` is used instead.
  - `NewChatModal.tsx:489`: `aria-invalid={!canCreate}` marks the path invalid whenever creating is blocked for any reason, not only path errors.
  - `NewChatModal.tsx:525`: "More options" uses a native `<details>`. `docs/ui/primitives.md` says `Disclosure` replaces native `<details>`.
  - `components/companion/CompanionPreferences.tsx:11`: `<label>Theme</label>` isn't attached to `ThemeSelect`.
- **Who:** screen-reader users, users with cognitive disabilities, and voice-control users (WCAG 1.3.1, 3.3.2).
- **Fix:**
  - Link labels and inputs with `htmlFor`/`id`.
  - Add a visible label above Chat name.
  - Set `aria-invalid` only from path validation (`remoteWorkspaceIssue`).
  - Use `Disclosure`, and label `ThemeSelect`.
- **Visual change:** yes (the visible label and the `Disclosure` look); needs screenshots.

### 12. Many sidebar actions share one name (low/medium)

- **Evidence:**
  - `components/sidebar/SessionItem.tsx:949`: "More actions".
  - `SessionItem.tsx:924`: "Pin chat" or "Unpin chat", without the chat name, while the neighbouring Done and Snooze buttons do include it (`:721`, `:738`).
  - `components/sidebar/FolderTree.tsx:2267`: "Folder actions".
- **What fails:** a screen reader's list of buttons shows many identical entries (WCAG 2.4.6).
- **Who:** screen-reader and voice-control users.
- **Fix:** include the item name, for example `More actions for ${sessionName}`, `Pin ${sessionName}`, `Actions for ${folderName}`. Keep the tooltip text short.
- **Visual change:** none.

### 13. Each outgoing message's delivery state is a focusable live region (low)

- **Evidence:**
  - `components/chat/ConversationTurn.tsx:37`: `<span tabIndex={0} role="status" aria-label=…>` on every outgoing message.
  - `components/chat/OutgoingMessageRow.tsx:41` also renders the error with `role="alert"`.
- **What fails:** queued messages each announce state changes, and a failure is announced twice.
- **Who:** screen-reader users.
- **Fix:** remove `role="status"`; keep the focusable span for the tooltip, without the live region. Report delivery changes once through the chat announcement from finding 4.
- **Visual change:** none.

### 14. The queue reorder animation ignores the Animation speed setting (low)

- **Evidence:** `components/chat/InlinePromptQueue.tsx:87`: the FLIP animation uses a fixed `duration: 180` instead of `scaledMs(180)`. `components/sidebar/sortableDrag.ts:390` and `components/settings/SettingsModal.tsx:700` both scale it.
- **What fails:** `docs/ui/motion.md` says "New transitions use those tokens rather than fixed timings", so the Fast and Slow settings are ignored here. Reduced motion and Motion Off are still respected through `motionAllowed()`.
- **Who:** users who chose a non-default animation speed.
- **Fix:** `duration: scaledMs(180)`.
- **Visual change:** none at the default speed.

## Fix tasks (no two tasks share a file)

| Task | Findings | Files | Visual change |
|---|---|---|---|
| **A. Chat announcements** | 1, 4, 13 | `components/chat/StatusHelpers.tsx`, `components/chat/ConversationWork.tsx`, `components/chat/ConversationTurn.tsx`, `components/chat/OutgoingMessageRow.tsx`, and `components/views/ChatView.tsx` transcript lines ~3740–3910 only (not the composer branch; coordinate with the agent editing that file) | No |
| **B. Sidebar rows** | 2, 3, 6, 12 | `components/sidebar/SessionItem.tsx`, `components/sidebar/FolderTree.tsx`, `components/sidebar/ChatSearchBar.tsx`, `components/sidebar/sidebar.css` (set sizes here, not in `index.css`) | Yes: screenshots |
| **C. Forced colours** | 5 | `index.css`, `components/chat/conversation.css` (rail marks; or `MessageNavigationRail.tsx` if they stay inline styles) | Forced-colours mode only |
| **D. Dialog focus and forms** | 7, 10, 11 | `components/ui/Overlay.tsx`, `utils/modalFocus.ts`, `components/sidebar/NewChatModal.tsx`, `components/sidebar/RemoteDirectoryBrowser.tsx`, `components/companion/CompanionPreferences.tsx` | Yes (label, `Disclosure`, possibly backdrop): screenshots |
| **E. Shared row semantics** | 8, 9, 14 | `components/shared/SelectionGrid.tsx`, `components/chat/ChangedFilesTree.tsx`, `components/chat/InlinePromptQueue.tsx` | Yes (tone icon): screenshots |

Each task should update the owning UI doc and add or adjust a Vitest assertion in the same commit, as `AGENTS.md` and `CLAUDE.md` require.

## Held back: files other agents are editing now

These are in the ChatView composer branch, the notification/toast components and the settings components. They were not included in the tasks above.

- **Composer: message box named only by its placeholder.** `components/views/ChatView.tsx:4414` sets `aria-label` only while editing (`"Edit message"`). Otherwise the name comes from the placeholder at `:4432`, which changes with the provider. Fix: a stable `aria-label` such as "Message".
- **Composer: invalid ARIA on slash-command options.** `components/views/ChatView.tsx:4330`: the options are `role="option"` buttons carrying `aria-expanded` and `aria-haspopup`, which that role doesn't support. Fix: remove them.
- **Composer: chip remove button too small.** `components/chat/ContextChip.css:52`: the context chip remove button is 18×18, below 24px. Fix: enlarge the hit area with padding or a pseudo-element.
- **Notifications: one landmark per alert.** `components/layout/AppShell.tsx:149`: each transient alert's message is a focusable `role="region"` named "Notification message", so every alert adds a landmark. Fix: drop the role or give the whole stack one region.
- **Notifications: extra Tab stop per row.** `components/layout/AppShell.tsx:738`: each notification row has `<time tabIndex={0}>`, used only to show a tooltip. Fix: put the exact time in the accessible name or description instead of a separate focus stop.
- **Settings: theme tiles are not a roving group.** `components/settings/SettingsModal.tsx:1896`: the Colour theme radio group handles arrow keys, but every tile is still a Tab stop. The lint also flags that the group itself is not focusable. Fix: give only the selected tile `tabIndex={0}`. **Needs checking in the running app.**

## What passed

- **Labels:** no icon-only control lacks an accessible name. `IconButton` requires `label` (`components/ui/IconButton.tsx:13`), and the lint's `control-has-associated-label` check found no unlabelled controls.
- **Visible focus:**
  - The global `:focus-visible` ring (`index.css:1188`) comes after Tailwind's utilities (`index.css:8`), so the many `outline-none` text fields still show a ring.
  - Wrappers that draw the ring for an inner control (chat search, composer surface, question rows, context chips) are correct.
  - The `outline: none` rules found in component CSS all have a `:focus-visible` replacement.
- **Reduced motion and Motion Off:**
  - One shared stop handles both (`index.css:261–273`).
  - The JavaScript list animations check `motionAllowed()`.
  - `usePresence` exit animations change immediately.
- **Dialog focus trap:** the app-wide Tab trap (`App.tsx:34`, `utils/modalFocus.ts`) covers every `aria-modal` dialog. ConfirmDialog, Tool details (`useToolQuestionDialog`), FilePreview, RepositoryDiff, ChatBranchPicker and GeminiChildBrowser all restore focus.
- **Primitives:** keyboard support in MenuSelect, Tabs, SegmentedControl, the message navigation rail slider and the changed-files tree is in place.
- **Hover-revealed actions:** these also appear when keyboard focus is in their row (`group-focus-within`, `group-has-[:focus-visible]`).
- **Error messages:** inline errors mostly use `role="alert"`, and field errors are linked with `aria-describedby` (for example `RemoteDirectoryBrowser.tsx:117–118`).
- **Transient alerts:** they pause their countdown on hover and focus, and each one is kept in the notifications panel.

## Not covered by this audit

- **Running app:** nothing was checked in the native app. There are no screenshots and no screen-reader session (NVDA, VoiceOver); live-region behaviour was judged from code.
- **Contrast:** colour contrast in each theme was not measured beyond the existing `onAccentContrast` and `themeTokens` tests.
- **Phone:** the native iOS companion was not audited.
- **Dragging alternatives (WCAG 2.5.7):** keyboard or single-pointer alternatives for every drag were not fully traced. Sidebar workspaces and chats have arrow-key moves and collections have "Move to collection" menus, but reordering collections relative to each other and dropping a chat onto a pane were not checked.
