# #947 state patch size: report

The fix is on `v5/perf-947-state-patch-size` at **`9af6b583398c58afececd333dc275e5ef1dce67f`**, based on `uam/v5.2-sync-main`. It has three commits: the change (`2006263c`) and two review follow-ups (`8d1e2499`, `9af6b583`). There are no `state_serializer` changes, so it should merge alongside `v5/fix-model-catalogue-desync`.

**The native test and the regression bound were written but never built or run.** The native build refuses Linux and the CEF download is blocked here. Every byte figure below is worked out from the test fixture, not measured.

## What changed
Even after PR #1000, each streamed token resent every live chat's large fields: all tool calls (up to 64 KB of content each), all turn events, the command list, the usage object and the chat's PNG icon data URL.

Native (`cef_push.cpp`) now remembers what it last sent for each chat. A later patch only says what to reuse: either "unchanged", or "keep the first N entries" followed by the new ones. So a token sends just the growing last turn event, and a finished tool call travels alone.

The page puts these fields back before sanitising, so nothing downstream sees a difference. If a patch refers to a copy the page doesn't have, the page leaves out only that chat, applies the rest, and pulls full state.

## Which fields shrank, per token patch (estimated)
The fixture is 2,063 chats. Three are streaming, with 30, 5 and 5 tool calls of 50 KB, PNG icons and 60 commands each.

| Field | Before | After |
|---|---|---|
| Tool calls | ≈2.05 MB | 0 |
| PNG icon data URLs | ≈490 KB | 0 |
| Command lists | ≈36 KB | 0 |
| Turn events | all events | only the last one |
| **Whole token patch** | **≈2.6 MB** | **≤16 KB** (bound the test asserts) |

`StatePushLiveSessionPatchCarriesOnlyWhatThePageLacks` prints the real bytes per field and checks that the rebuilt fields match the chat's full summary.

## Review rounds
Two fresh-context `claude-opus-5-5` sub-agents reviewed the branch: one independent review, then one re-check of the fixes. Across both rounds they made 13 suggestions and I implemented 11.

**Round 1 (review of `2006263c`): 8 suggested, 6 implemented.** That was 1 blocker, 2 should-fix and 5 nits.
- **Implemented:**
  - A chat with a missing copy now drops only that chat, not the whole patch.
  - A pulled full state no longer prunes copies.
  - Native forgets a chat's copy when it can't find the chat.
  - Arrays are compared once instead of twice.
  - Three stale doc sentences are corrected.
  - The test assertion is tighter, and the test reset helper is separate.
- **Declined:**
  - **Blocker, native build and run:** this environment can't build the native code. It needs a macOS or Windows run.
  - **Secret registered after a value was sent:** fixing it needs a new registration-generation API in `secret_redactor`. It's rare and outside this change.
  - **Partly declined, the memory suggestion:** I didn't switch native's stored copies to digests. That would be a larger redesign.

**Round 2 (re-check of `8d1e2499`): 5 suggested, 5 implemented.** That was 1 should-fix and 4 nits.
- A patch that skipped a chat during a pull no longer marks the pull as overflowed, which used to force another pull. A new test covers this; it fails on the old behaviour and passes now.
- Two stale comments are fixed.
- A pushed `stateUpdate` now drops copies for chats it no longer lists.
- A skipped patch the store will ignore no longer starts a pull.
- One garbled doc sentence is fixed.

## What's unverified
- **Native code:** the native test cases (new and updated), the `[desync]` cache checks and the real byte figures were not run. The native code only passed a syntax check against stand-in CEF headers, with no errors in the changed code. The round-1 reviewer also compiled the new logic separately against the pinned nlohmann/json.
- **Your logs:** I can't confirm that tool calls and icons are what produced the 640 KB patches you saw.
- **Memory:** native keeps an extra copy of each live chat's large fields, and of the icon for each patched PNG chat, until the chat is removed or the page pulls full state.
- **Secrets:** see the declined round-1 item above.

## Issue previews
None. No new issues are needed.

## Screens needing Before/After capture
None. This changes transport only; nothing on screen changes.

## PR preview
**Title:** `perf(push): stop resending chat fields the page already holds`

PR #1000 stopped resending model catalogues, but a live chat's `statePatch` still carried every tool call (up to 64 KB of content each), turn event, command list, usage object and PNG icon data URL on each streamed token. Native now remembers what it last sent for each chat. Later patches name the fields to reuse (unchanged, or the first N array entries) and carry only the rest, so a token sends just the growing last turn event. The page restores the fields before sanitising. A chat whose copy the page lacks is left out of that patch, the rest applies, and the store pulls full state. Related to #947.

**Proof:** On Linux:
- `npm ci`, `npx vitest run` (239 files, 4219 tests passed) and `npx tsc --noEmit -p .` all pass.
- The new in-flight resync test fails on the earlier behaviour and passes with the fix.
- `python3 scripts/check_docs.py` passes.
- `cef_push.cpp` and the native test file show no errors in changed code in a syntax-only compile against stand-in CEF headers.

Not run: the native cases `StatePushLiveSessionPatchCarriesOnlyWhatThePageLacks` (byte figures per field, 16 KB bound for a token patch), `StatePushSerializesLiveToolCallsOnlyWhenTheyChange` and the other push tests, because the native build refuses Linux. The fixture estimate is ≈2.6 MB per token patch before and ≤16 KB after. Measured figures need a macOS or Windows run.

**Review:** 1 independent reviewer, 2 iterations.
- Independent claude-opus-5-5 (review): 8 suggested, 6 implemented. Declined: the native build (not possible here) and secret-registration resets (needs a new API, rare).
- Independent claude-opus-5-5 (re-check): 5 suggested, 5 implemented.

The frontend change still needs David's approval.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
