#pragma once

#include <algorithm>

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_repository.h"
#include "common/state/app_state.h"

inline bool HasPendingCallForChat(const uam::AppState& app, const std::string& chat_id)
{
	for (const PendingRuntimeCall& call : app.pending_calls)
	{
		if (call.chat_id == chat_id)
		{
			return true;
		}
	}

	return false;
}

inline bool HasAnyPendingCall(const uam::AppState& app)
{
	return !app.pending_calls.empty();
}

inline const PendingRuntimeCall* FirstPendingCallForChat(const uam::AppState& app, const std::string& chat_id)
{
	for (const PendingRuntimeCall& call : app.pending_calls)
	{
		if (call.chat_id == chat_id)
		{
			return &call;
		}
	}

	return nullptr;
}

inline bool ChatHasRunningGemini(const uam::AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return false;
	}

	if (HasPendingCallForChat(app, chat_id))
	{
		return true;
	}

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->running && terminal->attached_chat_id == chat_id && terminal->turn_state == uam::CliTerminalTurnState::Busy)
		{
			return true;
		}
	}

	return false;
}

inline bool ChatHasActiveCliTerminal(const uam::AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return false;
	}

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->running && terminal->attached_chat_id == chat_id)
		{
			return true;
		}
	}

	return false;
}

inline void MarkChatUnseen(uam::AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return;
	}

	const ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected != nullptr && selected->id == chat_id)
	{
		return;
	}

	app.chats_with_unseen_updates.insert(chat_id);
}

inline void MarkSelectedChatSeen(uam::AppState& app)
{
	const ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected != nullptr)
	{
		app.chats_with_unseen_updates.erase(selected->id);
	}
}

inline void FinalizeChatSyncSelection(uam::AppState& app, const std::string& selected_before, const std::string& preferred_chat_id, const bool preserve_selection)
{
	const std::string previous_selected = !selected_before.empty() ? selected_before : preferred_chat_id;

	if (preserve_selection && !selected_before.empty() && ChatDomainService().FindChatIndexById(app, selected_before) >= 0)
	{
		ChatDomainService().SelectChatById(app, selected_before);
	}
	else if (!preferred_chat_id.empty() && ChatDomainService().FindChatIndexById(app, preferred_chat_id) >= 0)
	{
		ChatDomainService().SelectChatById(app, preferred_chat_id);
	}
	else if (!previous_selected.empty() && ChatDomainService().FindChatIndexById(app, previous_selected) >= 0)
	{
		ChatDomainService().SelectChatById(app, previous_selected);
	}
	else if (!app.chats.empty())
	{
		app.selected_chat_index = 0;
	}
	else
	{
		app.selected_chat_index = -1;
	}

	for (auto it = app.chats_with_unseen_updates.begin(); it != app.chats_with_unseen_updates.end();)
	{
		if (ChatDomainService().FindChatIndexById(app, *it) < 0)
		{
			it = app.chats_with_unseen_updates.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto it = app.collapsed_branch_chat_ids.begin(); it != app.collapsed_branch_chat_ids.end();)
	{
		if (ChatDomainService().FindChatIndexById(app, *it) < 0)
		{
			it = app.collapsed_branch_chat_ids.erase(it);
		}
		else
		{
			++it;
		}
	}

	const ChatSession* selected_now = ChatDomainService().SelectedChat(app);
	const std::string selected_now_id = (selected_now != nullptr) ? selected_now->id : "";

	if (selected_now_id != selected_before)
	{
		app.composer_text.clear();
	}

	MarkSelectedChatSeen(app);
}

inline void SyncChatsFromLoadedNative(uam::AppState& app, std::vector<ChatSession> native_chats, const std::string& preferred_chat_id, const bool preserve_selection = false)
{
	const std::string selected_before = (ChatDomainService().SelectedChat(app) != nullptr) ? ChatDomainService().SelectedChat(app)->id : "";
	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats);
	
	// Only save the specifically requested chat (e.g. the one just discovered)
	for (ChatSession& chat : native_chats)
	{
		if (chat.id == preferred_chat_id || chat.native_session_id == preferred_chat_id)
		{
			ChatRepository::SaveChat(app.data_root, chat);
			break;
		}
	}

	app.chats = ChatRepository::LoadLocalChats(app.data_root);
	app.chats = ChatDomainService().DeduplicateChatsById(std::move(app.chats));
	ChatBranching::Normalize(app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(app);
	ProviderProfileMigrationService().MigrateChatProviderBindingsToFixedModes(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_chat_id, preserve_selection);
}

inline void SyncChatsFromNative(uam::AppState& app, const std::string& preferred_chat_id, const bool preserve_selection = false)
{
	const std::string selected_before = (ChatDomainService().SelectedChat(app) != nullptr) ? ChatDomainService().SelectedChat(app)->id : "";
	
	// Import from native to local before reloading the sidebar.
	// We only import the target chat to avoid re-importing chats the user manually deleted.
	ChatHistorySyncService().ImportAllNativeChatsToLocal(app, false, preferred_chat_id);
	
	ChatHistorySyncService().LoadSidebarChats(app);
	ProviderProfileMigrationService().MigrateChatProviderBindingsToFixedModes(app);
	FinalizeChatSyncSelection(app, selected_before, preferred_chat_id, preserve_selection);
}
