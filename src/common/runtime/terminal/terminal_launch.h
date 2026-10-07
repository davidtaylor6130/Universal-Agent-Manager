#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "app/chat_domain_service.h"
#include "app/git_worktree_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_dimensions.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/runtime/terminal/terminal_native_identity.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/utils/string_utils.h"
#include "remote/runner_proxy.h"

namespace uam
{

	inline bool FailCliTerminalStart(CliTerminalState& terminal, CliTerminalLifecycleState failure_state, std::string error_message)
	{
		if (terminal.context_preparation != nullptr)
		{
			terminal.context_preparation->cancellation.request_stop();
			terminal.context_preparation.reset();
		}
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

	/// <summary>Bind a new Claude terminal before launch, or open the native picker for a legacy chat.</summary>
	inline bool PrepareFreshClaudeTerminalArgv(AppState& app, ChatSession& chat,
	    std::vector<std::string>& argv, std::string& error)
	{
		if (!chat.native_session_id.empty()) return true;
		if (!chat.remote_claude_session_unstarted && !chat.native_session_reset_pending)
		{
			// Older chats may have launched before UAM recorded remote Claude IDs.
			argv.push_back("--resume");
			return true;
		}

		// Claude has no empty-session creation command. Save the ID before launch
		// starts so every later launch can target the same conversation.
		const std::string session_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (session_id.empty())
		{
			error = "Could not create a Claude session ID.";
			return false;
		}
		const bool previous_reset_pending = chat.native_session_reset_pending;
		chat.native_session_id = session_id;
		chat.native_session_reset_pending = false;
		chat.remote_claude_session_unstarted = false;
		if (!ChatRepository::SaveChat(app.data_root, chat))
		{
			chat.native_session_id.clear();
			chat.native_session_reset_pending = previous_reset_pending;
			chat.remote_claude_session_unstarted = true;
			error = "Could not save the Claude session ID. Retry when storage is available.";
			return false;
		}
		argv.push_back("--session-id");
		argv.push_back(session_id);
		return true;
	}

	inline bool PrepareRemoteClaudeTerminalArgv(AppState& app, ChatSession& chat,
	    std::vector<std::string>& argv, std::string& error)
	{
		return PrepareFreshClaudeTerminalArgv(app, chat, argv, error);
	}

	inline bool StartCliTerminalForChat(AppState& app, CliTerminalState& terminal, ChatSession& chat, int rows, int cols)
	{
		if (terminal.context_preparation == nullptr) StopCliTerminal(terminal);
		if (terminal.running)
		{
			terminal.last_error = "The previous terminal is still stopping.";
			return false;
		}
		if (chat.side_cleanup_requested)
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled,
			                            "This side chat is closing.");
		if (chat.imported_read_only)
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled,
			                            "Imported transcripts are read-only. Create a new chat in a workspace to continue.");
		}
		const ExecutionHost* execution_host =
		    uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const bool remote = execution_host != nullptr &&
		                    execution_host->id != uam::execution_hosts::kLocalHostId;
		if (execution_host == nullptr || (remote && execution_host->runner_status != "ready"))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped,
			                            remote ? "The selected remote runner is not ready. Recheck it in Settings."
			                                   : "The selected execution host no longer exists.");
		}
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(provider);
		LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "begin", &terminal, "chat_id=" + chat.id + ", provider_id=" + provider.id);

		if (!ProviderRuntime::IsRuntimeEnabled(provider))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, uam::strings::NonEmptyOrFallback(ProviderRuntime::DisabledReason(provider), "Selected provider runtime is disabled in this build."));
		}

		if (!ProviderRuntime::UsesCliOutput(provider))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, "Selected provider is fixed to structured output.");
		}

		if (!provider.supports_interactive)
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Disabled, "Active provider does not expose an interactive runtime command.");
		}
		if (const std::string permission_flag_error = runtime.InteractiveConfigurationError(provider, app.settings); !permission_flag_error.empty())
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, permission_flag_error);
		}

		std::string runtime_handoff_error;
		if (!PrepareAcpSessionForCliTerminalLaunch(app, chat, &runtime_handoff_error))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, runtime_handoff_error);
		}
		if (!GitWorktreeService().PrepareForFirstLaunch(app, chat, &runtime_handoff_error))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, runtime_handoff_error);
		}

		const bool chat_uses_native_history = !remote &&
		    ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat);
		std::filesystem::path native_history_chats_dir;

		if (chat_uses_native_history)
		{
			native_history_chats_dir = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat);
			app.native_history_chats_dir = native_history_chats_dir;
		}

		std::string session_id_error;
		if (!runtime.PrepareInteractiveSession(app, chat, provider,
		    ResolveProviderInteractiveResumeId(app, chat, provider), *execution_host, &session_id_error))
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, session_id_error);
		}

		std::vector<std::string> provider_argv = BuildProviderInteractiveArgv(app, chat);
		if (provider_argv.empty())
		{
			return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, "Active provider does not expose an interactive CLI command.");
		}
		if (provider.id == uam::provider_ids::kClaudeCli &&
		    ResolveProviderInteractiveResumeId(app, chat, provider).empty())
		{
			std::string error;
			if (!PrepareFreshClaudeTerminalArgv(app, chat, provider_argv, error))
				return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, error);
		}
		terminal.rows = ClampCliTerminalLaunchRows(rows);
		terminal.cols = ClampCliTerminalLaunchCols(cols);
		terminal.attached_chat_id = chat.id;
		terminal.attached_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);
		terminal.linked_files_snapshot = chat.linked_files;

		// Capture the provider baseline before an unbound native CLI starts.
		if (chat_uses_native_history)
		{
			ChatHistorySyncService().ExportChatToNative(app, chat);
			terminal.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(ChatHistorySyncService().LoadNativeSessionChats(native_history_chats_dir, provider));
		}
		else if (!remote && terminal.attached_session_id.empty())
		{
			terminal.session_ids_before = runtime.SnapshotInteractiveSessionIds();
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
		terminal.native_identity_requires_owned_reply = terminal.attached_session_id.empty() &&
		    (provider.id == provider_ids::kCodexCli || provider.id == provider_ids::kGeminiCli);
		terminal.native_identity_command.clear();
		terminal.native_identity_output.clear();
		terminal.native_identity_deferred_input.clear();
		terminal.native_identity_query_phase = 0;
		terminal.native_identity_query_time_s = launch_time_s;
		const std::string version_key = CliProviderVersionStateKey(provider.id, execution_host->id);
		if (terminal.attached_session_id.empty())
		{
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(version_key);
			if (version != app.runtime_cli_versions_by_provider_id.end() && version->second.checked)
				terminal.native_identity_command = NativeSessionStatusCommand(provider.id, version->second.installed_version);
		}
		if (!terminal.native_identity_command.empty()) terminal.native_identity_query_phase = 1;
		MarkCliTerminalStopped(terminal);

		const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
		std::filesystem::path process_working_directory = workspace_root;
		std::vector<std::string> launch_argv = provider_argv;
		std::string startup_error;

		std::vector<std::pair<std::string, std::string>> launch_environment = remote
		    ? std::vector<std::pair<std::string, std::string>>{}
		    : runtime.BuildInteractiveEnvironment(provider);
		std::string context_launch_channel;
		if (!PrepareCliProviderHandoffAsync(app, terminal, chat, *execution_host, provider_argv, launch_environment, context_launch_channel, startup_error))
		{
			if (!startup_error.empty()) return FailCliTerminalStart(terminal, CliTerminalLifecycleState::Stopped, startup_error);
			terminal.should_launch = true;
			return true;
		}
		launch_argv = provider_argv;
		if (remote && provider.id == provider_ids::kCodexCli && !context_launch_channel.empty() && !chat.provider_handoff_context.empty() && (chat.native_session_id.empty() || chat.provider_handoff_session_id != chat.native_session_id))
		{
			terminal.attached_session_id.clear();
			terminal.native_identity_requires_owned_reply = true;
		}
		if (remote)
		{
			process_working_directory = uam::remote::PackagedRunnerPath().parent_path();
			launch_argv = uam::remote::BuildRemoteTerminalSshArgv(
			    execution_host->ssh_alias, execution_host->platform,
			    execution_host->runner_version, workspace_root, provider_argv,
			    execution_host->runner_directory, context_launch_channel);
			if (launch_argv.empty()) startup_error = "The remote terminal launch request is invalid.";
		}
		if (!startup_error.empty() ||
		    !PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(
		        terminal, process_working_directory, launch_argv, &startup_error,
		        launch_environment))
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
		terminal.uses_prompt_activity_tracking = runtime.SupportsInteractivePromptTracking();
		MarkCliTerminalTurnBusy(terminal, false);
		LogCliDiagnosticEvent(app, "start_cli_terminal_for_chat", "process_launched", &terminal,
		                      "host=" + execution_host->id + ", argv0=" + launch_argv.front());

		return true;
	}

} // namespace uam
