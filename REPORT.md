# Report: v5/fix-remote-upload-abort-reconnect

**Final SHA:** `a180f860a8b9f90e202867973623107725b99715` on `v5/fix-remote-upload-abort-reconnect`, pushed. It is three commits on `origin/uam/v5.2-sync-main` @ `c252f9a9`. No PR, issue, comment or label was created.

The scout finding is confirmed by reading the code at `c252f9a9`, and fixed as proposed:
- **Abort after a drop:** `UploadFileFromPath` reconnects before `file.abort`. The wait is bounded by the 5-second cleanup budget, not the user's cancel.
- **Removal after a drop:** `RemoveFile` reconnects when it is already disconnected, and once more after a transport failure.
- **Helper backstop:** the helper expires uploads that have had no write for 10 minutes and removes their `.tmp` file. This runs from the transient-process sweep.

Beyond the proposal:
- A commit whose reply was lost removes its target once reconnected. The helper refuses an existing target at begin, so that file can only be this upload's copy.
- The stage cleanup in `acp_handlers.cpp` stops at the first copy whose host can't be reached. It logs a fixed-reason line and leaves the remaining copies for the chat-delete sweep.

Overlap check: I fetched both other branches and touched none of their hunks. `v5/fix-companion-upload-restart-warning` edits UI/companion files. `v5/fix-ssh-acp-refusal-and-duplicates` edits `docs/remote-environments/ssh.md`, the execution-hosts reference page and `runner_proxy.cpp`, plus a test near line 4348 of `tests/remote_runner_tests.cpp`. My test sits at about line 3624, and my docs change is in `docs/providers/attachments.md`.

## Review rounds

Both rounds used a fresh-context Opus sub-agent.

| Round | Suggested | Implemented |
|---|---|---|
| 1 | 7 (0 blocker, 3 should-fix, 4 nit) | 4 fully, 3 partly |
| 2 (re-check) | 6 (0 blocker, 1 should-fix, 5 nit) | 3 |

Round 1, the three partial items:
- **Test record in the docs:** finished in round 2.
- **Diagnostics:** I added the desktop-side line only. `RunnerState` has no logging facility, so the helper got no log line.
- **Test includes and lost-commit test:** I moved the includes. I did not add a test for the lost-commit-reply branch because it needs a bridge hook.

Round 2, the three I declined:
- **Abort waiting under `m_stateMutex` behind a stalled write:** the blocking wait on the upload mutex while holding the state lock already existed. This fix only makes it reachable after a timeout, and changing the helper's file-request locking is outside this fix.
- **More tests:** a budget, cancel or lost-commit case needs a wall-clock bound and a `ci_serial` entry, or a new bridge hook.
- **Making `UploadFile` (files of 25 MB or less) match:** this is an optional follow-up. The docs scope the reconnect to streamed files, and the helper expiry covers smaller copies.

## Issue preview (not created)

**Title:** `bug(remote): dropped SSH connection mid-upload leaves the partial attachment on the host`

**Body:**

A streamed SSH attachment upload whose connection drops leaves its partial temporary file and upload record on the host. Copies of the same batch that were already committed also stay. A local read-only scout reported this; it was confirmed by reading the code at `uam/v5.2-sync-main` @ `c252f9a9d`.

**Reproduction:** Use an SSH workspace whose helper runs as the resident service.
1. Attach a file over 25 MB, which is streamed from disk.
2. Drop the connection part-way: turn Wi-Fi off, sleep the laptop or kill `ssh`.
3. Reconnect and attach the same file again.

**Expected:** Nothing from the failed copy stays on the host, and the retry is not refused for space.
**Actual:** `<workspace>/.UAM/attachments/<chat>/.uam-upload-<id>.tmp` stays on the host until the helper stops. With little free space, the retry is refused with "The host does not have enough free space for this file." With two files, the first, already committed copy also stays.

**Environment:** SSH execution host with the resident helper service (Linux or Windows helper), streamed attachments (over 25 MB).

**Evidence:**
- `src/remote/runner_client.cpp`: in `UploadFileFromPath`, `abort` sent `file.abort` only while connected. `Request` disconnects on any transport failure or request deadline.
- `RemoveFile` never reconnected, so the batch cleanup in `src/cef/acp_handlers.cpp` failed at once.
- `src/remote/runner_state.cpp`: the helper removes the temporary file only on abort, commit or destruction. The service's `RunnerState` lives as long as the service.

**Reproduction status:** No live host was tried. A stand-alone harness on a Linux helper build killed the bridge mid-upload against the real resident service. The partial file stayed before the fix.

**Acceptance criteria:**
- [ ] A streamed upload cut off by a dropped connection leaves no partial file or upload record when the host can be reached again.
- [ ] Copies the batch already committed are removed when the host can be reached again.
- [ ] The helper discards an upload left without writes for a bounded time, covering a desktop that ended mid-copy.

**Review:** Report triaged by code reading; no independent review of the report itself.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code), on behalf of David Taylor.

Related to #894 (streamed uploads).

## PR preview (not created)

**Title:** `fix(remote): clean up an SSH upload cut off by a dropped connection`

**Body:**

A streamed SSH attachment upload whose connection dropped left its partial temporary file and upload record on the host. The one resident helper outlives every SSH session, so the file stayed until the helper stopped, and the space it held could refuse a retry. The batch also could not remove copies it had already committed.

What changes:
- **Abort after a drop:** UAM now reaches the helper again, within five seconds and also after a cancel, to abort the upload.
- **Lost commit reply:** a copy whose commit reply was lost is removed once UAM reaches the helper again.
- **Batch cleanup:** `RemoveFile` reconnects, so the batch can remove its committed copies. It stops at a host it cannot reach and leaves the rest to the chat-delete sweep.
- **Helper backstop:** the helper discards any upload with no write for ten minutes, partial file included.

Docs: `docs/providers/attachments.md` gains the agreement. Provider capability rows are unchanged, because this is shared SSH staging and no row status changes.

**Proof:**
- `cmake --build Builds/runner --target uam_runner` builds on Linux. The SQLite download was blocked by the network proxy, so it was configured with `-DFETCHCONTENT_SOURCE_DIR_UAM_SQLITE` pointing at the same-series amalgamation (3.54.1) from the npm registry.
- `python3 scripts/check_docs.py` passes.
- New native tests:
  - `RemoteStreamedUploadCutOffByADroppedConnectionLeavesNothingOnTheHost` kills the bridge mid-upload against a private helper service. It is macOS only.
  - `RunnerStateExpiresAQuietUploadWithItsTemporaryFile` covers the expiry.
- Neither test has been compiled or run in `uam_core_tests`, because the native build refuses Linux.
- The same test bodies ran in a stand-alone harness against the runner objects and a real `uam-runner serve`/`bridge` on Linux.
  - **Before the fix:** the drop case failed on the leaked `.uam-upload-*.tmp`.
  - **After the fix:** both pass, 5 of 5 runs.
- `src/cef/acp_handlers.cpp` was not compiled.
- Not covered by tests: the lost-commit-reply removal, the cleanup-budget timing, and the stage loop's stop at an unreachable host.
- Not exercised against a live host or a real network loss.

**Review:** Reviewed by 2 agents over 2 iterations.

- Agent 1: claude-opus-5-5 — suggested 7; implemented 4 in full, 3 in part. No helper log line, because the helper has no logging facility. No lost-commit test, because it needs a bridge hook.
- Agent 2: claude-opus-5-5 — suggested 6; implemented 3. Declined as out of scope:
  - the existing helper lock wait behind a stalled write;
  - extra timing and lost-commit tests;
  - aligning the small-file `UploadFile`.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code), on behalf of David Taylor.

**Screens needing Before/After capture:** none. Nothing in the UI changes; the existing upload-failure notification keeps its text.
