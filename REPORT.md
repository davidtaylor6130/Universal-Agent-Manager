# Report: v5/fix-durable-write-global-lock

The review loop is finished and pushed. No PR, issue or comment has been created.

**Branch:** `v5/fix-durable-write-global-lock`, final SHA `f4a42b3fe1ccc881ea0033e9e093f04b5ff1441d`. Its base is `uam/v5.2-sync-main` @ `6f235eb9`. The branch has three commits:
- `a58e0a2d`: the fix
- `33d19b8b`: the round-1 review fixes
- `f4a42b3f`: two test tidy-ups from the re-check

## What changed
`AtomicWriteFileDetailed` used to take one process-wide mutex for the whole write, including the fsync or `F_FULLFSYNC`. It now takes a lock for its destination file only (`DurableWriteDestinationLock`).
- **How the lock is keyed:** by the absolute, normalised path of the destination. On Windows and macOS, ASCII letters are folded to lower case.
- **What the lock guarantees:** writes to the same file still run one at a time. Writes to different files run in parallel.
- **Table of locks:** one entry per file that is being written right now. An entry is removed when its last writer finishes. The table's own mutex is held only to add or remove an entry, never across disk I/O, a sync or a stage hook.
- **Clean-up of old temp files:** the throttled sweep keeps its cached directory listings under its own small mutex. Directory reads and file removals happen outside that mutex.
- **Stage hooks:** a caller's stage hooks now run under their own file's lock only, never under a process-wide one.
- **Durability:** temp write, sync, rename, directory sync and `.bak` backup are all unchanged.

## Review rounds
**Round 1: independent claude-opus-5-5 with a fresh context. 8 suggested (1 blocker, 3 should-fix, 4 nits), 8 implemented.**
1. **Blocker:** the macOS-only test `StorageRetentionWriterHoldsActualRootLockUntilCompletion` relied on the old global lock. It now blocks a write of `storage-retention.json` itself.
2. **Should-fix:** a failed assertion in the new tests could hang the test run. They now use an `OpenGateOnExit` guard declared after the futures. I forced a failure to check: the tests fail cleanly instead of hanging.
3. **Should-fix:** the docs overstated what the lock covers. They now say that publications through `CommitSyncedAtomicTemp` do not take it.
4. **Should-fix:** two hooks writing each other's files could deadlock. The docs and the class comment now say a stage hook must not perform durable writes.
5. **Nit:** fixed the out-of-date comment in `acp_session_lifecycle.cpp`.
6. **Nit:** the lock table, its mutex and the sweep listings are now deliberately leaked. A writer thread still running at shutdown therefore never touches a destroyed object.
7. **Nit:** documented that a second writer arriving while a directory is first being listed leaves its old temp files for a later write. The cross-path test now writes into the same directory.
8. **Nit:** documented the path spellings the key does not merge: links, non-ASCII case, Unicode normalisation forms and Windows `\\?\` prefixes.

**Round 2: fresh-context claude-opus-5-5 re-check of the round-1 fixes. 0 blockers, 0 should-fix, 2 nits suggested, 2 implemented.**
- The reviewer confirmed all 8 round-1 fixes.
- It found no other test that assumed the old global lock.
- The 2 nits were an over-claiming test comment and a stray blank line.

## Issue preview
**Title:** `bug(io): durable writes share one process-wide lock held across fsync`

UI-thread saves can wait behind unrelated background disk writes. `AtomicWriteFileDetailed` holds a single static mutex for the whole write: directory creation, the temp sweep, the temp write with `F_FULLFSYNC`, the rename, the backup and the directory sync. Reported by David Taylor through a scheduled task.

**Reproduction:** start a long durable write on one path, for example a usage snapshot of up to 64 MiB (`usage_collector.cpp:92`). Then save settings on the UI thread (`application.cpp:546`, `PollTickStages`), or save a folder or a chat synchronously.
**Expected:** writes to different files proceed independently.
**Actual:** the UI-thread save waits until the background write has finished its fsync. Caller stage hooks also run under the process-wide lock (`data_folder_tree.cpp:559`, `data_folder_settings_service.cpp:341`), so a hook that blocks or writes stalls every durable write.

**Environment:** `uam/v5.2-sync-main` @ `6f235eb9`, all platforms. The fsync cost is highest with `F_FULLFSYNC` on macOS.

**Evidence:** seen in the code at `src/common/utils/io_utils.h:1087-1088`; the throttled sweep's static `listings` map also relied on this lock. The UI stall is inferred from the code path and has not been measured in the app.
**Reproduction status:** reproduced with a standalone Linux test. With the old lock, a write to another path does not finish while one write is blocked in its stage hook.

**Acceptance criteria:**
- [ ] A write to one path completes while another path's write is blocked in a hook or fsync.
- [ ] Writes to the same path stay serialised and leave a complete primary and `.bak`.
- [ ] No process-wide lock is held across I/O, fsync or caller hooks.

**Review:** self-triaged only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview
**Title:** `fix(io): lock durable writes per destination instead of process-wide`

Durable writes used one process-wide mutex held across `F_FULLFSYNC`. UI-thread saves therefore waited behind unrelated background writes, and caller stage hooks ran under that lock.

Writes now take a lock for their destination path only. Writes to the same path stay serialised; different paths run in parallel. The temp sweep's listing cache has its own small mutex, and a write's hooks run under its own file's lock only. Temp write, sync, rename, directory sync and backup are unchanged. The macOS storage-retention root-lock test now blocks the policy file itself instead of relying on the old lock. The durable-write section is added to the app-state persistence reference.

**Proof:**
- `uam_runner` builds on Linux. sqlite.org is blocked in this environment, so the build used a substitute SQLite source from the npm `sqlite3` package (3.52, not the pinned 3.54).
- The four atomic-write cases (two of them new) were built as a standalone program with a stand-in harness. They pass over 5 repetitions with g++ and over 3 repetitions under ThreadSanitizer.
- With the old global lock put back, `DurableWriteBlockedInStageHookDoesNotBlockOtherDestination` fails. With no lock at all, `DurableWritesToOneDestinationStaySerialisedAndIntact` fails.
- When an assertion is forced to fail, both new tests fail cleanly rather than hang.
- `scripts/check_docs.py` passes.
- Not run: the `uam_core_tests` build and the full CTest gates, macOS (including the storage-retention test) and Windows, and any UI-thread latency measurement.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 10 suggested, 10 implemented.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## Before/After captures
None needed: this is a native I/O change with no UI change.
