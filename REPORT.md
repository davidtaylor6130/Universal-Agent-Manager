# Report: #513, reliable child history for Gemini and Antigravity

**Work branch:** `v5/feat-513-child-history-gemini-antigravity`
**Final SHA:** `e6e819deb4b8ef674281957f5eaffa0bc573b494`
**Base:** `origin/uam/v5.2-sync-main` (`6f235eb9`)

Commits on the work branch:
1. `63cfddc0` feat(antigravity): refresh local chats from the exact native conversation
2. `abede699` fix(antigravity): keep saved history while a refreshed conversation is unfinished
3. `e6e819de` fix(antigravity): treat only pinned in-progress step states as still running

## Outcome

#513 is partly addressed and should stay open.

- **Gemini:** no code change was needed. The parent-owned child browser (#814) landed in V5.1.0. Its remaining gaps need live provider and real SSH-host checks.
- **Antigravity:** switching a controller-local chat from CLI View to Chat now refreshes it from its exact native conversation, through the guarded local media refresh route Gemini uses.
  - A shorter native snapshot keeps the fuller saved history.
  - A conversation Antigravity is still writing keeps its saved history without an error.
  - Any other refusal, such as a nested child with inherited parent context, reports a plain reason and leaves saved history unchanged.
  - SSH chats keep their saved history.
- **Still unmet:** the issue's Antigravity criterion, an upstream export or a continuity-preserving SDK reader. There is also no format evidence for nested child stores. Both Antigravity rows (`agent-history`, `terminal-sync`) therefore stay Partial. Data-storage routing is unchanged.

## Review rounds

| Round | Reviewer | Suggested (about #513) | Implemented | Declined / not applicable |
|---|---|---|---|---|
| 1 | Independent fresh-context Opus review (REVIEW.md on `agent-reports/review-R2`) | 4 | 3 | 1 |
| 2 | Fresh-context Opus sub-agent re-check of the round-1 fixes | 4 | 2 | 2 |

### Round 1

- **F1, should-fix: implemented.**
  - Added core-inventory cases `AntigravityExactLocalTranscript*` and a parser case for the refusal reason.
  - The handler cases now use the Antigravity flag only, with their own callback doubles.
  - The docs say the handler program is opt-in.
  - **Still open:** running the opt-in `uam_managed_subagent_handler_tests` program on macOS, which isn't possible from this Linux sandbox.
- **F2, should-fix: implemented.** An unfinished conversation now serves saved history without an error, and a handler case covers an unfinished store. There is no separate "explicit Reload" path to keep the error for: the frontend's Retry does not request a native refresh.
- **F3, nit: implemented.** An exact read returns the bare reason in plain words, with no provider prefix or raw ID.
- **F4, informational: no code change.** #513 stays open and the rows stay Partial.

### Round 2

- **Should-fix 1: implemented.** Only the pinned agy 1.4.4 in-progress step statuses are treated as still running: 1, 2, 8, 9 and 11. Unknown states, and a call with no result once every step is terminal, are refused with a reason. The nested/unsupported check now runs first.
- **Should-fix 2: implemented.** The stale doc row about refusing an active run is corrected.
- **Nit, `cache_filled` on the unsettled path: declined.** It deliberately matches the existing short-snapshot path and already has a comment.
- **Nit, refusals matched by session ID only: declined.** The read uses that exact session filter, so the match is unambiguous.

## Issue previews

None. No new issues are proposed.

## PR preview

**Title:** feat(antigravity): refresh local chats from the exact native conversation (#513)

**Summary**
- Switching a controller-local Antigravity chat from CLI View to Chat now refreshes it from its exact native conversation, through the guarded local media refresh route Gemini uses (renamed `HandleRefreshLocalNativeMedia`). A shorter native snapshot keeps the fuller saved history.
- A conversation Antigravity is still writing keeps its saved history without an error. That means a step in a pinned in-progress state (pending, running, generating, waiting or queued) or a not-idle index row.
- Any other refusal shows the reader's plain reason and leaves saved history unchanged. Examples are a nested child with inherited parent context, an unknown step state, or a call with no result.
- SSH Antigravity chats keep their saved history, because no remote helper reads this store.
- The synthetic Antigravity store builder is now shared in `tests/antigravity_history_fixture.h`. The focused parser project now defines the build-feature macros it was missing.
- Docs updated: the Antigravity `terminal-sync` and `agent-history` rows, session continuity, agent history and the Antigravity history reference.

**Refs #513** (keep open). Both Antigravity rows stay Partial: there is no upstream export or continuity-preserving reader, and no format evidence for nested child stores.

**Review:** 2 rounds.
- Round 1 (independent Opus): 4 suggested, 3 implemented, 1 informational.
- Round 2 (Opus re-check): 4 suggested, 2 implemented, 2 nits declined.

**Proof** (Linux sandbox only):
- **Focused Antigravity parser project (`uam_antigravity_history_tests`), built and run:** 25 of 28 pass, including the new `AntigravityNativeHistoryRecordsPlainRefusalReasonAndUnsettledSource`.
  - The 3 failures are image cases that fail identically on the base sources.
  - Built with SQLite 3.54.1 from npm instead of the pinned 3.54.0 (sqlite.org is blocked), and with the CEF commit and Chromium header fetched by git (pinned hash matched).
- **Compile-only against the pinned CEF headers:** `runtime_orchestration_services.cpp`, `uam_query_handler.cpp`, `startup_native_import_service.cpp` and the core-mode `antigravity_native_import_tests.cpp` (which contains the new core cases).
- **Syntax-only:** `chat_handlers.cpp`, and `native_media_refresh_handler_tests.cpp` with the Gemini flag both on and off.
- **Scratch probe against the real reader:** the handler fixture's store reads as 2 messages, and the nested variant is refused.
- `python3 scripts/check_docs.py` passes (203 pages, 6 providers × 32 capabilities).
- **Not run:**
  - the full native core inventory (CMake supports only Windows and macOS);
  - the opt-in handler program;
  - frontend tests (no UI change);
  - live CLI and SSH-host checks.

**Acceptance:** David's approval is pending.

## Screens needing Before/After capture

1. **Chat view after switching a local Antigravity chat from CLI View to Chat once a terminal turn has finished.** Before: saved history only. After: the native transcript, including the terminal turn.
2. **The same switch while the terminal turn is still running.** Before and after: saved history with no error notice.
3. **A nested Antigravity child chat after CLI View → Chat.** Before: saved history silently. After: the "Chat history did not refresh" notice with the plain inherited-context reason, and saved history kept.
