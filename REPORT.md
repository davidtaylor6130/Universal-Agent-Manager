# Report: v5/fix-ui-store-stream-leaks

**Work branch:** `v5/fix-ui-store-stream-leaks`. Final SHA: `70894231`. Branched from `uam/v5.2-sync-main` at `6f235eb9`. That base has since gained a CMake-only change (`c252f9a9`), which merges cleanly. Nothing was opened or posted: no PR, issue, comment or label, merge or force-push.

## Commits

1. `d445838f` fix(ui-store): keep unopened chats unloaded while they stream
2. `11c7fc06` fix(ui-store): use the page-local assistant index at streamDone
3. `5acdca5e` fix(ui-store): drop history read state for chats native removes
4. `6378d020` fix(ui-store): re-pull state rejected before startup hydration
5. `5adb3c27` fix(ui-store): drop unloaded-chat tokens on arrival and prune page starts (review round 1)
6. `70894231` test(ui-store): tighten history-map identity and stream test typing (review round 2)

## Review loop

Each round used one independent reviewer: a fresh-context Opus sub-agent (`opus` alias, `claude-opus-5-5` in this session). Each got the issue text, the diff and the project docs.

**Round 1 (full branch): 4 suggested, 4 implemented (0 blocker, 1 should-fix, 3 nit).**

1. **Should-fix:** a token for an unloaded chat was checked only when the buffer flushed. A token that arrived while the first history page was loading stayed buffered, then was appended to a page that already held it, so the newest text showed twice ("Background answer answer"). **Fixed:** such tokens are now dropped when they arrive. The test now answers the history read after such a token, and that test fails on the earlier code.
2. **Nit:** a chat whose summary still counts 0 messages keeps the old behaviour (a fragment, then a kept page). This was kept as the issue asked. **Fixed:** the transport reference now says so.
3. **Nit:** removing chats rebuilt all four history maps even when none of their entries changed. **Fixed:** an unchanged map keeps its identity.
4. **Nit:** a full state never pruned `historyStartIndexBySessionId` (the loaded page's start index) for chats it does not list. **Fixed:** it is now pruned alongside the four maps. The test checks this, and that assertion fails on the earlier code.

**Round 2 (fresh re-check of the round-1 fixes): 2 suggested, 2 implemented (0 blocker, 0 should-fix, 2 nit).**

1. **Nit:** the new test's deferred reply was inferred as `() => never`, which is a type error. Test files are excluded from the type check, so nothing caught it. **Fixed:** it is typed `() => void`, and the error is gone in a type check of the test file.
2. **Nit:** only one of the four history maps was checked for identity. **Fixed:** all four are now checked after a no-op patch, a patch that removes only unknown chats, and a full state that prunes nothing.

**Totals:** 6 suggested, 6 implemented, none declined.

## Verification

- **On `5adb3c27`:**
  - `npx vitest run` passed: 239 files, 4,221 tests.
  - `npm run test:performance` passed: 7 files, 33 tests.
  - `npx tsc --noEmit -p .` passed, but it checks nothing because the root tsconfig has `files: []`.
  - `npx tsc --noEmit -p tsconfig.app.json` passed.
  - `npm run build` passed.
  - `python3 scripts/check_docs.py` passed.
- **On `70894231` (test-only change):** `src/store/statePatchResync.test.ts` and `src/store/useAppStore.test.ts` passed, 379 tests.
- **Regression tests:** each fix's new case failed on the code before it and passes after. The one exception is the bug 4 case where the snapshot already covers the rejected revision, which passes both ways by design.
- **Not run:** native builds and tests, the packaged app, the performance suite script, and before/after memory or timing measurements. The diff contains no C++.

## Points to know

- **Bug 4 differs from the request.** It does not always resync after startup. It records the highest rejected revision (an unknown revision counts as infinite) and pulls again only if the startup snapshot is older, because a snapshot at revision R contains every patch up to R. Making it unconditional is a one-line change if David prefers that.
- **Bug 1 has a narrow window.** Tokens for a chat that is still loading its first page are dropped. A token sent around the moment native reads the page can be missing, or occasionally repeated, until the turn ends and the transcript reloads. The transport reference documents this.
- **Coordination:** no goal-mapping lines, no `sessionsSlice.ts` changes and no C++ changes.

## Screens needing Before/After capture

These are store-only changes, and no captures were taken. Visible effects to capture in the packaged app:

1. **Long chat at turn completion (bug 2).** In a chat with more than 50 messages, the reply appears twice before the fix and once after.
2. **Opening a background chat mid-turn (bug 1).** Open a running chat that has saved history. History loading should show, then the live reply continuing, without repeated text.
3. **Optional, memory rather than a screen (bug 1).** Let several background chats finish turns without opening them. Their transcripts should no longer stay loaded; this could be checked with the performance suite's memory figures.

## Issue previews

### Issue 1

```markdown
Title: bug(ui-store): streaming in an unopened chat loads and keeps its history

Native sends `streamToken` for every running chat. For a chat whose history was never loaded, the store's token flush created a one-message transcript, so `streamDone` then read the chat's latest 50-message page and kept it for the rest of the session. Nothing evicts it: sidebar warm-up only tracks hover prefetches, the pane eviction only covers chats shown in a pane, and `releaseChatMessages` is never sent for such a chat.

**Reproduction:** Run a turn in a chat that has saved messages without opening it, and let the turn finish.
**Expected:** The chat stays unloaded in the UI and in native.
**Actual:** `messages[chatId]` holds a 50-message page until the app restarts.

**Environment:** UI-V2 store on `uam/v5.2-sync-main` (6f235eb9); all providers that stream tokens.

**Evidence:** Code path: `useAppStore.ts` flush (~723–790) skips the partial-page guard when the page start is 0, and `streamDone` (~1337) reloads whenever `messages[chatId]` is defined. Observed in a Vitest case: a chat with `messageCount` 30 and no loaded messages ended with 30 loaded messages after `streamToken` and `streamDone`.
**Reproduction status:** Reproduced in a synthetic store test only; not checked in the packaged app.

**Acceptance criteria:**

- [ ] Tokens for an unopened chat with saved messages leave `messages[chatId]` undefined and send no `getChatMessages`.
- [ ] Opening that chat mid-turn shows history loading, then the live stream.
- [ ] A brand-new chat with no saved messages still shows its streamed transcript.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
```

### Issue 2

```markdown
Title: bug(ui-store): long chats can show a reply twice when a turn finishes

`streamDone` used `turnAssistantMessageIndex`, a position in the whole transcript, as an index into the loaded page and compared it with the page length. In a chat with more than 50 messages the loaded page starts later than message 0, so the duplicate stream placeholder was not merged and the reply showed twice until the history reload finished.

**Reproduction:** In a chat longer than 50 messages, finish a turn where a stream placeholder follows the native assistant message.
**Expected:** One assistant message with the merged text.
**Actual:** Two copies until the reload replaces the page.

**Environment:** UI-V2 store on `uam/v5.2-sync-main` (6f235eb9).

**Evidence:** `useAppStore.ts` ~1304–1310 does not subtract the page start, while the token flush (~740–744) and the history reader do. Observed in a Vitest case with page start 60: 51 rows remained instead of 50.
**Reproduction status:** Reproduced in a synthetic store test only.

**Acceptance criteria:**

- [ ] With page start 60, `streamDone` merges the placeholder into the native message and leaves 50 rows.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
```

### Issue 3

```markdown
Title: bug(ui-store): history page state outlives chats native removes

`applyStatePatch` removes a chat's messages, bindings and goals for `removedChatIds`, but not `chatHistoryPageBySessionId`, `chatHistoryReadBySessionId`, `chatHistoryErrorBySessionId` or `chatHistoryErrorWasRefreshBySessionId`. The full-state projection does not prune them either, so each removed chat leaves these entries for the rest of the session.

**Reproduction:** Load or fail a history read for a chat, then have native remove it through a patch or a full state.
**Expected:** The four maps no longer contain that chat.
**Actual:** The entries remain.

**Environment:** UI-V2 store on `uam/v5.2-sync-main` (6f235eb9).

**Evidence:** `useAppStore.ts` ~459/480/507 cleanup lists; observed in a Vitest case where all four maps kept the removed id.
**Reproduction status:** Reproduced in a synthetic store test only.

**Acceptance criteria:**

- [ ] A patch removing a chat drops its entries from all four maps.
- [ ] A full state keeps only entries for the chats it lists.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
```

### Issue 4

```markdown
Title: bug(ui-store): a state push rejected before startup is never pulled again

A rejected state push asks for a full-state resync, but `requestStateResync` returns early until the startup state has loaded, and nothing records the miss. If the rejected patch is newer than the startup snapshot, its changes stay missing until some later gap triggers a pull. The rejection log also printed the payload as `[object Object]`.

**Reproduction:** A large `statePatch` overtakes the startup `getInitialState` reply and is rejected by the sanitiser.
**Expected:** After startup loads, the store pulls full state again.
**Actual:** No pull; the log shows no useful detail.

**Environment:** UI-V2 store on `uam/v5.2-sync-main` (6f235eb9).

**Evidence:** User-reported log: `statePatch is large: bytes=575086, changedChats=126`, then `[frontend] [uamPush] Rejected payload: statePatch.data does not match CppStatePatch shape. [object Object]`, before `Initial state loaded`. The cause of that rejection (for example a model catalogue carried by the startup reply) is a hypothesis; the log did not say. Code: `useAppStore.ts` ~1070 and ~1196–1197.
**Reproduction status:** Reproduced in a synthetic store test (no second `getInitialState`); the app log is user-reported.

**Acceptance criteria:**

- [ ] A state push rejected before the startup reply causes a second `getInitialState` once startup loads, unless the snapshot already covers its revision.
- [ ] The rejection log names the revision, the reason and any missing model catalogue id, without the payload.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
```

## PR preview

```markdown
Title: fix(ui-store): stop stream and state-push leaks in the store

Base: uam/v5.2-sync-main · Head: v5/fix-ui-store-stream-leaks (70894231)

Fixes four UI-V2 store defects, one commit each, plus two review follow-ups:

- Streamed tokens for an unopened chat with saved messages are dropped, both when they arrive and when the buffer flushes, instead of starting a transcript. So `streamDone` no longer loads and keeps that chat's latest page for the session. Opening such a chat mid-turn loads history, then the live stream continues; a token sent around the page read can be missing or repeated until the turn reloads. New chats with no saved messages still stream into a fresh transcript.
- `streamDone` converts the native assistant index to the loaded page's local index, so long chats no longer show the reply twice.
- Saved history page, read and error entries, and page starts on full state, are removed for chats native removes. A map whose entries do not change keeps its identity.
- A state push rejected before startup hydration is re-pulled after hydration unless the startup snapshot's revision covers it. The rejection log is one line with revision, reason and missing catalogue id instead of the payload.

Docs: `docs/reference/architecture/react-cef-transport.md` records the stream rule and the pre-hydration rule.

Closes <issue 1>, closes <issue 2>, closes <issue 3>, closes <issue 4>

**Proof:** On 5adb3c27, all passed: `npm ci`; `npx vitest run` (239 files, 4,221 tests); `npm run test:performance` (7 files, 33 tests); `npx tsc --noEmit -p .` and `npx tsc --noEmit -p tsconfig.app.json`; `npm run build`; `python3 scripts/check_docs.py`. On 70894231, a test-only change, the two edited store test files passed 379 tests. Each fix's regression case failed on the code before it and passes now; the bug 4 case for a revision the snapshot already covers passes both ways by design. Not run: native code, the packaged app, the performance suite script, and any before/after memory or timing measurement. No visual captures were taken; see "Screens needing Before/After capture".

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 (fresh-context review of the full branch): suggested 4; implemented 4.
- Agent 2: claude-opus-5-5 (fresh-context re-check of the round-1 fixes): suggested 2; implemented 2.

David's approval of this change is still pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.
```
