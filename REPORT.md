# Report: v5/fix-subagent-transcript-async

**Final SHA:** `9daaa65cdd5905001a21b8f1a101ea29136c8514` on `v5/fix-subagent-transcript-async`. Commits: `cb876a76` (fix), `3b8b751d` (review round 1), `9daaa65c` (fixes from the round 2 re-check). Merges cleanly with `origin/uam/v5.2-performance` (`64671114`) and with `v5/fix-oversize-chat-read` (`7e2130a3`). No PR, issue or comment was created.

## What changed
- `HandleGetManagedAgentTranscript` used to load, hydrate and serialize the saved file on the CEF UI thread when a transcript was missing from `app.chats` (for example an unfinished run). That work now runs on a worker through `BeginNativeHistoryRead` and `DispatchManagedSubagentQuery` (`RunAsyncCefQuery` in production).
- **Checks on the UI thread before replying:** the data folder, parent, run id, run status, `updated_at` and the managed-continuation `state_revision` must all be unchanged, and no live copy may have appeared during the read. If any check fails, the reply is 409 "changed while it was loading".
- **A second open of the same cold transcript during a read** waits for that read to finish, then reads again. `AwaitNativeHistoryRead` gained `replace_older`, which defaults to true so existing callers are unchanged. With it false, the older request keeps its own checked result. A cancelled request gets no history.
- **Unchanged cases:**
  - A transcript already loaded in memory is still answered directly.
  - A transcript held in memory without its messages still uses the existing captured-read route and its "already loading" refusal.
- **Docs:** `docs/chat/subagents.md` has the agreement paragraph and an `Implementation: Needs verification` note.

## Review rounds
**Round 1** (fresh-context `claude-opus-5-5`, on `cb876a76`): 6 suggested (0 blocker, 2 should-fix, 4 nit), 5 implemented.
- Implemented:
  - Docs: moved the verification status out of the feature prose.
  - A newer open now waits instead of getting 409.
  - Reply refused when the run status or `updated_at` changed during the read.
  - Commented the defensive hydrate and removed a redundant check.
  - Added tests for cancellation and a refused dispatch.
- Declined: the cold read itself has no size limit and runs on the shared file thread. Size caps belong to `v5/fix-oversize-chat-read`; noted in the PR.

**Round 2** (one fresh-context `claude-opus-5-5` re-check, on `3b8b751d`): 5 suggested (0 blocker, 1 should-fix, 4 nit), 4 implemented.
- Implemented:
  - A waiting open no longer withdraws the older request's delivery (`replace_older=false`).
  - Docs narrowed to transcripts not held in memory.
  - `state_revision` added to the checks.
  - Tests for a cancelled waiter and an `updated_at`-only change.
- Declined: prefixing the request key. The bare chat id is deliberate, so other history reads and the guard at `src/cef/chat_handlers.cpp:1217` see this read. I added a comment saying so instead.

The round 2 fixes (`9daaa65c`) had no further independent review, because the brief allowed only one re-check.

**Totals:** 1 independent reviewer model, 2 iterations: 11 suggested, 9 implemented.

## Issue preview
**Title:** `bug(agents): opening a managed sub-agent transcript can freeze the app`

Opening a managed sub-agent conversation whose transcript is not in memory can read and parse the whole saved transcript on the CEF UI thread. The app stays unresponsive until the read finishes. Source: code inspection requested by David Taylor.

**Reproduction:** Open a managed child whose transcript chat is not in `app.chats`, and whose run is unfinished or whose result has not been delivered to the parent yet. Steps with real data are not recorded.
**Expected:** The app stays responsive. The transcript loads in the background and is checked against the same run before it is shown.
**Actual (from code):** `HandleGetManagedAgentTranscript`, called from `OnQuery` under `CEF_REQUIRE_UI_THREAD`, runs `ChatRepository::LoadLocalChat`, then `HydrateChatMessages` and `SerializeSession`, all on the UI thread.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; all platforms.

**Evidence:** I read the code path in `src/cef/chat_config_handlers.cpp` and `src/app/transcript_retention_service.cpp`. `CaptureManagedAgentTranscriptRead` returns nothing for unfinished or undelivered runs, and for transcripts missing from `app.chats`. A transcript in memory without its messages already gets 409 and does not block. The freeze is inferred from the code; nobody has observed it.
**Reproduction status:** Not attempted at runtime.

**Acceptance criteria:**
- [ ] Requesting an unfinished run's transcript performs no synchronous `LoadLocalChat` or hydrate.
- [ ] The reply is checked on the UI thread and never carries another run's transcript.
- [ ] A cancelled or replaced request receives no history.

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code), on behalf of David Taylor.

## PR preview
**Title:** `fix(agents): read a cold managed transcript off the UI thread`

When a managed transcript is not in memory and the captured-read route declines it (for example an unfinished run), `getManagedAgentTranscript` used to load, hydrate and serialize the saved file on the CEF UI thread. That work now runs on a worker through the existing native-history slot and `RunAsyncCefQuery`. Before replying, the completion checks on the UI thread that the data folder, parent, run (status, `updated_at`, continuation revision) and transcript ownership are unchanged. If anything changed, it replies 409. A newer open of the same transcript waits for the pending read and then reads again. The older request keeps its checked result, and a cancelled request gets no history. A transcript already in memory is answered as before.

The cold read itself still has no size limit. That depends on `v5/fix-oversize-chat-read`, which merges without conflicts. Related: the issue above (not filed).

**Proof:**
- `g++ 13 -std=c++20 -fsyntax-only`, against hand-written CEF header stubs (the CEF download is blocked and the native build refuses Linux): `src/cef/chat_config_handlers.cpp` and `tests/managed_subagent_handler_tests.cpp` pass. `src/cef/uam_query_handler.cpp` shows the same 26 stub-only errors before and after the change.
- `python3 scripts/check_docs.py`: passes.
- Three handler cases in `tests/managed_subagent_handler_tests.cpp` were written but not built or run:
  - `ManagedSubagentTranscriptOfUnfinishedRunReadsSavedFileOffTheUiThread`
  - `ManagedSubagentTranscriptNewerOpenWaitsAndCancelledOpenGetsNoHistory`
  - `ManagedSubagentTranscriptDetachedReadNeverRepliesWithAnotherRunsTranscript`

  That test program is opt-in and outside CTest and CI.
- Not shown failing before the fix. No timing was measured.
- Still needed: a macOS or Windows build and a run of `uam_managed_subagent_handler_tests`.

**Review:** 1 independent reviewer, 2 iterations. Independent `claude-opus-5-5`: 11 suggested, 9 implemented (declined: size cap, which belongs to `v5/fix-oversize-chat-read`; key prefix, kept the shared chat-id key on purpose). Round 2 fixes were not re-reviewed. Required Claude review per AGENTS.md and David's acceptance are pending.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code), on behalf of David Taylor.

## Screens needing Before/After capture
No static UI changed, so no screenshots are needed. Useful evidence would be a short Before/After recording of opening a managed child conversation with a large transcript that is not in memory, on an unfinished run. Before: the window freezes. After: it stays responsive while history loads. This has not been captured.
