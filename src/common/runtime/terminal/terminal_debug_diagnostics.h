#pragma once

#include "app/chat_domain_service.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace uam
{

inline constexpr std::size_t kCliDiagnosticFieldMaxBytes = 4096;

inline const char* CliTurnStateLabel(const CliTerminalState& terminal)
{
	return terminal.turn_state == CliTerminalTurnState::Busy ? "busy" : "idle";
}

inline const char* CliLifecycleStateLabel(const CliTerminalState& terminal)
{
	return CliTerminalLifecycleStateLabel(terminal);
}

inline const char* CliBoolLabel(bool value)
{
	return value ? "true" : "false";
}

inline std::string CliSelectedChatId(const AppState& app)
{
	return ChatDomainService().SelectedChatId(app);
}

inline const ChatSession* FindChatForCliDiagnostics(const AppState& app, const CliTerminalState& terminal)
{
	return FindChatForCliTerminal(app, terminal);
}

inline std::string CliProviderIdForDiagnostics(const AppState& app, const CliTerminalState& terminal)
{
	if (const ChatSession* chat = FindChatForCliDiagnostics(app, terminal); chat != nullptr)
	{
		return chat->provider_id;
	}

	return "";
}

inline std::string CliNativeSessionIdForDiagnostics(const AppState& app, const CliTerminalState& terminal)
{
	if (const ChatSession* chat = FindChatForCliDiagnostics(app, terminal); chat != nullptr)
	{
		return chat->native_session_id;
	}

	return "";
}

inline std::string CliProcessHandleLabel(const CliTerminalState& terminal)
{
#if defined(_WIN32)
	if (terminal.process_info.dwProcessId != 0)
	{
		return std::to_string(static_cast<unsigned long long>(terminal.process_info.dwProcessId));
	}

	return "0";
#elif defined(__APPLE__)
	return std::to_string(static_cast<long long>(terminal.child_pid));
#else
	return "";
#endif
}

inline std::string CliDiagnosticQuotedField(std::string_view value)
{
	std::string quoted;
	quoted.reserve(std::min(value.size(), kCliDiagnosticFieldMaxBytes) + 5);
	quoted.push_back('"');

	std::size_t emitted = 0;
	bool truncated = false;
	for (const char ch : value)
	{
		const bool escaped = ch == '"' || ch == '\\';
		const std::size_t required = escaped ? 2 : 1;
		if (emitted + required > kCliDiagnosticFieldMaxBytes)
		{
			truncated = true;
			break;
		}

		if (escaped)
		{
			quoted.push_back('\\');
		}

		if (ch == '\n' || ch == '\r' || ch == '\t')
		{
			quoted.push_back(' ');
		}
		else
		{
			quoted.push_back(ch);
		}
		emitted += required;
	}

	if (truncated)
	{
		quoted.append("...");
	}

	quoted.push_back('"');
	return quoted;
}

inline void LogCliDiagnosticEvent(const AppState&            app,
                                  std::string_view          event_name,
                                  std::string_view          reason,
                                  const CliTerminalState*   terminal = nullptr,
                                  std::string_view          note = "",
                                  long long                 bytes = -1)
{
	std::ostringstream out;
	out << "[cli-diag]"
	    << " event=" << event_name
	    << " reason=" << reason
	    << " selected_chat_id=" << CliSelectedChatId(app);

	if (terminal != nullptr)
	{
		out << " terminal_id=" << terminal->terminal_id
		    << " frontend_chat_id=" << terminal->frontend_chat_id
		    << " attached_chat_id=" << terminal->attached_chat_id
		    << " attached_session_id=" << terminal->attached_session_id
		    << " provider_id=" << CliProviderIdForDiagnostics(app, *terminal)
		    << " native_session_id=" << CliNativeSessionIdForDiagnostics(app, *terminal)
		    << " process_id=" << CliProcessHandleLabel(*terminal)
		    << " ui_attached=" << CliBoolLabel(terminal->ui_attached)
		    << " running=" << CliBoolLabel(terminal->running)
		    << " turn_state=" << CliTurnStateLabel(*terminal)
		    << " lifecycle_state=" << CliLifecycleStateLabel(*terminal);
	}

	if (bytes >= 0)
	{
		out << " bytes=" << bytes;
	}

	if (!note.empty())
	{
		out << " note=" << CliDiagnosticQuotedField(note);
	}

	out << " t=" << GetAppTimeSeconds();
	std::cerr << out.str() << '\n';
}

} // namespace uam
