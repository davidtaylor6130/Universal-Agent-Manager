#pragma once

#include "app/chat_domain_service.h"
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

	inline bool CliTerminalIsBetterExactTerminalIdMatch(const CliTerminalState& candidate, const CliTerminalState& best)
	{
		if (candidate.running != best.running)
		{
			return candidate.running;
		}

		if (candidate.ui_attached != best.ui_attached)
		{
			return candidate.ui_attached;
		}

		if (candidate.last_activity_time_s != best.last_activity_time_s)
		{
			return candidate.last_activity_time_s > best.last_activity_time_s;
		}

		if (candidate.last_output_time_s != best.last_output_time_s)
		{
			return candidate.last_output_time_s > best.last_output_time_s;
		}

		return true;
	}

	inline bool CliTerminalIsBetterChatMatch(const CliTerminalState& candidate, const CliTerminalState& best)
	{
		if (candidate.running != best.running)
		{
			return candidate.running;
		}

		if (candidate.ui_attached != best.ui_attached)
		{
			return candidate.ui_attached;
		}

		if (candidate.last_activity_time_s != best.last_activity_time_s)
		{
			return candidate.last_activity_time_s > best.last_activity_time_s;
		}

		if (candidate.last_output_time_s != best.last_output_time_s)
		{
			return candidate.last_output_time_s > best.last_output_time_s;
		}

		return true;
	}

	inline bool ChatIsBetterCliTerminalMatch(const ChatSession& candidate, const ChatSession& best)
	{
		if (candidate.last_opened_at != best.last_opened_at)
		{
			return candidate.last_opened_at > best.last_opened_at;
		}

		if (candidate.updated_at != best.updated_at)
		{
			return candidate.updated_at > best.updated_at;
		}

		if (candidate.created_at != best.created_at)
		{
			return candidate.created_at > best.created_at;
		}

		return candidate.id > best.id;
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

	inline std::string ResolvedNativeSessionIdForChat(const AppState& app, const ChatSession& chat)
	{
		const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
		if (resolved == app.resolved_native_sessions_by_chat_id.end())
		{
			return "";
		}

		return uam::strings::Trim(resolved->second);
	}

	inline int CliTerminalMatchPriority(const AppState& app, const CliTerminalState& terminal, const ChatSession& chat);

	inline bool CliTerminalMatchesChat(const AppState& app, const CliTerminalState& terminal, const ChatSession& chat)
	{
		return CliTerminalMatchPriority(app, terminal, chat) >= 0;
	}

	inline int CliTerminalMatchPriority(const AppState& app, const CliTerminalState& terminal, const ChatSession& chat)
	{
		if (CliTerminalMatchesChatId(terminal, chat.id))
		{
			return 0;
		}

		const std::string native_session_id = NativeSessionLinkService().RealNativeSessionId(chat);
		if (!native_session_id.empty() && CliTerminalMatchesChatId(terminal, native_session_id))
		{
			return 1;
		}

		const std::string resolved_native_session_id = ResolvedNativeSessionIdForChat(app, chat);
		if (!resolved_native_session_id.empty() && CliTerminalMatchesChatId(terminal, resolved_native_session_id))
		{
			return 2;
		}

		const std::string_view raw_native_session_id = uam::strings::TrimAsciiView(chat.native_session_id);
		return !raw_native_session_id.empty() && raw_native_session_id != native_session_id && CliTerminalMatchesChatId(terminal, raw_native_session_id) ? 3 : -1;
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
		auto best = app.chats.end();
		int best_priority = 4;
		for (auto it = app.chats.begin(); it != app.chats.end(); ++it)
		{
			const int priority = CliTerminalMatchPriority(app, terminal, *it);
			if (priority >= 0 && (priority < best_priority || (priority == best_priority && (best == app.chats.end() || ChatIsBetterCliTerminalMatch(*it, *best)))))
			{
				best = it;
				best_priority = priority;
			}
		}

		return best;
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

	inline const CliTerminalState* FindCliTerminalForChat(const AppState& app, const ChatSession& chat);
	inline CliTerminalState* FindCliTerminalForChat(AppState& app, const ChatSession& chat);

	inline CliTerminalState* FindCliTerminalForRoutingKey(AppState& app, std::string_view chat_id, std::string_view terminal_id)
	{
		const std::string target_terminal_id = TrimCliTerminalIdentity(terminal_id);
		const std::string target_chat_id = TrimCliTerminalIdentity(chat_id);
		if (!target_terminal_id.empty())
		{
			CliTerminalState* best_terminal = nullptr;
			CliTerminalState* best_chat_terminal = nullptr;
			for (auto& terminal : app.cli_terminals)
			{
				if (terminal == nullptr || !CliTerminalMatchesTerminalId(*terminal, target_terminal_id))
				{
					continue;
				}

				if (best_terminal == nullptr || CliTerminalIsBetterExactTerminalIdMatch(*terminal, *best_terminal))
				{
					best_terminal = terminal.get();
				}

				if (!target_chat_id.empty() && CliTerminalMatchesChatId(*terminal, target_chat_id) &&
				    (best_chat_terminal == nullptr || CliTerminalIsBetterExactTerminalIdMatch(*terminal, *best_chat_terminal)))
				{
					best_chat_terminal = terminal.get();
				}
			}

			if (best_chat_terminal != nullptr)
			{
				return best_chat_terminal;
			}

			if (best_terminal != nullptr)
			{
				return best_terminal;
			}
		}

		if (target_chat_id.empty())
		{
			return nullptr;
		}

		if (ChatSession* target_chat = ChatDomainService().FindChatById(app, target_chat_id); target_chat != nullptr)
		{
			if (CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
			{
				return terminal;
			}
		}

		CliTerminalState target_terminal;
		target_terminal.attached_session_id = target_chat_id;
		if (ChatSession* target_chat = FindChatForCliTerminal(app, target_terminal); target_chat != nullptr)
		{
			if (CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
			{
				return terminal;
			}
		}

		ChatSession synthetic_chat;
		synthetic_chat.id = target_chat_id;
		synthetic_chat.native_session_id = target_chat_id;
		return FindCliTerminalForChat(app, synthetic_chat);
	}

	template <typename AppStateType> inline auto FindCliTerminalIteratorForChat(AppStateType& app, const ChatSession& chat)
	{
		auto best = app.cli_terminals.end();
		int best_priority = 4;
		for (auto it = app.cli_terminals.begin(); it != app.cli_terminals.end(); ++it)
		{
			if (it == app.cli_terminals.end() || *it == nullptr)
			{
				continue;
			}

			const int priority = CliTerminalMatchPriority(app, **it, chat);
			if (priority >= 0 && (priority < best_priority || (priority == best_priority && (best == app.cli_terminals.end() || CliTerminalIsBetterChatMatch(**it, **best)))))
			{
				best = it;
				best_priority = priority;
			}
		}

		return best;
	}

	inline int FindCliTerminalIndexForChat(const AppState& app, const ChatSession& chat)
	{
		const auto found = FindCliTerminalIteratorForChat(app, chat);
		return found == app.cli_terminals.end() ? -1 : static_cast<int>(std::distance(app.cli_terminals.begin(), found));
	}

	inline const CliTerminalState* FindCliTerminalForChat(const AppState& app, const ChatSession& chat)
	{
		const auto found = FindCliTerminalIteratorForChat(app, chat);
		if (found == app.cli_terminals.end())
		{
			return nullptr;
		}

		return found->get();
	}

	inline CliTerminalState* FindCliTerminalForChat(AppState& app, const ChatSession& chat)
	{
		const auto found = FindCliTerminalIteratorForChat(app, chat);
		if (found == app.cli_terminals.end())
		{
			return nullptr;
		}

		return found->get();
	}

} // namespace uam
