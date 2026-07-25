#include "chat_lifecycle_service.h"

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/provider_chat_defaults.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	struct DeletedChatsSelection
	{
		std::vector<ChatSession> chats;
		std::unordered_set<std::string> ids;
	};

	bool ContainsDeletedChatId(const std::unordered_set<std::string>& deleted_chat_ids, const std::string& chat_id)
	{
		const std::string normalized_chat_id = uam::strings::Trim(chat_id);
		return !normalized_chat_id.empty() && deleted_chat_ids.contains(normalized_chat_id);
	}

	bool ChatBelongsToFolder(const ChatSession& chat, const std::string& folder_id)
	{
		return uam::strings::TrimmedEquals(chat.folder_id, folder_id);
	}

	bool FolderHasRunningChat(const uam::AppState& app, const std::string& folder_id)
	{
		return std::ranges::any_of(app.chats, [&app, &folder_id](const ChatSession& chat) {
			return ChatBelongsToFolder(chat, folder_id) && uam::ChatHasRunningRuntime(app, chat.id);
		});
	}

	bool SaveChatHistories(uam::AppState& app, const std::vector<ChatSession>& chats)
	{
		bool all_saved = true;
		for (const ChatSession& chat : chats)
		{
			const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
			if (!ProviderRuntime::SaveHistory(provider, app.data_root, chat))
			{
				all_saved = false;
			}
		}
		return all_saved;
	}

	DeletedChatsSelection CollectChatsInFolder(const std::vector<ChatSession>& chats, const std::string& folder_id)
	{
		DeletedChatsSelection selection;
		selection.chats.reserve(chats.size());
		selection.ids.reserve(chats.size());
		for (const ChatSession& chat : chats)
		{
			if (!ChatBelongsToFolder(chat, folder_id))
			{
				continue;
			}

			selection.chats.push_back(chat);
			selection.ids.insert(chat.id);
		}
		return selection;
	}

	void StopChatRuntimes(uam::AppState& app, const std::string& chat_id)
	{
		StopAcpSession(app, chat_id);
		uam::StopAndEraseCliTerminalForChat(app, chat_id, false);
	}

	void StopChatRuntimes(uam::AppState& app, const std::vector<ChatSession>& chats)
	{
		for (const ChatSession& chat : chats)
		{
			StopChatRuntimes(app, chat.id);
		}
	}

	void RemovePendingRuntimeCallsForDeletedChats(uam::AppState& app, const std::unordered_set<std::string>& deleted_chat_ids)
	{
		std::erase_if(app.pending_calls,
		              [&](PendingRuntimeCall& call)
		              {
			              if (!ContainsDeletedChatId(deleted_chat_ids, call.chat_id))
			              {
				              return false;
			              }

			              ResetPendingRuntimeCall(call);
			              return true;
		              });
	}

	void ForgetDeletedChatReferences(uam::AppState& app, const std::unordered_set<std::string>& deleted_chat_ids)
	{
		if (deleted_chat_ids.empty())
		{
			return;
		}

		RemovePendingRuntimeCallsForDeletedChats(app, deleted_chat_ids);

		for (const std::string& deleted_chat_id : deleted_chat_ids)
		{
			app.resolved_native_sessions_by_chat_id.erase(deleted_chat_id);
			app.chats_with_unseen_updates.erase(deleted_chat_id);
			app.collapsed_branch_chat_ids.erase(deleted_chat_id);
			app.filtered_chat_ids.erase(deleted_chat_id);
		}

		for (auto it = app.resolved_native_sessions_by_chat_id.begin(); it != app.resolved_native_sessions_by_chat_id.end();)
		{
			if (ContainsDeletedChatId(deleted_chat_ids, it->first) || ContainsDeletedChatId(deleted_chat_ids, it->second))
			{
				it = app.resolved_native_sessions_by_chat_id.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	bool DeleteNativeHistoryForChatIfNeeded(uam::AppState& app, const ChatSession& chat, std::error_code* error_out = nullptr)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}

		const ProviderProfile& chat_provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		if (!ProviderRuntime::UsesNativeOverlayHistory(chat_provider) || uam::strings::IsBlank(chat.native_session_id))
		{
			return false;
		}

		ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, error_out);
		return true;
	}

	std::string StatusAfterFolderDelete(std::size_t deleted_chat_count, bool settings_saved)
	{
		std::string status_line = "Folder deleted. Deleted " + std::to_string(deleted_chat_count) + " chat(s).";

		if (!settings_saved)
		{
			status_line += " Settings persistence also failed.";
		}

		return status_line;
	}

	std::string StatusAfterFolderDeletePreconditionFailure(std::string_view failure, bool rollback_saved)
	{
		std::string status_line(failure);
		if (!rollback_saved)
		{
			status_line += ", and rollback also failed.";
		}
		return status_line;
	}

	struct FolderInput
	{
		std::string title;
		std::string directory;
	};

	bool ValidateFolderInput(uam::AppState& app, const std::string& title, const std::string& directory, FolderInput& input)
	{
		input.title = uam::strings::Trim(title);
		input.directory = uam::strings::Trim(directory);

		if (input.title.empty())
		{
			app.status_line = "Folder title is required.";
			return false;
		}

		if (input.directory.empty())
		{
			app.status_line = "Folder directory is required.";
			return false;
		}

		return true;
	}

	std::string AllocateUniqueFolderId(const uam::AppState& app)
	{
		constexpr int kMaxFolderIdAttempts = 16;
		for (int attempt = 0; attempt < kMaxFolderIdAttempts; ++attempt)
		{
			const std::string folder_id = ChatDomainService().NewFolderId();
			if (!folder_id.empty() && ChatDomainService().FindFolderById(app, folder_id) == nullptr)
			{
				return folder_id;
			}
		}

		return "";
	}

	void ApplyFolderInput(ChatFolder& folder, const FolderInput& input)
	{
		folder.title = input.title;
		folder.directory = input.directory;
	}

	void SelectAfterDeletingChats(uam::AppState& app, const std::unordered_set<std::string>& deleted_chat_ids, const std::string& selected_before_id, int selected_before_index)
	{
		if (!ContainsDeletedChatId(deleted_chat_ids, selected_before_id))
		{
			ChatDomainService().SelectChatById(app, selected_before_id);
			return;
		}

		ChatDomainService().SetSelectedChatIndexOrNearest(app, selected_before_index);
	}

	void ApplyDeletedChatsToUiState(uam::AppState& app,
	                                const std::unordered_set<std::string>& deleted_chat_ids,
	                                const std::string& selected_before_id,
	                                int selected_before_index,
	                                bool clear_composer)
	{
		SelectAfterDeletingChats(app, deleted_chat_ids, selected_before_id, selected_before_index);

		if (clear_composer)
		{
			app.composer_text.clear();
		}

		ForgetDeletedChatReferences(app, deleted_chat_ids);
	}
} // namespace

uam::ChatProviderSwitchResult uam::SwitchChatProvider(AppState& app, std::string_view chat_id, std::string_view provider_id)
{
	const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, provider_id);
	if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
	{
		return ChatProviderSwitchResult::UnsupportedProvider;
	}

	ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id));
	if (chat == nullptr)
	{
		return ChatProviderSwitchResult::ChatNotFound;
	}
	if (uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat->provider_id) == provider->id)
	{
		return ChatProviderSwitchResult::Unchanged;
	}
	if (const AcpSessionState* session = FindAcpSessionForChat(app, chat->id); session != nullptr && AcpSessionHasActiveTurn(*session))
	{
		return ChatProviderSwitchResult::ActiveRuntime;
	}
	if (ChatHasBusyCliTerminal(app, chat->id))
	{
		return ChatProviderSwitchResult::ActiveRuntime;
	}

	const ChatSession previous_chat = *chat;
	const auto previous_resolved_native_session = app.resolved_native_sessions_by_chat_id.find(chat->id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	const std::string previous_provider_id = ProviderResolutionService().ProviderForChatOrDefault(app, *chat).id;
	for (Message& message : chat->messages)
	{
		if (message.role == MessageRole::Assistant && uam::strings::IsBlank(message.provider))
		{
			message.provider = previous_provider_id;
		}
	}
	chat->provider_id = provider->id;
	uam::provider_chat_defaults::ApplyToChat(app.settings, *chat);
	chat->native_session_id.clear();
	ChatHistorySyncService().ForgetResolvedNativeSessionForChat(app, chat->id);
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(app, *chat, "Chat provider updated.", "Chat provider changed in UI, but failed to save."))
	{
		*chat = previous_chat;
		if (had_previous_resolved_native_session)
		{
			app.resolved_native_sessions_by_chat_id[chat->id] = previous_resolved_native_session_id;
		}
		else
		{
			app.resolved_native_sessions_by_chat_id.erase(chat->id);
		}
		return ChatProviderSwitchResult::SaveFailed;
	}

	(void)StopAcpSession(app, chat->id);
	std::erase_if(app.acp_sessions, [&](const auto& session) { return session != nullptr && session->chat_id == chat->id; });
	StopAndEraseCliTerminalForChat(app, chat->id, false);
	return ChatProviderSwitchResult::Changed;
}

bool uam::BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out, std::string* error_out)
{
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(app);
	if (!ChatDomainService().CreateBranchFromMessage(app, source_chat_id, message_index, replacement_content))
	{
		if (error_out != nullptr)
		{
			*error_out = app.status_line;
		}
		return false;
	}

	const ChatSession* branch = ChatDomainService().SelectedChat(app);
	if (branch == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Branch was created but could not be selected.";
		}
		return false;
	}
	const std::string branch_id = branch->id;
	const ChatSession branch_snapshot = *branch;
	if (branch_id_out != nullptr)
	{
		*branch_id_out = branch_id;
	}
	std::string retry_error;
	if (RetryLastAcpPrompt(app, branch_id, &retry_error))
	{
		return true;
	}

	StopChatRuntimes(app, branch_id);
	std::erase_if(app.acp_sessions, [&branch_id](const auto& session) { return session != nullptr && session->chat_id == branch_id; });
	const ChatStorageDeleteResult storage_delete = ChatRepository::DeleteChatStorageFiles(app.data_root, branch_id);
	if (storage_delete.Failed())
	{
		const bool history_restored = SaveChatHistories(app, {branch_snapshot});
		app.status_line = retry_error + " Branch cleanup failed, so the branch was kept." +
		                  (history_restored ? "" : " Its local history could not be restored.");
		if (error_out != nullptr)
		{
			*error_out = app.status_line;
		}
		return false;
	}

	ChatDomainService().SelectChatById(app, previous_selected_chat_id);
	if (!PersistenceCoordinator().SaveSettings(app))
	{
		const bool history_restored = SaveChatHistories(app, {branch_snapshot});
		ChatDomainService().SelectChatById(app, branch_id);
		ChatDomainService().RefreshRememberedSelection(app);
		app.status_line = retry_error + " Branch selection rollback failed, so the branch was kept." +
		                  (history_restored ? "" : " Its local history could not be restored.");
		if (error_out != nullptr)
		{
			*error_out = app.status_line;
		}
		return false;
	}

	std::erase_if(app.chats, [&branch_id](const ChatSession& chat) { return chat.id == branch_id; });
	app.status_line = retry_error;
	if (error_out != nullptr)
	{
		*error_out = retry_error;
	}
	return false;
}

bool RemoveChatById(uam::AppState& app, const std::string& chat_id)
{
	const std::string target_chat_id = uam::strings::Trim(chat_id);
	if (target_chat_id.empty())
	{
		app.status_line = "Chat id is required.";
		return false;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, target_chat_id);

	if (chat_index < 0)
	{
		app.status_line = "Chat no longer exists.";
		return false;
	}

	const ChatSession chat = app.chats[chat_index];
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;

	if (uam::ChatHasRunningRuntime(app, chat.id))
	{
		app.status_line = "Cannot delete a chat while its runtime is still running.";
		return false;
	}

	std::vector<ChatSession> next_chats = app.chats;
	ChatBranching::ReparentChildrenAfterDelete(next_chats, chat.id);
	std::erase_if(next_chats, [&](const ChatSession& existing_chat) { return existing_chat.id == chat.id; });

	const ChatStorageDeleteResult storage_delete = ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id);
	if (storage_delete.Failed())
	{
		const bool rollback_saved = SaveChatHistories(app, app.chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to delete local chat history; chat was kept.", rollback_saved);
		return false;
	}

	if (!SaveChatHistories(app, next_chats))
	{
		const bool rollback_saved = SaveChatHistories(app, app.chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist chat reparenting before delete.", rollback_saved);
		return false;
	}

	std::error_code native_delete_ec;
	DeleteNativeHistoryForChatIfNeeded(app, chat, &native_delete_ec);
	if (native_delete_ec)
	{
		const bool rollback_saved = SaveChatHistories(app, app.chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to delete native provider history; chat was kept.", rollback_saved);
		return false;
	}

	StopChatRuntimes(app, chat.id);

	app.chats = std::move(next_chats);
	ChatBranching::Normalize(app.chats);

	ApplyDeletedChatsToUiState(app, {chat.id}, selected_chat_id, previous_selected_chat_index, true);

	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);
	app.status_line = settings_saved ? "Chat deleted." : "Chat deleted, but failed to persist settings.";
	return true;
}

bool DeleteFolderById(uam::AppState& app, const std::string& folder_id)
{
	const std::string target_folder_id = uam::strings::Trim(folder_id);
	if (target_folder_id.empty())
	{
		app.status_line = "Folder id is required.";
		return false;
	}

	const int folder_index = ChatDomainService().FindFolderIndexById(app, target_folder_id);

	if (folder_index < 0)
	{
		app.status_line = "Folder no longer exists.";
		return false;
	}

	const ChatFolder deleted_folder = app.folders[folder_index];
	const std::vector<ChatSession> original_chats = app.chats;
	const std::vector<ChatFolder> original_folders = app.folders;

	if (FolderHasRunningChat(app, target_folder_id))
	{
		app.status_line = "Cannot delete a folder while one of its chats has a running runtime.";
		return false;
	}

	const DeletedChatsSelection deleted = CollectChatsInFolder(app.chats, target_folder_id);

	const auto restore_original_chats = [&]()
	{
		return SaveChatHistories(app, original_chats);
	};
	const auto restore_original_state = [&]()
	{
		const bool chats_saved = restore_original_chats();
		const bool folders_saved = ChatFolderStore::Save(app.data_root, original_folders);
		return chats_saved && folders_saved;
	};

	std::vector<ChatSession> next_chats = app.chats;

	for (const ChatSession& deleted_chat : deleted.chats)
	{
		ChatBranching::ReparentChildrenAfterDelete(next_chats, deleted_chat.id);
	}

	std::erase_if(next_chats, [&](const ChatSession& chat) { return deleted.ids.contains(chat.id); });
	ChatBranching::Normalize(next_chats);

	for (const ChatSession& deleted_chat : deleted.chats)
	{
		if (ChatRepository::DeleteChatStorageFiles(app.data_root, deleted_chat.id).Failed())
		{
			app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to delete local chat history; folder was kept.", restore_original_chats());
			return false;
		}
	}

	if (!SaveChatHistories(app, next_chats))
	{
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist chat updates before folder delete.", restore_original_chats());
		return false;
	}

	std::vector<ChatFolder> next_folders = app.folders;
	next_folders.erase(next_folders.begin() + folder_index);

	if (!ChatFolderStore::Save(app.data_root, next_folders))
	{
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist folder metadata before delete.", restore_original_state());
		return false;
	}

	for (const ChatSession& deleted_chat : deleted.chats)
	{
		std::error_code native_delete_ec;
		DeleteNativeHistoryForChatIfNeeded(app, deleted_chat, &native_delete_ec);
		if (native_delete_ec)
		{
			app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to delete native provider history; folder was kept.", restore_original_state());
			return false;
		}
	}

	std::error_code native_workspace_delete_ec;
	ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(app, deleted_folder, &native_workspace_delete_ec);
	if (native_workspace_delete_ec)
	{
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to delete native workspace history; folder was kept.", restore_original_state());
		return false;
	}

	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;

	StopChatRuntimes(app, deleted.chats);

	app.chats = std::move(next_chats);
	ApplyDeletedChatsToUiState(app, deleted.ids, selected_chat_id, previous_selected_chat_index, !deleted.chats.empty());

	if (uam::strings::TrimmedEquals(app.new_chat_folder_id, target_folder_id))
	{
		app.new_chat_folder_id.clear();
	}

	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);

	app.folders = std::move(next_folders);

	app.status_line = StatusAfterFolderDelete(deleted.chats.size(), settings_saved);
	return true;
}

bool CreateFolder(uam::AppState& app, const std::string& title, const std::string& directory, std::string* created_folder_id)
{
	if (created_folder_id != nullptr)
	{
		created_folder_id->clear();
	}

	FolderInput input;
	if (!ValidateFolderInput(app, title, directory, input))
	{
		return false;
	}

	ChatFolder folder;
	folder.id = AllocateUniqueFolderId(app);
	if (folder.id.empty())
	{
		app.status_line = "Failed to allocate a unique folder id.";
		return false;
	}

	ApplyFolderInput(folder, input);
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

bool RenameFolderById(uam::AppState& app, const std::string& folder_id, const std::string& title, const std::string& directory)
{
	const std::string target_folder_id = uam::strings::Trim(folder_id);
	if (target_folder_id.empty())
	{
		app.status_line = "Folder id is required.";
		return false;
	}

	const int folder_index = ChatDomainService().FindFolderIndexById(app, target_folder_id);

	if (folder_index < 0)
	{
		app.status_line = "Folder no longer exists.";
		return false;
	}

	FolderInput input;
	if (!ValidateFolderInput(app, title, directory, input))
	{
		return false;
	}

	ChatFolder& folder = app.folders[folder_index];
	const ChatFolder original = folder;
	const std::string original_status_line = app.status_line;
	ApplyFolderInput(folder, input);

	if (!ChatFolderStore::Save(app.data_root, app.folders))
	{
		folder = original;
		app.status_line = uam::strings::NonEmptyOrFallback(original_status_line, "Failed to persist folder settings.");
		return false;
	}

	app.status_line = "Folder settings saved.";
	return true;
}

std::string ResolveRequestedNewChatFolderId(uam::AppState& app, const std::string& requested_folder_id)
{
	ChatDomainService().EnsureNewChatFolderSelection(app);
	const std::string target_folder_id = uam::strings::Trim(requested_folder_id);

	if (target_folder_id.empty())
	{
		app.status_line = "A workspace folder is required to create a chat.";
		return "";
	}

	if (ChatDomainService().FindFolderById(app, target_folder_id) == nullptr)
	{
		app.status_line = "Selected workspace folder no longer exists.";
		return "";
	}

	app.new_chat_folder_id = target_folder_id;
	ChatDomainService().EnsureNewChatFolderSelection(app);
	return ChatDomainService().FolderForNewChat(app);
}
