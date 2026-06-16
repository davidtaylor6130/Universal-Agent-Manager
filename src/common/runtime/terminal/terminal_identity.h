#pragma once

#include "common/state/app_state.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

namespace uam
{

std::string_view TrimCliTerminalIdentityView(std::string_view value);
std::string TrimCliTerminalIdentity(std::string_view value);
std::string CliTerminalAttachedChatId(const CliTerminalState& terminal);
std::string CliTerminalAttachedSessionId(const CliTerminalState& terminal);
bool TrimmedCliTerminalIdMatches(std::string_view candidate_id, std::string_view target_id);
bool CliTerminalMatchesNonEmptyIdentity(std::string_view candidate_id, std::string_view target_id);
bool CliTerminalMatchesTerminalId(const CliTerminalState& terminal, std::string_view terminal_id);
bool CliTerminalIsBetterExactTerminalIdMatch(const CliTerminalState& candidate, const CliTerminalState& best);
bool CliTerminalIsBetterChatMatch(const CliTerminalState& candidate, const CliTerminalState& best);
bool ChatIsBetterCliTerminalMatch(const ChatSession& candidate, const ChatSession& best);
bool CliTerminalMatchesChatId(const CliTerminalState& terminal, std::string_view chat_id);
bool CliTerminalMatchesChat(const CliTerminalState& terminal, const ChatSession& chat);
std::string ResolvedNativeSessionIdForChat(const AppState& app, const ChatSession& chat);
bool CliTerminalMatchesChat(const AppState& app, const CliTerminalState& terminal, const ChatSession& chat);
int CliTerminalMatchPriority(const AppState& app, const CliTerminalState& terminal, const ChatSession& chat);
std::string CliTerminalPrimaryChatId(const CliTerminalState& terminal);
std::string CliTerminalSyncTargetId(const CliTerminalState& terminal);
int FindChatIndexForCliTerminal(const AppState& app, const CliTerminalState& terminal);
const ChatSession* FindChatForCliTerminal(const AppState& app, const CliTerminalState& terminal);
ChatSession* FindChatForCliTerminal(AppState& app, const CliTerminalState& terminal);
const CliTerminalState* FindCliTerminalForChat(const AppState& app, const ChatSession& chat);
CliTerminalState* FindCliTerminalForChat(AppState& app, const ChatSession& chat);
CliTerminalState* FindCliTerminalForRoutingKey(AppState& app, std::string_view chat_id, std::string_view terminal_id);
int FindCliTerminalIndexForChat(const AppState& app, const ChatSession& chat);

template <typename AppStateType>
inline auto FindChatIteratorForCliTerminal(AppStateType& app, const CliTerminalState& terminal)
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

template <typename AppStateType>
inline auto FindCliTerminalIteratorForChat(AppStateType& app, const ChatSession& chat)
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

} // namespace uam
