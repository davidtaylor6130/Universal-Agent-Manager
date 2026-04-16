#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_lifecycle.h"

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

	return ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);
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
			if (!existing->running)
			{
				MarkCliTerminalDisabled(*existing);
			}
		}

		return *existing;
	}

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "term-" + chat.id;
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = resume_id;
	terminal->should_launch = can_launch_terminal;
	if (!can_launch_terminal)
	{
		MarkCliTerminalDisabled(*terminal);
	}
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
