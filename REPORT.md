# Report: stale empty queue journal fix

**Work branch:** `v5/fix-stale-empty-queue-journal`
**Final SHA:** `16501f2d`
**Commits** (on `origin/uam/v5.2-sync-main`):
- `7f9dc88d`: the fix
- `4b543ace`: changes from the first review
- `16501f2d`: changes from the re-check

No PR has been opened.

## What changed

Before this fix, a queue journal that didn't match its saved chat marked the chat with a storage error on every load. That blocked every later save and all memory progress for the chat. It happened on the reported install after a workspace folder move.

`ApplyQueueJournal` in `src/common/chat/chat_repository.cpp` now treats a journal as already used when all of these hold:
- the journal is readable;
- the journal has no prompts, is not held and has nothing dispatched;
- the saved chat's queue is also empty, with nothing dispatched;
- the journal's copy of the unsent-input recovery records and the managed birth matches the saved chat.

In that case the next full save uses the journal up, the next queue save replaces it, and no storage error is set. Memory progress stored in that journal is ignored, so some rows may be scanned again but none are skipped.

Every other mismatched or unreadable journal still holds the chat with the storage error. Each outcome is now logged once per chat per launch instead of on every load. The docs are updated:
- `docs/architecture/source-of-truth.md`
- `docs/architecture/app-state-persistence.md`
- `docs/documentation-review.md` ("Unrepairable queue journal")

## Tests

Five new cases are in `tests/core_chat_repository_tests.cpp`:
- `EmptyQueueJournalForMovedWorkspaceDoesNotBlockSavesOrMemoryProgress`
- `EmptyQueueJournalForMovedWorkspaceAcceptsQueueSaveMemoryProgressAndHydration`: covers the metadata load, the load from the summary, hydration, and a queue save with no full save first.
- `EmptyQueueJournalCarryingOtherRecoveryRecordsStaysHeld`
- `QueueJournalWithInputForMovedWorkspaceStaysHeldWithStorageError`
- `EmptyQueueJournalOverSavedQueuedInputForMovedWorkspaceStaysHeld`

**What ran (Linux):**
- **Scratch driver:** I compiled the real repository code by hand with g++ 13, using stub CEF headers, and ran a scratch copy of these scenarios against it.
  - On the base code it fails 15 checks: the save is refused, memory progress is lost and the error is logged on every load.
  - With the fix, every check passes.
- **Existing suite:** `uam_data_folder_tests`, compiled by hand, gives the same 57 of 94 failures before and after the change. They come from this Linux container, not from the fix.
- **Docs:** `python3 scripts/check_docs.py` passes.

**Not verified:**
- **Core tests:** `uam_core_tests` and the committed cases were not compiled or run, because CEF headers are unavailable.
- **Runner-only build:** it can't configure, because sqlite.org is blocked by the network policy.
- **Also not run:** the full native gates, the app and CI.
- **Positive twin of the recovery-record test:** not added. It needs matching recovery records on both sides, plus a moved workspace, and should rebind. Chats with no records on either side are covered.
- **Genuine storage errors (#747):** in a running app, the flag only clears when the chat is reloaded.

## Review

There was 1 independent reviewer over 2 iterations: fresh-context Opus. In total it made 10 suggestions, and 6 were implemented.

**Round 1** (review of `7f9dc88d`): 6 suggested (0 blockers, 1 should-fix, 5 nits); 3 implemented.

Implemented:
- Test the queue-save and hydration path the memory service uses.
- Don't rebind when the journal's copy of the recovery records or managed birth differs.
- Make the docs name the dispatched-count condition.

Declined:
- **Empty-held-queue acknowledgement:** it stays refused while a foreign-owner journal is on disk. That is better than before, when the chat was blocked outright, and is a follow-up.
- **"Rebound" log line:** it repeats each launch until the chat is saved. This meets the "once per launch" requirement; using the journal up on an idle save would widen the scope.
- **The writer that changed `workspace_directory` without using up the journal:** not identified. That needs a separate investigation, as a follow-up next to #747.

**Round 2** (re-check of `4b543ace`): 4 suggested (0 blockers, 1 should-fix, 3 nits); 3 implemented.

Implemented:
- Add a test for the recovery-record guard. Only the case that must stay held was added; the positive twin is listed under "Not verified".
- Correct the rebind comment.
- Add a load from the published summary to the test.

Declined:
- **Making the guard one-sided:** the symmetric guard is conservative and matches what round 1 asked for.

## Issue preview

#747 is a different problem: the save retries forever once the flag is set. This bug is what sets the flag. I recommend a new issue, marked related to #747.

> **bug(chat): a stale empty queue journal blocks every save for its chat**
>
> After a chat's workspace folder moves, its queue journal can stop matching the saved chat even though it holds no input. Every load then marks the chat as a storage error, so saves and memory progress for that chat are refused.
>
> **Expected:** an empty, unheld journal does not block anything, and a journal that holds input still does.
> **Actual:** on one real install:
> - "Queued inputs held: journal is unreadable or does not match saved chat" was logged 2,108 times across 175 chats.
> - The same ~32 chats failed the save at every clean quit.
> - "Memory scan could not save chat progress" was logged 767 times.
> - 54 journals differ from their chat only in `workspace_directory`, each with 0 prompts and held=false.
>
> **Evidence:** the user's logs above. The cause is `ApplyQueueJournal` in `src/common/chat/chat_repository.cpp`: an owner mismatch always sets the error. A scratch driver reproduces it on `uam/v5.2-sync-main`. The writer that changed `workspace_directory` without using up the journal is not yet identified.
>
> **Acceptance criteria:**
> - [ ] An empty, unheld journal does not block saves or memory progress.
> - [ ] A mismatched journal that holds input still holds the chat.
> - [ ] The message is logged once, not on every load.
>
> Related to #747.
>
> **Review:** self-triage only.
>
> **Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

> **fix(chat): stop a stale empty queue journal blocking saves**
>
> A queue journal that no longer matched its saved chat marked the chat as a storage error on every load, even when it held no input. After a workspace folder move, that blocked every later save and memory progress for the chat. Now an empty, unheld journal beside an empty saved queue is treated as already used, and the next save takes it over, with no error. This applies only when the journal's copy of the recovery records and managed birth matches the saved chat. Journals that hold input, or sit beside a saved queue that does, still hold the chat. Each outcome is logged once per chat per launch. The docs are updated.
>
> **Proof:**
> - On Linux, a scratch driver using the real repository code ran the new scenarios. They fail on the base (15 checks) and pass with the fix.
> - The standalone `uam_data_folder_tests` suite gives the same 57 environment failures before and after.
> - `scripts/check_docs.py` passes.
> - Not run: `uam_core_tests` and the five committed cases (no CEF here), the runner build (network blocked), the full native gates and CI.
>
> **Review:** Reviewed by 1 agent over 2 iterations.
>
> - Agent 1: claude-opus-5-5 (fresh context): suggested 10; implemented 6.
>
> Declined:
> - Empty-held-queue acknowledgement: a follow-up.
> - Rebound log line: already logged once per launch.
> - Folder-move writer: needs a separate investigation.
> - One-sided guard: the symmetric guard is conservative.
>
> **Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
>
> Related to #747.

## Screens needing Before/After capture

None. This is a native storage change with no UI change.
