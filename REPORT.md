# Oversized chat read crash: report

**Work branch:** `v5/fix-oversize-chat-read` at `8cf03530`, based on `origin/uam/v5.2-sync-main`.

Commits:
- `7e2130a3`: the fix.
- `e5b064ca`: changes from review round 1.
- `8cf03530`: changes from review round 2.

No PR, issue or comment was created.

## The cause and the fix

Your hypotheses were right. When a caller passed no limit, `ParseLocalChatFile` read the whole file with no cap. That covered every `LoadLocalChat` and `HydrateChatMessages` caller, including the history request in `chat_handlers.cpp` and the other callers you listed. The read went through `ReadTextFile` and `TryReadFile` (an `ostringstream` and then a copy), so it needed about twice the file size in memory.

- **One shared check:** every full chat read now checks the file size first and refuses anything over the 256 MiB save limit. The check sits in the bounded `TryReadTextFile` overload and the new `TryReadFileWithinLimit` helper in `io_utils.h`, so every chat read goes through the same code.
- **What the user and the log see:** the user sees "This chat is too large to open.", and the history request keeps that message even when its page fallback also fails. The log gets one line per file per run with the file name, its size and the limit.
- **The oversized file is protected:** none of these paths replaces it with its older `.bak`:
  - startup recovery;
  - recovery from a `.bak` that has no primary file;
  - a save of the chat while its messages aren't loaded, including the copy path used for unopened chats.
- **Old-format chat folders:** the migration now reads them within the same limit. A folder over the limit is left alone, skipped and named in the load warnings.
- **Test hook:** `ChatRepository::SetMaxChatReadBytesForTests` lowers the limit for a test.
- **Docs:** `docs/architecture/app-state-persistence.md` describes the limit, the message and what happens to the files. `python3 scripts/check_docs.py` passes.

Files changed: `src/common/utils/io_utils.h`, `src/common/chat/chat_repository.{h,cpp}`, `src/cef/chat_handlers.cpp`, `tests/core_chat_repository_tests.cpp`, `docs/architecture/app-state-persistence.md`.

## Tests

- **New test cases** in `tests/core_chat_repository_tests.cpp`:
  - `BoundedFileReadRefusesOversizedFileBeforeReadingIt`
  - `ChatRepositoryRefusesOversizedChatWithoutChangingItsFiles`
  - `ChatRepositoryOversizedBoundedLoadSaveAndLegacyFolderLeaveFilesUnchanged`
- **Scratch Linux driver:** it runs the same steps as the new tests against the real repository code, linked the same way the existing data-folder test build links it.
  - All steps pass with the fix.
  - Each check was confirmed to fail without the change it covers: the original read cap, and the guard that stops a save from adopting the backup.
- **Real 256 MiB limit, no test hook:** opening a sparse 300 MiB chat file used about 10 MB of memory and returned the new message. Before the fix it used about 619 MB.
- **Compile checks:** `chat_repository.cpp` and `io_utils.h` pass g++ 13 and clang++ 18 syntax checks with stubbed CEF headers, with no new warnings.

## What's unverified

- **Not built or run here:** `uam_core_tests`, the desktop build and CTest. The desktop build refuses Linux, so the native tests need the Mac.
- **Runner-only build blocked:** configure failed because the proxy refused the sqlite.org download (403).
- **`chat_handlers.cpp` never compiled:** CEF headers aren't available here. The change there is small: one new argument and one local string.
- **No screen capture:** I haven't seen the history view showing the message.
- **Known behaviour that remains:** saving an unopened oversized chat still copies its unchanged transcript into a new primary file and rotates the `.bak`. That copy doesn't read the file into memory. The docs now say this.

## Independent review

1 independent reviewer (claude-opus-5-5, fresh context), 2 iterations, 14 suggested and 12 implemented in total.

**Round 1** (review of `7e2130a3`): 8 suggested (0 blockers, 3 should-fix, 5 nits), 7 implemented in `e5b064ca`.
- **Implemented:**
  - the size check inside the bounded `TryReadTextFile`;
  - the streaming save no longer adopts the backup over an oversized primary;
  - the capped legacy-folder migration;
  - doc wording;
  - one log line per file;
  - `LoadLocalChatBounded` reports the too-large message;
  - extra tests: the orphan `.bak` here, and the unloaded-save case in round 2.
- **Declined:** nit 7, which noted that a few callers outside the open path (provider change, terminal sync) put their own prefix in front of the message. Those are not the open path and the result is still readable, so I left them.

**Round 2** (re-check of `e5b064ca`): 6 suggested (0 blockers, 3 should-fix, 3 nits), 5 implemented in `8cf03530`.
- **Implemented:**
  - keep the too-large message when the page fallback fails;
  - log once and warn for a skipped legacy folder;
  - three regression tests;
  - de-duplicate log lines by full path;
  - doc wording.
- **Declined:** nit 5, which noted `TryReadFileWithinLimit` resolves the path and reads the file size twice per read. The result is correct and the cost is one extra metadata call, so I didn't refactor it.

## Screens needing a Before/After capture

- **Chat history view:** opening a chat whose saved file is over the limit. Before the fix the app crashed. After the fix the history view should show the error "This chat is too large to open." I haven't captured this, and I haven't confirmed how the view presents a failed history request (status 500 with that text).
- **Load warnings or status line:** after a scan that skips an oversized chat, its `.bak`, or an old-format folder. This is a secondary check.

---

## Issue preview

**Title:** `bug(chat-storage): opening an oversized saved chat aborts UAM with out-of-memory`

Opening a saved chat whose file is larger than the save limit makes UAM read the whole file into memory, and the app aborts. This was reported from a real crash report on macOS.

**Reproduction:** Have a saved chat whose `chats/<chat-id>.json` is far above 256 MiB (the report had about 2.04 GB, plus a `.bak` of the same size), then open that chat's history.
**Expected:** UAM refuses the file with a short message and stays running. The file stays on disk.
**Actual:** UAM aborts with `EXC_BREAKPOINT` (out of memory) after trying to allocate about 3 GiB.

**Environment:** macOS desktop build, `uam/v5.2-sync-main`. The exact app revision is not recorded.

**Evidence:**
- Observed in the crash report: the crashing thread was `UamQueryHandler::HandleGetChatMessages` → `ChatRepository::LoadLocalChat` → `ParseLocalChatFile` → `uam::io::TryReadFile` → `basic_stringbuf::overflow` → `string::__grow_by`, with an allocation of about 3 GiB.
- From the code: saves are limited to `kMaxChatJsonBytes` (256 MiB), but `ParseLocalChatFile` reads with no limit unless the caller passes one, and `LoadLocalChat` doesn't. `TryReadFile` streams into an `ostringstream` and then copies it, so it needs about twice the file size.
- Hypothesis: the history page read was refused first, so the handler fell back to a full `LoadLocalChat`.

**Reproduction status:** Not reproduced with a 2 GB file. On Linux, a sparse 300 MiB chat file used about 619 MB of memory to load through the same path.

**Acceptance criteria:**
- [ ] Every full chat read checks the file size and refuses files above the chat limit without reading them.
- [ ] The user sees one short line, and the size detail goes to the diagnostic log.
- [ ] The oversized file and its `.bak` are not deleted, changed or replaced from backup.
- [ ] A regression test covers the refusal.

**Review:** Triage and self-review only, by the drafting model. No independent review of the report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

---

## PR preview

**Title:** `fix(chat-storage): refuse oversized chat files before reading them`

Opening a saved chat larger than the 256 MiB save limit read the whole file into memory and could abort UAM.
- **One shared check:** every full chat read now goes through one bounded reader that checks the file size first. Files over the limit are refused without being read.
- **What the user and the log see:** the user sees "This chat is too large to open.", including when the history request's page fallback also fails. The file name, size and limit go to one diagnostic line per file per run.
- **The oversized file is protected:** startup recovery, recovery from a `.bak` with no primary, and saves of the unopened chat never replace an oversized file with its older `.bak`.
- **Old-format chat folders:** a folder over the limit is left unmigrated and named in the load warnings.
- **Docs:** the persistence docs describe the limit and its outcome.

Related to #<issue>

**Proof:**
- **Regression tests:** three new cases in `tests/core_chat_repository_tests.cpp`. A scratch Linux driver linked against the real repository code ran the same steps.
  - All steps pass with the fix.
  - Each check was confirmed to fail without the change it covers: the read cap, and the guard that stops a save from adopting the backup.
- **Memory:** with the default limit and a sparse 300 MiB chat file, peak memory went from about 619 MB before the fix to about 10 MB after.
- **Compile:** `chat_repository.cpp` and `io_utils.h` pass g++ 13 and clang++ 18 syntax checks with stubbed CEF headers.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Not run:**
  - `uam_core_tests`, the desktop build and CTest (Linux can't build the desktop app).
  - The runner-only build: the proxy refused the sqlite.org download.
  - A compile of `chat_handlers.cpp`: no CEF headers here.
  - A capture of the history view's error state.
- **Known remaining behaviour:** saving an unopened oversized chat still copies its unchanged transcript into a new primary file without loading it. The docs state this.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 14 suggested, 12 implemented.

- Round 1: 8 suggested, 7 implemented. Declined: callers outside the open path that put their own prefix in front of the message.
- Round 2: 6 suggested, 5 implemented. Declined: a duplicate path and size lookup in the read helper, which is correct and costs one extra metadata call.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
