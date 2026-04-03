#ifndef UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_REMOVE_CHAT_H
#define UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_REMOVE_CHAT_H

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal_common.h"
#include "common/ui/chat_actions/chat_action_editing.h"

#include <algorithm>
#include <filesystem>
#include <thread>

/// <summary>
/// Chat deletion flow, including local/native history cleanup and selection repair.
/// </summary>
inline bool RemoveChatById(AppState& app, const std::string& chat_id)
{
	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);

	if (chat_index < 0)
	{
		app.status_line = "Chat no longer exists.";
		return false;
	}

	const ChatSession chat = app.chats[chat_index];

	if (HasPendingCallForChat(app, chat.id))
	{
		app.status_line = "Cannot delete a chat while Gemini is still running for it.";
		return false;
	}

	// Stop any attached CLI first so history file deletion does not race an
	// active process. Windows uses a non-blocking fast-stop path in
	// StopAndEraseCliTerminalForChat; macOS retains the existing behavior.
	StopAndEraseCliTerminalForChat(app, chat.id);

	std::error_code local_delete_ec;
	bool local_delete_async_scheduled = false;

	if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
	{
		const std::filesystem::path local_chat_path = AppPaths::ChatPath(app.data_root, chat.id);
		auto delete_local_chat_path = [local_chat_path]()
		{
			std::error_code async_local_delete_ec;
			std::filesystem::remove_all(local_chat_path, async_local_delete_ec);
		};

		std::thread(delete_local_chat_path).detach();
		local_delete_async_scheduled = true;
	}
	else
	{
		std::filesystem::remove_all(AppPaths::ChatPath(app.data_root, chat.id), local_delete_ec);
	}

	const std::string native_session_id = chat.uses_native_session ? chat.native_session_id : "";
	std::error_code native_delete_ec;
	bool native_delete_attempted = false;
	bool native_delete_async_scheduled = false;

	if (ProviderResolutionService().ActiveProviderUsesNativeOverlayHistory(app) && !native_session_id.empty())
	{
		ChatHistorySyncService().RefreshNativeSessionDirectory(app);
		native_delete_attempted = true;

		if (PlatformServicesFactory::Instance().ui_traits.UseWindowsLayoutAdjustments())
		{
			const std::filesystem::path chats_dir_snapshot = app.native_history_chats_dir;
			const std::string native_session_id_snapshot = native_session_id;
			auto delete_native_session_file = [chats_dir_snapshot, native_session_id_snapshot]()
			{
				const auto native_file = ChatHistorySyncService().FindNativeSessionFilePath(chats_dir_snapshot, native_session_id_snapshot);

				if (!native_file.has_value())
				{
					return;
				}

				std::error_code async_delete_ec;
				std::filesystem::remove(native_file.value(), async_delete_ec);
			};

			std::thread(delete_native_session_file).detach();
			native_delete_async_scheduled = true;
		}
		else if (const auto native_file = ChatHistorySyncService().FindNativeSessionFilePath(app.native_history_chats_dir, native_session_id); native_file.has_value())
		{
			std::filesystem::remove(native_file.value(), native_delete_ec);
		}

	}

	ChatBranching::ReparentChildrenAfterDelete(app.chats, chat.id);

	for (const ChatSession& existing_chat : app.chats)
	{
		if (existing_chat.id == chat.id)
		{
			continue;
		}

		ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, existing_chat), app.data_root, existing_chat);
	}

	app.chats.erase(app.chats.begin() + chat_index);
	ChatBranching::Normalize(app.chats);

	if (app.chats.empty())
	{
		app.selected_chat_index = -1;
	}
	else if (app.selected_chat_index > chat_index)
	{
		--app.selected_chat_index;
	}
	else if (app.selected_chat_index == chat_index)
	{
		app.selected_chat_index = std::min(chat_index, static_cast<int>(app.chats.size()) - 1);
	}

	app.composer_text.clear();

	app.pending_calls.erase(std::remove_if(app.pending_calls.begin(), app.pending_calls.end(), [&](const PendingRuntimeCall& call) { return call.chat_id == chat.id; }), app.pending_calls.end());
	app.resolved_native_sessions_by_chat_id.erase(chat.id);

	for (auto it = app.resolved_native_sessions_by_chat_id.begin(); it != app.resolved_native_sessions_by_chat_id.end();)
	{
		if (it->second == chat.id)
		{
			it = app.resolved_native_sessions_by_chat_id.erase(it);
		}
		else
		{
			++it;
		}
	}

	app.chats_with_unseen_updates.erase(chat.id);
	app.collapsed_branch_chat_ids.erase(chat.id);

	if (app.editing_chat_id == chat.id)
	{
		ClearEditMessageState(app);
	}

	if (app.pending_branch_chat_id == chat.id)
	{
		app.pending_branch_chat_id.clear();
		app.pending_branch_message_index = -1;
	}

	if (app.sidebar_chat_options_popup_chat_id == chat.id)
	{
		app.sidebar_chat_options_popup_chat_id.clear();
		app.open_sidebar_chat_options_popup = false;
	}

	ChatDomainService().RefreshRememberedSelection(app);
	PersistenceCoordinator().SaveSettings(app);

	if (local_delete_ec)
	{
		app.status_line = "Chat removed from UI, but deleting local history failed.";
	}
	else if (local_delete_async_scheduled && native_delete_async_scheduled)
	{
		app.status_line = "Chat deleted. Local and native history cleanup are running in background.";
	}
	else if (local_delete_async_scheduled)
	{
		app.status_line = "Chat deleted. Local history cleanup is running in background.";
	}
	else if (native_delete_async_scheduled)
	{
		app.status_line = "Chat deleted. Native Gemini history cleanup is running in background.";
	}
	else if (native_delete_attempted && native_delete_ec)
	{
		app.status_line = "Chat removed from UI, but deleting native Gemini history failed.";
	}
	else
	{
		app.status_line = "Chat deleted.";
	}

	return true;
}

#endif // UAM_COMMON_UI_CHAT_ACTIONS_CHAT_ACTION_REMOVE_CHAT_H
