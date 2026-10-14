# Durable memory

Memory extraction and recall are optional app services over local Markdown files. Chat policy controls whether a chat may use memory; the store and workspace services own file access and scanning.

## Flow and invariants

After eligible chat activity, `MemoryService` prepares extraction through a configured worker and writes supported entries under the resolved memory root. Recall prepares a bounded preface for a prompt. `MemoryLibraryService` handles browse and scan candidates; workspace-local entries use `<workspace>/.codex/memories/`. Secret checks and storage guards apply before writes. Preserve user-authored file contents and workspace ownership; never infer permission to overwrite from a successful scan.

Memory settings include recall budget and worker bindings. Keep those limits normalized at load and save boundaries. If file ownership or workspace routing changes, update the focused tests for local and global roots.

## Policy inheritance

Policy resolution uses an explicit chat selection, then an explicit provider selection, then the app default. Missing provider defaults and new chats select `default`, so later changes to app or provider defaults apply through inheritance. Existing persisted concrete selections are retained because legacy automatically seeded values cannot be distinguished from intentional overrides.

Off prevents future UAM recall and AI memory extraction. Read Only permits recall and prevents AI extraction. Local prepared output and remote publication admission check the current resolved policy before accepting worker results. These policies do not remove memory text already included in a resumed provider's conversation history.

The provider-switch contract preserves an existing chat's memory selection: `default` follows the destination provider, while an explicit selection remains explicit. Creation inheritance and switch preservation pass focused native and frontend verification; live acceptance remains pending.

## Compact recall and detail files

First-send recall keeps the existing normalized byte budget, default 2,048 with a 512–8,192 range. Each selected fact becomes one UTF-8-safe line of at most 320 bytes, ending before its Evidence section; detail references do not replace selected facts. Off still suppresses recall and Read Only still permits it.

For local sessions with approved shared setup and memory enabled, validated global and effective-workspace category files are copied into the existing immutable owned provider bundle under `memory/global/` and `memory/workspace/`. `memory/index.md` lists titles and relative paths, and the provider instructions refer to that index. The references identify actual readable files rather than embedding full bodies and evidence in the initial prompt. Mandatory workspace, host and selected-agent instructions remain unchanged. A failed resource snapshot, file or manifest size limit, or disabled shared setup retains the original setup bundle and bounded facts without enabling sharing. Effective host/provider setup selection runs through existing asynchronous preparation rather than adding memory file reads to the GUI thread.

Remote `context.memory` callers can request optional `detailReferences: true`. The helper selects facts under the existing budget first, then adds references only within spare total bytes. Detail reads use existing `workspace.file.read` ownership, root and link validation; controller-global memory files are not exported to SSH. Older helpers ignore the optional request and return bounded facts. Remote optimization requires the compatible helper, with activation kept separate from live process ownership.

## Ownership and checks

- Services: `src/app/memory_service.*`, `src/app/memory_library_service.*`.
- Policy, receipts and storage: `src/common/memory/`, `src/common/config/settings_store.*`.
- Frontend actions and views: `src/cef/memory_handlers.cpp`, `UI-V2/src/components/settings/MemoryLibraryModal.tsx`, `UI-V2/src/types/memory.ts`.
- Focused checks: `tests/memory_tests.cpp`, `tests/memory_policy_tests.cpp`, `tests/memory_recall_preparation_tests.cpp`, `tests/memory_workspace_ownership_tests.cpp`, `tests/remote_memory_publication_tests.cpp`.

The recall byte budget is configurable and bounded by settings normalization; the default and maximum are defined in source. Do not document a separate throughput target without measurement.
