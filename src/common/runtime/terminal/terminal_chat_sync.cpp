#include "common/runtime/terminal/terminal_chat_sync.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle_states.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>

namespace uam
{

bool ChatSyncIdsMatch(std::string_view lhs, std::string_view rhs)
{
	return uam::strings::TrimmedEqualsNonEmpty(lhs, rhs);
}

std::string NormalizeChatSyncTargetId(std::string_view chat_id)
{
	return uam::strings::Trim(chat_id);
}

auto FindPendingCallForChat(const AppState& app, std::string_view chat_id) -> decltype(app.pending_calls.end())
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return app.pending_calls.end();
	}

	return std::ranges::find_if(app.pending_calls, [&target_id](const PendingRuntimeCall& call) { return ChatSyncIdsMatch(call.chat_id, target_id); });
}

bool HasPendingCallForChat(const AppState& app, std::string_view chat_id)
{
	return FindPendingCallForChat(app, chat_id) != app.pending_calls.end();
}

bool HasAnyPendingCall(const AppState& app)
{
	return !app.pending_calls.empty();
}

const PendingRuntimeCall* FirstPendingCallForChat(const AppState& app, std::string_view chat_id)
{
	const auto found = FindPendingCallForChat(app, chat_id);
	return found == app.pending_calls.end() ? nullptr : &*found;
}

bool ChatHasActiveAcpSession(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	return std::ranges::any_of(app.acp_sessions, [&target_id](const auto& session) { return session != nullptr && ChatSyncIdsMatch(session->chat_id, target_id) && session->running && AcpSessionHasActiveTurn(*session); });
}

bool CliTerminalHasActiveTurn(const CliTerminalState& terminal)
{
	return uam::CliTerminalLifecycleStateIsProcessing(terminal.lifecycle_state) || terminal.turn_state == uam::CliTerminalTurnState::Busy;
}

bool ChatHasBusyCliTerminal(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	const ChatSession* target_chat = ChatDomainService().FindChatById(app, target_id);
	if (target_chat == nullptr)
	{
		uam::CliTerminalState target_terminal;
		target_terminal.attached_session_id = target_id;
		target_chat = FindChatForCliTerminal(app, target_terminal);
	}

	if (target_chat != nullptr)
	{
		if (const CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
		{
			return terminal->running && CliTerminalHasActiveTurn(*terminal);
		}
	}

	return std::ranges::any_of(app.cli_terminals, [&target_id](const auto& terminal) { return terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, target_id) && CliTerminalHasActiveTurn(*terminal); });
}

bool ChatHasRunningRuntime(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	if (HasPendingCallForChat(app, target_id))
	{
		return true;
	}

	if (ChatHasActiveAcpSession(app, target_id))
	{
		return true;
	}

	return ChatHasBusyCliTerminal(app, target_id);
}

bool ChatHasActiveCliTerminal(const AppState& app, std::string_view chat_id)
{
	const std::string target_id = NormalizeChatSyncTargetId(chat_id);
	if (target_id.empty())
	{
		return false;
	}

	const ChatSession* target_chat = ChatDomainService().FindChatById(app, target_id);
	if (target_chat == nullptr)
	{
		uam::CliTerminalState target_terminal;
		target_terminal.attached_session_id = target_id;
		target_chat = FindChatForCliTerminal(app, target_terminal);
	}

	if (target_chat != nullptr)
	{
		if (const CliTerminalState* terminal = FindCliTerminalForChat(app, *target_chat); terminal != nullptr)
		{
			return terminal->running;
		}
	}

	return std::ranges::any_of(app.cli_terminals, [&target_id](const auto& terminal) { return terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, target_id); });
}

bool HasAnyActiveCliTerminal(const AppState& app)
{
	return std::ranges::any_of(app.cli_terminals, [](const auto& terminal) { return terminal != nullptr && terminal->running; });
}

void MarkChatUnseen(AppState& app, std::string_view chat_id)
{
	const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
	if (normalized_chat_id.empty())
	{
		return;
	}

	if (ChatDomainService().SelectedChatId(app) == normalized_chat_id)
	{
		return;
	}

	app.chats_with_unseen_updates.emplace(normalized_chat_id);
}

void MarkSelectedChatSeen(AppState& app)
{
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	if (!selected_chat_id.empty())
	{
		app.chats_with_unseen_updates.erase(selected_chat_id);
	}
}

bool ChatExists(const AppState& app, std::string_view chat_id)
{
	const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
	return !normalized_chat_id.empty() && ChatDomainService().FindChatById(app, normalized_chat_id) != nullptr;
}

bool NativeChatMatchesPreferredSyncId(const ChatSession& chat, std::string_view preferred_chat_id)
{
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	if (preferred_id.empty())
	{
		return false;
	}

	const NativeSessionLinkService native_session_links;
	return ChatSyncIdsMatch(chat.id, preferred_id) || ChatSyncIdsMatch(native_session_links.RealNativeSessionId(chat), preferred_id);
}

void RemoveMissingChatIds(const AppState& app, std::unordered_set<std::string>& chat_ids)
{
	std::unordered_set<std::string> normalized_existing_ids;
	normalized_existing_ids.reserve(chat_ids.size());
	for (const std::string& chat_id : chat_ids)
	{
		const std::string normalized_chat_id = NormalizeChatSyncTargetId(chat_id);
		if (ChatExists(app, normalized_chat_id))
		{
			normalized_existing_ids.emplace(normalized_chat_id);
		}
	}

	chat_ids = std::move(normalized_existing_ids);
}

void FinalizeChatSyncSelection(uam::AppState& app, std::string_view selected_before, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before_id = NormalizeChatSyncTargetId(selected_before);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	const std::string previous_selected = uam::strings::NonEmptyOrFallback(selected_before_id, preferred_id);

	if (preserve_selection && ChatExists(app, selected_before_id))
	{
		ChatDomainService().SelectChatById(app, selected_before_id);
	}
	else if (ChatExists(app, preferred_id))
	{
		ChatDomainService().SelectChatById(app, preferred_id);
	}
	else if (ChatExists(app, previous_selected))
	{
		ChatDomainService().SelectChatById(app, previous_selected);
	}
	else if (!app.chats.empty())
	{
		ChatDomainService().SelectChatById(app, app.chats.front().id);
	}
	else
	{
		ChatDomainService().SetSelectedChatIndexOrNearest(app, -1);
	}

	RemoveMissingChatIds(app, app.chats_with_unseen_updates);
	RemoveMissingChatIds(app, app.collapsed_branch_chat_ids);
	RemoveMissingChatIds(app, app.filtered_chat_ids);

	const std::string selected_now_id = ChatDomainService().SelectedChatId(app);

	if (selected_now_id != selected_before_id)
	{
		app.composer_text.clear();
	}

	MarkSelectedChatSeen(app);
}

bool SyncChatsFromLoadedNative(AppState& app, std::vector<ChatSession> native_chats, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before = ChatDomainService().SelectedChatId(app);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);
	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats);

	// Only save the specifically requested chat (e.g. the one just discovered)
	for (ChatSession& chat : native_chats)
	{
		if (NativeChatMatchesPreferredSyncId(chat, preferred_id))
		{
			ChatRepository::SaveChat(app.data_root, chat);
			break;
		}
	}

	app.chats = ChatRepository::LoadLocalChats(app.data_root);
	app.chats = ChatDomainService().DeduplicateChatsById(std::move(app.chats));
	ChatBranching::Normalize(app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_id, preserve_selection);
	return true;
}

bool SyncChatsFromNative(AppState& app, std::string_view preferred_chat_id, bool preserve_selection)
{
	const std::string selected_before = ChatDomainService().SelectedChatId(app);
	const std::string preferred_id = NormalizeChatSyncTargetId(preferred_chat_id);

	// Import from native to local before reloading the sidebar.
	// We only import the target chat to avoid re-importing chats the user manually deleted.
	ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, preferred_id);

	ChatHistorySyncService().LoadSidebarChats(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_id, preserve_selection);
	return true;
}

} // namespace uam
