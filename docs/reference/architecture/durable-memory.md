# Durable memory

> Engineering reference retained from the trial base (`3a7c8477`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../../README.md); its agreements and [documented corrections](../../documentation-review.md) take precedence over stale claims here.

Memory extraction and recall are optional app services over local Markdown files. Chat policy controls whether a chat may use memory; the store and workspace services own file access and scanning.

## Flow and invariants

After eligible chat activity, `MemoryService` prepares extraction through a configured worker and writes supported entries under the resolved memory root. Recall prepares a bounded preface for a prompt. `MemoryLibraryService` handles browse and scan candidates; workspace-local entries use `<workspace>/.UAM/`. Secret checks and storage guards apply before writes. Preserve user-authored file contents and workspace ownership; never infer permission to overwrite from a successful scan.

Memory settings include recall budget and worker bindings. Keep those limits normalized at load and save boundaries. If file ownership or workspace routing changes, update the focused tests for local and global roots.

## Policy inheritance

Policy resolution uses an explicit chat selection, then an explicit provider selection, then the app default. Missing provider defaults and new chats select `default`, so later changes to app or provider defaults apply through inheritance. Existing persisted concrete selections are retained because legacy automatically seeded values cannot be distinguished from intentional overrides.

Off prevents future UAM recall and AI memory extraction. Read Only permits recall and prevents AI extraction. Local prepared output and remote publication admission check the current resolved policy before accepting worker results. These policies do not remove memory text already included in a resumed provider's conversation history.

The provider-switch contract preserves an existing chat's memory selection: `default` follows the destination provider, while an explicit selection remains explicit. Creation inheritance and switch preservation pass focused native and frontend verification; live acceptance remains pending.

## Compact recall and detail files

First-send recall keeps the existing normalized byte budget, default 2,048 with a 512–8,192 range. Each selected fact becomes one UTF-8-safe line of at most 320 bytes, ending before its Evidence section; detail references do not replace selected facts. Off still suppresses recall and Read Only still permits it.

For local sessions with approved shared setup and memory enabled, validated global and effective-workspace category files are copied into the existing immutable owned provider bundle under `memory/global/` and `memory/workspace/`. `memory/index.md` lists titles and relative paths, and the provider instructions refer to that index. The references identify actual readable files rather than embedding full bodies and evidence in the initial prompt. Mandatory workspace, host and selected-agent instructions remain unchanged. A failed resource snapshot, file or manifest size limit, or disabled shared setup retains the original setup bundle and bounded facts without enabling sharing. Effective host/provider setup selection runs through existing asynchronous preparation rather than adding memory file reads to the GUI thread.

Remote `context.memory` callers can request optional `detailReferences: true`. The helper selects facts under the existing budget first, then adds references only within spare total bytes. Detail reads use existing `workspace.file.read` ownership, root and link validation; controller-global memory files are not exported to SSH. Older helpers ignore the optional request and return bounded facts. Remote optimization requires the compatible helper, with activation kept separate from live process ownership.

## Failed-worker retries

A failed worker sets a per-chat retry delay (5 minutes doubling to 1 hour) and a per host and worker provider pause (30 seconds to 5 minutes). Worker exits and rejected or truncated output take a category from `ClassifyWorkerFailure` (`src/common/memory/memory_worker_failure_pause.h`), which reads only the last 4 KiB of the process error and stderr text and returns a fixed token; start, history-read and input-preparation failures use fixed literals. Log lines read `(category=<token> code=<exit>)`. Rate-limit, network (including server and overload errors) and timeout categories are matched before auth and version wording and stop there. Other categories feed `AppState::memory_failure_streak_by_chat_id`, keyed by the category and exit code and by a fingerprint of the chat's resolved memory policy, host, workspace directory, folder and that folder's directory, worker provider and model, the provider command and flags, and the CLI version entry for that host (`CliProviderVersionStateKey`, so an SSH host's own version). After `kIdenticalWorkerFailuresBeforePause` (3) identical entries the chat is paused: the idle sweep skips it, its queued automatic scan is dropped and a queued manual scan is parked on the streak. The idle sweep, which already visits every chat each poll, compares a paused chat's message count; a difference clears the pause but keeps the streak, the pending retry time and the per-chat failure count, so the chat still waits out its backoff and the next identical failure pauses again. Every 30 seconds `RecheckMemoryFailureStreaks` rebuilds paused chats' fingerprints, ends pauses whose inputs changed and drops streaks of deleted chats; it is the only step that builds the chat lookup table for streaks. Either resume puts a parked manual scan back at the front of the queue. Changed inputs also clear the retry time. A successful run and `QueueManualScan` erase the streak; nothing is persisted, so a restart clears it. Focused cases are the `MemoryService*Pause*`, `MemoryServiceDifferentOrTransientWorkerFailuresDoNotPauseRetries` and `MemoryWorkerFailureClassifier*` tests in `tests/memory_tests.cpp`; `PerformanceMemoryPollOnLargeInstall` includes 500 paused chats.

## Ownership and checks

- Services: `src/app/memory_service.*`, `src/app/memory_library_service.*`.
- Policy, receipts and storage: `src/common/memory/`, `src/common/config/settings_store.*`.
- Frontend actions and views: `src/cef/memory_handlers.cpp`, `UI-V2/src/components/settings/MemoryLibraryModal.tsx`, `UI-V2/src/types/memory.ts`.
- Focused checks: `tests/memory_tests.cpp`, `tests/memory_policy_tests.cpp`, `tests/memory_recall_preparation_tests.cpp`, `tests/memory_workspace_ownership_tests.cpp`, `tests/remote_memory_publication_tests.cpp`.

The recall byte budget is configurable and bounded by settings normalization; the default and maximum are defined in source. Do not document a separate throughput target without measurement.
