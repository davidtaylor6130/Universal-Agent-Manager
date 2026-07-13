# Codebase deduplication report

Reviewed TypeScript/React utilities, state handlers, and native service/configuration patterns for
issue #16. No dependency was added and behavior was preserved.

| Finding | Resolution |
| --- | --- |
| `LocalAttachment` was declared twice consecutively in `ChatView.tsx`. | Removed the duplicate declaration. |
| Model IDs were formatted by identical `titleFromModelId` functions in chat and settings. | Exported and reused the existing chat helper. |
| Permission-mode icon selection was duplicated in `Composer.tsx` and `ChatView.tsx`. | Reused the Composer helper with its existing size difference as an argument. |
| Stream placeholder handling re-checked `isStreaming` immediately after the opposite case had already continued. | Removed the redundant condition. |
| `SidebarHeader` duplicated responsibilities now owned by `LeftActivityRail` and was no longer imported. | Removed the dead component and its obsolete test. |

Small local helpers with coincidental names, test fixture builders, platform-specific implementations,
and distinct status-label functions were left local because consolidating them would add coupling without
removing duplicated behavior.
