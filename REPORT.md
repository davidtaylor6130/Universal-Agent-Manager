# Memory worker retry storm: final report

**Branch:** `v5/fix-memory-worker-retry-storm` (base `uam/v5.2-sync-main`)
**Final SHA:** `721b47b1c34f7530ef5a086cde771b6073371fa6`

Commits:
- `a333cfd5` fix(memory): pause retries after repeated identical worker failures
- `666d8044` fix(memory): address review of the worker retry pause
- `721b47b1` fix(memory): keep backoff on message resume and widen transient failures

Files changed: `src/common/memory/memory_worker_failure_pause.h` (new), `src/common/state/app_state.h`, `src/app/memory_service.cpp`, `tests/memory_tests.cpp`, `tests/interaction_performance_tests.cpp`, `docs/memory/scanning.md`, `docs/reference/architecture/durable-memory.md`, `docs/reference/architecture/react-cef-transport.md`, `docs/documentation-review.md`.

## Review rounds

Both reviewers were fresh-context Opus sub-agents (claude-opus-5-5). Neither was the implementing session.

**Round 1** (independent review of `a333cfd5`): 11 suggested (0 blocker, 5 should-fix, 6 nit), 11 implemented in `666d8044`.
- Should-fix 1: transient categories (rate-limit, network, timeout) no longer pause.
- Should-fix 2: a resume caused by new messages keeps the streak and failure count.
- Should-fix 3: the fingerprint uses the SSH-aware `CliProviderVersionStateKey`.
- Should-fix 4: a manual scan that reaches a paused chat is parked and requeued, not dropped.
- Should-fix 5: the per-poll check lives in the existing idle sweep, and the chat table is built only by the 30 s recheck. The performance case now includes 500 paused chats.
- Nits: test renamed and cases added; stdout no longer classified; deleted-chat streaks dropped; log reads `category=<token> code=<n>`; docs corrected; the status gives the paused-chat count. The content-identity nit was handled as a docs change: the agreement now promises resume on new messages rather than "same messages".

**Round 2** (fresh-context re-check of `666d8044`): 8 suggested (0 blocker, 2 should-fix, 6 nit), 6 implemented in `721b47b1`.
- Implemented:
  - A message resume keeps the pending retry time.
  - Network, server-error and overload wording is classified before auth, and bare "quota" is no longer a rate limit.
  - Docs say transient failures in between are not counted.
  - A parked manual scan is requeued at the front.
  - The folder's directory is in the fingerprint.
  - The stale status count is recorded as a known limit.
- Declined: content-change detection without a new message (edit or regenerate). It needs a per-chat digest. It is recorded as a known limit in `docs/documentation-review.md`, and the agreement promises resume on new messages.
- Declined: extra SSH-host, start-failed and history-read cases, and a timed 30 s recheck. These need an SSH host fixture and a runnable native build; they are left for the macOS run.

**Totals:** 1 independent reviewer, 2 iterations; 19 suggested, 17 implemented.

## Proof (what actually ran)

The native build refuses Linux, so none of the native tests have run. What did run:
- `clang++ -std=c++20 -fsyntax-only` on `src/app/memory_service.cpp` (with a one-line stand-in for `include/internal/cef_ptr.h` and all build flags set to 1): no errors.
- The same syntax-only check on the new test block extracted from `tests/memory_tests.cpp`, with minimal stand-in harness macros: no errors.
- The round-2 reviewer also syntax-checked `tests/interaction_performance_tests.cpp`. The only errors it found were in CEF-dependent code the stand-ins cannot satisfy, none near the changed lines.
- A scratch driver that compiles and runs the pause, streak, transient and classifier logic in `memory_worker_failure_pause.h`: all assertions passed.
- `python3 scripts/check_docs.py`: passed (203 pages, 6 providers x 32 capabilities).

Not run:
- `uam_core_tests`: the cases `MemoryWorkerFailureClassifierReturnsFixedCategoriesWithoutWorkerText`, `MemoryServiceRepeatedIdenticalWorkerFailuresPauseRetriesUntilChatGainsMessages`, `MemoryServiceDifferentOrTransientWorkerFailuresDoNotPauseRetries`, `MemoryServicePausedRetriesResumeOnInputChangeManualScanOrDeletion` and `PerformanceMemoryPollOnLargeInstall`.
- The full native and CTest gates.
- Any MSVC or Apple clang build.
- Live behaviour with real failing workers.

## Screens needing Before/After capture

None. The change is native only. The pause appears as text in the existing Settings → Memory status line. If David wants a capture, it would show that line after three identical failures: "Memory paused for N chats after the same failure repeated; …".

## Issue preview

Title: `bug(memory): failing memory workers retry forever and flood the log`

> Memory extraction for a chat whose worker fails the same way every time is retried forever. The backoff doubles up to a one-hour cap and never stops, which wastes processes and load on the user's machine. Reported from a real install's logs.
>
> **Reproduction:** Exact steps not recorded. Any chat whose memory worker exits non-zero on every run, for example a CLI version refused for text workers.
> **Expected:** After a few identical failures, retries pause until something relevant changes, with one plain notice and a diagnosable log reason.
> **Actual:** "Memory worker exited with code 1" was logged 1,239 times across 81 chats (OpenCode, Codex and Gemini; local and SSH), steadily at about 40 an hour. One chat failed at about 09:54, 09:58, 10:11, 10:33, 11:05 and 11:47. The log leaves out the worker's output, so the cause isn't visible.
>
> **Environment:** `uam/v5.2-sync-main`; OS not recorded.
>
> **Evidence:** Log counts are user-reported. In source, the per-chat retry starts at 300 s and is capped at 3600 s (`src/app/memory_service.cpp` ~107-110, ~1147-1163, ~1217-1221), and nothing gives up. Hypothesis: part of this is OpenCode 2.3.x being refused for text workers (#1028).
> **Reproduction status:** Not reproduced at runtime. Confirmed by reading the code.
>
> **Acceptance criteria:**
> - [ ] Repeated identical failures for the same chat and inputs pause retries.
> - [ ] New messages, a provider/CLI/settings change, a restart or a manual scan resume them.
> - [ ] The pause is surfaced once as a one-line plain message.
> - [ ] The log gives an exit code and a category, never user content.
>
> **Review:** Self-triage only; no independent review of the report.
>
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

Title: `fix(memory): pause retries after repeated identical worker failures`

> Memory workers that failed the same way were retried forever, with the backoff capped at one hour. A real install logged 1,239 such failures across 81 chats.
>
> After three identical deterministic failures, a chat's memory retries now pause. "Identical" means the same category and exit code with unchanged inputs: memory policy, host, workspace and folder, worker provider and model, command and flags, and that host's CLI version. Failures that clear on their own never pause and keep the ordinary backoff: rate or usage limits, network, server or overload errors, and timeouts.
>
> New messages resume a paused chat, but it still waits out its pending retry time, and the same failure on the new messages pauses it again at once. A provider, model, CLI, folder or memory-setting change resumes it within 30 seconds. A restart, a successful run or Scan chats also clear it. A manual scan that reaches a paused chat is kept and requeued at the front when the pause ends.
>
> The pause is announced once in the memory status, with the number of paused chats, and once in the diagnostic log. Each failure's log line now ends `category=<token> code=<exit>`. The category comes from error and stderr text, never from model output or chat text. Paused-chat checks ride the existing idle sweep, and fingerprints are rebuilt at most every 30 seconds.
>
> Recorded as open in documentation review: a bell notice for the pause, which needs a native notice route; the status count is not refreshed when chats resume; and an edited or regenerated reply does not resume a paused chat.
>
> **Proof:** On Linux, because CMake refuses Linux:
> - Syntax-only clang compiles of `memory_service.cpp` and the new memory tests passed, using stand-in CEF and harness headers.
> - A scratch driver of the pause, transient and classifier logic passed.
> - `python3 scripts/check_docs.py` passed.
>
> Not run: `uam_core_tests` (the four new memory cases and the updated `PerformanceMemoryPollOnLargeInstall`), the full native and CTest gates, MSVC and Apple clang builds, and live failing-worker behaviour. No before/after performance figures yet.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 19 suggested, 17 implemented. The 2 declined were content-change detection without a new message, recorded as a known limit, and extra SSH, start-failure and timed-recheck cases that need a runnable macOS build.
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
>
> Related to #1028
