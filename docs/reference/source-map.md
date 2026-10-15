# Implementation source map

These are repository entry points inspected for the documentation trial, not an assertion that every route has passed live verification. Shared feature agreements are in the [main guide](../README.md). Tests linked here are available evidence to inspect; this docs-only task did not execute application tests.

| Area | Source entry points | Existing checks |
|---|---|---|
| Provider registry | [Registry](../../src/common/provider/runtime/provider_runtime_registry.cpp), [runtime interface](../../src/common/provider/provider_runtime.h) | [Availability tests](../../tests/provider_availability_tests.cpp) |
| Gemini | [Runtime](../../src/common/provider/gemini/cli/gemini_cli_provider_runtime.cpp) | [ACP tests](../../tests/acp_session_tests.cpp) |
| Codex | [Runtime](../../src/common/provider/codex/cli/codex_cli_provider_runtime.cpp) | [Codex permission tests](../../tests/codex_permission_tests.cpp) |
| Claude Code | [Runtime](../../src/common/provider/claude/cli/claude_cli_provider_runtime.cpp), [messages](../../src/common/provider/claude/cli/claude_acp_message_handlers.cpp) | [ACP tests](../../tests/acp_session_tests.cpp) |
| OpenCode | [Runtime](../../src/common/provider/opencode/cli/opencode_cli_provider_runtime.cpp) | [Native identity tests](../../tests/native_chat_identity_tests.cpp) |
| Copilot | [Runtime](../../src/common/provider/copilot/cli/copilot_cli_provider_runtime.cpp) | [ACP tests](../../tests/acp_session_tests.cpp) |
| Antigravity | [Runtime](../../src/common/provider/antigravity/cli/antigravity_cli_provider_runtime.cpp) | [Availability tests](../../tests/provider_availability_tests.cpp) |
| Permission choices | [Composer choices](../../UI-V2/src/components/chat/Composer.tsx), [request handling](../../src/common/runtime/acp/acp_permission_handlers.cpp) | [Codex permission tests](../../tests/codex_permission_tests.cpp) |
| Settings scope | [Settings shell](../../UI-V2/src/components/settings/SettingsModal.tsx), [scope bar](../../UI-V2/src/components/settings/SettingsScopeBar.tsx), [backend scope](../../src/common/config/host_scoped_settings.h) | [Scope-bar tests](../../UI-V2/src/components/settings/SettingsScopeBar.test.tsx) |
| Snooze and wake | [Row](../../UI-V2/src/components/sidebar/SessionItem.tsx), [snooze menu](../../UI-V2/src/components/sidebar/SnoozeMenu.tsx) | [Sidebar search tests](../../UI-V2/src/components/sidebar/chatSearch.test.ts) |
| Shared skills | [Resource delivery](../../src/common/provider/attached_skill_resources.cpp) | [Central setup reference](central-configuration.md) |
| Agents | [Agent definitions](../../src/app/agent_definition_service.cpp) | [Agent metadata tests](../../tests/agent_target_metadata_tests.cpp) |
| Shell actions | [Service](../../src/app/shell_action_service.cpp), [settings](../../UI-V2/src/components/settings/ShellActionsSettings.tsx) | [Settings tests](../../UI-V2/src/components/settings/ShellActionsSettings.test.tsx) |
| Voice | [Bridge](../../src/cef/dictation_handlers.cpp), [settings](../../src/common/config/voice_input_settings.h) | [Voice tests](../../tests/voice_input_tests.cpp) |
| Phone | [Server](../../src/cef/companion_server.cpp), [browser UI](../../UI-V2/src/components/companion/CompanionShell.tsx) | [Integration checks](../../tests/companion_integration.py) |
| Memory | [Service](../../src/app/memory_service.cpp), [store](../../src/common/memory/memory_library_store.cpp) | [Policy tests](../../tests/memory_policy_tests.cpp) |
| Storage root | [Startup](../../src/app/application.cpp), [platform defaults](../../src/common/paths/app_paths.cpp) | [Open decision](../documentation-review.md#data-root) |
| Build exclusions | [CMake](../../CMakeLists.txt), [feature macros](../../src/common/config/build_features.h) | [Configurator checks](../../tools/build-configurator/test_configurator.py) |

The provider checklist labels **Implemented** only identify an existing source route. Partial and Needs verification rows deliberately retain limits from the base evidence; a full audit must exercise the relevant native CLI, platform and remote combinations.
