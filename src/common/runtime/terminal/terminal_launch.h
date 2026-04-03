#pragma once

#include <algorithm>
#include <filesystem>
#include <utility>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_local_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_vt_callbacks.h"

inline std::vector<std::string> BuildProviderInteractiveArgv(const uam::AppState& app, const ChatSession& chat);

inline void QueueStructuredPromptForTerminal(uam::CliTerminalState& terminal, const std::string& prompt)
{
	if (prompt.empty())
	{
		return;
	}

	terminal.pending_structured_prompts.push_back(prompt);
	terminal.generation_in_progress = true;
	terminal.last_activity_time_s = ImGui::GetTime();
}

inline bool InjectPromptAsPasteAndSubmit(uam::CliTerminalState& terminal, const std::string& prompt, std::string* error_out = nullptr)
{
	if (prompt.empty())
	{
		return true;
	}

	if (!terminal.running)
	{
		if (error_out != nullptr)
		{
			*error_out = "Provider terminal is not running.";
		}

		return false;
	}

	if (terminal.vt == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Provider terminal VT is not initialized.";
		}

		return false;
	}

	static constexpr char kBracketedPasteStart[] = "\x1b[200~";
	static constexpr char kBracketedPasteEnd[] = "\x1b[201~";

	if (!WriteToCliTerminal(terminal, kBracketedPasteStart, sizeof(kBracketedPasteStart) - 1))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write bracketed paste start marker.";
		}

		return false;
	}

	if (!WriteToCliTerminal(terminal, prompt.c_str(), prompt.size()))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write prompt to provider terminal.";
		}

		return false;
	}

	if (!WriteToCliTerminal(terminal, kBracketedPasteEnd, sizeof(kBracketedPasteEnd) - 1))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write bracketed paste end marker.";
		}

		return false;
	}

	vterm_keyboard_key(terminal.vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
	terminal.generation_in_progress = true;
	terminal.last_activity_time_s = ImGui::GetTime();
	return true;
}

inline bool FlushQueuedStructuredPromptsForTerminal(uam::CliTerminalState& terminal, std::string* error_out = nullptr)
{
	while (!terminal.pending_structured_prompts.empty())
	{
		std::string prompt = std::move(terminal.pending_structured_prompts.front());
		terminal.pending_structured_prompts.pop_front();

		if (InjectPromptAsPasteAndSubmit(terminal, prompt, error_out))
		{
			continue;
		}

		terminal.pending_structured_prompts.push_front(std::move(prompt));
		return false;
	}

	return true;
}

inline void ReportDroppedQueuedStructuredPromptsForTerminal(uam::AppState& app, const uam::CliTerminalState& terminal, const std::string& reason)
{
	if (terminal.pending_structured_prompts.empty() || terminal.attached_chat_id.empty())
	{
		return;
	}

	std::string message = "Structured prompt delivery failed before terminal became ready.";

	if (!reason.empty())
	{
		message += " Reason: " + reason;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, terminal.attached_chat_id);

	if (chat_index >= 0)
	{
		ChatDomainService().AddMessage(app.chats[chat_index], MessageRole::System, message);
		ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[chat_index]), app.data_root, app.chats[chat_index]);
	}

	app.status_line = message;
}

inline bool StartCliTerminalForChat(uam::AppState& app, uam::CliTerminalState& terminal, const ChatSession& chat, const int rows, const int cols)
{
	StopCliTerminal(terminal);
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		terminal.last_error = ProviderRuntime::DisabledReason(provider);

		if (terminal.last_error.empty())
		{
			terminal.last_error = "Selected provider runtime is disabled in this build.";
		}

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

	if (RuntimeLocalService().ProviderUsesLocalBridgeRuntime(provider))
	{
		if (!RuntimeLocalService().EnsureSelectedLocalRuntimeModelForProvider(app))
		{
			terminal.last_error = "Select a local runtime model to continue.";
			return false;
		}

		std::string bridge_error;

		if (!RuntimeLocalService().RestartLocalBridgeIfModelChanged(app, &bridge_error))
		{
			terminal.last_error = bridge_error.empty() ? "Failed to start OpenCode bridge." : bridge_error;
			return false;
		}
	}

	if (ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat))
	{
		ChatHistorySyncService().RefreshNativeSessionDirectory(app);
	}

	std::string template_status;
	std::string bootstrap_prompt;
	const TemplatePreflightOutcome template_outcome = ProviderRequestService().PreflightWorkspaceTemplateForChat(app, provider, chat, &bootstrap_prompt, &template_status);

	if (template_outcome == TemplatePreflightOutcome::BlockingError)
	{
		terminal.last_error = template_status.empty() ? "Prompt profile preflight failed." : template_status;
		return false;
	}

	if (template_outcome == TemplatePreflightOutcome::ReadyWithoutTemplate && !template_status.empty())
	{
		app.status_line = template_status;
	}

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
		terminal.session_ids_before = ChatHistorySyncService().SessionIdsFromChats(ChatHistorySyncService().LoadNativeSessionChats(app.native_history_chats_dir, provider));
	}
	else
	{
		terminal.session_ids_before.clear();
	}

	terminal.last_error.clear();
	terminal.last_sync_time_s = ImGui::GetTime();
	terminal.last_output_time_s = 0.0;
	terminal.last_activity_time_s = ImGui::GetTime();
	terminal.last_polled_time_s = 0.0;
	terminal.input_ready = false;
	terminal.startup_time_s = ImGui::GetTime();
	terminal.pending_structured_prompts.clear();
	terminal.generation_in_progress = false;

	terminal.vt = vterm_new(terminal.rows, terminal.cols);

	if (terminal.vt == nullptr)
	{
		terminal.last_error = "Failed to initialize libvterm.";
		StopCliTerminal(terminal, false);
		return false;
	}

	vterm_set_utf8(terminal.vt, 1);
	terminal.screen = vterm_obtain_screen(terminal.vt);
	terminal.state = vterm_obtain_state(terminal.vt);
	vterm_screen_set_callbacks(terminal.screen, &kVTermScreenCallbacks, &terminal);
	vterm_screen_set_damage_merge(terminal.screen, VTERM_DAMAGE_CELL);
	vterm_output_set_callback(terminal.vt, WriteBytesToPty, &terminal);
	vterm_screen_reset(terminal.screen, 1);
	const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
	std::string startup_error;

	if (!PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, workspace_root, interactive_argv, &startup_error))
	{
		if (terminal.last_error.empty())
		{
			terminal.last_error = startup_error.empty() ? "Failed to start provider terminal." : startup_error;
		}

		StopCliTerminal(terminal, false);
		return false;
	}

	terminal.running = true;
	terminal.should_launch = false;
	terminal.needs_full_refresh = true;

	if (!chat.prompt_profile_bootstrapped && chat.messages.empty() && template_outcome == TemplatePreflightOutcome::ReadyWithTemplate)
	{
		if (!bootstrap_prompt.empty())
		{
			std::string bootstrap_command = bootstrap_prompt + "\n";
			WriteToCliTerminal(terminal, bootstrap_command.c_str(), bootstrap_command.size());
		}

		const int chat_index = ChatDomainService().FindChatIndexById(app, chat.id);

		if (chat_index >= 0)
		{
			app.chats[chat_index].prompt_profile_bootstrapped = true;
			ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[chat_index]), app.data_root, app.chats[chat_index]);
		}
	}

	return true;
}
