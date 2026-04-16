#pragma once

#include <algorithm>
#include <filesystem>
#include <utility>
#include <string>
#include <vector>

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_vt_callbacks.h"

inline std::vector<std::string> BuildProviderInteractiveArgv(const uam::AppState& app, const ChatSession& chat);

inline bool StartCliTerminalForChat(uam::AppState& app, uam::CliTerminalState& terminal, const ChatSession& chat, const int rows, const int cols)
{
	StopCliTerminal(terminal);
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "begin", &terminal, "chat_id=" + chat.id + ", provider_id=" + provider.id);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		terminal.last_error = ProviderRuntime::DisabledReason(provider);
		if (terminal.last_error.empty())
			terminal.last_error = "Selected provider runtime is disabled in this build.";
		return false;
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		terminal.last_error = "Selected provider is fixed to structured output.";
		return false;
	}

	if (ProviderRuntime::UsesInternalEngine(provider))
	{
		terminal.last_error = "Active provider does not support terminal mode.";
		return false;
	}

	if (!provider.supports_interactive)
	{
		terminal.last_error = "Active provider does not expose an interactive runtime command.";
		return false;
	}

	if (ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat))
		app.native_history_chats_dir = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat);

	const std::vector<std::string> interactive_argv = BuildProviderInteractiveArgv(app, chat);

	if (interactive_argv.empty())
	{
		terminal.last_error = "Active provider does not expose an interactive CLI command.";
		return false;
	}

	terminal.rows = std::max(8, rows);
	terminal.cols = std::max(20, cols);
	terminal.attached_chat_id = chat.id;
	terminal.attached_session_id = ChatHistorySyncService().ResolveResumeSessionIdForChat(app, chat);
	terminal.linked_files_snapshot = chat.linked_files;

	if (ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat))
	{
		ChatHistorySyncService().ExportChatToNative(app, chat);
		terminal.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(ChatHistorySyncService().LoadNativeSessionChats(ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat), provider));
	}
	else
	{
		terminal.session_ids_before.clear();
	}

	terminal.last_error.clear();
	terminal.last_sync_time_s    = GetAppTimeSeconds();
	terminal.last_output_time_s  = 0.0;
	terminal.last_activity_time_s = GetAppTimeSeconds();
	terminal.last_user_input_time_s = 0.0;
	terminal.last_ai_output_time_s = 0.0;
	terminal.last_polled_time_s  = 0.0;
	terminal.input_ready         = false;
	terminal.startup_time_s      = GetAppTimeSeconds();
	terminal.generation_in_progress = false;
	terminal.turn_state = uam::CliTerminalTurnState::Idle;

	const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
	std::string startup_error;

	if (!PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, workspace_root, interactive_argv, &startup_error))
	{
		if (terminal.last_error.empty())
			terminal.last_error = startup_error.empty() ? "Failed to start provider terminal." : startup_error;
		LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "process_launch_failed", &terminal, terminal.last_error);
		StopCliTerminal(terminal, false);
		return false;
	}

	terminal.running      = true;
	terminal.should_launch = false;
	LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "process_launched", &terminal, "argv0=" + interactive_argv.front());

	return true;
}
