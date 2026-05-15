#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/string_utils.h"

namespace uam
{
	inline constexpr std::string_view kCliTerminalIdPrefix = "term-";

	inline std::string CliTerminalIdForChat(std::string_view chat_id)
	{
		std::string terminal_id;
		terminal_id.reserve(kCliTerminalIdPrefix.size() + chat_id.size());
		terminal_id.append(kCliTerminalIdPrefix);
		terminal_id.append(chat_id);
		return terminal_id;
	}

	inline bool ProviderUsesCodexCli(const ProviderProfile& provider)
	{
		return uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCodexCli);
	}

	inline bool ProviderSupportsInteractiveTerminal(const ProviderProfile& provider)
	{
		return ProviderRuntime::IsRuntimeEnabled(provider) &&
		       ProviderRuntime::UsesCliOutput(provider) &&
		       !ProviderRuntime::UsesInternalEngine(provider) &&
		       provider.supports_interactive;
	}

	inline std::string ProviderInteractiveTerminalUnavailableReason(const ProviderProfile& provider)
	{
		if (!ProviderRuntime::IsRuntimeEnabled(provider))
		{
			return uam::strings::NonEmptyOrFallback(ProviderRuntime::DisabledReason(provider), "Selected provider runtime is disabled in this build.");
		}

		if (!ProviderRuntime::UsesCliOutput(provider))
		{
			return "CLI output is unavailable for the selected provider.";
		}

		if (ProviderRuntime::UsesInternalEngine(provider) || !provider.supports_interactive)
		{
			return "Provider does not expose an interactive CLI runtime.";
		}

		return "";
	}

	inline std::string ResolveProviderInteractiveResumeId(const AppState& app, const ChatSession& chat, const ProviderProfile& provider)
	{
		if (ProviderUsesCodexCli(provider))
		{
			return uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
		}

		const NativeSessionLinkService native_session_linker;
		if (native_session_linker.HasRealNativeSessionId(chat))
		{
			return native_session_linker.RealNativeSessionId(chat);
		}

		return ChatHistorySyncService().ResolveResumeSessionIdForChat(app, chat);
	}

	inline std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat)
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		ChatSession effective_chat = chat;
		effective_chat.native_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);

		return ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);
	}

	inline CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat)
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		const std::string resume_id = ResolveProviderInteractiveResumeId(app, chat, provider);
		const bool can_launch_terminal = ProviderSupportsInteractiveTerminal(provider);

		if (CliTerminalState* existing = FindCliTerminalForChat(app, chat.id))
		{
			if (existing->frontend_chat_id.empty())
			{
				existing->frontend_chat_id = chat.id;
			}

			if (existing->terminal_id.empty())
			{
				existing->terminal_id = CliTerminalIdForChat(chat.id);
			}

			if (CliTerminalAttachedSessionId(*existing).empty() && !resume_id.empty())
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

		auto terminal = std::make_unique<CliTerminalState>();
		terminal->terminal_id = CliTerminalIdForChat(chat.id);
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

	inline void MarkSelectedCliTerminalForLaunch(AppState& app)
	{
		ChatSession* selected = ChatDomainService().SelectedChat(app);

		if (selected == nullptr)
		{
			return;
		}

		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *selected);
		const std::string unavailable_reason = ProviderInteractiveTerminalUnavailableReason(provider);
		if (!unavailable_reason.empty())
		{
			app.status_line = unavailable_reason;
			return;
		}

		CliTerminalState& terminal = EnsureCliTerminalForChat(app, *selected);

		if (!terminal.last_error.empty())
		{
			return;
		}

		terminal.should_launch = true;
	}

} // namespace uam
