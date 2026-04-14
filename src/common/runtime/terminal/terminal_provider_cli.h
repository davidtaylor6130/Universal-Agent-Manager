#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_local_service.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/runtime/terminal/terminal_lifecycle.h"

inline std::vector<std::string> ForceOpenCodeModelFlag(std::vector<std::string> argv, const std::string& provider_model_id)
{
	if (argv.empty() || Trim(provider_model_id).empty())
	{
		return argv;
	}

	std::vector<std::string> filtered;
	filtered.reserve(argv.size() + 2);

	for (std::size_t i = 0; i < argv.size(); ++i)
	{
		const std::string& arg = argv[i];

		if (arg == "--model" || arg == "-m")
		{
			if (i + 1 < argv.size())
			{
				++i;
			}

			continue;
		}

		if (arg.rfind("--model=", 0) == 0 || arg.rfind("-m=", 0) == 0)
		{
			continue;
		}

		filtered.push_back(arg);
	}

	filtered.push_back("--model");
	filtered.push_back(provider_model_id);
	return filtered;
}

inline std::vector<std::string> BuildProviderInteractiveArgv(const uam::AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	ChatSession effective_chat = chat;

	if (!NativeSessionLinkService().HasRealNativeSessionId(effective_chat))
	{
		effective_chat.native_session_id.clear();
		const std::string resume_id = ChatHistorySyncService().ResolveResumeSessionIdForChat(app, chat);

		if (!resume_id.empty())
		{
			effective_chat.native_session_id = resume_id;
		}
	}

	std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);

	if (RuntimeLocalService().ProviderUsesLocalBridgeRuntime(provider))
	{
		std::string selected_model = Trim(app.opencode_bridge.selected_model);

		if (selected_model.empty())
		{
			selected_model = Trim(app.settings.selected_model_id);
		}

		if (!selected_model.empty())
		{
			std::string provider_model_id = selected_model;

			if (provider_model_id.rfind("uam_local/", 0) != 0)
			{
				provider_model_id = "uam_local/" + provider_model_id;
			}

			argv = ForceOpenCodeModelFlag(std::move(argv), provider_model_id);
		}
	}

	return argv;
}

inline uam::CliTerminalState& EnsureCliTerminalForChat(uam::AppState& app, const ChatSession& chat)
{
	const std::string resume_id = ChatHistorySyncService().ResolveResumeSessionIdForChat(app, chat);
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const bool can_launch_terminal = ProviderRuntime::IsRuntimeEnabled(provider) && ProviderRuntime::UsesCliOutput(provider) && !ProviderRuntime::UsesInternalEngine(provider) && provider.supports_interactive;

	if (uam::CliTerminalState* existing = FindCliTerminalForChat(app, chat.id))
	{
		if (existing->frontend_chat_id.empty())
		{
			existing->frontend_chat_id = chat.id;
		}

		if (existing->terminal_id.empty())
		{
			existing->terminal_id = "term-" + chat.id;
		}

		if (existing->attached_session_id.empty() && !resume_id.empty())
		{
			existing->attached_session_id = resume_id;
		}

		if (!can_launch_terminal)
		{
			existing->should_launch = false;
		}

		return *existing;
	}

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "term-" + chat.id;
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = resume_id;
	terminal->should_launch = can_launch_terminal;
	app.cli_terminals.push_back(std::move(terminal));
	return *app.cli_terminals.back();
}

inline void MarkSelectedCliTerminalForLaunch(uam::AppState& app)
{
	ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected == nullptr)
	{
		return;
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *selected);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		std::string reason = ProviderRuntime::DisabledReason(provider);
		app.status_line = reason.empty() ? "Selected provider runtime is disabled in this build." : reason;
		return;
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		app.status_line = "CLI output is unavailable for the selected provider.";
		return;
	}

	if (ProviderRuntime::UsesInternalEngine(provider) || !provider.supports_interactive)
	{
		app.status_line = "Provider does not expose an interactive CLI runtime.";
		return;
	}

	uam::CliTerminalState& terminal = EnsureCliTerminalForChat(app, *selected);

	if (!terminal.last_error.empty())
	{
		return;
	}

	terminal.should_launch = true;
}

inline bool SendPromptToCliRuntime(uam::AppState& app, ChatSession& chat, const std::string& prompt, std::string* error_out = nullptr)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		if (error_out != nullptr)
		{
			*error_out = ProviderRuntime::DisabledReason(provider);

			if (error_out->empty())
			{
				*error_out = "Selected provider runtime is disabled in this build.";
			}
		}

		return false;
	}

	if (RuntimeLocalService().ProviderUsesLocalBridgeRuntime(provider))
	{
		if (!RuntimeLocalService().RestartLocalBridgeIfModelChanged(app, error_out))
		{
			if (error_out != nullptr && error_out->empty())
			{
				*error_out = "Failed to start OpenCode bridge.";
			}

			if (error_out != nullptr && *error_out == "OpenCode bridge is starting.")
			{
				app.status_line = *error_out;
			}

			return false;
		}
	}

	uam::CliTerminalState& terminal = EnsureCliTerminalForChat(app, chat);
	LogCliDiagnosticEvent(app, "send_prompt_to_cli_runtime", "terminal_resolved", &terminal, "prompt_bytes=" + std::to_string(prompt.size()));

	if (!terminal.running)
	{
		if (!StartCliTerminalForChat(app, terminal, chat, 30, 120))
		{
			LogCliDiagnosticEvent(app, "send_prompt_to_cli_runtime", "start_failed", &terminal, terminal.last_error);
			if (error_out != nullptr)
			{
				*error_out = terminal.last_error.empty() ? "Failed to start provider terminal." : terminal.last_error;
			}

			return false;
		}
	}

	QueueStructuredPromptForTerminal(terminal, prompt);
	LogCliDiagnosticEvent(app, "send_prompt_to_cli_runtime", "prompt_queued", &terminal, "", static_cast<long long>(prompt.size()));

	if (!terminal.input_ready)
	{
		return true;
	}

	if (!FlushQueuedStructuredPromptsForTerminal(terminal, error_out))
	{
		LogCliDiagnosticEvent(app, "send_prompt_to_cli_runtime", "prompt_flush_failed", &terminal, (error_out != nullptr) ? *error_out : "");
		if (error_out != nullptr && error_out->empty())
		{
			*error_out = "Failed to flush queued prompt(s) to provider terminal.";
		}

		return false;
	}

	LogCliDiagnosticEvent(app, "send_prompt_to_cli_runtime", "prompt_flushed", &terminal);

	return true;
}
