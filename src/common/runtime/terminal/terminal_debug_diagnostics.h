#pragma once

#include "common/runtime/app_time.h"
#include "common/state/app_state.h"

#include <iostream>
#include <sstream>
#include <string>

namespace uam
{

inline std::string CliTurnStateLabel(const CliTerminalState& terminal)
{
	return terminal.turn_state == CliTerminalTurnState::Busy ? "busy" : "idle";
}

inline std::string CliSelectedChatId(const AppState& app)
{
	if (app.selected_chat_index < 0 || app.selected_chat_index >= static_cast<int>(app.chats.size()))
	{
		return "";
	}

	return app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
}

inline const ChatSession* FindChatForCliDiagnostics(const AppState& app, const CliTerminalState& terminal)
{
	for (const ChatSession& chat : app.chats)
	{
		if (chat.id == terminal.frontend_chat_id || chat.id == terminal.attached_chat_id || (!terminal.attached_session_id.empty() && chat.native_session_id == terminal.attached_session_id))
		{
			return &chat;
		}
	}

	return nullptr;
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

inline void LogCliDiagnosticEvent(const AppState&            app,
                                  const std::string&        event_name,
                                  const std::string&        reason,
                                  const CliTerminalState*   terminal = nullptr,
                                  const std::string&        note = "",
                                  const long long           bytes = -1)
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
		    << " ui_attached=" << (terminal->ui_attached ? "true" : "false")
		    << " running=" << (terminal->running ? "true" : "false")
		    << " turn_state=" << CliTurnStateLabel(*terminal);
	}

	if (bytes >= 0)
	{
		out << " bytes=" << bytes;
	}

	if (!note.empty())
	{
		out << " note=\"" << note << "\"";
	}

	out << " t=" << GetAppTimeSeconds();
	std::cerr << out.str() << std::endl;
}

} // namespace uam
