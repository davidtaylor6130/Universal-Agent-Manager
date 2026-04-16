#pragma once

#include "common/state/app_state.h"

#include <cstddef>
#include <string>
#include <string_view>

inline bool CliTerminalMatchesChatId(const uam::CliTerminalState& terminal, const std::string_view chat_id)
{
	if (chat_id.empty())
	{
		return false;
	}

	return std::string_view(terminal.frontend_chat_id) == chat_id ||
	       std::string_view(terminal.attached_chat_id) == chat_id ||
	       std::string_view(terminal.attached_session_id) == chat_id;
}

inline bool CliTerminalMatchesChat(const uam::CliTerminalState& terminal, const ChatSession& chat)
{
	if (CliTerminalMatchesChatId(terminal, chat.id))
	{
		return true;
	}

	return !chat.native_session_id.empty() && CliTerminalMatchesChatId(terminal, chat.native_session_id);
}

inline std::string CliTerminalPrimaryChatId(const uam::CliTerminalState& terminal)
{
	if (!terminal.frontend_chat_id.empty())
	{
		return terminal.frontend_chat_id;
	}

	return terminal.attached_chat_id;
}

inline std::string CliTerminalSyncTargetId(const uam::CliTerminalState& terminal)
{
	if (!terminal.attached_session_id.empty())
	{
		return terminal.attached_session_id;
	}

	const std::string primary_chat_id = CliTerminalPrimaryChatId(terminal);
	if (!primary_chat_id.empty())
	{
		return primary_chat_id;
	}

	return terminal.attached_chat_id;
}

inline int FindChatIndexForCliTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal)
{
	for (std::size_t i = 0; i < app.chats.size(); ++i)
	{
		if (CliTerminalMatchesChat(terminal, app.chats[i]))
		{
			return static_cast<int>(i);
		}
	}

	return -1;
}

inline const ChatSession* FindChatForCliTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal)
{
	const int index = FindChatIndexForCliTerminal(app, terminal);
	if (index < 0)
	{
		return nullptr;
	}

	return &app.chats[static_cast<std::size_t>(index)];
}
