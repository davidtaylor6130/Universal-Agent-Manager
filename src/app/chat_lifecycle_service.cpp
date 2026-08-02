#include "chat_lifecycle_service.h"

#include "app/chat_domain_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/config/provider_chat_defaults.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
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
#include <unordered_map>
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

	bool ChatHasDeletionBlockingRuntime(const uam::AppState& app, const std::string& chat_id)
	{
		if (uam::ChatHasRunningRuntime(app, chat_id))
		{
			return true;
		}
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat_id);
		return session != nullptr && session->running && uam::AcpSessionHasBlockingRuntimeWork(*session);
	}

	bool FolderHasRunningChat(const uam::AppState& app, const std::string& folder_id)
	{
		return std::ranges::any_of(app.chats, [&app, &folder_id](const ChatSession& chat) {
			return ChatBelongsToFolder(chat, folder_id) && ChatHasDeletionBlockingRuntime(app, chat.id);
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

	bool HydrateDeletedChatsForRollback(
	    const std::filesystem::path& data_root,
	    std::vector<ChatSession>& chats,
	    const std::unordered_set<std::string>& deleted_chat_ids)
	{
		for (ChatSession& chat : chats)
		{
			if (deleted_chat_ids.contains(chat.id) && !ChatRepository::HydrateChatMessages(data_root, chat))
			{
				return false;
			}
		}
		return true;
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

		return ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, error_out);
	}

	std::string StatusAfterFolderDelete(std::size_t deleted_chat_count, bool settings_saved, bool native_cleanup_failed)
	{
		std::string status_line = "Folder deleted. Deleted " + std::to_string(deleted_chat_count) + " chat(s).";

		if (!settings_saved)
		{
			status_line += " Settings persistence also failed.";
		}
		if (native_cleanup_failed)
		{
			status_line += " Some provider-native history could not be removed.";
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

	bool IsUnsortedChat(const uam::AppState& app, const ChatSession& chat)
	{
		return uam::strings::IsBlank(chat.folder_id) || ChatDomainService().FindFolderById(app, chat.folder_id) == nullptr;
	}

	std::filesystem::path RecordedWorkspaceDirectory(const ChatSession& chat)
	{
		const std::string& recorded = uam::paths::HasGitWorktreeSource(chat)
		                                  ? chat.workspace_source_directory
		                                  : chat.workspace_directory;
		return uam::paths::ExpandTrimmedWorkspacePath(recorded);
	}

	std::string WorkspaceTitle(const std::filesystem::path& directory)
	{
		std::filesystem::path name = directory.filename();
		if (name.empty()) name = directory.root_name();
		if (name.empty()) name = directory.root_directory();
		return uam::strings::NonEmptyOrFallback(uam::paths::Utf8PathString(name), "Workspace");
	}

	uam::WorkspaceFolderRecoveryChat RecoveryChat(const ChatSession& chat, const std::filesystem::path& directory, std::string reason)
	{
		return {
		    chat.id,
		    uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.title), "Untitled chat"),
		    uam::paths::Utf8PathString(directory),
		    std::move(reason),
		};
	}

	std::string AllocateRecoveryFolderId(const std::unordered_set<std::string>& used_ids)
	{
		constexpr int kMaxFolderIdAttempts = 16;
		for (int attempt = 0; attempt < kMaxFolderIdAttempts; ++attempt)
		{
			const std::string folder_id = ChatDomainService().NewFolderId();
			if (!folder_id.empty() && !used_ids.contains(folder_id)) return folder_id;
		}
		return "";
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

uam::WorkspaceFolderRecoveryPreview uam::PreviewUnsortedWorkspaceFolders(const AppState& app)
{
	WorkspaceFolderRecoveryPreview preview;
	for (const ChatSession& chat : app.chats)
	{
		if (!IsUnsortedChat(app, chat)) continue;

		const std::filesystem::path recorded_directory = RecordedWorkspaceDirectory(chat);
		if (recorded_directory.empty())
		{
			preview.no_location.push_back(RecoveryChat(chat, {}, "No workspace location recorded"));
			continue;
		}

		std::error_code status_error;
		const std::filesystem::file_status status = std::filesystem::status(recorded_directory, status_error);
		if (status_error)
		{
			if (status_error == std::errc::no_such_file_or_directory)
			{
				preview.missing.push_back(RecoveryChat(chat, recorded_directory, "Folder not found"));
			}
			else
			{
				preview.unavailable.push_back(RecoveryChat(chat, recorded_directory, status_error.message()));
			}
			continue;
		}
		if (!std::filesystem::exists(status))
		{
			preview.missing.push_back(RecoveryChat(chat, recorded_directory, "Folder not found"));
			continue;
		}
		if (!std::filesystem::is_directory(status))
		{
			preview.unavailable.push_back(RecoveryChat(chat, recorded_directory, "Location is not a folder"));
			continue;
		}

		const std::filesystem::path directory = uam::paths::NormalizeExistingPath(recorded_directory);
		auto group = std::ranges::find_if(preview.groups, [&](const WorkspaceFolderRecoveryGroup& candidate) {
			return FolderDirectoryMatches(candidate.directory, directory);
		});
		if (group == preview.groups.end())
		{
			WorkspaceFolderRecoveryGroup created;
			created.title = WorkspaceTitle(directory);
			created.directory = uam::paths::Utf8PathString(directory);
			const auto existing = std::ranges::find_if(app.folders, [&](const ChatFolder& folder) {
				return FolderDirectoryMatches(folder.directory, directory);
			});
			if (existing != app.folders.end()) created.existing_folder_id = existing->id;
			preview.groups.push_back(std::move(created));
			group = std::prev(preview.groups.end());
		}
		group->chat_ids.push_back(chat.id);
	}
	return preview;
}

bool uam::RebuildUnsortedWorkspaceFolders(AppState& app, WorkspaceFolderRecoveryResult* result_out)
{
	if (result_out != nullptr) *result_out = {};
	WorkspaceFolderRecoveryPreview preview = PreviewUnsortedWorkspaceFolders(app);
	if (preview.groups.empty())
	{
		app.status_line = "No available workspace folders were found for Unsorted chats.";
		return false;
	}

	std::vector<ChatFolder> next_folders = app.folders;
	const std::vector<ChatSession> original_chats = app.chats;
	std::vector<ChatSession> next_chats = original_chats;
	std::unordered_set<std::string> used_folder_ids;
	for (const ChatFolder& folder : next_folders) used_folder_ids.insert(folder.id);
	std::unordered_map<std::string, std::string> recovered_folder_by_chat_id;

	WorkspaceFolderRecoveryResult result;
	for (const WorkspaceFolderRecoveryGroup& group : preview.groups)
	{
		std::string folder_id = group.existing_folder_id;
		if (folder_id.empty())
		{
			folder_id = AllocateRecoveryFolderId(used_folder_ids);
			if (folder_id.empty())
			{
				app.status_line = "Failed to allocate a workspace folder id.";
				return false;
			}
			used_folder_ids.insert(folder_id);
			ChatFolder folder;
			folder.id = folder_id;
			folder.title = group.title;
			folder.directory = group.directory;
			folder.collapsed = false;
			next_folders.push_back(std::move(folder));
			++result.created_folder_count;
		}
		else
		{
			++result.reused_folder_count;
		}

		for (const std::string& chat_id : group.chat_ids) recovered_folder_by_chat_id[chat_id] = folder_id;
		result.organized_chat_count += group.chat_ids.size();
	}
	for (ChatSession& chat : next_chats)
	{
		const auto recovered = recovered_folder_by_chat_id.find(chat.id);
		if (recovered != recovered_folder_by_chat_id.end()) chat.folder_id = recovered->second;
	}

	std::vector<ChatSession> original_changed_chats;
	std::vector<ChatSession> next_changed_chats;
	for (std::size_t index = 0; index < next_chats.size(); ++index)
	{
		if (next_chats[index].folder_id == original_chats[index].folder_id) continue;
		original_changed_chats.push_back(original_chats[index]);
		next_changed_chats.push_back(next_chats[index]);
	}
	if (!SaveChatHistories(app, next_changed_chats))
	{
		const bool rollback_saved = SaveChatHistories(app, original_changed_chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to save reorganized chats.", rollback_saved);
		return false;
	}
	if (!ChatFolderStore::Save(app.data_root, next_folders))
	{
		const bool rollback_saved = SaveChatHistories(app, original_changed_chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to save rebuilt workspace folders.", rollback_saved);
		return false;
	}

	app.chats = std::move(next_chats);
	app.folders = std::move(next_folders);
	app.status_line = "Organized " + std::to_string(result.organized_chat_count) + " chat(s) into workspace folders.";
	if (result_out != nullptr) *result_out = result;
	return true;
}

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

bool RemoveChatsByIds(uam::AppState& app, const std::vector<std::string>& chat_ids)
{
	std::vector<std::string> target_chat_ids;
	std::unordered_set<std::string> deleted_chat_ids;
	for (const std::string& requested_id : chat_ids)
	{
		const std::string id = uam::strings::Trim(requested_id);
		if (!id.empty() && deleted_chat_ids.insert(id).second)
		{
			target_chat_ids.push_back(id);
		}
	}
	if (target_chat_ids.empty())
	{
		app.status_line = "Chat id is required.";
		return false;
	}

	for (const std::string& id : target_chat_ids)
	{
		if (ChatDomainService().FindChatIndexById(app, id) < 0)
		{
			app.status_line = "A selected chat no longer exists.";
			return false;
		}
		if (app.worktree_operation_chat_ids.contains(id))
		{
			app.status_line = "Wait for the chat worktree operation to finish.";
			return false;
		}
		if (ChatHasDeletionBlockingRuntime(app, id))
		{
			app.status_line = "Cannot delete chats while a selected runtime is still running.";
			return false;
		}
	}

	std::vector<ChatSession> original_chats = app.chats;
	if (!HydrateDeletedChatsForRollback(app.data_root, original_chats, deleted_chat_ids))
	{
		app.status_line = "Failed to prepare chat history for safe deletion.";
		return false;
	}
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;

	std::vector<ChatSession> deleted_chats;
	deleted_chats.reserve(target_chat_ids.size());
	for (const ChatSession& chat : original_chats)
	{
		if (deleted_chat_ids.contains(chat.id))
		{
			deleted_chats.push_back(chat);
		}
	}

	std::vector<ChatSession> next_chats = app.chats;
	for (const std::string& id : target_chat_ids)
	{
		ChatBranching::ReparentChildrenAfterDelete(next_chats, id);
	}
	std::erase_if(next_chats, [&](const ChatSession& chat) { return deleted_chat_ids.contains(chat.id); });
	ChatBranching::Normalize(next_chats);

	if (!SaveChatHistories(app, next_chats))
	{
		const bool rollback_saved = SaveChatHistories(app, original_chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist chat reparenting before delete.", rollback_saved);
		return false;
	}

	std::vector<std::string> added_tombstones;
	if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, deleted_chats, added_tombstones))
	{
		const bool rollback_saved = SaveChatHistories(app, original_chats);
		app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to prevent deleted chats from being reimported.", rollback_saved);
		return false;
	}

	StopChatRuntimes(app, deleted_chats);
	for (const ChatSession& chat : deleted_chats)
	{
		if (ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id).Failed())
		{
			const bool rollback_saved = SaveChatHistories(app, original_chats);
			const bool tombstones_restored = ChatHistorySyncService().RemoveNativeImportTombstones(app.data_root, added_tombstones);
			app.status_line = StatusAfterFolderDeletePreconditionFailure(
			    "Failed to delete local chat history; chats were kept.", rollback_saved && tombstones_restored);
			return false;
		}
	}

	bool native_cleanup_failed = false;
	for (const ChatSession& chat : deleted_chats)
	{
		std::error_code native_delete_error;
		DeleteNativeHistoryForChatIfNeeded(app, chat, &native_delete_error);
		if (native_delete_error)
		{
			native_cleanup_failed = true;
		}
	}

	app.chats = std::move(next_chats);
	ApplyDeletedChatsToUiState(app, deleted_chat_ids, selected_chat_id, previous_selected_chat_index, true);

	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);
	const std::string count = std::to_string(deleted_chats.size());
	app.status_line = settings_saved ? count + " chat(s) deleted." : count + " chat(s) deleted, but failed to persist settings.";
	if (native_cleanup_failed)
	{
		app.status_line += " Some provider-native history could not be removed.";
	}
	return true;
}

bool RemoveChatById(uam::AppState& app, const std::string& chat_id)
{
	return RemoveChatsByIds(app, {chat_id});
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
	std::vector<ChatSession> original_chats = app.chats;
	const std::vector<ChatFolder> original_folders = app.folders;

	if (FolderHasRunningChat(app, target_folder_id))
	{
		app.status_line = "Cannot delete a folder while one of its chats has a running runtime.";
		return false;
	}

	const DeletedChatsSelection deleted = CollectChatsInFolder(app.chats, target_folder_id);
	if (!HydrateDeletedChatsForRollback(app.data_root, original_chats, deleted.ids))
	{
		app.status_line = "Failed to prepare folder chat history for safe deletion.";
		return false;
	}
	std::vector<std::string> added_tombstones;
	if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, deleted.chats, added_tombstones))
	{
		app.status_line = "Failed to prevent deleted folder chats from being reimported.";
		return false;
	}

	const auto restore_original_chats = [&]()
	{
		const bool chats_saved = SaveChatHistories(app, original_chats);
		const bool tombstones_restored = ChatHistorySyncService().RemoveNativeImportTombstones(app.data_root, added_tombstones);
		return chats_saved && tombstones_restored;
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

	StopChatRuntimes(app, deleted.chats);

	bool native_cleanup_failed = false;
	for (const ChatSession& deleted_chat : deleted.chats)
	{
		std::error_code native_delete_ec;
		DeleteNativeHistoryForChatIfNeeded(app, deleted_chat, &native_delete_ec);
		if (native_delete_ec)
		{
			native_cleanup_failed = true;
		}
	}

	std::error_code native_workspace_delete_ec;
	ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(app, deleted_folder, &native_workspace_delete_ec);
	if (native_workspace_delete_ec)
	{
		native_cleanup_failed = true;
	}

	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;

	app.chats = std::move(next_chats);
	ApplyDeletedChatsToUiState(app, deleted.ids, selected_chat_id, previous_selected_chat_index, !deleted.chats.empty());

	if (uam::strings::TrimmedEquals(app.new_chat_folder_id, target_folder_id))
	{
		app.new_chat_folder_id.clear();
	}

	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);

	app.folders = std::move(next_folders);

	app.status_line = StatusAfterFolderDelete(deleted.chats.size(), settings_saved, native_cleanup_failed);
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
	const std::vector<ChatSession> original_chats = app.chats;
	const std::string original_status_line = app.status_line;
	const bool directory_changed = !FolderDirectoryMatches(original.directory, input.directory);
	if (directory_changed && FolderHasRunningChat(app, target_folder_id))
	{
		app.status_line = "Cannot change a folder directory while one of its chats has a running runtime.";
		return false;
	}

	if (directory_changed)
	{
		for (ChatSession& chat : app.chats)
		{
			if (!ChatBelongsToFolder(chat, target_folder_id))
			{
				continue;
			}
			if (FolderDirectoryMatches(chat.workspace_directory, original.directory))
			{
				chat.workspace_directory = input.directory;
			}
			if (FolderDirectoryMatches(chat.workspace_source_directory, original.directory))
			{
				chat.workspace_source_directory = input.directory;
			}
		}
		if (!SaveChatHistories(app, app.chats))
		{
			const bool rollback_saved = SaveChatHistories(app, original_chats);
			app.chats = original_chats;
			app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist chat workspace updates; folder was kept.", rollback_saved);
			return false;
		}
	}
	ApplyFolderInput(folder, input);

	if (!ChatFolderStore::Save(app.data_root, app.folders))
	{
		folder = original;
		if (directory_changed)
		{
			const bool rollback_saved = SaveChatHistories(app, original_chats);
			app.chats = original_chats;
			app.status_line = StatusAfterFolderDeletePreconditionFailure("Failed to persist folder settings.", rollback_saved);
		}
		else
		{
			app.status_line = uam::strings::NonEmptyOrFallback(original_status_line, "Failed to persist folder settings.");
		}
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
