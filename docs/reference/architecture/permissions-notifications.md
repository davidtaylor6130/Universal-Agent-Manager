# Permissions and notifications

> Engineering reference retained from the trial base (`3a7c8477`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../../README.md); its agreements and [documented corrections](../../documentation-review.md) take precedence over stale claims here.

The native permission layer decides whether structured provider tool requests need a user response. The UI displays those requests and returns an explicit decision. App-wide notices currently have multiple presentation paths; a single notification system is an agreed target, still pending implementation.

## Flow and invariants

ACP permission handlers classify requests and consult per-chat approval mode, command auto-approval, global computer-use policy and any configured reviewer. The plan mode has a read-only ceiling. The frontend answers through `resolveAcpPermission` or the relevant computer-use action. Keep the original request identity attached to each response and fail closed when policy or reviewer state is unavailable.

AI Review uses Claude's native `auto` mode or Codex's confirmed native `auto_review` setting when active; residual native prompts remain user decisions. Other structured requests can use the configured provider reviewer. Its approval selects an explicit `allow_once` option or Codex's single-use `accept` decision, never a persistent choice inferred from its label. Codex permission grants use `turn` scope unless the user explicitly selects the offered `acceptForSession` option. Generic ACP `rawInput` preserves any valid JSON value, including strings, arrays and null, while native request parameters remain objects; malformed, oversized or sensitive review input stays manual. Reviewer admission and verdict delivery require the same runtime incarnation and request ID, and each actual process start renews that incarnation. Plan and managed read-only ceilings still apply. Codex reviewer workers disable shell tools and web search, use the read-only sandbox and reject tool-bearing output. These restrictions do not disable every MCP or provider tool; broader isolation remains an unsupported capability, so the worker is not described as fully text-only.

The agreed notification direction routes app-wide messages through the notifications panel and one shared transient alert. Existing separate notices remain until that work lands. UI presentation follows the [design language](../../ui/design-language.md); settings controls follow [settings rules](../../ui/settings.md).

`UI-V2/src/services/updateCatalog.ts` normalizes the first 10,000 characters of UAM and managed-provider release bodies into optional plain-text `releaseNotesExcerpt` values capped at 500 characters. Provider notes come from exact official release tags matching the npm or Homebrew version, with duplicate versions sharing a request and drafts or mismatched tags omitted. Failed optional notes preserve independently obtained update versions; the existing 15-second deadline covers both version and note requests, with five provider groups active at most. Validated caching and local/remote update projection preserve each installation method's own excerpt. GUI rendering remains pending.

## Provider-bound agent imports

OpenCode Markdown imports support only a flat permission subset: `edit: deny` is required, with optional `bash: ask` or `bash: deny`, and import requires read workspace access. Nested or path-pattern rules, aliases, merges, duplicate keys and other security fields remain rejected. Literal and simple quoted top-level keys are recognized; escaped or complex keys and unsupported indentation fail before security classification. Source descriptions do not establish a universal no-write guarantee: `bash: ask` retains its native approval semantics.

The canonical ordered `openCodePermission` policy travels inside the immutable agent Markdown snapshot. Generic edits preserve it even when an older frontend DTO omits it, and cannot widen or change it; replacement imports cannot strip or change the existing binding. Provider-only metadata omission still requires acknowledgment. Import validates the definition before atomic publication.

A bound policy can launch only through the local structured OpenCode ACP isolated adapter. SSH, central-native setup, provider preferences, terminal launches and other providers reject it rather than falling back to a weaker representation. Both adapter config and native-agent policy retain the source edit/bash rules plus task and computer-action denials, without a generic permissive wildcard. Existing generic agent adapters keep their previous behavior. Inherited `OPENCODE_CONFIG_CONTENT` is rejected for a bound policy when invalid, non-object, larger than 16 KiB, or containing agent, default-agent or legacy-mode overrides; the value is neither rewritten nor logged. Compatible permission-only and MCP-only inline configuration remains supported, with the bound native-agent policy retaining precedence. This is a scoped launch guard, not general inline-configuration sanitization.

## Ownership and checks

- Permission policy and ACP decisions: `src/common/config/approval_modes.h`, `src/common/security/`, `src/common/runtime/acp/acp_permission_handlers.cpp`, `src/app/computer_use_service.*`.
- Frontend permission surfaces: `UI-V2/src/components/chat/`, `UI-V2/src/components/settings/PermissionsSettings.tsx`.
- Notification primitives and state: `UI-V2/src/components/ui/Notice.tsx`, `UI-V2/src/store/`.
- Focused checks: `tests/acp_session_tests.cpp`, `tests/codex_permission_tests.cpp`, `tests/computer_use_global_policy_tests.cpp`, `tests/computer_use_tests.cpp`, `UI-V2/src/components/ui/Notice.test.tsx`.

No blanket response-time budget applies to human approval. Keep policy decisions deterministic and preserve timeout/cancel handling.
