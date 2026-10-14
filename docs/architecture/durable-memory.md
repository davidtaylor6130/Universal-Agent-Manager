# Durable memory

Memory extraction and recall are optional app services over local Markdown files. Chat policy controls whether a chat may use memory; the store and workspace services own file access and scanning.

## Flow and invariants

After eligible chat activity, `MemoryService` prepares extraction through a configured worker and writes supported entries under the resolved memory root. Recall prepares a bounded preface for a prompt. `MemoryLibraryService` handles browse and scan candidates; workspace-local entries use `<workspace>/.codex/memories/`. Secret checks and storage guards apply before writes. Preserve user-authored file contents and workspace ownership; never infer permission to overwrite from a successful scan.

Memory settings include recall budget and worker bindings. Keep those limits normalized at load and save boundaries. If file ownership or workspace routing changes, update the focused tests for local and global roots.

## Policy inheritance

Policy resolution uses an explicit chat selection, then an explicit provider selection, then the app default. Missing provider defaults and new chats select `default`, so later changes to app or provider defaults apply through inheritance. Existing persisted concrete selections are retained because legacy automatically seeded values cannot be distinguished from intentional overrides.

Off prevents future UAM recall and AI memory extraction. Read Only permits recall and prevents AI extraction. Local prepared output and remote publication admission check the current resolved policy before accepting worker results. These policies do not remove memory text already included in a resumed provider's conversation history.

The provider-switch contract preserves an existing chat's memory selection: `default` follows the destination provider, while an explicit selection remains explicit. Creation inheritance and switch preservation pass focused native and frontend verification; live acceptance remains pending.

## Ownership and checks

- Services: `src/app/memory_service.*`, `src/app/memory_library_service.*`.
- Policy, receipts and storage: `src/common/memory/`, `src/common/config/settings_store.*`.
- Frontend actions and views: `src/cef/memory_handlers.cpp`, `UI-V2/src/components/settings/MemoryLibraryModal.tsx`, `UI-V2/src/types/memory.ts`.
- Focused checks: `tests/memory_tests.cpp`, `tests/memory_policy_tests.cpp`, `tests/memory_recall_preparation_tests.cpp`, `tests/memory_workspace_ownership_tests.cpp`, `tests/remote_memory_publication_tests.cpp`.

The recall byte budget is configurable and bounded by settings normalization; the default and maximum are defined in source. Do not document a separate throughput target without measurement.
