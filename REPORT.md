# Report: v5/fix-ui-messages-notifications

App notices in UI-V2 now go through the shared notification system as one short sentence. Backend text and next steps move to the expandable detail in the bell row. The review loop is finished: 2 fresh-context Opus agents over 2 rounds, and all blockers and should-fixes are implemented. Frontend tests, `tsc`, the build and the docs check all pass.

**Final SHA:** `419a1f6f8f8c0413736c2e8b54c8b09047648f6d`. No PR or issue has been opened.

| SHA | Commit |
|---|---|
| `ec676fb7` | fix(notifications): keep every notice to one line with details in the bell row |
| `4f918103` | fix(settings): show fixed one-line notices and move backend text to detail |
| `be6ffb27` | fix(chat,sidebar): route menu and workspace outcomes through notifications |
| `e6b012ba` | fix(notifications): resolve independent review findings for one-line notices |
| `419a1f6f` | fix(notifications): resolve re-check findings for permission and bridge text |

## Review rounds

**Round 1:** fresh-context Opus (`claude-opus-5-5`). 13 suggested, 13 implemented (0 blocker, 6 should-fix, 7 nit).
- The paused unsent-edit notice is reverted to its documented wording, because that code may belong to the composer work.
- Raw bridge text is now removed from the detail.
- A cancel the user asked for is reported as information, not a failure.
- Queue, slash-command, goal, attachment and dictation messages are now single sentences.
- `documentation-review.md` is corrected.
- Causes are punctuated before the next step.
- Smaller wording and tone fixes, plus tighter tests.

**Round 2:** a separate fresh-context Opus re-check of the round 1 fixes (`claude-opus-5-5`). It confirmed 10 fixes and found 3 only partly done. It made 11 new suggestions (1 blocker, 2 should-fix, 8 nit), and I implemented 9:
- **Blocker:** permission-mode and Computer Use refusals put backend text in the headline. They now name the control, and the reason goes to the detail.
- **Should-fix:** removed bridge text is now logged to the diagnostics log.
- **Should-fix:** the chat-history notice and the list of remaining sites are fixed.
- **Nits:** the title-cancel race, the bulk-failure sentence, the tone rule in the docs, a second bridge-text form, the vague queue hint and a duplicated Computer Use detail.

Two were declined:
- **Loosened assertions:** I pinned the permission-refusal assertion. The other two still search message and detail together, but the text is still in the right place.
- **`firstSentence` edge cases** (abbreviations, closing quotes): only fixed strings reach it now that backend text goes to the detail.

The round 2 fixes were not reviewed again; the brief allowed one re-check.

## Inventory
- **Shared notification calls:** 172 found, about 85 changed. Both notice helpers now keep only the first line as the message and move the rest into the detail.
- **Bell rows:** these used to be titled "Change not saved" for every failure. They now show the failure's own message.
- **`alert()` or a separate toast system:** none found.
- **Console-only errors:** about 40, left as they were. Their callers already show a notice.
- **Moved from inline to notifications:** title-generation results, snooze save results, and four sidebar workspace failures.
- **Kept inline, because the docs require it:** field validation (snooze amount, workspace directory), errors inside dialogs, the chat's own rows, panel load failures with Retry, and copy confirmations.
- **Kept as documented:** a refused chat control keeps the backend reason in its message. This is an open question for David in `documentation-review.md`, which also lists the remaining sites that still show native text.

## Key messages
| Where | Old | New message (detail) |
|---|---|---|
| Bell row for any failure | "Change not saved" | The failure's own message |
| Remote helper install | `ssh: connect to host … Connection refused` | "Remote helper setup failed." (SSH text and hint) |
| Settings saves | raw `response.error` | e.g. "Could not save Computer Use settings." (cause) |
| Chat workspace action | "Workspace action failed" | "Could not open a terminal at the workspace." |
| Slash command warning | "Command" / "Message not sent" | "The chat was replaced." (next step) |
| Permission refusal | raw backend text | "The permission mode was not changed." (reason) |
| Queue | "Queue changed or could not be reordered. Try again." | "The queue could not be reordered." (hint) |
| Chat export | "Exported 2 chats to /path." | "Exported 2 chats." (folder) |
| Title generation | raw exception inside the menu | toast "Could not generate a title." / "Title generation cancelled." |
| Snooze | inline "Could not save. Try again." | toast "Could not snooze the chat." |

## Tests
- `npx vitest run`: 240 files, 4220 tests passed. The baseline was 239 files and 4215 tests.
- `npx tsc --noEmit -p tsconfig.app.json` passes. The root `tsconfig.json` has no files of its own, so this is the check that actually type-checks the app.
- `npm run build` passes.
- `python3 scripts/check_docs.py` passes.

## Screens needing Before/After capture
1. Settings › Remote hosts: install failure, as a toast and as a bell row (collapsed and expanded).
2. Bell panel holding several different failures.
3. Chat row menu › Regenerate title: success, failure and cancel.
4. Snooze menu: single failure, bulk partial failure, success, and the custom-amount validation (still inline).
5. New workspace form: create failure (toast) and directory validation (inline).
6. Chat workspace menu: Open terminal failure, Move chat success, port warning.
7. Slash commands: `/permission` refusal and the "chat was replaced" warning.
8. Inline prompt queue: reorder failure.
9. Settings › Chat data: export complete.
10. Settings › Computer Use: permission request.
11. Paused unsent-message edit warning.
12. Settings › Data folder: rejected choice.

## Issue preview (no matching issue; not filed)
**Title:** `bug(notifications): app messages show raw errors and several sentences`

```markdown
App notices often use backend or exception text as the message (for example raw SSH errors), run to two or three sentences, and every failure in the bell panel is titled "Change not saved". Some outcomes (title generation, snooze) only appear inside a context menu that can close first. Product standard from David: one short plain sentence, such as "Local copy doesn't match what we expect.", with details in the log or an expandable detail.

**Desired outcome:** Every app-wide message goes through the shared notification path as one plain sentence; causes, codes, paths and steps go to the detail or the diagnostics log. Inline display remains only where docs/ui/notifications.md requires it.

**Environment:** UI-V2 on uam/v5.2-sync-main @ 6f235eb9.

**Evidence:** Source inspection found about 70 shared-path call sites passing raw `response.error` text or multi-sentence messages, the fixed "Change not saved" bell title in AppShell.tsx, and menu-local feedback in SessionItem.tsx and SnoozeMenu.tsx. Not reproduced in the running app.
**Reproduction status:** Not attempted in the desktop app; observed in source and unit tests.

**Acceptance criteria:**
- [ ] Notice messages are one plain sentence; backend text appears only in the detail.
- [ ] Bell rows show each failure's own message.
- [ ] Menu outcomes appear as shared notices.
- [ ] David decides whether refused chat controls keep the reason in the message.

**Review:** Self-triage only; no independent review of the report.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code), on behalf of David Taylor.
```

## PR preview (not opened)
**Title:** `fix(notifications): show every app message as one line with details in the bell`

```markdown
App notices could show raw backend or SSH text, several sentences or a cause chain, and every failure in the bell panel was titled "Change not saved". Each notice now shows one plain sentence; backend text, paths and next steps move to the detail, which the bell row expands and Copy message includes. Raw bridge exception text is logged rather than shown. Title-generation and snooze results, which were shown inside menus, and sidebar workspace failures now go through the shared notifications, with warnings and user cancels in their own tones. Field validation, dialog errors, chat-owned rows and panel load states stay inline as the notification agreement requires. Refused chat controls keep their documented reason-in-message format; that tension and the remaining native-text sites are recorded in documentation-review.md.

Before/after screenshots to be added: Remote host install failure toast and expanded bell row; bell panel with several failures; Regenerate title success, failure and cancel; snooze single and bulk failure, success and inline custom validation; new-workspace create failure and inline directory validation; chat workspace terminal failure, move success and port warning; `/permission` refusal and chat-replaced warning; prompt queue reorder failure; chat export success; Computer Use permission notice; paused unsent-edit warning; rejected data-folder choice.

**Proof:**
- `npx vitest run` (UI-V2): 240 files, 4220 tests passed, including the new `src/utils/appFailure.test.ts` (one-line split, detail building, raw bridge text, first-sentence split) and a bulk-snooze notice test.
- `npx tsc --noEmit -p tsconfig.app.json`: passed. `npm run build`: passed (existing chunk-size warning only).
- `python3 scripts/check_docs.py`: passed.
- Not run: native builds or tests (no native changes), visual or GUI checks.

**Review:** Reviewed by 2 agents over 2 iterations. Required Claude GUI review and David's approval remain pending.

- Agent 1: claude-opus-5-5 (fresh context, full branch) — suggested 13; implemented 13.
- Agent 2: claude-opus-5-5 (fresh context, re-check of the fixes) — suggested 11; implemented 9. Declined: loosening two message-or-detail test searches (text location unchanged) and `firstSentence` abbreviation edge cases (only fixed wording reaches it).

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code), on behalf of David Taylor.
```
