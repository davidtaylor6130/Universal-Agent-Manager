# Report: v5/fix-companion-upload-restart-warning

Both bugs are fixed and pushed to `v5/fix-companion-upload-restart-warning`, based on `uam/v5.2-sync-main`.

**Final SHA:** `32136da4c151c957783b52bbaacbe1cfafa91d3d`

## Commits

| SHA | Commit |
|---|---|
| `549704dd` | fix(companion): upload large browser requests in chunks |
| `96cefc3b` | fix(settings): confirm phone access restart that stops running chats |
| `c2a2611c` | test(companion): cover browser upload failures before commit (round 1 fixes) |
| `32136da4` | test(companion): cover the final pre-commit check and chunk cap boundary (round 2 fixes) |

Each commit ends with the `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` line. No native code changed. No PRs, issues or comments were created.

## Decisions

- **Bug 1:** Requests of 61,440 bytes or more now go through capabilities, begin, append and a single commit, like the iPhone client. No new request ever exceeds the 64 KiB wire limit.
  - A failure before the commit says the request was not sent.
  - A lost commit reply keeps the existing "may have reached UAM" message and is never retried.
  - The browser also refuses any upload needing more than 4,096 chunks, as the Swift client does.
- **Bug 2:** I chose the confirm dialog, not a refusal.
  - `docs/settings/phone-access.md` requires Restart Universal Agent Manager to stay available beside the failure as a recovery. Refusing it would remove that recovery while any chat runs.
  - The docs also say phone changes must not stop provider work, so the restart now asks first and names the chats.
  - A chat counts as running when it is working, waiting for input, or has a live CLI session.
  - The button is in a new `PhoneRestartButton.tsx`. `SettingsModal.tsx` changes only one import and one control line.
- **Pre-existing, not changed:** The Swift client sends exactly 61,440 bytes directly (`<=`), while the docs and the new browser code upload at 61,440. Both stay under the wire limit.

## Review rounds

Both reviewers were fresh-context Opus subagents (`claude-opus-5-5`).

### Round 1: independent review of the whole diff

6 suggested (0 blocker, 1 should-fix, 5 nits); 3 implemented, 1 partly implemented, 2 declined.

1. **should-fix**, implemented: tests for failures before the commit (token change during upload, 401 on append, 403/404 capabilities fallback, invalid acknowledgement).
2. **nit**, declined: the dialog disappears if a status refresh clears the failure while it is open. Fixing it needs more lines in `SettingsModal.tsx`, which other agents are editing. The dialog only closes when restart is no longer offered.
3. **nit**, declined: cache the capabilities reply. The task asked to mirror the Swift client, and a cache adds per-token state for a rare path.
4. **nit**, implemented: cap uploads at 4,096 chunks, as the Swift client does.
5. **nit**, implemented: corrected the `docs/phone-browser.md` wording for messages before the commit.
6. **nit**, partly implemented: the new test file is now listed among the focused checks in `react-cef-transport.md`.
   - I kept it as a separate file with its own storage helper, to avoid conflicts with other agents.
   - I did not add a `Claude-Session:` trailer, because the task sets the commit ending line.

### Round 2: re-check of the round 1 fixes

3 suggested (1 should-fix, 2 nits); 3 implemented.

1. **should-fix**, implemented: test the token check just before the commit (logout after the final append).
2. **nit**, implemented: test the cap boundary at exactly 4,096 chunks.
3. **nit**, implemented: run the older-UAM fallback for both 403 and 404, and assert that nothing is dispatched after a 401.

**Totals:** 9 suggested, 7 implemented (one partly), 2 declined.

## Issue previews (not filed)

### Issue 1

**Title:** `bug(companion): phone browser cannot send requests over 64 KiB`

The phone browser companion sends every request as one `/api` body. The companion listener rejects bodies over 65,536 bytes with 413, so a long prompt or other large request from the browser never reaches UAM. The protocol reference says clients upload larger requests in chunks, and the server advertises `requestChunks: true`. The iPhone client does this, but the browser bridge never calls `beginCompanionRequestTransfer`.

**Reproduction:** Open the phone browser companion and send a chat prompt whose request is 64 KiB or more.
**Expected:** The prompt is uploaded in chunks and sent.
**Actual:** The request fails with "Request exceeds 64 KiB."

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:**
- Observed in code: `UI-V2/src/ipc/cefBridge.ts` sends one `fetch('/api')` body. `src/cef/companion_server.cpp` returns 413 above 65,536 bytes.
- `docs/reference/companion-protocol.md` (Connection and authorization; Chunked request envelopes) describes the upload. `src/common/companion/protocol.h` `Capabilities` advertises it. `Mobile/iOS/UAMKit/Sources/UAMKit/CompanionClient.swift` `sendEnvelope` implements it.

**Reproduction status:** Reproduced with a mocked-server Vitest case (a 200 KiB request receives 413). Not tried against a live app.

**Acceptance criteria:**

- [ ] Requests of 61,440 bytes or more are uploaded with begin, append and one commit, and no request body exceeds 64 KiB.
- [ ] Failures before the commit say the request was not sent.
- [ ] A lost commit reply is reported as an unknown outcome and is never retried.

**Review:** Report checked by the implementing agent against the code. No independent report review.

**Prepared by:** Claude Opus 5.5 in host app not recorded (Claude Code, cloud session), on behalf of David Taylor.

### Issue 2

**Title:** `bug(settings): phone access restart stops running chats without warning`

On the Phone settings page, Restart Universal Agent Manager (shown beside a phone access failure or a restart-required change) restarts the app at once. That ends running provider turns and CLI sessions without warning. The data-folder restart refuses while sessions run, and `docs/settings/phone-access.md` says phone access changes apply without restarting provider work.

**Reproduction:** Start a chat turn. Open Settings > Phone while phone access has failed or needs a restart, then choose Restart Universal Agent Manager.
**Expected:** UAM warns which running chats will stop before restarting.
**Actual:** The app restarts straight away and the running work ends.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:**
- Observed in code: `SettingsModal.tsx` `restartApplication` sends `restartApplication` directly.
- `src/cef/uam_query_handler.cpp` `HandleWindowChrome` restarts with no session check.
- `HandleRestartToApplyDataFolder` refuses with 409 while ACP sessions or CLI terminals run.

**Reproduction status:** Reproduced with a SettingsModal Vitest case (restart is sent with no dialog). Not tried in the running app.

**Acceptance criteria:**

- [ ] With any chat working, waiting for input or holding a live CLI session, a confirmation names the chats and their count ("Restart will stop 3 running chats.") before restarting.
- [ ] Cancel leaves the app running.
- [ ] With no running chats, restart proceeds at once.

**Review:** Report checked by the implementing agent against the code. No independent report review.

**Prepared by:** Claude Opus 5.5 in host app not recorded (Claude Code, cloud session), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `fix(companion): upload large browser requests and confirm restarts that stop chats`
**Base:** `uam/v5.2-sync-main`
**Head:** `v5/fix-companion-upload-restart-warning`
**Status:** draft until the required UI review and David's approval are recorded.

The phone browser companion now uploads requests of 61,440 bytes or more in chunks: capabilities, begin, append, then exactly one commit, like the iPhone client. Long prompts no longer fail with 413.
- A failure before the commit says the request was not sent.
- A lost commit reply keeps the unknown-outcome message and is never retried.
- Uploads are capped at 4,096 chunks.

On the Phone settings page, Restart Universal Agent Manager now asks first when chats are running. The confirmation names them and their count ("Restart will stop 3 running chats."). With none running it restarts at once. A refusal was not used, because the docs keep this restart as the recovery beside phone access failures. `SettingsModal.tsx` changes only one import and one line.

Docs updated in the same commits: `docs/phone-browser.md`, `docs/settings/phone-access.md`, `docs/reference/companion-protocol.md`, `docs/reference/architecture/react-cef-transport.md`.

Closes #<bug 1 issue>. Closes #<bug 2 issue>.

<Visual evidence needed: Before/After captures listed below.>

**Proof:**
- **Regression tests fail on the old code:**
  - New `UI-V2/src/ipc/companionRequestUpload.test.ts`: against the old bridge, 4 of the 5 cases in its first version failed. All pass with the fix; the file now has 13 cases.
  - New SettingsModal case "asks before an application restart that would stop a running chat": failed against the old `SettingsModal.tsx`, passes now.
- **Tests on final HEAD `32136da4`:**
  - `npx vitest run src/ipc`: 27/27 pass.
  - `npm run test:unit`: 234 files, 4,200 tests pass.
  - `PhoneRestartButton.test.tsx` and `SettingsModal.test.tsx` pass (run on `96cefc3b`, unchanged since).
- **Performance suite:** `npm run test:performance`, run serially on `96cefc3b`: 7 files, 33 tests pass.
  - An earlier unsupported parallel run of everything missed the ChatView "empty new-chat composer" 100 ms budget, at 103.29 ms. ChatView is untouched, and the case passed in the serial run.
- **Type check and build on final HEAD:**
  - `npx tsc --noEmit -p .` is clean.
  - `npm run build` passes. It runs `tsc -b`, which caught a TS7022 inference error that `tsc -p` missed; that was fixed before push.
- **Docs:** `python3 scripts/check_docs.py` passes.
- **Not run:**
  - native builds or tests (no native code changed);
  - live app, phone browser or HTTPS checks;
  - Swift checks;
  - visual or theme checks of the dialog.

**Review:** Reviewed by 1 independent reviewer over 2 iterations. Independent claude-opus-5-5: 9 suggested, 7 implemented (one partly). Two nits were declined:
- the dialog disappearing when the failure clears (it would need more lines in the shared `SettingsModal.tsx`);
- a capabilities cache (the task asked to mirror the Swift client).

The required Claude UI review and David's approval are still pending.

**Created with:** Claude Opus 5.5 in host app not recorded (Claude Code, cloud session), on behalf of David Taylor.

## Screens needing Before/After capture

1. **Settings > Phone, phone access failed or restart required, with at least one running chat, after clicking Restart Universal Agent Manager.**
   - Before: the app restarts at once.
   - After: the "Restart Universal Agent Manager?" dialog with "Restart will stop N running chats." and the list of chat names.
   - Capture light and dark themes, and the case with more than 5 chats ("and N more").
2. **The same page with no running chats.** Restart proceeds at once, with no dialog (unchanged behaviour).
3. **Phone browser companion, sending a prompt of 64 KiB or more in a chat.**
   - Before: the "Request exceeds 64 KiB." error.
   - After: the prompt is sent and appears in the transcript.
