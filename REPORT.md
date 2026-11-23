# Report: v5/fix-settings-silent-losses

All three settings bugs are fixed and the review loop is done. The branch is pushed at `75029f4e028de6fa200469fb8e3d860b8ea4267d`, based on `origin/uam/v5.2-sync-main` at `6f235eb9`.

Commits:

- `76c82438` fix(settings): ask before leaving an unsaved skill draft
- `38eda848` fix(settings): ask before an imported theme replaces a saved one
- `26e9b7dc` fix(settings): report a helper install warning instead of "ready"
- `38148bca` fix(settings): address review of the settings silent-loss fixes
- `75029f4e` test(settings): prove Escape on the Skills leave prompt drops the leave

No native code changed (only UI-V2 and docs), so I didn't run the runner build. Nothing has been visually checked in the running app.

## Review rounds

Each round was a separate Opus subagent with fresh context. I requested it with the `opus` alias, so the exact model ID it ran on is not recorded.

**Round 1** reviewed the full diff against the base. It found 7 issues (0 blocker, 2 should-fix, 5 nit). I implemented all 7, so none were declined:

1. **Should-fix:** after Discard or Save & close on the Skills leave prompt, focus fell to the page body. It now returns to the control that asked to leave.
2. **Should-fix:** Keep both failed for theme names longer than 59 characters, because adding " copy" passed the 64-character limit. The name is now shortened to fit.
3. **Nit:** a save failure after the file had passed validation blamed the file. It now says "Theme could not be saved. Try again."
4. **Nit:** Keep both could make two themes with the same name. It now picks "copy", "copy 2" and so on, and the themes doc states the rule exactly.
5. **Nit:** the clash dialog used jargon. It is now one plain sentence.
6. **Nit:** the success notice named the host. It now says "Remote helper is ready."
7. **Nit:** two flows had no tests: Save & close going through Overwrite, and Escape on the leave prompt. Both now have tests.

Round 1's fixes were material, so I ran one re-check.

**Round 2** re-checked the round-1 fix commit. It found 1 nit and I implemented it: the Escape test passed even when Escape kept the pending leave. It now fails in that case; I confirmed this by breaking the code on purpose.

## Issue previews

```markdown
bug(settings): unsaved Skills edits are lost when leaving the page or closing Skills

Edits to a skill are thrown away without asking when you switch Settings pages, close Settings, or click the standalone Skills window's X. The skill library agreement promises drafts are kept until save or a deliberate discard. Reported by David Taylor.

**Reproduction:** Settings › Skills › New skill › Create, type a title, then click another Settings page (or Back to chats, or the standalone window's X).
**Expected:** A prompt to keep editing, discard, or save first.
**Actual:** The page changes or the window closes and the draft is gone.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** Settings mounts the Skills store with no ref and has no Skills branch in its page-exit check; the standalone X calls close directly. Memory Store already uses a requestLeave ref.
**Reproduction status:** Reproduced with new Vitest cases that fail on the base revision. Not reproduced in the running app.

**Acceptance criteria:**

- [ ] Changing page, closing Settings and the standalone X all ask when a skill draft is unsaved.
- [ ] Discard or a successful save continues the leave; Keep editing, Escape or a failed save keeps the draft.

**Review:** Self-triaged; the report itself was not independently reviewed.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

```markdown
bug(settings): importing a theme overwrites a custom theme with the same id

A custom theme's id comes from its name, so importing a theme whose id is already saved (for example one still using the default name) silently replaces the saved theme. Reported by David Taylor.

**Reproduction:** Save a custom theme, export it, change it, then import the exported file.
**Expected:** A choice between replacing the saved theme and keeping both.
**Actual:** The saved theme is overwritten without asking.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** The import keeps the imported id, the store replaces the theme with that id, and the native theme service writes its file by id.
**Reproduction status:** Reproduced with new Vitest cases that fail on the base revision. Not reproduced in the running app.

**Acceptance criteria:**

- [ ] A clashing import asks Replace or Keep both; Cancel saves nothing.
- [ ] Keep both saves under a new id and an unused name within the 64-character limit, leaving the saved theme unchanged.

**Review:** Self-triaged; the report itself was not independently reviewed.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

```markdown
bug(settings): a helper install that succeeds with a warning still says the host is ready

Helper setup can succeed with a warning (startup not confirmed, or the old helper backup could not be removed). The app still shows "<host> is ready.", and the Updates path marks the helper ready straight away. Reported by David Taylor.

**Reproduction:** No live steps recorded; this needs a host whose startup confirmation or backup removal fails during install.
**Expected:** The warning is shown and the host is not reported as ready.
**Actual:** A success message, and the Updates card marks the helper ready.

**Environment:** `uam/v5.2-sync-main` at 6f235eb9.

**Evidence:** The native install handler returns `{ok: true, warning}` in two places. The Settings install handler ignores the warning, and the update monitor sets the helper ready on any `ok`.
**Reproduction status:** Reproduced with new Vitest cases that simulate the native reply and fail on the base revision. No live host test.

**Acceptance criteria:**

- [ ] A warning reply shows a one-line warning notice instead of the ready message.
- [ ] The Updates path does not mark the helper ready when there is a warning.

**Review:** Self-triaged; the report itself was not independently reviewed.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

## PR preview

```markdown
fix(settings): stop silent losses in Skills drafts, theme import and helper installs

Base `uam/v5.2-sync-main`, head `v5/fix-settings-silent-losses`. Related to <issue numbers once filed>.

- Skills drafts: changing page, closing Settings and the standalone X now show the existing unsaved-changes prompt. Discard or a successful save continues the leave; Keep editing, Escape or a failed save keeps the draft. If the page closes, focus returns to the control that asked to leave.
- Theme import: a clashing id asks Replace or Keep both. Keep both saves under a new id and an unused "<name> copy" / "copy N" name within the 64-character limit. A save failure says "Theme could not be saved. Try again."
- Helper installs: a success with a warning shows the warning as a one-line warning notice and no longer marks the helper ready. The clean success message no longer names the host ("Remote helper is ready.").

Docs updated in the same commits: skill library, themes and remote hosts pages.

<Before/After captures needed; see the list below.>

**Proof:**
- 10 new Vitest cases.
  - 7 of the original 8 failed against the base code and pass with the fix. The 8th was tightened afterwards to require the prompt.
  - The cases added in review also fail without their fix: focus return, Escape dropping the leave, and Keep both naming.
- Full Vitest suite at 38148bca: 239 files, 4,225 tests passed. The final commit 75029f4e changes one test only, and its file was rerun: 26 passed.
- `npm run build` (which runs `tsc -b`) succeeds at 38148bca, with the existing chunk-size warning; the final commit changes tests only.
- `npx tsc --noEmit -p tsconfig.app.json` is clean. A bare `tsc -p .` checks nothing here because the root config has no files.
- `python3 scripts/check_docs.py` passed.
- Not run: native build or tests (no .cpp/.h changes), a visual check in the running app, or a live helper install.

**Review:** Reviewed by 2 fresh-context agents over 2 iterations. Required Claude review sign-off and David's approval are still pending.

- Agent 1: Opus (opus alias; exact model ID not recorded), full diff — suggested 7; implemented 7.
- Agent 2: Opus (opus alias; exact model ID not recorded), re-check of the review fixes — suggested 1; implemented 1.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

## Screens that need Before/After capture

1. **Settings › Skills:** with an unsaved new skill, click another Settings page, and separately Back to chats. Before, the page changes; after, the "Discard your changes?" prompt appears. After Discard, focus should be on the clicked page button.
2. **Standalone Skills window:** click the X with an unsaved draft. Before, it closes; after, it prompts.
3. **Settings › Appearance › Themes:** import a theme whose id is already saved. Before, it is overwritten silently; after, the Replace / Keep both dialog appears. Also capture the theme list showing "<name> copy" (or "copy 2") after Keep both.
4. **Settings › Remote Hosts:** an install that returns a warning. Before, a "<host> is ready." success toast; after, a warning notice. A clean install now shows "Remote helper is ready."
5. **Updates panel:** a helper update that returns a warning. Before, the card clears straight away; after, a warning notice appears and the card waits for the app's next state update.

## Open points for David

- **Notice wording:** with your no-host-names preference, neither notice names the host. With "Update everything" and several helpers, a warning doesn't say which host it came from.
- **Native warning text:** it is shown as-is. The "backup could not be removed" warning doesn't say the helper was installed.
- **Process:** I didn't claim or file issues, because you asked for none. My commits end with only the Co-Authored-By line you specified.
