#include "chat_lifecycle_service.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/paths/app_paths.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal_common.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <utility>

using uam::AppState;

namespace
{
	void ResetPendingRuntimeCall(PendingRuntimeCall& call)
	{
		if (call.worker != nullptr)
		{
			call.worker->request_stop();
			call.worker.reset();
		}

		call.state.reset();
	}
} // namespace

bool RemoveChatById(AppState& app, const std::string& chat_id)
{
	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);

	if (chat_index < 0)
	{
		app.status_line = "Chat no longer exists.";
		return false;
	}

	const ChatSession chat = app.chats[chat_index];

	if (ChatHasRunningGemini(app, chat.id))
	{
		app.status_line = "Cannot delete a chat while Gemini is still running for it.";
		return false;
	}

	StopAcpSession(app, chat.id);
	StopAndEraseCliTerminalForChat(app, chat.id, false);

	const ProviderProfile& chat_provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	std::vector<ChatSession> next_chats = app.chats;
	ChatBranching::ReparentChildrenAfterDelete(next_chats, chat.id);
	next_chats.erase(std::remove_if(next_chats.begin(), next_chats.end(), [&](const ChatSession& existing_chat) { return existing_chat.id == chat.id; }), next_chats.end());

	for (const ChatSession& existing_chat : next_chats)
	{
		if (!ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, existing_chat), app.data_root, existing_chat))
		{
			for (const ChatSession& original_chat : app.chats)
			{
				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, original_chat), app.data_root, original_chat);
			}

			app.status_line = "Failed to persist chat reparenting before delete.";
			return false;
		}
	}

	app.chats = std::move(next_chats);
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

	app.pending_calls.erase(std::remove_if(app.pending_calls.begin(), app.pending_calls.end(),
	                                       [&](PendingRuntimeCall& call)
	                                       {
		                                       if (call.chat_id != chat.id)
		                                       {
			                                       return false;
		                                       }

		                                       ResetPendingRuntimeCall(call);
		                                       return true;
	                                       }),
	                        app.pending_calls.end());
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
	app.filtered_chat_ids.erase(chat.id);

	if (app.editing_chat_id == chat.id)
	{
		app.editing_chat_id.clear();
		app.editing_message_index = -1;
		app.editing_message_text.clear();
		app.open_edit_message_popup = false;
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
	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);
	std::string status_line = settings_saved ? "Chat deleted." : "Chat deleted, but failed to persist settings.";

	std::error_code local_delete_ec;
	std::filesystem::remove_all(AppPaths::ChatPath(app.data_root, chat.id), local_delete_ec);

	std::error_code uam_json_delete_ec;
	std::filesystem::remove(AppPaths::UamChatFilePath(app.data_root, chat.id), uam_json_delete_ec);

	const std::string native_session_id = chat.native_session_id;
	std::error_code native_delete_ec;
	bool native_delete_attempted = false;

	if (ProviderRuntime::UsesNativeOverlayHistory(chat_provider) && !native_session_id.empty())
	{
		native_delete_attempted = true;
		ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, &native_delete_ec);
	}

	if (local_delete_ec)
	{
		status_line = "Chat removed from UI, but deleting local history failed.";
	}
	else if (uam_json_delete_ec)
	{
		status_line = "Chat removed from UI, but deleting chat metadata failed.";
	}
	else if (native_delete_attempted && native_delete_ec)
	{
		status_line = "Chat removed from UI, but deleting native Gemini history failed.";
	}

	if (!settings_saved)
	{
		status_line += " Settings persistence also failed.";
	}

	app.status_line = status_line;
	return true;
}

bool DeleteFolderById(AppState& app, const std::string& folder_id)
{
	if (folder_id.empty())
	{
		app.status_line = "Folder id is required.";
		return false;
	}

	const int folder_index = ChatDomainService().FindFolderIndexById(app, folder_id);

	if (folder_index < 0)
	{
		app.status_line = "Folder no longer exists.";
		return false;
	}

	const ChatFolder deleted_folder = app.folders[folder_index];
	const std::vector<ChatSession> original_chats = app.chats;
	std::vector<ChatSession> deleted_chats;
	std::unordered_set<std::string> deleted_chat_ids;

	for (const ChatSession& chat : app.chats)
	{
		if (chat.folder_id != folder_id)
		{
			continue;
		}

		if (ChatHasRunningGemini(app, chat.id))
		{
			app.status_line = "Cannot delete a folder while Gemini is still running for one of its chats.";
			return false;
		}

		deleted_chats.push_back(chat);
		deleted_chat_ids.insert(chat.id);
	}

	const auto restore_original_chats = [&]()
	{
		bool restore_failed = false;

		for (const ChatSession& original_chat : original_chats)
		{
			if (!ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, original_chat), app.data_root, original_chat))
			{
				restore_failed = true;
			}
		}

		return !restore_failed;
	};

	std::vector<ChatSession> next_chats = app.chats;

	for (const ChatSession& deleted_chat : deleted_chats)
	{
		ChatBranching::ReparentChildrenAfterDelete(next_chats, deleted_chat.id);
	}

	next_chats.erase(std::remove_if(next_chats.begin(), next_chats.end(), [&](const ChatSession& chat) { return deleted_chat_ids.contains(chat.id); }), next_chats.end());
	ChatBranching::Normalize(next_chats);

	for (const ChatSession& remaining_chat : next_chats)
	{
		if (!ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, remaining_chat), app.data_root, remaining_chat))
		{
			app.status_line = restore_original_chats() ? "Failed to persist chat updates before folder delete." : "Failed to persist chat updates before folder delete, and rollback also failed.";
			return false;
		}
	}

	std::vector<ChatFolder> next_folders = app.folders;
	next_folders.erase(next_folders.begin() + folder_index);

	if (!ChatFolderStore::Save(app.data_root, next_folders))
	{
		app.status_line = restore_original_chats() ? "Failed to persist folder metadata before delete." : "Failed to persist folder metadata before delete, and rollback also failed.";
		return false;
	}

	const ChatSession* selected_chat = ChatDomainService().SelectedChat(app);
	const std::string selected_chat_id = (selected_chat != nullptr) ? selected_chat->id : "";
	const int previous_selected_chat_index = app.selected_chat_index;

	for (const ChatSession& deleted_chat : deleted_chats)
	{
		StopAcpSession(app, deleted_chat.id);
		StopAndEraseCliTerminalForChat(app, deleted_chat.id, false);
	}

	app.chats = std::move(next_chats);

	if (app.chats.empty())
	{
		app.selected_chat_index = -1;
	}
	else if (!selected_chat_id.empty() && !deleted_chat_ids.contains(selected_chat_id))
	{
		app.selected_chat_index = ChatDomainService().FindChatIndexById(app, selected_chat_id);
		if (app.selected_chat_index < 0)
		{
			app.selected_chat_index = 0;
		}
	}
	else
	{
		app.selected_chat_index = std::min(previous_selected_chat_index, static_cast<int>(app.chats.size()) - 1);
	}

	if (!deleted_chats.empty())
	{
		app.composer_text.clear();
	}

	app.pending_calls.erase(std::remove_if(app.pending_calls.begin(), app.pending_calls.end(),
	                                       [&](PendingRuntimeCall& call)
	                                       {
		                                       if (!deleted_chat_ids.contains(call.chat_id))
		                                       {
			                                       return false;
		                                       }

		                                       ResetPendingRuntimeCall(call);
		                                       return true;
	                                       }),
	                        app.pending_calls.end());

	for (const ChatSession& deleted_chat : deleted_chats)
	{
		app.resolved_native_sessions_by_chat_id.erase(deleted_chat.id);
		app.chats_with_unseen_updates.erase(deleted_chat.id);
		app.collapsed_branch_chat_ids.erase(deleted_chat.id);
		app.filtered_chat_ids.erase(deleted_chat.id);
	}

	for (auto it = app.resolved_native_sessions_by_chat_id.begin(); it != app.resolved_native_sessions_by_chat_id.end();)
	{
		if (deleted_chat_ids.contains(it->second))
		{
			it = app.resolved_native_sessions_by_chat_id.erase(it);
		}
		else
		{
			++it;
		}
	}

	if (deleted_chat_ids.contains(app.editing_chat_id))
	{
		app.editing_chat_id.clear();
		app.editing_message_index = -1;
		app.editing_message_text.clear();
		app.open_edit_message_popup = false;
	}

	if (deleted_chat_ids.contains(app.pending_branch_chat_id))
	{
		app.pending_branch_chat_id.clear();
		app.pending_branch_message_index = -1;
	}

	if (deleted_chat_ids.contains(app.sidebar_chat_options_popup_chat_id))
	{
		app.sidebar_chat_options_popup_chat_id.clear();
		app.open_sidebar_chat_options_popup = false;
	}

	if (app.new_chat_folder_id == folder_id)
	{
		app.new_chat_folder_id.clear();
	}

	ChatDomainService().RefreshRememberedSelection(app);
	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);

	bool chat_history_delete_failed = false;

	for (const ChatSession& deleted_chat : deleted_chats)
	{
		const ProviderProfile& chat_provider = ProviderResolutionService().ProviderForChatOrDefault(app, deleted_chat);

		std::error_code local_delete_ec;
		std::filesystem::remove_all(AppPaths::ChatPath(app.data_root, deleted_chat.id), local_delete_ec);

		std::error_code uam_json_delete_ec;
		std::filesystem::remove(AppPaths::UamChatFilePath(app.data_root, deleted_chat.id), uam_json_delete_ec);

		std::error_code native_delete_ec;
		if (ProviderRuntime::UsesNativeOverlayHistory(chat_provider) && !deleted_chat.native_session_id.empty())
		{
			ChatHistorySyncService().DeleteNativeSessionFileForChat(app, deleted_chat, &native_delete_ec);
		}

		chat_history_delete_failed = chat_history_delete_failed || static_cast<bool>(local_delete_ec) || static_cast<bool>(uam_json_delete_ec) || static_cast<bool>(native_delete_ec);
	}

	std::error_code native_workspace_delete_ec;
	ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(app, deleted_folder, &native_workspace_delete_ec);
	chat_history_delete_failed = chat_history_delete_failed || static_cast<bool>(native_workspace_delete_ec);

	app.folders = std::move(next_folders);

	app.status_line = "Folder deleted. Deleted " + std::to_string(deleted_chats.size()) + " chat(s).";

	if (chat_history_delete_failed)
	{
		app.status_line = "Folder removed from UI, but deleting some chat history failed.";
	}

	if (!settings_saved)
	{
		app.status_line += " Settings persistence also failed.";
	}

	return true;
}

bool CreateFolder(AppState& app, const std::string& title, const std::string& directory, std::string* created_folder_id)
{
	const std::string trimmed_title = Trim(title);
	const std::string trimmed_directory = Trim(directory);

	if (trimmed_title.empty())
	{
		app.status_line = "Folder title is required.";
		return false;
	}

	if (trimmed_directory.empty())
	{
		app.status_line = "Folder directory is required.";
		return false;
	}

	ChatFolder folder;
	for (int attempt = 0; attempt < 16; ++attempt)
	{
		folder.id = ChatDomainService().NewFolderId();
		if (!folder.id.empty() && ChatDomainService().FindFolderById(app, folder.id) == nullptr)
		{
			break;
		}
		folder.id.clear();
	}

	if (folder.id.empty())
	{
		app.status_line = "Failed to allocate a unique folder id.";
		return false;
	}

	folder.title = trimmed_title;
	folder.directory = trimmed_directory;
	folder.collapsed = false;

	const std::string previous_new_chat_folder_id = app.new_chat_folder_id;
	app.folders.push_back(std::move(folder));
	app.new_chat_folder_id = app.folders.back().id;

	if (!ChatFolderStore::Save(app.data_root, app.folders))
	{
		app.folders.pop_back();
		app.new_chat_folder_id = previous_new_chat_folder_id;
		app.status_line = "Failed to persist the new folder.";
		return false;
	}

	if (created_folder_id != nullptr)
	{
		*created_folder_id = app.folders.back().id;
	}

	app.status_line = "Folder created.";
	return true;
}

bool RenameFolderById(AppState& app, const std::string& folder_id, const std::string& title, const std::string& directory)
{
	const int folder_index = ChatDomainService().FindFolderIndexById(app, folder_id);

	if (folder_index < 0)
	{
		app.status_line = "Folder no longer exists.";
		return false;
	}

	const std::string trimmed_title = Trim(title);
	const std::string trimmed_directory = Trim(directory);

	if (trimmed_title.empty())
	{
		app.status_line = "Folder title is required.";
		return false;
	}

	if (trimmed_directory.empty())
	{
		app.status_line = "Folder directory is required.";
		return false;
	}

	ChatFolder& folder = app.folders[folder_index];
	const ChatFolder original = folder;
	const std::string original_status_line = app.status_line;
	folder.title = trimmed_title;
	folder.directory = trimmed_directory;

	if (!ChatFolderStore::Save(app.data_root, app.folders))
	{
		folder = original;
		app.status_line = original_status_line.empty() ? "Failed to persist folder settings." : original_status_line;
		return false;
	}

	app.status_line = "Folder settings saved.";
	return true;
}

std::string ResolveRequestedNewChatFolderId(AppState& app, const std::string& requested_folder_id)
{
	ChatDomainService().EnsureNewChatFolderSelection(app);

	if (requested_folder_id.empty())
	{
		app.status_line = "A workspace folder is required to create a chat.";
		return "";
	}

	if (ChatDomainService().FindFolderById(app, requested_folder_id) == nullptr)
	{
		app.status_line = "Selected workspace folder no longer exists.";
		return "";
	}

	app.new_chat_folder_id = requested_folder_id;
	ChatDomainService().EnsureNewChatFolderSelection(app);
	return ChatDomainService().FolderForNewChat(app);
}
