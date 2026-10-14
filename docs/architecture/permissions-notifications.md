# Permissions and notifications

The native permission layer decides whether structured provider tool requests need a user response. The UI displays those requests and returns an explicit decision. App-wide notices currently have multiple presentation paths; a single notification system is an agreed target, still pending implementation.

## Flow and invariants

ACP permission handlers classify requests and consult per-chat approval mode, command auto-approval, global computer-use policy and any configured reviewer. The plan mode has a read-only ceiling. The frontend answers through `resolveAcpPermission` or the relevant computer-use action. Keep the original request identity attached to each response and fail closed when policy or reviewer state is unavailable.

AI Review uses Claude's native `auto` mode or Codex's confirmed native `auto_review` setting when active; residual native prompts remain user decisions. Other structured requests can use the configured provider reviewer. Its approval selects an explicit `allow_once` option or Codex's single-use `accept` decision, never a persistent choice inferred from its label. Codex permission grants use `turn` scope unless the user explicitly selects the offered `acceptForSession` option. Generic ACP `rawInput` preserves any valid JSON value, including strings, arrays and null, while native request parameters remain objects; malformed, oversized or sensitive review input stays manual. Reviewer admission and verdict delivery require the same runtime incarnation and request ID, and each actual process start renews that incarnation. Plan and managed read-only ceilings still apply. Reviewer workers use provider-specific restrictions; Codex's read-only sandbox and rejection of tool-bearing output do not establish a complete text-only execution boundary, which remains an unresolved provider capability check.

The agreed notification direction routes app-wide messages through the notifications panel and one shared transient alert. Existing separate notices remain until that work lands. UI presentation follows the [design language](../ui/design-language.md); settings controls follow [settings rules](../ui/settings.md).

`UI-V2/src/services/updateCatalog.ts` normalizes the first 10,000 characters of UAM and managed-provider release bodies into optional plain-text `releaseNotesExcerpt` values capped at 500 characters. Provider notes come from exact official release tags matching the npm or Homebrew version, with duplicate versions sharing a request and drafts or mismatched tags omitted. Failed optional notes preserve independently obtained update versions; the existing 15-second deadline covers both version and note requests, with five provider groups active at most. Validated caching and local/remote update projection preserve each installation method's own excerpt. GUI rendering remains pending.

## Ownership and checks

- Permission policy and ACP decisions: `src/common/config/approval_modes.h`, `src/common/security/`, `src/common/runtime/acp/acp_permission_handlers.cpp`, `src/app/computer_use_service.*`.
- Frontend permission surfaces: `UI-V2/src/components/chat/`, `UI-V2/src/components/settings/PermissionsSettings.tsx`.
- Notification primitives and state: `UI-V2/src/components/ui/Notice.tsx`, `UI-V2/src/store/`.
- Focused checks: `tests/acp_session_tests.cpp`, `tests/codex_permission_tests.cpp`, `tests/computer_use_global_policy_tests.cpp`, `tests/computer_use_tests.cpp`, `UI-V2/src/components/ui/Notice.test.tsx`.

No blanket response-time budget applies to human approval. Keep policy decisions deterministic and preserve timeout/cancel handling.
