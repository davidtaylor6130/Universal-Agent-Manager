# Report: v5/fix-diagnostics-retention-save-reasons

**Final SHA (work branch):** `ac47eb12`
**Commits:** `45a0fd92` (Bug A), `f9a3cc39` (Bug B), `41bb4842` (round 1 review fixes), `ac47eb12` (round 2 re-check fixes). Based on `uam/v5.2-sync-main` 6f235eb9; merges into `origin/uam/v5.2-performance` without conflict (checked with `git merge-tree`), so no merge commit was made.

## What changed
- **Bug A, log retention:**
  - Storage and deletion audit lines (`Chat storage delete`) go to a new `uam-audit.log` through `WriteAudit`, and are also copied to `uam.log`. The audit log is kept 90 days within 1 MiB, with an opened and a closed marker per app run. If the file can't be rewritten, it keeps appending and retries hourly.
  - PollTick per-tick lines are replaced by one p50/p95/max-per-stage summary per minute that had a tick of 50 ms or more. A single line is written only from 250 ms. A clean quit flushes the partial minute.
  - Model-discovery sessions keep routine acp-diag success traffic in memory only.
- **Bug B, save failure reasons:**
  - `SaveChat`, `SaveLoadedChatWithPrimaryWitness`, `PrepareChatSave` and `PrepareLoadedChatBindingSave` take an optional `ChatSaveFailure` that receives the cause. A write error keeps the atomic writer's step and OS error text, never a path.
  - Each failure writes one line with the chat ID and reason, classified as "refused" (held or unreadable journal, unstorable content, ownership checks) or "failed". The forced saves at quit add at most one more line per chat.
  - The status-line rule is unchanged.

## Log budget
| | Before | After |
|---|---|---|
| Disk cap | `uam.log` 5 MiB + `.1` ≈ 10 MiB | Same, plus `uam-audit.log` 1 MiB ≈ 11 MiB |
| Deletion audit lines | Kept about 2.5 days | Kept 90 days |
| PollTick | One line per slow tick (≥ 50 ms), at most every 5 s (26,893 lines retained) | At most 1 summary a minute, only in minutes with a slow tick; single lines only from 250 ms |
| Discovery acp-diag | Every protocol event (8,513 lines) | Failures only |

How much longer `uam.log` lasts is an estimate, not a measurement.

## Review rounds
**Round 1 (independent claude-opus-5-5, fresh context): 14 suggested (0 blocker, 4 should-fix, 10 nit), 11 implemented.**

Implemented:
- All 4 should-fixes:
  - one barrier line at quit
  - the real cause for background-prepared saves
  - perf-suite PollTick metrics, a 60 s window and a shutdown flush
  - audit compaction failure keeps appending
- 7 nits:
  - audit drop counter
  - unsafe chat IDs hidden
  - UTF-8-safe trimming
  - percentile ceiling
  - RAII log stop in tests
  - serial CI for the RLIMIT test
  - `<utility>` include
- Barrier message wording, and the logging.md wording.

Declined or not changed:
- **Status line chosen from the cause:** declined. It changes user-facing wording and is out of scope; recorded as a follow-up.
- **Merge-risk note on chat_repository.cpp:** informational, no code change. Coordinate with the file's owner.

**Round 2 (one fresh-context claude-opus-5-5 re-check): 8 suggested (0 blocker, 1 should-fix, 7 nit), 6 implemented.**

Implemented:
- The should-fix: back off audit compaction after a failure.
- Classify an unreadable primary at preparation as a failure.
- Flush on the companion `_Exit` path.
- Hide unsafe IDs in the recovered-save line.
- Audit drop-line wording.
- logging.md clauses.
- A compaction-failure test, which covers part of the test-coverage nit.

Declined:
- **Cause for the background publication stage** (`background_publication_not_confirmed`): needs a second cause record on the publisher thread, which goes beyond the minimal chat_repository changes allowed. Left as a follow-up.
- **Runtime-level streaming-save log test:** can't be built or run without CEF here. A repository-level test covers the cause plumbing instead.

## Issue previews
**`bug(diagnostics): uam.log keeps ~2.5 days, so deletion audit lines are lost`**
> uam.log is capped at 5 MiB with one `.1` backup. On the reported install the 10.1 MB retained covered about 2.5 days, so the "Chat storage delete" lines that source-of-truth.md relies on are rotated away.
> **Expected:** deletion audit lines kept long enough to investigate a missing chat, with total log disk bounded.
> **Actual:** `[performance]` lines are 44% (26,893 PollTick lines) and `[acp-diag]` 47% (8,513 from model-discovery chats).
> **Environment:** V5.2 branch.
> **Evidence:** David's log counts. The cap and rotation are confirmed in `diagnostic_log.h`/`.cpp`.
> **Reproduction status:** not reproduced on a live install.
> **Acceptance criteria:**
> - [ ] Deletion lines kept at least 30 days in a bounded file
> - [ ] PollTick summarised
> - [ ] Discovery success traffic not logged at full detail
> - [ ] Docs updated
>
> **Review:** self-triage only.
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

**`bug(chat-save): failed chat saves log no reason and mislabel refusals as failures`**
> 288 "Chat save failed" and 288 "not saved at the durable barrier" lines appeared for the same 32 chats, once per run, with no reason. `SaveChat` returned a bool, dropping `AtomicWriteResult.error`, and only `ChatContentRefusesSave` counted as a refusal.
> **Expected:** one line per failure with a redacted cause, and deliberate refusals labelled "refused".
> **Environment:** V5.2 branch.
> **Evidence:** David's log counts. The log sites are confirmed in `acp_session_runtime.cpp`.
> **Reproduction status:** not reproduced live. The cause for the 32 chats is unknown.
> **Acceptance criteria:**
> - [ ] One line with the reason and no path
> - [ ] Held journals labelled "refused"
>
> **Review:** self-triage only.
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## PR preview
**Title:** `fix(diagnostics): keep deletion audit lines and name chat-save failure causes`
**Head → base:** `v5/fix-diagnostics-retention-save-reasons` → `uam/v5.2-performance`

> Deletion and storage audit lines now also go to `uam-audit.log`, kept 90 days within 1 MiB with markers when each run opens and closes the log. If the file can't be rewritten, lines keep being appended and the rewrite is retried hourly.
>
> PollTick slow ticks become one p50/p95/max-per-stage summary per minute that had a slow tick. Single lines appear only from 250 ms, and a clean quit flushes the last partial minute. `perf_suite.py` reads the summaries.
>
> Model-discovery sessions keep routine acp-diag traffic in memory.
>
> Failed chat saves, including streaming saves, log one line naming the chat and its cause. Deliberate refusals such as a held or unreadable queue journal are labelled "refused". The quit barriers add at most one line per chat.
>
> Docs updated: `logging.md`, `source-of-truth.md`, `react-cef-transport.md`, `performance-suite.md`, `documentation-review.md`.
>
> Related to the two issues above.
>
> **Proof:**
> - `python3 scripts/check_docs.py` and `python3 tests/perf_suite_test.py` pass.
> - The `uam_runner` build passes, though it doesn't compile the changed files.
> - The changed `chat_repository.cpp`, `diagnostic_log.cpp`, `acp_diagnostics.cpp` and `acp_session_lifecycle.cpp` compile with no errors when built against small stand-in CEF headers.
> - `acp_session_runtime.cpp`, `application.cpp` and the C++ test files produce only errors from the stand-in headers. For the two sources the set is identical to the unmodified files, and none falls in the test code.
> - In a scratch Linux harness:
>   - the six cases in `diagnostic_retention_tests.cpp` pass, also under ASan/UBSan and TSan;
>   - the four existing diagnostic-log cases from `core_agent_control_tests.cpp` pass;
>   - `ChatSaveReturnsItsRefusalOrWriteErrorCause` (including a real write failure via `RLIMIT_FSIZE`) and `BackgroundChatSavePreparationReturnsItsCause` pass.
> - **Not run:** `uam_core_tests`, CTest, any macOS or Windows build, and `ChatSaveFailureLogsOneLineWithItsCauseAtDurableBarrier`. The desktop build refuses Linux and CEF headers were unavailable. The successful witness-save path can't run on Linux (no SHA-256 implementation).
> - Retention improvement is an estimate, not a measurement.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 22 suggested, 17 implemented (round 1: 14 suggested, 11 implemented; round 2: 8 suggested, 6 implemented). Declined: status line chosen from the cause (user-facing, out of scope), publication-stage cause (needs wider chat_repository changes), runtime-level log test (needs CEF). The chat_repository merge-risk note is informational. David's review and acceptance are pending.
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## Screens needing Before/After capture
None. The changes are native diagnostics, logging and docs. The status-line text is unchanged.
