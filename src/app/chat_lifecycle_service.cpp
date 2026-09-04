#include "chat_lifecycle_service.h"

#include "app/chat_domain_service.h"
#include "app/computer_use_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_ids.h"
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
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

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
		const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat != nullptr &&
		    (chat->remote_turn_reconnect_pending || chat->remote_stop_cleanup_pending ||
		     chat->remote_restart_pending) &&
		    chat->execution_host_id != uam::execution_hosts::kLocalHostId &&
		    !chat->imported_read_only)
		{
			return true;
		}
		if (uam::ChatHasRunningRuntime(app, chat_id))
		{
			return true;
		}
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat_id);
		return session != nullptr &&
		       (session->remote_stop_pending ||
		        session->remote_stop_unconfirmed ||
		        (session->running &&
		         ((chat != nullptr &&
		           chat->execution_host_id != uam::execution_hosts::kLocalHostId) ||
		          uam::AcpSessionHasBlockingRuntimeWork(*session))) ||
		        session->reconnect_pending || session->recovering_remote_turn);
	}

	bool BeginIdleRemoteStopForDeletion(uam::AppState& app, const std::string& chat_id)
	{
		const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat_id);
		if (chat == nullptr || session == nullptr ||
		    !session->running || session->remote_stop_unconfirmed ||
		    chat->execution_host_id == uam::execution_hosts::kLocalHostId ||
		    uam::AcpSessionHasBlockingRuntimeWork(*session))
		{
			return false;
		}
		(void)uam::StopAcpSession(app, chat_id);
		return true;
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

	bool IsTerminalAgentRun(const AgentRun& run)
	{
		return run.status == "completed" || run.status == "failed" ||
		       run.status == "cancelled" || run.status == "interrupted";
	}

	void ExpandDependentChatIds(const uam::AppState& app,
	                            std::vector<std::string>& target_chat_ids,
	                            std::unordered_set<std::string>& deleted_chat_ids)
	{
		for (std::size_t owner_index = 0; owner_index < target_chat_ids.size(); ++owner_index)
		{
			const std::string owner_id = target_chat_ids[owner_index];
			for (const ChatSession& chat : app.chats)
			{
				if (uam::strings::TrimmedEquals(chat.goal_owner_chat_id, owner_id) &&
				    deleted_chat_ids.insert(chat.id).second)
				{
					target_chat_ids.push_back(chat.id);
				}
			}
			for (const AgentRun& run : app.agent_runs)
			{
				if (uam::strings::TrimmedEquals(run.root_chat_id, owner_id) &&
				    !uam::strings::IsBlank(run.transcript_chat_id) &&
				    std::ranges::any_of(app.chats, [&](const ChatSession& chat)
				    {
					    return uam::strings::TrimmedEquals(chat.id, run.transcript_chat_id) &&
					           uam::strings::TrimmedEquals(chat.agent_run_id, run.id);
				    }) &&
				    deleted_chat_ids.insert(run.transcript_chat_id).second)
				{
					target_chat_ids.push_back(run.transcript_chat_id);
				}
			}
		}
	}

	bool HasActiveDependentAgentRun(const uam::AppState& app,
	                                const std::unordered_set<std::string>& chat_ids)
	{
		return std::ranges::any_of(app.agent_runs, [&](const AgentRun& run)
		{
			return !IsTerminalAgentRun(run) &&
			       (chat_ids.contains(run.root_chat_id) ||
			        chat_ids.contains(run.transcript_chat_id));
		});
	}

	DeletedChatsSelection CollectChatsInFolder(const uam::AppState& app,
	                                           const std::string& folder_id)
	{
		DeletedChatsSelection selection;
		selection.ids.reserve(app.chats.size());
		std::vector<std::string> target_chat_ids;
		for (const ChatSession& chat : app.chats)
		{
			if (ChatBelongsToFolder(chat, folder_id) && selection.ids.insert(chat.id).second)
				target_chat_ids.push_back(chat.id);
		}
		ExpandDependentChatIds(app, target_chat_ids, selection.ids);
		selection.chats.reserve(selection.ids.size());
		for (const ChatSession& chat : app.chats)
			if (selection.ids.contains(chat.id)) selection.chats.push_back(chat);
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

	constexpr std::string_view kDeletionIntentFile = "deletion-transaction.json";
	constexpr std::string_view kDeletionStagingDirectory = ".deletion-transaction";

	struct DeletionIntent
	{
		std::vector<std::string> chat_ids;
		std::string folder_id;
	};

	std::filesystem::path DeletionIntentPath(const std::filesystem::path& data_root)
	{
		return data_root / kDeletionIntentFile;
	}

	std::filesystem::path DeletionStagingRoot(const std::filesystem::path& data_root)
	{
		return data_root / kDeletionStagingDirectory;
	}

	bool ParseDeletionIntent(std::string_view text, DeletionIntent& intent)
	{
		const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
		const auto version = parsed.find("version");
		const auto complete = parsed.find("complete");
		const auto chat_ids = parsed.find("chatIds");
		const auto folder_id = parsed.find("folderId");
		if (!parsed.is_object() || version == parsed.end() || !version->is_number_integer() ||
		    *version != 1 || complete == parsed.end() || !complete->is_boolean() ||
		    !complete->get<bool>() || chat_ids == parsed.end() || !chat_ids->is_array() ||
		    (folder_id != parsed.end() && !folder_id->is_string()))
		{
			return false;
		}

		DeletionIntent decoded;
		for (const nlohmann::json& value : *chat_ids)
		{
			if (!value.is_string()) return false;
			const std::string id = uam::strings::Trim(value.get_ref<const std::string&>());
			if (!uam::chat_ids::IsSafeStorageChatId(id) || std::ranges::find(decoded.chat_ids, id) != decoded.chat_ids.end()) return false;
			decoded.chat_ids.push_back(id);
		}
		if (folder_id != parsed.end()) decoded.folder_id = uam::strings::Trim(folder_id->get_ref<const std::string&>());
		if (decoded.chat_ids.empty() && decoded.folder_id.empty()) return false;
		intent = std::move(decoded);
		return true;
	}

	bool LoadDeletionIntent(const std::filesystem::path& data_root, DeletionIntent& intent)
	{
		const std::filesystem::path path = DeletionIntentPath(data_root);
		for (const std::filesystem::path& candidate : {path, uam::io::MakeBackupPath(path)})
		{
			std::string text;
			if (uam::io::TryReadTextFile(candidate, text) && ParseDeletionIntent(text, intent)) return true;
		}
		return false;
	}

	bool BeginDeletionTransaction(const uam::AppState& app, const std::vector<ChatSession>& deleted_chats, std::string_view folder_id)
	{
		const std::filesystem::path intent_path = DeletionIntentPath(app.data_root);
		const std::filesystem::path staging_root = DeletionStagingRoot(app.data_root);
		if (uam::paths::PathExistsNoThrow(intent_path) || uam::paths::PathExistsNoThrow(uam::io::MakeBackupPath(intent_path))) return false;

		std::error_code status_error;
		const std::filesystem::file_status staging_status = std::filesystem::symlink_status(staging_root, status_error);
		if (!status_error && std::filesystem::exists(staging_status))
		{
			if (uam::paths::IsLinkOrReparsePointNoThrow(staging_root) || !uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staging_root)) return false;
		}
		if (!uam::paths::CreateDirectoriesNoThrow(staging_root)) return false;
		for (const ChatSession& chat : deleted_chats)
		{
			if (!ChatRepository::SaveChat(staging_root, chat)) return false;
		}
		if (!folder_id.empty() && !ChatFolderStore::Save(staging_root, app.folders)) return false;

		nlohmann::json encoded = {
		    {"version", 1},
		    {"complete", true},
		    {"folderId", std::string(folder_id)},
		    {"chatIds", nlohmann::json::array()},
		};
		for (const ChatSession& chat : deleted_chats) encoded["chatIds"].push_back(chat.id);
		if (!uam::io::WriteTextFileWithBackup(intent_path, encoded.dump()))
		{
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staging_root);
			return false;
		}
		return true;
	}

	std::vector<ChatSession> DeletedChatSnapshots(const std::filesystem::path& data_root, const DeletionIntent& intent, const std::vector<ChatSession>& current_chats)
	{
		std::vector<ChatSession> staged = ChatRepository::LoadLocalChats(DeletionStagingRoot(data_root));
		std::vector<ChatSession> result;
		for (const std::string& id : intent.chat_ids)
		{
			const ChatSession* snapshot = nullptr;
			if (const auto found = std::ranges::find_if(staged, [&](const ChatSession& chat) { return chat.id == id; }); found != staged.end()) snapshot = &*found;
			if (snapshot == nullptr)
			{
				if (const auto found = std::ranges::find_if(current_chats, [&](const ChatSession& chat) { return chat.id == id; }); found != current_chats.end()) snapshot = &*found;
			}
			if (snapshot != nullptr) result.push_back(*snapshot);
			else
			{
				ChatSession already_deleted;
				already_deleted.id = id;
				result.push_back(std::move(already_deleted));
			}
		}
		return result;
	}

	bool RemoveDeletionTransactionFiles(const std::filesystem::path& data_root)
	{
		const std::filesystem::path intent_path = DeletionIntentPath(data_root);
		std::error_code error;
		std::filesystem::remove(uam::io::MakeBackupPath(intent_path), error);
		if (error) return false;
		std::filesystem::remove(intent_path, error);
		if (error) return false;

		const std::filesystem::path staging_root = DeletionStagingRoot(data_root);
		const std::filesystem::file_status status = std::filesystem::symlink_status(staging_root, error);
		if (error == std::errc::no_such_file_or_directory) return true;
		if (error || uam::paths::IsLinkOrReparsePointNoThrow(staging_root)) return false;
		return uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staging_root);
	}

	bool CompleteDeletionTransaction(uam::AppState& app,
	                                 const DeletionIntent& intent,
	                                 std::vector<ChatSession>& next_chats,
	                                 std::vector<ChatFolder>& next_folders,
	                                 std::vector<ChatSession>& deleted_chats,
	                                 bool& native_cleanup_failed)
	{
		const std::unordered_set<std::string> deleted_ids(intent.chat_ids.begin(), intent.chat_ids.end());
		next_chats = app.chats;
		for (const std::string& id : intent.chat_ids) ChatBranching::ReparentChildrenAfterDelete(next_chats, id);
		std::erase_if(next_chats, [&](const ChatSession& chat) { return deleted_ids.contains(chat.id); });
		ChatBranching::Normalize(next_chats);
		next_folders = app.folders;
		if (!intent.folder_id.empty())
		{
			std::erase_if(next_folders, [&](const ChatFolder& folder) { return uam::strings::TrimmedEquals(folder.id, intent.folder_id); });
		}
		deleted_chats = DeletedChatSnapshots(app.data_root, intent, app.chats);
		std::vector<std::string> added_tombstones;
		if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, deleted_chats, added_tombstones)) return false;

		for (const ChatSession& chat : next_chats)
		{
			if (!ChatRepository::SaveChat(app.data_root, chat)) return false;
		}

		if (!intent.folder_id.empty())
		{
			if (!ChatFolderStore::Save(app.data_root, next_folders)) return false;
		}

		for (const std::string& id : intent.chat_ids)
		{
			if (ChatRepository::DeleteChatStorageFiles(app.data_root, id).Failed()) return false;
		}

		native_cleanup_failed = false;
		for (const ChatSession& chat : deleted_chats)
		{
			std::error_code error;
			DeleteNativeHistoryForChatIfNeeded(app, chat, &error);
			native_cleanup_failed = native_cleanup_failed || static_cast<bool>(error);
		}
		if (!intent.folder_id.empty())
		{
			const std::vector<ChatFolder> staged_folders = ChatFolderStore::Load(DeletionStagingRoot(app.data_root));
			if (const auto folder = std::ranges::find_if(staged_folders, [&](const ChatFolder& value) { return uam::strings::TrimmedEquals(value.id, intent.folder_id); }); folder != staged_folders.end())
			{
				std::error_code error;
				ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(app, *folder, &error);
				native_cleanup_failed = native_cleanup_failed || static_cast<bool>(error);
			}
		}

		return RemoveDeletionTransactionFiles(app.data_root);
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
		std::string execution_host_id;
	};

	bool ValidateFolderInput(uam::AppState& app, const std::string& title,
	                         const std::string& directory,
	                         const std::string& execution_host_id,
	                         FolderInput& input)
	{
		input.title = uam::strings::Trim(title);
		input.directory = uam::strings::Trim(directory);
		input.execution_host_id = uam::strings::NonEmptyOrFallback(
		    uam::strings::Trim(execution_host_id), "local");

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

		const ExecutionHost* host = uam::execution_hosts::Find(
		    app.settings.execution_hosts, input.execution_host_id);
		if (host == nullptr)
		{
			app.status_line = "The selected execution host no longer exists.";
			return false;
		}
		if (host->id != uam::execution_hosts::kLocalHostId &&
		    !uam::execution_hosts::IsAbsoluteRemotePath(host->platform, input.directory))
		{
			app.status_line = "An absolute remote workspace path is required.";
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
		folder.execution_host_id = input.execution_host_id;
	}

	std::string WorkspaceOwnershipKey(const uam::AppState& app, std::string_view host_id,
	                                  std::string_view directory)
	{
		return uam::paths::WorkspaceOwnershipKey(app, host_id, directory);
	}

	std::string WorkspaceTitle(std::string_view directory, std::string_view fallback)
	{
		std::string value(uam::strings::TrimAsciiView(directory));
		while (value.size() > 1 && (value.back() == '/' || value.back() == '\\')) value.pop_back();
		const std::size_t separator = value.find_last_of("/\\");
		const std::string title = uam::strings::Trim(
		    separator == std::string::npos ? value : value.substr(separator + 1));
		return uam::strings::NonEmptyOrFallback(title, std::string(fallback));
	}

	bool IsUnsortedChat(const uam::AppState& app, const ChatSession& chat)
	{
		return uam::strings::IsBlank(chat.folder_id) || ChatDomainService().FindFolderById(app, chat.folder_id) == nullptr;
	}

	std::string RecordedWorkspaceDirectory(const ChatSession& chat)
	{
		const std::string& recorded = uam::paths::HasGitWorktreeSource(chat)
		                                  ? chat.workspace_source_directory
		                                  : chat.workspace_directory;
		return uam::strings::Trim(recorded);
	}

	std::string WorkspaceTitle(const std::filesystem::path& directory)
	{
		std::filesystem::path name = directory.filename();
		if (name.empty()) name = directory.root_name();
		if (name.empty()) name = directory.root_directory();
		return uam::strings::NonEmptyOrFallback(uam::paths::Utf8PathString(name), "Workspace");
	}

	uam::WorkspaceFolderRecoveryChat RecoveryChat(const ChatSession& chat, std::string directory, std::string reason)
	{
		return {
		    chat.id,
		    uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.title), "Untitled chat"),
		    std::move(directory),
		    uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.execution_host_id), "local"),
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

bool uam::MigrateWorkspaceFolderOwnership(AppState& app)
{
	const std::vector<ChatFolder> original_folders = app.folders;
	const std::vector<ChatSession> original_chats = app.chats;
	const std::string original_status_line = app.status_line;
	std::vector<ChatSession> changed_chats;
	const std::size_t original_folder_count = app.folders.size();
	std::size_t split_count = 0;
	bool changed = false;

	for (std::size_t folder_index = 0; folder_index < original_folder_count; ++folder_index)
	{
		ChatFolder& folder = app.folders[folder_index];
		const std::string folder_id = folder.id;
		const std::string folder_title = folder.title;
		std::unordered_map<std::string, std::vector<std::size_t>> chat_indices_by_owner;
		std::unordered_map<std::string, std::pair<std::string, std::string>> owner_values;
		for (std::size_t chat_index = 0; chat_index < app.chats.size(); ++chat_index)
		{
			const ChatSession& chat = app.chats[chat_index];
			if (!ChatBelongsToFolder(chat, folder_id)) continue;
			const std::string host = uam::strings::NonEmptyOrFallback(
			    uam::strings::Trim(chat.execution_host_id), "local");
			const std::string directory = uam::strings::NonEmptyOrFallback(
			    uam::strings::Trim(chat.workspace_directory), folder.directory);
			const std::string key = WorkspaceOwnershipKey(app, host, directory);
			chat_indices_by_owner[key].push_back(chat_index);
			owner_values.try_emplace(key, host, directory);
		}

		std::string primary_key;
		if (uam::strings::IsBlank(folder.execution_host_id))
		{
			for (const auto& [key, indices] : chat_indices_by_owner)
			{
				if (primary_key.empty() || indices.size() > chat_indices_by_owner.at(primary_key).size() ||
				    (indices.size() == chat_indices_by_owner.at(primary_key).size() && key < primary_key))
					primary_key = key;
			}
			if (primary_key.empty())
			{
				folder.execution_host_id = "local";
			}
			else
			{
				folder.execution_host_id = owner_values.at(primary_key).first;
				folder.directory = owner_values.at(primary_key).second;
			}
			changed = true;
		}
		else
		{
			primary_key = WorkspaceOwnershipKey(app, folder.execution_host_id, folder.directory);
		}

		for (const auto& [key, chat_indices] : chat_indices_by_owner)
		{
			if (key == primary_key) continue;
			const auto& [host, directory] = owner_values.at(key);
			auto destination = std::ranges::find_if(app.folders, [&](const ChatFolder& candidate) {
				return candidate.id != folder_id &&
				       WorkspaceOwnershipKey(app, candidate.execution_host_id, candidate.directory) == key;
			});
			if (destination == app.folders.end())
			{
				ChatFolder created;
				created.id = AllocateUniqueFolderId(app);
				if (created.id.empty())
				{
					app.folders = original_folders;
					app.chats = original_chats;
					app.status_line = "Could not split a mixed-machine workspace safely.";
					return false;
				}
				created.title = WorkspaceTitle(directory, folder_title);
				created.directory = directory;
				created.execution_host_id = host;
				app.folders.push_back(std::move(created));
				destination = std::prev(app.folders.end());
			}
			for (const std::size_t chat_index : chat_indices)
			{
				app.chats[chat_index].folder_id = destination->id;
				changed_chats.push_back(app.chats[chat_index]);
			}
			++split_count;
			changed = true;
		}
	}

	if (!changed) return true;
	if (!SaveChatHistories(app, changed_chats) || !ChatFolderStore::Save(app.data_root, app.folders))
	{
		const bool rollback_saved = SaveChatHistories(app, original_chats) &&
		                            ChatFolderStore::Save(app.data_root, original_folders);
		app.folders = original_folders;
		app.chats = original_chats;
		app.status_line = StatusAfterFolderDeletePreconditionFailure(
		    "Could not split a mixed-machine workspace safely", rollback_saved);
		return false;
	}

	app.status_line = split_count == 0
	    ? original_status_line
	    : "Separated chats that belonged to different workspace machines.";
	return true;
}

uam::WorkspaceFolderRecoveryPreview uam::PreviewUnsortedWorkspaceFolders(const AppState& app)
{
	WorkspaceFolderRecoveryPreview preview;
	for (const ChatSession& chat : app.chats)
	{
		if (!IsUnsortedChat(app, chat)) continue;

		const std::string host_id = uam::strings::NonEmptyOrFallback(
		    uam::strings::Trim(chat.execution_host_id), "local");
		const std::string recorded_directory = RecordedWorkspaceDirectory(chat);
		if (recorded_directory.empty())
		{
			preview.no_location.push_back(RecoveryChat(chat, "", "No workspace location recorded"));
			continue;
		}
		if (host_id != uam::execution_hosts::kLocalHostId)
		{
			const ExecutionHost* host = uam::execution_hosts::Find(
			    app.settings.execution_hosts, host_id);
			if (host == nullptr)
			{
				preview.unavailable.push_back(RecoveryChat(
				    chat, recorded_directory, "Execution computer no longer exists"));
				continue;
			}
			const std::string directory = uam::execution_hosts::NormalizeRemotePath(
			    host->platform, recorded_directory);
			if (!uam::execution_hosts::IsAbsoluteRemotePath(host->platform, directory))
			{
				preview.unavailable.push_back(RecoveryChat(
				    chat, recorded_directory, "Saved remote workspace path is not absolute"));
				continue;
			}
			const std::string key = WorkspaceOwnershipKey(app, host_id, directory);
			auto group = std::ranges::find_if(preview.groups, [&](const WorkspaceFolderRecoveryGroup& candidate) {
				return WorkspaceOwnershipKey(app, candidate.execution_host_id,
				                             candidate.directory) == key;
			});
			if (group == preview.groups.end())
			{
				WorkspaceFolderRecoveryGroup created;
				created.title = WorkspaceTitle(directory, "Workspace");
				created.directory = directory;
				created.execution_host_id = host_id;
				const auto existing = std::ranges::find_if(app.folders, [&](const ChatFolder& folder) {
					return WorkspaceOwnershipKey(app, folder.execution_host_id,
					                             folder.directory) == key;
				});
				if (existing != app.folders.end()) created.existing_folder_id = existing->id;
				preview.groups.push_back(std::move(created));
				group = std::prev(preview.groups.end());
			}
			group->chat_ids.push_back(chat.id);
			continue;
		}

		const std::filesystem::path local_directory =
		    uam::paths::ExpandTrimmedWorkspacePath(recorded_directory);

		std::error_code status_error;
		const std::filesystem::file_status status = std::filesystem::status(local_directory, status_error);
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

		const std::filesystem::path directory = uam::paths::NormalizeExistingPath(local_directory);
		const std::string key = WorkspaceOwnershipKey(
		    app, uam::execution_hosts::kLocalHostId, uam::paths::Utf8PathString(directory));
		auto group = std::ranges::find_if(preview.groups, [&](const WorkspaceFolderRecoveryGroup& candidate) {
			return WorkspaceOwnershipKey(app, candidate.execution_host_id,
			                             candidate.directory) == key;
		});
		if (group == preview.groups.end())
		{
			WorkspaceFolderRecoveryGroup created;
			created.title = WorkspaceTitle(directory);
			created.directory = uam::paths::Utf8PathString(directory);
			created.execution_host_id = uam::execution_hosts::kLocalHostId;
			const auto existing = std::ranges::find_if(app.folders, [&](const ChatFolder& folder) {
				return WorkspaceOwnershipKey(app, folder.execution_host_id,
				                             folder.directory) == key;
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
			folder.execution_host_id = group.execution_host_id;
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
	AcpSessionState* session = FindAcpSessionForChat(app, chat->id);
	if (!EnsureAcpStopProgress(app, chat->id))
	{
		return ChatProviderSwitchResult::RuntimeStopping;
	}
	if (session != nullptr && AcpSessionHasActiveTurn(*session))
	{
		return ChatProviderSwitchResult::ActiveRuntime;
	}
	if (ChatHasBusyCliTerminal(app, chat->id))
	{
		return ChatProviderSwitchResult::ActiveRuntime;
	}
	const bool remote_session = chat->execution_host_id != uam::execution_hosts::kLocalHostId;
	if (session != nullptr && session->running && remote_session &&
	    !StopAcpSession(app, chat->id))
	{
		return ChatProviderSwitchResult::RuntimeStopping;
	}
	std::string hydration_warning;
	if (!ChatRepository::HydrateChatMessages(app.data_root, *chat, &hydration_warning))
	{
		app.status_line = "Could not change provider because the complete chat history could not be loaded";
		if (!hydration_warning.empty())
		{
			app.status_line += ": " + hydration_warning;
		}
		return ChatProviderSwitchResult::SaveFailed;
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
	for (Goal& goal : chat->goals)
	{
		if (goal.execution_owner != "provider")
		{
			continue;
		}
		if (uam::strings::IsBlank(provider->native_goal_command))
		{
			goal.execution_owner = "uam";
			goal.provider_command.clear();
		}
		else
		{
			goal.provider_command = uam::strings::Trim(provider->native_goal_command);
		}
	}
	chat->native_session_id.clear();
	chat->computer_use_enabled = false;
	chat->computer_use_target_kind = "window";
	chat->computer_use_target_id.clear();
	chat->computer_use_target_process_id.clear();
	chat->computer_use_target_title.clear();
	chat->computer_use_target_input_mode.clear();
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

	(void)ComputerUseService::SetControlState(app, chat->id, "stopped");
	if (session != nullptr && session->running) (void)StopAcpSession(app, chat->id);
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
	ExpandDependentChatIds(app, target_chat_ids, deleted_chat_ids);
	if (target_chat_ids.empty())
	{
		app.status_line = "Chat id is required.";
		return false;
	}
	if (HasActiveDependentAgentRun(app, deleted_chat_ids))
	{
		app.status_line = "Wait for the selected managed agent run to finish or cancel it first.";
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
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, id);
		if (session != nullptr && session->remote_stop_unconfirmed)
		{
			app.status_line = uam::strings::NonEmptyOrFallback(
			    session->last_error, "Remote stop could not be confirmed.");
			return false;
		}
		if (BeginIdleRemoteStopForDeletion(app, id))
		{
			app.status_line = "The remote runtime is stopping. Retry deletion after it finishes.";
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

	if (!BeginDeletionTransaction(app, deleted_chats, {}))
	{
		app.status_line = "Failed to create a durable chat deletion transaction.";
		return false;
	}
	StopChatRuntimes(app, deleted_chats);
	DeletionIntent intent{target_chat_ids, {}};
	std::vector<ChatSession> next_chats;
	std::vector<ChatFolder> next_folders;
	bool native_cleanup_failed = false;
	if (!CompleteDeletionTransaction(app, intent, next_chats, next_folders, deleted_chats, native_cleanup_failed))
	{
		app.chats = std::move(next_chats);
		ApplyDeletedChatsToUiState(app, deleted_chat_ids, selected_chat_id,
		                          previous_selected_chat_index, true);
		(void)PersistenceCoordinator().SaveSettings(app);
		app.status_line = "Chat deletion is committed and hidden. Disk cleanup will finish safely on restart.";
		return true;
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

bool uam::RecoverPendingDeletionTransaction(AppState& app)
{
	const std::filesystem::path intent_path = DeletionIntentPath(app.data_root);
	const bool has_intent = uam::paths::PathExistsNoThrow(intent_path) || uam::paths::PathExistsNoThrow(uam::io::MakeBackupPath(intent_path));
	if (!has_intent)
	{
		const std::filesystem::path staging_root = DeletionStagingRoot(app.data_root);
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(staging_root, error);
		if (!error && std::filesystem::exists(status) && !uam::paths::IsLinkOrReparsePointNoThrow(staging_root)) (void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staging_root);
		return true;
	}

	DeletionIntent intent;
	if (!LoadDeletionIntent(app.data_root, intent))
	{
		app.status_line = "A pending deletion transaction is corrupt. UAM left the recovery evidence untouched and will not open the chat database.";
		return false;
	}

	AppState recovery;
	recovery.data_root = app.data_root;
	recovery.settings = app.settings;
	recovery.provider_profiles = app.provider_profiles;
	recovery.chats = ChatRepository::LoadLocalChats(app.data_root);
	recovery.folders = ChatFolderStore::Load(app.data_root);
	std::vector<ChatSession> next_chats;
	std::vector<ChatFolder> next_folders;
	std::vector<ChatSession> deleted_chats;
	bool native_cleanup_failed = false;
	if (!CompleteDeletionTransaction(recovery, intent, next_chats, next_folders, deleted_chats, native_cleanup_failed))
	{
		app.status_line = "A pending deletion transaction could not be completed. Recovery evidence was preserved and UAM will not open a partial chat database.";
		return false;
	}
	app.status_line = "Recovered and completed an interrupted deletion transaction.";
	if (native_cleanup_failed) app.status_line += " Some provider-native history could not be removed, but durable tombstones prevent reimport.";
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

	DeletedChatsSelection deleted = CollectChatsInFolder(app, target_folder_id);
	if (HasActiveDependentAgentRun(app, deleted.ids))
	{
		app.status_line = "Wait for managed agent runs in this folder to finish or cancel them first.";
		return false;
	}
	for (const std::string& chat_id : deleted.ids)
	{
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, chat_id);
		if (session != nullptr && session->remote_stop_unconfirmed)
		{
			app.status_line = uam::strings::NonEmptyOrFallback(
			    session->last_error, "Remote stop could not be confirmed.");
			return false;
		}
	}
	bool remote_stop_started = false;
	for (const std::string& chat_id : deleted.ids)
		remote_stop_started = BeginIdleRemoteStopForDeletion(app, chat_id) || remote_stop_started;
	if (remote_stop_started)
	{
		app.status_line = "Remote runtimes in this folder are stopping. Retry deletion after they finish.";
		return false;
	}
	if (std::ranges::any_of(deleted.ids, [&app](const std::string& chat_id)
	    { return ChatHasDeletionBlockingRuntime(app, chat_id); }))
	{
		app.status_line = "Cannot delete a folder while one of its chats has a running runtime.";
		return false;
	}
	if (!HydrateDeletedChatsForRollback(app.data_root, deleted.chats, deleted.ids))
	{
		app.status_line = "Failed to prepare folder chat history for safe deletion.";
		return false;
	}
	if (!BeginDeletionTransaction(app, deleted.chats, target_folder_id))
	{
		app.status_line = "Failed to create a durable folder deletion transaction.";
		return false;
	}
	StopChatRuntimes(app, deleted.chats);
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;
	DeletionIntent intent;
	intent.folder_id = target_folder_id;
	intent.chat_ids.assign(deleted.ids.begin(), deleted.ids.end());
	std::ranges::sort(intent.chat_ids);
	std::vector<ChatSession> next_chats;
	std::vector<ChatFolder> next_folders;
	std::vector<ChatSession> deleted_chats;
	bool native_cleanup_failed = false;
	if (!CompleteDeletionTransaction(app, intent, next_chats, next_folders, deleted_chats, native_cleanup_failed))
	{
		app.chats = std::move(next_chats);
		ApplyDeletedChatsToUiState(app, deleted.ids, selected_chat_id,
		                          previous_selected_chat_index, !deleted.chats.empty());
		if (uam::strings::TrimmedEquals(app.new_chat_folder_id, target_folder_id))
			app.new_chat_folder_id.clear();
		app.folders = std::move(next_folders);
		(void)PersistenceCoordinator().SaveSettings(app);
		app.status_line = "Folder deletion is committed and hidden. Disk cleanup will finish safely on restart.";
		return true;
	}

	app.chats = std::move(next_chats);
	ApplyDeletedChatsToUiState(app, deleted.ids, selected_chat_id, previous_selected_chat_index, !deleted.chats.empty());

	if (uam::strings::TrimmedEquals(app.new_chat_folder_id, target_folder_id))
	{
		app.new_chat_folder_id.clear();
	}

	const bool settings_saved = PersistenceCoordinator().SaveSettings(app);

	app.folders = std::move(next_folders);

	app.status_line = StatusAfterFolderDelete(deleted_chats.size(), settings_saved, native_cleanup_failed);
	return true;
}

bool CreateFolder(uam::AppState& app, const std::string& title,
	const std::string& directory, std::string* created_folder_id,
	const std::string& execution_host_id)
{
	if (created_folder_id != nullptr)
	{
		created_folder_id->clear();
	}

	FolderInput input;
	if (!ValidateFolderInput(app, title, directory, execution_host_id, input))
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

	ChatFolder& folder = app.folders[folder_index];
	FolderInput input;
	if (!ValidateFolderInput(app, title, directory,
	        uam::strings::NonEmptyOrFallback(folder.execution_host_id, "local"), input))
	{
		return false;
	}

	const ChatFolder original = folder;
	const std::vector<ChatSession> original_chats = app.chats;
	const std::string original_status_line = app.status_line;
	const bool directory_changed = !FolderDirectoryMatches(original.directory, input.directory);
	if (directory_changed && original.execution_host_id != uam::execution_hosts::kLocalHostId)
	{
		app.status_line = "A remote workspace directory cannot be changed. Create a new workspace instead.";
		return false;
	}
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
