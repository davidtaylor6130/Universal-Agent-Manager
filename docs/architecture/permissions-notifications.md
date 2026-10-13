# Permissions and notifications

The native permission layer decides whether structured provider tool requests need a user response. The UI displays those requests and returns an explicit decision. App-wide notices currently have multiple presentation paths; a single notification system is an agreed target, still pending implementation.

## Flow and invariants

ACP permission handlers classify requests and consult per-chat approval mode, command auto-approval, global computer-use policy and any configured reviewer. The plan mode has a read-only ceiling. The frontend answers through `resolveAcpPermission` or the relevant computer-use action. Keep the original request identity attached to each response and fail closed when policy or reviewer state is unavailable.

The agreed notification direction routes app-wide messages through the notifications panel and one shared transient alert. Existing separate notices remain until that work lands. UI presentation follows the [design language](../ui/design-language.md); settings controls follow [settings rules](../ui/settings.md).

`UI-V2/src/services/updateCatalog.ts` normalizes the first 10,000 characters of the UAM release body into an optional plain-text `releaseNotesExcerpt` capped at 500 characters, preserves it through validated caching and UAM update projection, and leaves GUI rendering and provider release excerpts pending.

## Ownership and checks

- Permission policy and ACP decisions: `src/common/config/approval_modes.h`, `src/common/security/`, `src/common/runtime/acp/acp_permission_handlers.cpp`, `src/app/computer_use_service.*`.
- Frontend permission surfaces: `UI-V2/src/components/chat/`, `UI-V2/src/components/settings/PermissionsSettings.tsx`.
- Notification primitives and state: `UI-V2/src/components/ui/Notice.tsx`, `UI-V2/src/store/`.
- Focused checks: `tests/codex_permission_tests.cpp`, `tests/computer_use_global_policy_tests.cpp`, `tests/computer_use_tests.cpp`, `UI-V2/src/components/ui/Notice.test.tsx`.

No blanket response-time budget applies to human approval. Keep policy decisions deterministic and preserve timeout/cancel handling.
