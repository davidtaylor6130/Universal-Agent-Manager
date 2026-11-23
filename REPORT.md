# Native error messages: one plain sentence

Work branch: `v5/fix-native-error-messages`, final SHA **`409bbbea6ac8657d338e101b44e8ab537758aa5d`**. It is based on `uam/v5.2-sync-main` @ `6f235eb9`.

Native tests have not been run (they need the Mac) and the desktop app has not been built, so the test updates below are unconfirmed.

## Commits

| SHA | Commit |
|---|---|
| `bf0e4eb9` | fix(errors): one-line runtime, provider and remote messages |
| `3fbee484` | fix(errors): one-line app service messages |
| `cd829ccf` | fix(errors): one-line bridge refusals |
| `d5185c71` | fix(errors): address independent review of one-line messages |
| `409bbbea` | fix(errors): address re-check nits on review fixes |

## What changed

- **Messages:** 757 distinct message strings were rewritten. About 200 more messages that stacked a cause, chat id or path behind a prefix now show one fixed sentence instead.
- **Logging:** where a message lost detail, that detail is now written to the diagnostics log at the same place (about 150 new log calls). Where the inner reason was already a single sentence the user can act on, it is shown on its own without the prefix.
- **Tests:** about 30 test files updated in total.
- **Docs:** `logging.md` now states the one-line rule; `permissions.md`, `sending.md`, `remote-hosts.md` and `notifications.md` quote the new wording.

## Review rounds

Two independent reviewers (both fresh-context `claude-opus-5-5`), over 2 iterations.

**Round 1: 17 suggested; 15 implemented, 1 partly implemented, 1 declined.**
- **Blockers (4, all fixed):**
  - Copilot CLI View retry: the frontend checks for the exact prefix "Checking GitHub Copilot CLI compatibility.", so the backend returns that sentence again.
  - About 50 test assertions were still looking for old wording; they now look for the new text.
- **Should-fix (6, all fixed):**
  - Native-history issue counts need errors to start with the provider name; they do again.
  - The workspace-move refusal names the chat and the action in one line, and still says the workspace was kept.
  - Live mode and permission refusals give the reason again: the provider is busy, another setting change is pending, or the provider is still starting.
  - The SSH helper connection's own setup and build refusals are shown instead of a generic "could not connect".
  - The final-save warnings say "unconfirmed" again.
  - Remote OpenCode export stdout (which holds the exported chat JSON) is no longer logged.
  - While fixing these I also put back three facts the first pass had dropped: "resend them" after a permission-queue overflow, the "change provider, it may run twice" way out, and the kept provider process in routing waits.
- **Nits:**
  - Implemented: 11, 12, 15, 16, 17.
  - Partly implemented: 13. I fixed the "Warning:" text in `git_worktree_service`. I kept the OpenCode V2 "not verified" wording because tests depend on it, kept "exited before session creation" because it passes on provider output, and left the memory-store chain because the runner that compiles it has no diagnostics log.
  - Declined: 14. The frontend's own copies and test mocks are frontend code, outside this native-only task, and they still work with the backend text.

**Round 2 (re-check of the round-1 fixes): 3 suggested, 3 implemented.**
- Made a weakened CLI-update test assertion specific again.
- Reworded the SSH helper no-connection refusal to fit all its triggers.
- Moved a doc comment back to the function it describes.

## Most important message changes

| Old | New |
|---|---|
| Queue changed. Refresh before reordering. | The queue changed; refresh it before reordering. |
| UAM cannot confirm whether an earlier message reached the provider, so this one was not sent. Wait for that turn to settle, or change the chat's provider and accept the warning that the prompt may run twice. | This message was not sent; wait for the earlier unconfirmed one, or change the chat's provider and accept it may run twice. |
| Direct input is held because its owned admission was not confirmed. | This message is held because UAM could not confirm it was accepted. |
| This response exceeds the captured read-only access ceiling. Only supported one-time read approvals are available. | In read-only mode, only one-time read approvals are allowed. |
| The runner restarted, but this exact provider turn is gone. Chat recovery is blocked… its goal is retained. | The runner lost this turn on restart; review the chat before retrying, and your goal is kept. |
| Chat not found: `<id>` (26 bridge sites) | Chat not found. |
| Stop active provider sessions before requesting a data-folder transfer. Their teardown has no generic confirmed-stop receipt. | Stop active provider sessions before transferring the data folder. |
| The chat or its saved history changed, so this refresh was not saved. Current history was kept. Try refreshing again. | The chat or its saved history changed, so this refresh was not saved and current history was kept. |
| “X” has a turn in progress on this host. Stop it or let it finish, then update the helper. (chat-process) | “X” has a turn in progress on this host; stop it or let it finish before updating the helper. |
| Attachments must exactly match this chat's companion staging results and workspace. Stage client bytes first. | Attachments do not match the files uploaded for this chat; attach them again. |
| SSH connection failed: `<error>` | Could not connect to the remote host. (Setup and build refusals pass through.) |
| Branch X could not be created before the chat started. `<message>` | Branch X could not be created, so the chat did not start. |
| Source workspace is not an available directory. Restore the folder, then retry. | The source folder is unavailable; restore it, then retry. |
| Your request was recorded, but delegation could not be confirmed. No agent was started. | Your request was saved, but no agent started because delegation could not be confirmed. |
| SVN did not confirm switching. Inspect the working copy before retrying; no automatic rollback was attempted. | SVN did not confirm the switch and nothing was rolled back; inspect the working copy before retrying. |
| Remote setup step failed (exit 3): Copy runner — `<stdout>` — `<stderr>` | Remote setup step failed: Copy runner. |
| Gemini Plan and read-only turns cannot start on an SSH host: UAM cannot install its Plan policy there, … | Gemini Plan and read-only modes cannot run safely over SSH; use a local workspace or Default mode. |
| Provider session shutdown could not be confirmed. The update was not installed. Stop the session and retry. | Could not confirm provider sessions stopped, so the update was not installed. |
| The primary settings file and its recovery backup are invalid. UAM did not overwrite either file. | The settings file and its backup are both invalid, so UAM left them unchanged. |
| The provider exceeded UAM's 64-request permission queue limit. Queued follow-up prompts were not sent; resend them. | The provider sent too many permission requests, so queued follow-up prompts were not sent; resend them. |

## Not verified

- No native core tests were run; they need the Mac.
- The desktop app was not built; Linux refuses it.
- CEF can't be downloaded here, so I wrote stand-in CEF headers. With them, every changed non-platform source and test file shows no new compile errors compared with its previous version. Five Windows- and macOS-only files were reviewed by eye only.
- The runner-only build passed, with SQLite taken from a local copy because the official download is blocked.
- `python3 scripts/check_docs.py` passed.
- Some frontend notices put their own sentence in front of the backend reason ("The chat was not renamed. " + reason). The frontend also keeps its own copies of a few messages. Both are frontend code and were not changed.
- Two strings the frontend compares exactly stay two sentences: "Chat history changed while loading. Retry to refresh it." and the bridge payload prefixes.

## Screens needing Before/After capture

- Toasts and the bell for chat send, queue and branch refusals.
- Chat mode and permission refusal toasts.
- Settings → Remote hosts install refusals.
- Updates panel CLI update errors.
- Settings → Native history: discovery issue counts.
- Data folder settings status.
- Permission card reasons.
- Timeout and stream-watch transcript notices.
- The new-chat branch picker.
- The Copilot CLI View auto-start after the compatibility check.

## PR preview

**Title:** `fix(errors): show native errors and status as one plain sentence`

```markdown
Native error and status messages often showed several sentences, stacked causes, ids, exit codes or paths. They are now one short plain sentence across the ACP runtime, provider runtimes, app services, remote setup and the CEF bridge, about 760 message strings plus about 200 messages that were built from several parts. Where detail was dropped, it is written to the diagnostics log at the same place. Refusals still refuse, and safety facts (nothing sent, input kept, no update started, nothing rolled back, resend queued prompts) stay in the sentence. Strings the frontend compares (the Copilot compatibility prefix, native-history provider prefixes, chat-history reload) were kept compatible. Model and agent text, and provider CLI output passed through as is, were left alone. The pull-before-chat default-base refusal is unchanged.

| Before | After |
|---|---|
| Queue changed. Refresh before reordering. | The queue changed; refresh it before reordering. |
| Chat not found: `<id>` | Chat not found. |
| SSH connection failed: `<error>` | Could not connect to the remote host. |
| Remote setup step failed (exit 3): Copy runner — `<stdout>` — `<stderr>` | Remote setup step failed: Copy runner. |
| “X” has a turn in progress on this host. Stop it or let it finish, then update the helper. (chat-process) | “X” has a turn in progress on this host; stop it or let it finish before updating the helper. |
| Provider session shutdown could not be confirmed. The update was not installed. Stop the session and retry. | Could not confirm provider sessions stopped, so the update was not installed. |
| The primary settings file and its recovery backup are invalid. UAM did not overwrite either file. | The settings file and its backup are both invalid, so UAM left them unchanged. |
| This response exceeds the captured read-only access ceiling. Only supported one-time read approvals are available. | In read-only mode, only one-time read approvals are allowed. |

Real app screenshots are still needed for the changed toasts, settings rows, transcript notices and the CLI View auto-start.

**Proof:**
- `cmake --build Builds/runner --target uam_runner` builds on Linux (SQLite from a local copy).
- A syntax-only compile of each changed non-platform source and test file, using stand-in CEF headers, shows no new errors compared with its previous version. 5 Windows/macOS-only files were reviewed by eye.
- `python3 scripts/check_docs.py` passed.
- Not run: native core tests and the desktop build.

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 — suggested 17; implemented 15, partly implemented 1. Declined 1: frontend copies and mocks, outside this native-only change. Partly implemented: OpenCode V2 and pass-through wording kept because tests depend on it.
- Agent 2: claude-opus-5-5 (re-check) — suggested 3; implemented 3.

David's own Claude review is still pending; it happens locally.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
```

## Issue preview (no matching issue found)

**Title:** `bug(errors): native error and status messages are hard to read at a glance`

```markdown
Error and status messages from the native code often run to several sentences, stack causes ("X failed: Y: Z") or include chat ids, paths, exit codes and internal terms. The product owner's standard is a single plain line such as "Local copy doesn't match what we expect.", with details kept in the diagnostics log.

**Expected:** Every user-facing native error or status message is one short plain sentence. Detail is logged at the same place.
**Actual:** Many refusals and notices are multi-sentence or carry ids, paths and stacked causes. Examples: "Chat not found: <id>"; "Remote setup step failed (exit 3): <step> — <stdout> — <stderr>"; "Queue changed. Refresh before reordering."

**Evidence:** Observed in source on `uam/v5.2-sync-main` @ 6f235eb9. A scan found about 870 multi-sentence literals and about 390 prefix chains under `src/`.
**Reproduction status:** Source review only; not reproduced in the running app.

**Acceptance criteria:**
- [ ] User-facing native messages are one plain sentence, without stacked causes, ids or paths unless the user needs them to act.
- [ ] Dropped detail is written to the diagnostics log.
- [ ] Safety refusals still refuse, and no error reads as success.
- [ ] Changed screens are checked in the real app.

**Review:** Not independently reviewed; self-triaged.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code harness), on behalf of David Taylor.
```
