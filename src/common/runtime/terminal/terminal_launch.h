#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_repository.h"
#include "common/paths/workspace_root.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_dimensions.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/utils/string_utils.h"

namespace uam
{

	inline bool FailCliTerminalStart(CliTerminalState& terminal, CliTerminalLifecycleState failure_state, std::string error_message)
	{
		if (failure_state == CliTerminalLifecycleState::Disabled)
		{
			MarkCliTerminalDisabled(terminal);
		}
		else
		{
			MarkCliTerminalStopped(terminal);
		}

		terminal.last_error = std::move(error_message);
		return false;
	}

	inline bool StartCliTerminalForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, int rows, int cols)
	{
		StopCliTerminal(terminal);
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "begin", &terminal, "chat_id=" + chat.id + ", provider_id=" + provider.id);

		if (!ProviderRuntime::IsRuntimeEnabled(provider))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, uam::strings::NonEmptyOrFallback(ProviderRuntime::DisabledReason(provider), "Selected provider runtime is disabled in this build."));
		}

		if (!ProviderRuntime::UsesCliOutput(provider))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, "Selected provider is fixed to structured output.");
		}

		if (ProviderRuntime::UsesInternalEngine(provider))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, "Active provider does not support terminal mode.");
		}

		if (!provider.supports_interactive)
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, "Active provider does not expose an interactive runtime command.");
		}

		const bool chat_uses_native_history = ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat);
		const bool chat_uses_codex_cli = uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCodexCli);
		std::filesystem::path native_history_chats_dir;

		if (chat_uses_native_history)
		{
			native_history_chats_dir = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat);
			app.native_history_chats_dir = native_history_chats_dir;
		}

		const std::vector<std::string> interactive_argv = BuildProviderInteractiveArgv(app, chat);

		if (interactive_argv.empty())
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, "Active provider does not expose an interactive CLI command.");
		}

		terminal.rows = ClampCliTerminalLaunchRows(rows);
		terminal.cols = ClampCliTerminalLaunchCols(cols);
		terminal.attached_chat_id = chat.id;
		terminal.attached_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);
		terminal.linked_files_snapshot = chat.linked_files;

		// RT-7: pre-launch session-id snapshot is per-provider. Unlike RT-3/PR-6 (pure argv),
		// these branches differ in data source (native history dir vs local chats vs codex index)
		// and the gemini branch has a side effect (ExportChatToNative), so it is intentionally
		// left inline rather than forced behind a single runtime virtual.
		if (chat_uses_native_history)
		{
			ChatHistorySyncService().ExportChatToNative(app, chat);
			terminal.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(ChatHistorySyncService().LoadNativeSessionChats(native_history_chats_dir, provider));
		}
		else if (uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kOpenCodeCli))
		{
			terminal.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(ChatRepository::LoadLocalChatSummaries(app.data_root));
		}
		else if (chat_uses_codex_cli && !uam::codex::IsValidThreadId(chat.native_session_id))
		{
			terminal.session_ids_before = uam::codex::ReadSessionIndexIds();
		}
		else
		{
			terminal.session_ids_before.clear();
		}

		const double launch_time_s = GetAppTimeSeconds();
		terminal.last_error.clear();
		terminal.last_sync_time_s = launch_time_s;
		terminal.last_output_time_s = 0.0;
		terminal.last_activity_time_s = launch_time_s;
		terminal.last_user_input_time_s = 0.0;
		terminal.last_ai_output_time_s = 0.0;
		terminal.last_polled_time_s = 0.0;
		terminal.input_ready = false;
		terminal.startup_time_s = launch_time_s;
		MarkCliTerminalStopped(terminal);

		const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
		std::string startup_error;

		if (!PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, workspace_root, interactive_argv, &startup_error))
		{
			if (terminal.last_error.empty())
			{
				terminal.last_error = uam::strings::NonEmptyOrFallback(startup_error, "Failed to start provider terminal.");
			}
			LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "process_launch_failed", &terminal, terminal.last_error);
			StopCliTerminal(terminal, false);
			return false;
		}

		terminal.running = true;
		terminal.should_launch = false;
		MarkCliTerminalTurnBusy(terminal, false);
		LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "process_launched", &terminal, "argv0=" + interactive_argv.front());

		return true;
	}

} // namespace uam
