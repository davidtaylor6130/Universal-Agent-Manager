#include "common/runtime/terminal/terminal_provider_cli.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/string_utils.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

std::string CliTerminalIdForChat(std::string_view chat_id)
{
	std::string terminal_id;
	terminal_id.reserve(kCliTerminalIdPrefix.size() + chat_id.size());
	terminal_id.append(kCliTerminalIdPrefix);
	terminal_id.append(chat_id);
	return terminal_id;
}

bool ProviderSupportsInteractiveTerminal(const ProviderProfile& provider)
{
	return ProviderRuntime::IsRuntimeEnabled(provider) &&
	       ProviderRuntime::UsesCliOutput(provider) &&
	       provider.supports_interactive;
}

std::string ProviderInteractiveTerminalUnavailableReason(const ProviderProfile& provider)
{
	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		return uam::strings::NonEmptyOrFallback(ProviderRuntime::DisabledReason(provider), "Selected provider runtime is disabled in this build.");
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		return "CLI output is unavailable for the selected provider.";
	}

	if (!provider.supports_interactive)
	{
		return "Provider does not expose an interactive CLI runtime.";
	}

	return "";
}

std::string ResolveProviderInteractiveResumeId(const AppState& app, const ChatSession& chat, const ProviderProfile& provider)
{
	if (const AcpSessionState* acp_session = FindAcpSessionForChat(app, chat.id); acp_session != nullptr && acp_session->running && !uam::strings::Trim(acp_session->session_id).empty())
	{
		return uam::strings::Trim(acp_session->session_id);
	}

	return ProviderRuntimeRegistry::Resolve(provider).ResolveInteractiveResumeId(app, chat);
}

/// <summary>False with an empty error means a remote stop is pending; retry after confirmation.</summary>
bool PrepareAcpSessionForCliTerminalLaunch(AppState& app, ChatSession& chat, std::string* error_out)
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	if (const std::string update_error = ProviderCliLaunchBlockReason(app, provider.id, chat.execution_host_id); !update_error.empty())
	{
		if (error_out != nullptr) *error_out = update_error;
		return false;
	}
	AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
	if (session != nullptr && AcpSessionHasBlockingRuntimeWork(*session))
	{
		if (error_out != nullptr)
		{
			*error_out = "Cannot start terminal fallback while the structured runtime is busy.";
		}
		return false;
	}

	if (!StopAcpSession(app, chat.id))
	{
		if ((session != nullptr && session->remote_stop_pending) ||
		    (session == nullptr && AcpStopInProgress(app, chat.id)))
		{
			return false;
		}
		if (error_out != nullptr)
		{
			*error_out = session != nullptr && session->remote_stop_unconfirmed
			    ? "The remote stop could not be confirmed. Reconnect structured chat before retrying the terminal."
			    : "Failed to stop the idle structured runtime before starting terminal fallback.";
		}
		return false;
	}
	return true;
}

std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	ChatSession effective_chat = chat;
	effective_chat.native_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);

	return ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);
}

void RepairCliTerminalIdentityForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const ProviderProfile& provider)
{
	const std::string resume_id = ResolveProviderInteractiveResumeId(app, chat, provider);

	if (terminal.frontend_chat_id != chat.id)
	{
		terminal.frontend_chat_id = chat.id;
	}

	if (CliTerminalAttachedChatId(terminal) != chat.id)
	{
		terminal.attached_chat_id = chat.id;
	}

	const std::string expected_terminal_id = CliTerminalIdForChat(chat.id);
	if (terminal.terminal_id != expected_terminal_id)
	{
		terminal.terminal_id = expected_terminal_id;
	}

	if (CliTerminalAttachedSessionId(terminal) != resume_id)
	{
		terminal.attached_session_id = resume_id;
	}
}

CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const std::string resume_id = ResolveProviderInteractiveResumeId(app, chat, provider);
	const bool can_launch_terminal = ProviderSupportsInteractiveTerminal(provider);

	if (CliTerminalState* existing = FindCliTerminalForChat(app, chat.id))
	{
		RepairCliTerminalIdentityForChat(app, *existing, chat, provider);

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

void MarkSelectedCliTerminalForLaunch(AppState& app)
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
