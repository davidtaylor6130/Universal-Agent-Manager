#pragma once

#include "app/native_session_link_service.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

namespace uam
{

	inline std::string_view TrimCliTerminalIdentityView(std::string_view value)
	{
		return uam::strings::TrimAsciiView(value);
	}

	inline std::string TrimCliTerminalIdentity(std::string_view value)
	{
		return std::string(TrimCliTerminalIdentityView(value));
	}

	inline std::string CliTerminalAttachedChatId(const CliTerminalState& terminal)
	{
		return TrimCliTerminalIdentity(terminal.attached_chat_id);
	}

	inline std::string CliTerminalAttachedSessionId(const CliTerminalState& terminal)
	{
		return TrimCliTerminalIdentity(terminal.attached_session_id);
	}

	inline bool TrimmedCliTerminalIdMatches(std::string_view candidate_id, std::string_view target_id)
	{
		return TrimCliTerminalIdentityView(candidate_id) == TrimCliTerminalIdentityView(target_id);
	}

	inline bool CliTerminalMatchesNonEmptyIdentity(std::string_view candidate_id, std::string_view target_id)
	{
		const std::string_view target_identity = TrimCliTerminalIdentityView(target_id);
		return !target_identity.empty() && TrimmedCliTerminalIdMatches(candidate_id, target_identity);
	}

	inline bool CliTerminalMatchesTerminalId(const CliTerminalState& terminal, std::string_view terminal_id)
	{
		return CliTerminalMatchesNonEmptyIdentity(terminal.terminal_id, terminal_id);
	}

	inline bool CliTerminalMatchesChatId(const CliTerminalState& terminal, std::string_view chat_id)
	{
		const std::string_view target_id = TrimCliTerminalIdentityView(chat_id);
		if (target_id.empty())
		{
			return false;
		}

		return CliTerminalMatchesNonEmptyIdentity(terminal.frontend_chat_id, target_id) || CliTerminalMatchesNonEmptyIdentity(terminal.attached_chat_id, target_id) || CliTerminalMatchesNonEmptyIdentity(terminal.attached_session_id, target_id);
	}

	inline bool CliTerminalMatchesChat(const CliTerminalState& terminal, const ChatSession& chat)
	{
		if (CliTerminalMatchesChatId(terminal, chat.id))
		{
			return true;
		}

		const std::string native_session_id = NativeSessionLinkService().RealNativeSessionId(chat);
		if (!native_session_id.empty() && CliTerminalMatchesChatId(terminal, native_session_id))
		{
			return true;
		}

		const std::string_view raw_native_session_id = uam::strings::TrimAsciiView(chat.native_session_id);
		return !raw_native_session_id.empty() && raw_native_session_id != native_session_id && CliTerminalMatchesChatId(terminal, raw_native_session_id);
	}

	inline std::string CliTerminalPrimaryChatId(const CliTerminalState& terminal)
	{
		const std::string_view frontend_chat_id = TrimCliTerminalIdentityView(terminal.frontend_chat_id);
		if (!frontend_chat_id.empty())
		{
			return std::string(frontend_chat_id);
		}

		return CliTerminalAttachedChatId(terminal);
	}

	inline std::string CliTerminalSyncTargetId(const CliTerminalState& terminal)
	{
		const std::string attached_session_id = CliTerminalAttachedSessionId(terminal);
		if (!attached_session_id.empty())
		{
			return attached_session_id;
		}

		return CliTerminalPrimaryChatId(terminal);
	}

	template <typename AppStateType> inline auto FindChatIteratorForCliTerminal(AppStateType& app, const CliTerminalState& terminal)
	{
		return std::ranges::find_if(app.chats, [&terminal](const ChatSession& chat) { return CliTerminalMatchesChat(terminal, chat); });
	}

	inline int FindChatIndexForCliTerminal(const AppState& app, const CliTerminalState& terminal)
	{
		const auto found = FindChatIteratorForCliTerminal(app, terminal);
		return found == app.chats.end() ? -1 : static_cast<int>(std::distance(app.chats.begin(), found));
	}

	inline const ChatSession* FindChatForCliTerminal(const AppState& app, const CliTerminalState& terminal)
	{
		const auto found = FindChatIteratorForCliTerminal(app, terminal);
		if (found == app.chats.end())
		{
			return nullptr;
		}

		return &*found;
	}

	inline ChatSession* FindChatForCliTerminal(AppState& app, const CliTerminalState& terminal)
	{
		const auto found = FindChatIteratorForCliTerminal(app, terminal);
		if (found == app.chats.end())
		{
			return nullptr;
		}

		return &*found;
	}

} // namespace uam
