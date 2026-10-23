#include "app/agent_run_scheduler.h"
#include "chat_lifecycle_service.h"

#include "core/chat_import_utils.h"

#include "app/chat_domain_service.h"
#include "app/computer_use_service.h"
#include "app/git_worktree_service.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/startup_native_import_service.h"
#include "common/chat/chat_branching.h"
#include "common/chat/conversation_handoff.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/provider_chat_defaults.h"
#include "common/memory/memory_levels.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <functional>
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

	bool ChatHasDeletionBlockingRuntime(const uam::AppState& app, const std::string& chat_id, const ChatSession* known_chat = nullptr)
	{
		if (uam::AgentRunScheduler::HasPendingInitialMention(app, chat_id))
			return true;
		for (const uam::CliContextPreparationTask& task : app.cli_context_preparation_tasks)
		{
			if (task.state && task.state->chat_id == chat_id && !task.state->finished.load()) return true;
		}
		const ChatSession* chat = known_chat != nullptr ? known_chat : ChatDomainService().FindChatById(app, chat_id);
		if (chat != nullptr && chat->side_cleanup_stop_finished)
		{
			if (!chat->side_cleanup_stop_finished->load()) return true;
			for (const uam::AsyncAcpProcessStopTask& task : app.acp_process_stop_tasks)
				if (task.finished == chat->side_cleanup_stop_finished && task.observe_exit && !task.result->exit_confirmed) return true;
		}
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

	bool BeginIdleRemoteStopForDeletion(uam::AppState& app, const std::string& chat_id, const ChatSession* known_chat = nullptr)
	{
		const ChatSession* chat = known_chat != nullptr ? known_chat : ChatDomainService().FindChatById(app, chat_id);
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

	bool IsTerminalAgentRun(const AgentRun& run)
	{
		return run.status == "completed" || run.status == "failed" ||
		       run.status == "cancelled" || run.status == "interrupted";
	}

	void ExpandDependentChatIds(const uam::AppState& app,
	                            std::vector<std::string>& target_chat_ids,
	                            std::unordered_set<std::string>& deleted_chat_ids)
	{
		std::unordered_map<std::string, const ChatSession*> chats_by_id;
		std::unordered_map<std::string, std::vector<std::string>> dependents_by_owner;
		for (const ChatSession& chat : app.chats)
		{
			chats_by_id.emplace(uam::strings::Trim(chat.id), &chat);
			const std::string owner = uam::strings::Trim(chat.goal_owner_chat_id);
			if (!owner.empty()) dependents_by_owner[owner].push_back(chat.id);
			const std::string parent = uam::strings::Trim(chat.temporary_parent_chat_id);
			if (!parent.empty() && parent != owner) dependents_by_owner[parent].push_back(chat.id);
		}
		for (const AgentRun& run : app.agent_runs)
		{
			const std::string owner = uam::strings::Trim(run.root_chat_id);
			const std::string transcript_id = uam::strings::Trim(run.transcript_chat_id);
			const std::unordered_map<std::string, const ChatSession*>::const_iterator transcript = chats_by_id.find(transcript_id);
			if (!owner.empty() && transcript != chats_by_id.end() && uam::strings::TrimmedEquals(transcript->second->agent_run_id, run.id))
				dependents_by_owner[owner].push_back(transcript->second->id);
		}
		for (std::size_t owner_index = 0; owner_index < target_chat_ids.size(); ++owner_index)
		{
			const std::unordered_map<std::string, std::vector<std::string>>::const_iterator dependents = dependents_by_owner.find(target_chat_ids[owner_index]);
			if (dependents == dependents_by_owner.end()) continue;
			for (const std::string& id : dependents->second)
			{
				if (deleted_chat_ids.insert(id).second) target_chat_ids.push_back(id);
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

	bool DeletionRewritesUnconfirmedInitialRoot(const uam::AppState& app, const std::unordered_set<std::string>& deleted_ids)
	{
		std::unordered_map<std::string, const ChatSession*> protected_roots;
		for (const ChatSession& chat : app.chats)
			if (!deleted_ids.contains(chat.id) && uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, chat))
				protected_roots.emplace(chat.id, &chat);
		if (protected_roots.empty())
			return false;

		// Match the existing deletion normalization exactly without copying transcript bodies.
		// These are the fields read by ChatBranching and its native-history identity lookup.
		std::vector<ChatSession> lineage;
		lineage.reserve(app.chats.size());
		for (const ChatSession& chat : app.chats)
		{
			ChatSession projected;
			projected.id = chat.id;
			projected.parent_chat_id = chat.parent_chat_id;
			projected.branch_root_chat_id = chat.branch_root_chat_id;
			projected.branch_from_message_index = chat.branch_from_message_index;
			projected.provider_id = chat.provider_id;
			projected.native_session_id = chat.native_session_id;
			projected.execution_host_id = chat.execution_host_id;
			projected.workspace_directory = chat.workspace_directory;
			projected.workspace_worktree_directory = chat.workspace_worktree_directory;
			lineage.push_back(std::move(projected));
		}
		ChatBranching::ReparentChildrenAfterDeletes(lineage, deleted_ids);
		std::erase_if(lineage, [&](const ChatSession& chat) { return deleted_ids.contains(chat.id); });
		ChatBranching::Normalize(lineage);
		for (const ChatSession& chat : lineage)
		{
			const auto found = protected_roots.find(chat.id);
			if (found == protected_roots.end())
				continue;
			const ChatSession& original = *found->second;
			if (chat.parent_chat_id != original.parent_chat_id || chat.branch_root_chat_id != original.branch_root_chat_id || chat.branch_from_message_index != original.branch_from_message_index)
				return true;
		}
		return false;
	}

	void CaptureDeletionChatLocation(const uam::AppState& app, ChatSession& snapshot)
	{
		const std::string resolved = uam::ResolvedNativeSessionIdForChat(app, snapshot);
		if (!resolved.empty()) snapshot.native_session_id = resolved;
		if (uam::paths::IsControllerLocalWorkspace(snapshot) &&
		    uam::strings::IsBlank(snapshot.workspace_directory) && uam::strings::IsBlank(snapshot.workspace_worktree_directory))
			snapshot.workspace_directory = uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, snapshot));
	}

	DeletedChatsSelection CollectChatsInFolder(const uam::AppState& app,
	                                           const std::unordered_set<std::string>& folder_ids)
	{
		DeletedChatsSelection selection;
		selection.ids.reserve(app.chats.size());
		std::vector<std::string> target_chat_ids;
		for (const ChatSession& chat : app.chats)
		{
			if (folder_ids.contains(uam::strings::Trim(chat.folder_id)) && selection.ids.insert(chat.id).second)
				target_chat_ids.push_back(chat.id);
		}
		ExpandDependentChatIds(app, target_chat_ids, selection.ids);
		selection.chats.reserve(selection.ids.size());
		for (const ChatSession& chat : app.chats)
			if (selection.ids.contains(chat.id))
			{
				selection.chats.push_back(chat);
				CaptureDeletionChatLocation(app, selection.chats.back());
			}
		return selection;
	}

	bool StopChatRuntimes(uam::AppState& app, const std::string& chat_id)
	{
		const bool acp_stopped = StopAcpSession(app, chat_id);
		const bool cli_stopped = uam::StopAndEraseCliTerminalForChat(app, chat_id, false);
		return acp_stopped && cli_stopped && !ChatHasDeletionBlockingRuntime(app, chat_id);
	}

	bool StopChatRuntimes(uam::AppState& app, const std::vector<ChatSession>& chats)
	{
		bool stopped = true;
		for (const ChatSession& chat : chats)
			stopped = StopChatRuntimes(app, chat.id) && stopped;
		return stopped;
	}

	void ForgetDeletedChatReferences(uam::AppState& app, const std::unordered_set<std::string>& deleted_chat_ids)
	{
		if (deleted_chat_ids.empty())
		{
			return;
		}

		for (const std::string& deleted_chat_id : deleted_chat_ids)
		{
			app.resolved_native_sessions_by_chat_id.erase(deleted_chat_id);
			app.chats_with_unseen_updates.erase(deleted_chat_id);
			app.collapsed_branch_chat_ids.erase(deleted_chat_id);
			app.filtered_chat_ids.erase(deleted_chat_id);
		}

		std::unordered_set<std::string> surviving_chat_ids;
		surviving_chat_ids.reserve(app.chats.size());
		for (const ChatSession& chat : app.chats) surviving_chat_ids.insert(chat.id);
		for (auto it = app.resolved_native_sessions_by_chat_id.begin(); it != app.resolved_native_sessions_by_chat_id.end();)
		{
			if (ContainsDeletedChatId(deleted_chat_ids, it->first) ||
			    !surviving_chat_ids.contains(it->first))
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
		std::vector<std::string> folder_ids;
		std::unordered_set<std::string> retained_native_keys;
		std::vector<uam::NativeDeletionOwner> retained_native_owners;
		bool ownership_finalized = true;
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
		const auto folder_ids = parsed.find("folderIds");
		if (!parsed.is_object() || version == parsed.end() || !version->is_number_integer() ||
		    (*version != 1 && *version != 2) || complete == parsed.end() || !complete->is_boolean() ||
		    !complete->get<bool>() || chat_ids == parsed.end() || !chat_ids->is_array() ||
		    (folder_id != parsed.end() && !folder_id->is_string()) ||
		    (folder_ids != parsed.end() && !folder_ids->is_array()) ||
		    (*version == 2 && folder_ids == parsed.end()))
		{
			return false;
		}

		DeletionIntent decoded;
		std::unordered_set<std::string> seen_chat_ids;
		for (const nlohmann::json& value : *chat_ids)
		{
			if (!value.is_string()) return false;
			const std::string id = uam::strings::Trim(value.get_ref<const std::string&>());
			if (!uam::chat_ids::IsSafeStorageChatId(id) || !seen_chat_ids.insert(id).second) return false;
			decoded.chat_ids.push_back(id);
		}
		if (folder_ids != parsed.end())
		{
			std::unordered_set<std::string> seen;
			for (const nlohmann::json& value : *folder_ids)
			{
				if (!value.is_string()) return false;
				const std::string id = uam::strings::Trim(value.get_ref<const std::string&>());
				if (id.empty() || !seen.insert(id).second) return false;
				decoded.folder_ids.push_back(id);
			}
		}
		else if (folder_id != parsed.end())
		{
			const std::string id = uam::strings::Trim(folder_id->get_ref<const std::string&>());
			if (!id.empty()) decoded.folder_ids.push_back(id);
		}
		if (decoded.chat_ids.empty() && decoded.folder_ids.empty()) return false;
		if (parsed.contains("retainedNativeKeys") || parsed.contains("retainedNativeOwners") || parsed.contains("ownershipFinalized"))
		{
			if (!parsed.contains("retainedNativeKeys") || !parsed["retainedNativeKeys"].is_array() ||
			    !parsed.contains("retainedNativeOwners") || !parsed["retainedNativeOwners"].is_array() ||
			    !parsed.contains("ownershipFinalized") || !parsed["ownershipFinalized"].is_boolean()) return false;
			for (const nlohmann::json& key : parsed["retainedNativeKeys"])
			{
				if (!key.is_string()) return false;
				decoded.retained_native_keys.insert(key.get<std::string>());
			}
			for (const nlohmann::json& owner : parsed["retainedNativeOwners"])
			{
				if (!owner.is_object() || !owner.contains("directory") || !owner["directory"].is_string() ||
				    !owner.contains("sessionId") || !owner["sessionId"].is_string()) return false;
				if (owner.contains("unresolvedSession") && !owner["unresolvedSession"].is_boolean()) return false;
				decoded.retained_native_owners.push_back({owner["directory"].get<std::string>(), owner["sessionId"].get<std::string>(), owner.value("unresolvedSession", false)});
			}
			decoded.ownership_finalized = parsed["ownershipFinalized"].get<bool>();
		}
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

	void EncodeDeletionOwnership(nlohmann::json& encoded, const std::unordered_set<std::string>& keys,
	                             const std::vector<uam::NativeDeletionOwner>& owners, bool finalized)
	{
		std::vector<std::string> ordered_keys(keys.begin(), keys.end());
		std::ranges::sort(ordered_keys);
		encoded["retainedNativeKeys"] = ordered_keys;
		encoded["retainedNativeOwners"] = nlohmann::json::array();
		for (const uam::NativeDeletionOwner& owner : owners)
			encoded["retainedNativeOwners"].push_back({{"directory", owner.workspace_directory}, {"sessionId", owner.native_session_id}, {"unresolvedSession", owner.unresolved_session}});
		encoded["ownershipFinalized"] = finalized;
	}

	bool BeginDeletionTransaction(const uam::AppState& app, const std::vector<ChatSession>& deleted_chats, const std::vector<std::string>& folder_ids,
	                              const std::unordered_set<std::string>& retained_keys,
	                              const std::vector<uam::NativeDeletionOwner>& retained_owners)
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
			// Hydrate only the current rollback snapshot, then release its transcript.
			if (chat.messages_loaded)
			{
				if (!ChatRepository::SaveChatIfAbsent(staging_root, chat)) return false;
			}
			else
			{
				ChatSession snapshot = chat;
				if (!ChatRepository::HydrateChatMessages(app.data_root, snapshot)) return false;
				snapshot.native_session_id = chat.native_session_id;
				if (!ChatRepository::SaveChatIfAbsent(staging_root, snapshot)) return false;
			}
		}
		if (!folder_ids.empty() && !ChatFolderStore::Save(staging_root, app.folders)) return false;

		nlohmann::json encoded = {
		    {"version", 2},
		    {"complete", true},
		    {"folderIds", folder_ids},
		    {"chatIds", nlohmann::json::array()},
		};
		for (const ChatSession& chat : deleted_chats) encoded["chatIds"].push_back(chat.id);
		EncodeDeletionOwnership(encoded, retained_keys, retained_owners, false);
		if (!uam::io::WriteTextFileWithBackup(intent_path, encoded.dump()))
		{
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staging_root);
			return false;
		}
		return true;
	}

	std::vector<ChatSession> DeletedChatSnapshots(const std::filesystem::path& data_root, const DeletionIntent& intent, const std::vector<ChatSession>& current_chats)
	{
		std::vector<ChatSession> staged = ChatRepository::LoadLocalChatSummaries(DeletionStagingRoot(data_root));
		std::unordered_map<std::string, const ChatSession*> snapshot_by_id;
		for (const ChatSession& chat : current_chats) snapshot_by_id.emplace(chat.id, &chat);
		for (const ChatSession& chat : staged) snapshot_by_id[chat.id] = &chat;
		std::vector<ChatSession> result;
		result.reserve(intent.chat_ids.size());
		for (const std::string& id : intent.chat_ids)
		{
			const auto found = snapshot_by_id.find(id);
			const ChatSession* snapshot = found == snapshot_by_id.end() ? nullptr : found->second;
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

	/// <summary>Retains native ownership without copying unrelated transcript bodies.</summary>
	void CollectRetainedDeletionOwnership(const uam::AppState& app, const DeletionIntent& intent,
	                                     std::unordered_set<std::string>& native_keys,
	                                     std::unordered_set<std::string>& workspace_keys,
	                                     std::vector<uam::NativeDeletionOwner>* native_owners = nullptr)
	{
		native_keys.clear();
		workspace_keys.clear();
		if (native_owners != nullptr) native_owners->clear();
		// Resolve repeated workspace paths once per collection, never across operations.
		std::map<std::pair<std::string, std::string>, std::string> workspace_cache;
		const std::function<void(const std::string&, const std::string&)> retain_workspace =
		    [&app, &workspace_keys, &workspace_cache](const std::string& host, const std::string& directory)
		{
			const std::pair<std::string, std::string> location(host, directory);
			std::map<std::pair<std::string, std::string>, std::string>::iterator found = workspace_cache.find(location);
			if (found == workspace_cache.end())
				found = workspace_cache.emplace(location, uam::paths::WorkspaceOwnershipKey(app, host, directory)).first;
			workspace_keys.insert(found->second);
		};
		const std::unordered_set<std::string> deleted_ids(intent.chat_ids.begin(), intent.chat_ids.end());
		const std::unordered_set<std::string> deleted_folders(intent.folder_ids.begin(), intent.folder_ids.end());
		for (const ChatSession& chat : app.chats)
		{
			if (deleted_ids.contains(chat.id)) continue;
			if (!uam::strings::IsBlank(chat.native_session_id))
				native_keys.insert(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat));
			ChatSession linked;
			linked.provider_id = chat.provider_id;
			linked.execution_host_id = chat.execution_host_id;
			linked.workspace_directory = chat.workspace_directory;
			linked.workspace_worktree_directory = chat.workspace_worktree_directory;
			linked.folder_id = chat.folder_id;
			if (uam::paths::IsControllerLocalWorkspace(chat) && uam::strings::IsBlank(linked.workspace_directory) &&
			    uam::strings::IsBlank(linked.workspace_worktree_directory))
				linked.workspace_directory = uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
			linked.native_session_id = chat.native_session_id;
			if (!uam::strings::IsBlank(linked.native_session_id)) native_keys.insert(uam::chat_identity::NativeIdentityKeyForHistoryImport(linked));
			const std::string resolved = uam::ResolvedNativeSessionIdForChat(app, chat);
			if (!resolved.empty())
			{
				linked.native_session_id = resolved;
				native_keys.insert(uam::chat_identity::NativeIdentityKeyForHistoryImport(linked));
			}
			const std::string& directory = uam::strings::IsBlank(chat.workspace_worktree_directory)
			    ? chat.workspace_directory : chat.workspace_worktree_directory;
			if (!uam::strings::IsBlank(directory))
				retain_workspace(chat.execution_host_id, directory);
			if (native_owners != nullptr && uam::paths::IsControllerLocalWorkspace(chat))
			{
				uam::NativeDeletionOwner owner;
				owner.workspace_directory = directory.empty()
				    ? uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, chat)) : directory;
				if (ProviderRuntime::UsesNativeOverlayHistory(ProviderResolutionService().ProviderForChatOrDefault(app, chat)))
					{
					owner.native_session_id = chat.native_session_id;
					owner.unresolved_session = uam::strings::IsBlank(chat.native_session_id) && resolved.empty();
					if (!resolved.empty() && resolved != chat.native_session_id)
						native_owners->push_back({owner.workspace_directory, resolved});
				}
				native_owners->push_back(std::move(owner));
			}
		}
		for (const ChatFolder& folder : app.folders)
			if (!deleted_folders.contains(uam::strings::Trim(folder.id)) && !uam::strings::IsBlank(folder.directory))
			{
				retain_workspace(folder.execution_host_id, folder.directory);
				if (native_owners != nullptr && uam::paths::IsControllerLocalWorkspace(folder))
				{
					uam::NativeDeletionOwner owner;
					owner.workspace_directory = folder.directory;
					native_owners->push_back(std::move(owner));
				}
			}
	}

	std::string NativeDeletionStore(const std::string& directory, std::unordered_map<std::string, std::string>& cache, bool allow_resolution = true)
	{
		if (directory.empty()) return {};
		const std::unordered_map<std::string, std::string>::const_iterator found = cache.find(directory);
		if (found != cache.end()) return found->second;
		if (!allow_resolution) return {};
		const std::optional<std::filesystem::path> store = AppPaths::ResolveGeminiProjectTmpDir(uam::paths::ExpandTrimmedWorkspacePath(directory));
		const std::string key = store ? uam::paths::Utf8PathString(*store) : std::string{};
		cache.emplace(directory, key);
		return key;
	}

	void CollectRetainedNativeStores(const std::vector<uam::NativeDeletionOwner>& owners,
	                                std::unordered_map<std::string, std::string>& cache,
	                                std::unordered_set<std::string>& stores,
	                                std::unordered_set<std::string>& sessions, bool allow_resolution = true)
	{
		for (const uam::NativeDeletionOwner& owner : owners)
		{
			const std::string store = NativeDeletionStore(owner.workspace_directory, cache, allow_resolution);
			if (store.empty()) continue;
			stores.insert(store);
			if (!uam::strings::IsBlank(owner.native_session_id))
				sessions.insert(store + "\n" + uam::strings::Trim(owner.native_session_id));
		}
	}

	/// <summary>Resolves physical shared-session ownership on the staging worker, keeping opaque import keys intact.</summary>
	void ProtectSharedNativeDeletionKeys(const uam::AppState& app, const std::vector<ChatSession>& deleted_chats,
	                                    const std::vector<uam::NativeDeletionOwner>& owners, std::unordered_set<std::string>& retained_keys,
	                                    std::unordered_map<std::string, std::string>& cache, bool allow_resolution = true)
	{
		std::unordered_set<std::string> stores;
		std::unordered_set<std::string> sessions;
		CollectRetainedNativeStores(owners, cache, stores, sessions, allow_resolution);
		for (const ChatSession& chat : deleted_chats)
		{
			if (!uam::paths::IsControllerLocalWorkspace(chat) ||
			    !ProviderRuntime::UsesNativeOverlayHistory(ProviderResolutionService().ProviderForChatOrDefault(app, chat))) continue;
			const std::string directory = uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
			if (sessions.contains(NativeDeletionStore(directory, cache, allow_resolution) + "\n" + uam::strings::Trim(chat.native_session_id)) ||
			    (!allow_resolution && std::ranges::any_of(owners, [&](const uam::NativeDeletionOwner& owner)
			    {
				    return !uam::strings::IsBlank(owner.native_session_id) && uam::strings::TrimmedEquals(owner.native_session_id, chat.native_session_id) &&
				        NativeDeletionStore(owner.workspace_directory, cache, false).empty();
			    })))
				retained_keys.insert(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat));
		}
	}

	bool CommitDeletionMetadata(uam::AppState& app,
	                            const DeletionIntent& intent,
	                            std::vector<ChatSession>& next_chats,
	                            std::vector<ChatFolder>& next_folders,
	                            const std::vector<ChatSession>& deleted_chats,
	                            const std::unordered_set<std::string>& retained_native_keys)
	{
		const std::unordered_set<std::string> deleted_ids(intent.chat_ids.begin(), intent.chat_ids.end());
		next_chats = app.chats;
		ChatBranching::ReparentChildrenAfterDeletes(next_chats, deleted_ids);
		std::erase_if(next_chats, [&](const ChatSession& chat) { return deleted_ids.contains(chat.id); });
		ChatBranching::Normalize(next_chats);
		next_folders = app.folders;
		if (!intent.folder_ids.empty())
		{
			const std::unordered_set<std::string> folder_ids(intent.folder_ids.begin(), intent.folder_ids.end());
			std::erase_if(next_folders, [&](const ChatFolder& folder) { return folder_ids.contains(uam::strings::Trim(folder.id)); });
		}

		std::vector<std::string> added_tombstones;
		if (!ChatHistorySyncService().AddNativeImportTombstones(app.data_root, deleted_chats, added_tombstones, retained_native_keys)) return false;

		// Only branch metadata can change on surviving chats. Avoid rewriting unrelated history.
		std::unordered_map<std::string, const ChatSession*> original_by_id;
		for (const ChatSession& chat : app.chats) original_by_id.emplace(chat.id, &chat);
		for (const ChatSession& chat : next_chats)
		{
			const ChatSession& original = *original_by_id.at(chat.id);
			if (chat.parent_chat_id == original.parent_chat_id &&
			    chat.branch_root_chat_id == original.branch_root_chat_id &&
			    chat.branch_from_message_index == original.branch_from_message_index) continue;
			if (!ChatRepository::SaveChat(app.data_root, chat)) return false;
		}

		if (!intent.folder_ids.empty())
		{
			if (!ChatFolderStore::Save(app.data_root, next_folders)) return false;
		}

		for (const ChatSession& chat : deleted_chats)
			if (!uam::StageCliProviderContextCleanup(app, chat)) return false;
		return true;
	}

	bool CommitDeletionMetadataInPlace(uam::AppState& app,
	                                   const DeletionIntent& intent,
	                                   std::vector<ChatFolder>& next_folders,
	                                   const std::vector<ChatSession>& deleted_chats,
	                            const std::unordered_set<std::string>& retained_native_keys)
	{
		struct BranchMetadata
		{
			std::string parent_chat_id;
			std::string branch_root_chat_id;
			int branch_from_message_index = -1;
		};
		const std::unordered_set<std::string> deleted_ids(intent.chat_ids.begin(), intent.chat_ids.end());
		std::unordered_map<std::string, BranchMetadata> original_branch_metadata;
		original_branch_metadata.reserve(app.chats.size());
		for (const ChatSession& chat : app.chats)
			original_branch_metadata.emplace(chat.id, BranchMetadata{chat.parent_chat_id, chat.branch_root_chat_id, chat.branch_from_message_index});

		bool saved = true;
		std::vector<std::string> added_tombstones;
		const bool tombstones_saved = ChatHistorySyncService().AddNativeImportTombstones(app.data_root, deleted_chats, added_tombstones, retained_native_keys);
		saved = tombstones_saved && saved;

		ChatBranching::ReparentChildrenAfterDeletes(app.chats, deleted_ids);
		std::erase_if(app.chats, [&deleted_ids](const ChatSession& chat) { return deleted_ids.contains(chat.id); });
		ChatBranching::Normalize(app.chats);
		for (const ChatSession& chat : app.chats)
		{
			if (!tombstones_saved) break;
			const std::unordered_map<std::string, BranchMetadata>::const_iterator original = original_branch_metadata.find(chat.id);
			if (original != original_branch_metadata.end() && chat.parent_chat_id == original->second.parent_chat_id &&
			    chat.branch_root_chat_id == original->second.branch_root_chat_id &&
			    chat.branch_from_message_index == original->second.branch_from_message_index) continue;
			if (!ChatRepository::SaveChat(app.data_root, chat)) saved = false;
		}

		next_folders = app.folders;
		if (!intent.folder_ids.empty())
		{
			const std::unordered_set<std::string> folder_ids(intent.folder_ids.begin(), intent.folder_ids.end());
			std::erase_if(next_folders, [&folder_ids](const ChatFolder& folder) { return folder_ids.contains(uam::strings::Trim(folder.id)); });
			if (!ChatFolderStore::Save(app.data_root, next_folders)) saved = false;
			app.folders = next_folders;
		}
		if (tombstones_saved && saved)
			for (const ChatSession& chat : deleted_chats)
				saved = uam::StageCliProviderContextCleanup(app, chat) && saved;
		return saved;
	}

	bool CleanupDeletionTransaction(uam::AppState& app, const DeletionIntent& intent, const std::vector<ChatSession>& deleted_chats, bool& native_cleanup_failed,
	                                const std::unordered_set<std::string>& retained_native_keys,
	                                const std::unordered_set<std::string>& retained_workspace_keys,
	                                const std::vector<uam::NativeDeletionOwner>& retained_native_owners,
	                                const std::function<void(std::size_t, std::size_t, const std::string&)>& progress = {})
	{
		std::size_t completed_items = 0;
		const std::size_t total_items = deleted_chats.size() + intent.folder_ids.size();
		const std::function<void(const std::string&)> complete_item = [&](const std::string& label)
		{
			++completed_items;
			if (progress) progress(completed_items, total_items, label);
		};
		for (const std::string& id : intent.chat_ids)
		{
			if (ChatRepository::DeleteChatStorageFiles(app.data_root, id).Failed()) return false;
		}

		// Gemini resolves related working directories to one physical project store.
		std::unordered_map<std::string, std::string> stores_by_directory;
		std::unordered_set<std::string> retained_stores;
		std::unordered_set<std::string> retained_sessions;
		CollectRetainedNativeStores(retained_native_owners, stores_by_directory, retained_stores, retained_sessions);
		native_cleanup_failed = false;
		if (!intent.folder_ids.empty())
		{
			const std::vector<ChatFolder> staged_folders = ChatFolderStore::Load(DeletionStagingRoot(app.data_root));
			const std::unordered_set<std::string> folder_ids(intent.folder_ids.begin(), intent.folder_ids.end());
			for (const ChatFolder& folder : staged_folders)
			{
				if (!folder_ids.contains(uam::strings::Trim(folder.id))) continue;
				if (retained_workspace_keys.contains(uam::paths::WorkspaceOwnershipKey(app, folder.execution_host_id, folder.directory)) ||
				    (uam::paths::IsControllerLocalWorkspace(folder) && retained_stores.contains(NativeDeletionStore(folder.directory, stores_by_directory))))
				{
					complete_item(folder.title);
					continue;
				}
				std::error_code error;
				ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(app, folder, &error);
				native_cleanup_failed = native_cleanup_failed || static_cast<bool>(error);
				if (!error) complete_item(folder.title);
			}
		}

		if (native_cleanup_failed) return false;
		for (const ChatSession& chat : deleted_chats)
		{
			if (retained_native_keys.contains(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat)))
			{
				complete_item(chat.title);
				continue;
			}
			const std::string directory = uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
			if (uam::paths::IsControllerLocalWorkspace(chat) &&
			    retained_sessions.contains(NativeDeletionStore(directory, stores_by_directory) + "\n" + uam::strings::Trim(chat.native_session_id)))
			{
				complete_item(chat.title);
				continue;
			}
			std::error_code error;
			DeleteNativeHistoryForChatIfNeeded(app, chat, &error);
			native_cleanup_failed = native_cleanup_failed || static_cast<bool>(error);
			if (!error) complete_item(chat.title);
		}
		if (native_cleanup_failed) return false;
		return RemoveDeletionTransactionFiles(app.data_root);
	}

	bool CompleteDeletionTransaction(uam::AppState& app,
	                                 const DeletionIntent& intent,
	                                 std::vector<ChatSession>& next_chats,
	                                 std::vector<ChatFolder>& next_folders,
	                                 std::vector<ChatSession>& deleted_chats,
	                                 bool& native_cleanup_failed)
	{
		deleted_chats = DeletedChatSnapshots(app.data_root, intent, app.chats);
		std::unordered_set<std::string> retained_native_keys;
		std::unordered_set<std::string> retained_workspace_keys;
		std::vector<uam::NativeDeletionOwner> retained_native_owners;
		CollectRetainedDeletionOwnership(app, intent, retained_native_keys, retained_workspace_keys, &retained_native_owners);
		std::unordered_map<std::string, std::string> native_stores_by_directory;
		retained_native_keys.insert(intent.retained_native_keys.begin(), intent.retained_native_keys.end());
		retained_native_owners.insert(retained_native_owners.end(), intent.retained_native_owners.begin(), intent.retained_native_owners.end());
		ProtectSharedNativeDeletionKeys(app, deleted_chats, retained_native_owners, retained_native_keys, native_stores_by_directory);
		if (!intent.ownership_finalized)
		{
			std::unordered_set<std::string> retained_stores;
			for (const uam::NativeDeletionOwner& owner : retained_native_owners)
			{
				if (!owner.unresolved_session) continue;
				const std::string store = NativeDeletionStore(owner.workspace_directory, native_stores_by_directory);
				if (!store.empty()) retained_stores.insert(store);
			}
			for (const ChatSession& chat : deleted_chats)
			{
				if (!uam::paths::IsControllerLocalWorkspace(chat) ||
				    !ProviderRuntime::UsesNativeOverlayHistory(ProviderResolutionService().ProviderForChatOrDefault(app, chat))) continue;
				const std::string directory = uam::paths::Utf8PathString(uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
				if (retained_stores.contains(NativeDeletionStore(directory, native_stores_by_directory)))
					retained_native_keys.insert(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat));
			}
		}
		return CommitDeletionMetadata(app, intent, next_chats, next_folders, deleted_chats, retained_native_keys) &&
		       CleanupDeletionTransaction(app, intent, deleted_chats, native_cleanup_failed, retained_native_keys, retained_workspace_keys, retained_native_owners);
	}

	std::string StatusAfterFolderDelete(std::size_t deleted_chat_count, bool settings_saved, bool native_cleanup_failed)
	{
		std::string status_line = "Workspace deletion complete. Deleted " + std::to_string(deleted_chat_count) + " chat(s).";

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
		return ChatFolderStore::AllocateId(app.folders);
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

	/// <summary>Compares exact workspace ownership, including aliases of local paths that do not exist yet.</summary>
	std::string FolderOwnershipKey(const uam::AppState& app, std::string_view host_id,
	                               std::string_view directory)
	{
		const std::string host = uam::strings::NonEmptyOrFallback(uam::strings::Trim(host_id), std::string(uam::execution_hosts::kLocalHostId));
		if (host == uam::execution_hosts::kLocalHostId)
		{
			std::filesystem::path path = uam::paths::NormalizeExistingOrAbsolutePath(uam::paths::ExpandTrimmedWorkspacePath(directory));
			while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
			return host + "\n" + uam::paths::Utf8PathString(path);
		}
		const ExecutionHost* execution_host = uam::execution_hosts::Find(app.settings.execution_hosts, host);
		const std::string_view platform = execution_host == nullptr ? std::string_view{} : execution_host->platform;
		const bool windows = platform == "windows" || platform == "Windows";
		std::string path = uam::execution_hosts::NormalizeRemotePath(platform, directory);
		// POSIX permits a distinct double-slash root; do not collapse it into the single-slash root.
		if (!windows && uam::strings::TrimAsciiView(directory) == "//") path = "//";
		std::size_t root_length = 0;
		while (root_length < path.size() && path[root_length] == '/') ++root_length;
		if (windows && root_length == 0 && path.size() >= 3 && path[1] == ':' && path[2] == '/') root_length = 3;
		std::string normalized = !windows && root_length > 2 ? "/" : path.substr(0, root_length);
		std::size_t position = root_length;
		while (position < path.size())
		{
			const std::size_t separator = path.find('/', position);
			const std::size_t end = separator == std::string::npos ? path.size() : separator;
			const std::string_view component(path.data() + position, end - position);
			// Remove only redundant separators and '.', since '..' can traverse a remote symlink.
			if (!component.empty() && component != ".")
			{
				if (!normalized.empty() && normalized.back() != '/') normalized.push_back('/');
				normalized.append(component);
			}
			if (separator == std::string::npos) break;
			position = separator + 1;
		}
		return host + "\n" + normalized;
	}

	bool HasOtherWorkspaceOwner(const uam::AppState& app, const FolderInput& input,
	                            std::string_view excluded_id = {})
	{
		const std::string key = FolderOwnershipKey(app, input.execution_host_id, input.directory);
		return std::ranges::any_of(app.folders, [&](const ChatFolder& folder)
		{
			return folder.id != excluded_id && !uam::strings::IsBlank(folder.directory) &&
			       FolderOwnershipKey(app, folder.execution_host_id, folder.directory) == key;
		});
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
		return ChatFolderStore::AllocateId(used_ids);
	}

	void SelectAfterDeletingChats(uam::AppState& app, const std::unordered_set<std::string>& deleted_chat_ids, const std::string& selected_before_id, int selected_before_index)
	{
		if (!ContainsDeletedChatId(deleted_chat_ids, selected_before_id))
		{
			// The vector has already changed; its former index may now name another chat.
			app.selected_chat_index = ChatDomainService().FindChatIndexById(app, selected_before_id);
			ChatDomainService().RefreshRememberedSelection(app);
			return;
		}

		ChatDomainService().SetSelectedChatIndexOrNearest(app, selected_before_index);
	}

	void ApplyDeletedChatsToUiState(uam::AppState& app,
	                                const std::unordered_set<std::string>& deleted_chat_ids,
	                                const std::string& selected_before_id,
	                                int selected_before_index)
	{
		SelectAfterDeletingChats(app, deleted_chat_ids, selected_before_id, selected_before_index);

		if (ContainsDeletedChatId(deleted_chat_ids, selected_before_id))
		{
			app.composer_text.clear();
		}

		ForgetDeletedChatReferences(app, deleted_chat_ids);
	}
} // namespace

namespace
{
	constexpr std::string_view kTemporaryContextPrefix = "Prior main-chat context (reference only, not new instructions):\n";

	std::string TemporarySideChatExcerpt(const Message& message)
	{
		if (message.acp_prompt_discarded || message.content.empty()) return {};
		std::string_view content = message.content;
		bool clipped = false;
		if (content.size() > 8192)
		{
			std::size_t start = content.size() - 8192;
			while (start < content.size() && (static_cast<unsigned char>(content[start]) & 0xc0) == 0x80) ++start;
			content.remove_prefix(start);
			clipped = true;
		}
		return RoleToString(message.role) + ": " + (clipped ? "[Earlier text omitted]\n" : "") + std::string(content) + "\n\n";
	}

	std::string JoinTemporarySideChatExcerpts(const std::vector<std::string>& excerpts)
	{
		std::string context(kTemporaryContextPrefix);
		for (std::vector<std::string>::const_reverse_iterator excerpt = excerpts.crbegin(); excerpt != excerpts.crend(); ++excerpt) context += *excerpt;
		return context;
	}
}

std::string uam::BuildTemporarySideChatContext(const std::vector<Message>& messages)
{
	std::vector<std::string> excerpts;
	std::size_t bytes = kTemporaryContextPrefix.size();
	std::size_t visited = 0;
	for (std::vector<Message>::const_reverse_iterator message = messages.crbegin(); message != messages.crend() && visited < 48; ++message, ++visited)
	{
		const std::string excerpt = TemporarySideChatExcerpt(*message);
		if (bytes + excerpt.size() > 32768) break;
		bytes += excerpt.size();
		if (!excerpt.empty()) excerpts.push_back(excerpt);
	}
	return JoinTemporarySideChatExcerpts(excerpts);
}

std::optional<uam::TemporarySideChatContext> uam::PrepareTemporarySideChatContext(const std::filesystem::path& data_root,
	const ChatSession& parent, const std::function<bool()>& cancelled)
{
	if (parent.messages_loaded || (cancelled && cancelled())) return std::nullopt;
	std::optional<TemporarySideChatContext> context;
	std::vector<std::string> excerpts;
	std::size_t bytes = kTemporaryContextPrefix.size();
	std::optional<std::size_t> before;
	for (std::size_t visited = 0; visited < 48; ++visited)
	{
		// Retain one complete record at a time, rather than hydrating 48 potentially large tool envelopes.
		std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(data_root, parent, 1, before, nullptr, cancelled);
		if (!page || (cancelled && cancelled())) return std::nullopt;
		if (context && (page->storage_revision != context->page.storage_revision || page->messages_digest != context->page.messages_digest)) return std::nullopt;
		const std::size_t start = page->start_index;
		const std::string excerpt = page->chat.messages.empty() ? std::string{} : TemporarySideChatExcerpt(page->chat.messages.back());
		page->chat.messages.clear();
		if (!context)
		{
			context.emplace();
			context->page = std::move(*page);
		}
		if (bytes + excerpt.size() > 32768) break;
		bytes += excerpt.size();
		if (!excerpt.empty()) excerpts.push_back(excerpt);
		if (start == 0) break;
		before = start;
	}
	if (cancelled && cancelled()) return std::nullopt;
	context->reference = JoinTemporarySideChatExcerpts(excerpts);
	return context;
}

std::optional<ChatSession> uam::BuildTemporarySideChatCandidate(AppState& app, const std::string& parent_id, const TemporarySideChatContext* prepared, bool defer_context)
{
	ChatSession* parent = ChatDomainService().FindChatById(app, parent_id);
	if (parent == nullptr || !parent->temporary_parent_chat_id.empty() || parent->imported_read_only)
	{
		app.status_line = "Choose an editable main chat to start a side chat.";
		return std::nullopt;
	}
	if (!parent->messages_loaded && !defer_context && (prepared == nullptr || prepared->reference.size() > 32768 ||
		!ChatRepository::MatchesMessagePage(*parent, prepared->page) ||
		ChatRepository::ChatStorageRevision(app.data_root, parent->id) != prepared->page.storage_revision))
	{
		app.status_line = "The main chat context changed or is not prepared. Retry the side chat.";
		return std::nullopt;
	}
	ChatSession side = ChatDomainService().CreateNewChat(parent->folder_id, parent->provider_id);
	side.temporary_parent_chat_id = parent->id;
	side.temporary_storage_owner_token = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (side.temporary_storage_owner_token.empty()) return std::nullopt;
	// Temporary conversations share the parent's current workspace without owning
	// the ordinary new-chat request to create another branch/worktree on first send.
	side.workspace_isolation_kind.clear();
	side.title = "Side chat: " + parent->title;
	side.execution_host_id = parent->execution_host_id;
	side.workspace_directory = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, *parent));
	side.model_id = parent->model_id;
	side.approval_mode = parent->approval_mode;
	side.command_safety_tier = parent->command_safety_tier;
	side.reasoning_effort = parent->reasoning_effort;
	side.service_tier = parent->service_tier;
	side.service_tier_explicit = parent->service_tier_explicit;
	side.small_model_mode = parent->small_model_mode;
	side.uam_agent_id = parent->uam_agent_id;
	side.memory_enabled = false;
	side.memory_level = uam::memory_levels::kOff;
	side.provider_handoff_context = parent->messages_loaded ? BuildTemporarySideChatContext(parent->messages) : (prepared == nullptr ? std::string{} : prepared->reference);
	return side;
}

bool uam::AdoptTemporarySideChat(AppState& app, ChatSession candidate, const std::filesystem::path& saved_root,
	std::string_view saved_revision, const std::shared_ptr<const std::uint8_t>& parent_incarnation, const TemporarySideChatContext* prepared)
{
	if (app.data_root != saved_root || saved_revision.empty() || saved_revision == "missing" ||
		ChatDomainService().FindChatById(app, candidate.id) != nullptr) return false;
	const ChatSession* parent = ChatDomainService().FindChatById(app, candidate.temporary_parent_chat_id);
	if (parent == nullptr || !parent_incarnation || parent->metadata_incarnation != parent_incarnation) return false;
	const std::optional<ChatSession> current = BuildTemporarySideChatCandidate(app, candidate.temporary_parent_chat_id, prepared);
	if (!current || current->folder_id != candidate.folder_id || current->provider_id != candidate.provider_id ||
		current->execution_host_id != candidate.execution_host_id || current->workspace_directory != candidate.workspace_directory ||
		current->model_id != candidate.model_id || current->approval_mode != candidate.approval_mode ||
		current->command_safety_tier != candidate.command_safety_tier || current->reasoning_effort != candidate.reasoning_effort ||
		current->service_tier != candidate.service_tier || current->service_tier_explicit != candidate.service_tier_explicit ||
		current->small_model_mode != candidate.small_model_mode || current->uam_agent_id != candidate.uam_agent_id ||
		current->provider_handoff_context != candidate.provider_handoff_context ||
		candidate.temporary_storage_owner_token.empty()) return false;
	app.chats.push_back(std::move(candidate));
	return true;
}

bool uam::CreateTemporarySideChat(AppState& app, const std::string& parent_id, std::string* created_id, const TemporarySideChatContext* prepared)
{
	std::optional<ChatSession> side = BuildTemporarySideChatCandidate(app, parent_id, prepared);
	if (!side) return false;
	if (!ChatRepository::SaveChatIfAbsent(app.data_root, *side))
	{
		app.status_line = "Could not save the temporary side chat.";
		return false;
	}
	if (created_id != nullptr) *created_id = side->id;
	app.chats.push_back(std::move(*side));
	return true;
}

bool uam::RequestTemporarySideChatCleanup(AppState& app, const std::string& chat_id)
{
	ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
	if (chat == nullptr || chat->temporary_parent_chat_id.empty()) return false;
	const bool previous = chat->side_cleanup_requested;
	chat->side_cleanup_requested = true;
	if (!ChatRepository::SaveChat(app.data_root, *chat))
	{
		chat->side_cleanup_requested = previous;
		app.status_line = "Could not save the side-chat cleanup request. The conversation was kept.";
		return false;
	}
	chat->side_cleanup_retry_time_s = 0.0;
	return true;
}

bool uam::PollTemporarySideChatCleanup(AppState& app)
{
	std::vector<std::string> pending;
	const double now = GetAppTimeSeconds();
	for (const ChatSession& chat : app.chats)
		if (!chat.temporary_parent_chat_id.empty() && chat.side_cleanup_requested && chat.side_cleanup_retry_time_s <= now)
			pending.push_back(chat.id);
	bool changed = false;
	for (const std::string& id : pending)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, id);
		if (chat == nullptr) continue;
		chat->side_cleanup_retry_time_s = now + 5.0;
		if (chat->side_cleanup_stop_finished)
		{
			if (!chat->side_cleanup_stop_finished->load()) continue;
			std::vector<AsyncAcpProcessStopTask>::iterator pending_stop = std::ranges::find_if(app.acp_process_stop_tasks, [&](const AsyncAcpProcessStopTask& task) { return task.finished == chat->side_cleanup_stop_finished; });
			if (pending_stop != app.acp_process_stop_tasks.end() && pending_stop->observe_exit && !pending_stop->result->exit_confirmed)
			{
				const std::shared_ptr<platform::StdioProcessPlatformFields> owned = pending_stop->owned_process;
				const std::shared_ptr<platform::ObservedProcessStopResult> result = pending_stop->result;
				const std::shared_ptr<std::atomic<bool>> finished = pending_stop->finished;
				finished->store(false);
				pending_stop->worker = std::make_unique<std::jthread>([owned, result, finished](std::stop_token) {
					IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
					*result = platform::StopStdioProcessObserved(service, *owned, std::chrono::milliseconds(0));
					if (result->exit_confirmed) service.CloseStdioProcessHandles(*owned);
					finished->store(true);
				});
				app.status_line = "Side-chat stop is not confirmed. Cleanup will retry.";
				continue;
			}
		}
		if (AcpSessionState* session = FindAcpSessionForChat(app, id); session != nullptr && session->running && chat->execution_host_id == "local")
		{
			QueueAcpProcessStop(app, *session, id, AcpStopPurpose::Interrupt, true);
			session->running = false;
			chat->side_cleanup_stop_finished = app.acp_process_stop_tasks.back().finished;
			(void)StopAcpSession(app, id);
			continue;
		}
		const std::size_t stop_tasks_before = app.acp_process_stop_tasks.size();
		if (!StopAcpSession(app, id)) continue;
		if (app.acp_process_stop_tasks.size() > stop_tasks_before)
		{
			chat->side_cleanup_stop_finished = app.acp_process_stop_tasks.back().finished;
			continue;
		}
		uam::StopAndEraseCliTerminalForChat(app, id, false);
		if (ChatHasDeletionBlockingRuntime(app, id)) continue;
		changed = RemoveChatById(app, id) || changed;
	}
	return changed;
}

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
			std::string directory = uam::strings::NonEmptyOrFallback(
			    uam::strings::Trim(chat.workspace_directory), folder.directory);
			// Use the same validated nested/worktree owner as native import, retaining the chat's cwd.
			if (host == "local")
			{
				const ChatFolder* owner = uam::FindImportedWorkspaceFolder(app.folders, uam::paths::ExpandTrimmedWorkspacePath(directory));
				if (owner != nullptr) directory = owner->directory;
			}
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

		const ChatFolder* matched_folder = uam::FindImportedWorkspaceFolder(app.folders, local_directory);
		const std::filesystem::path directory = matched_folder != nullptr
		    ? uam::paths::PathFromUtf8(matched_folder->directory)
		    : uam::ResolveImportedWorkspaceFolderDirectory(local_directory);
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
	if (!StopAndEraseCliTerminalForChat(app, chat->id, false)) return ChatProviderSwitchResult::RuntimeStopping;
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
	chat->provider_handoff_context = BuildProviderHandoffContext(*chat);
	chat->provider_handoff_session_id.clear();
	chat->provider_id = provider->id;
	uam::provider_chat_defaults::ApplyToChat(app.settings, *chat);
	chat->memory_level = previous_chat.memory_level;
	chat->memory_enabled = previous_chat.memory_enabled;
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

bool uam::BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out, std::string* error_out, const std::optional<std::string>& operation_id, std::string* warning_out, const std::optional<std::vector<std::string>>& retained_attachment_ids, const ChatSession* prepared_source, const ChatSession* prepared_existing_branch, bool select_chat, bool regenerate)
{
	return BranchFromMessageAndRetry(app, source_chat_id, message_index, replacement_content, branch_id_out, error_out, operation_id, warning_out, retained_attachment_ids, prepared_source, prepared_existing_branch, nullptr, std::nullopt, select_chat, regenerate);
}

bool uam::BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out, std::string* error_out, const std::optional<std::string>& operation_id, std::string* warning_out, const std::optional<std::vector<std::string>>& retained_attachment_ids, const ChatSession* prepared_source, const ChatSession* prepared_existing_branch, uam::PreparedBranchOriginals* prepared_originals, const std::optional<std::vector<std::string>>& retained_markdown_store_files, bool select_chat, bool regenerate)
{
	if (branch_id_out != nullptr) branch_id_out->clear();
	if (error_out != nullptr) error_out->clear();
	if (warning_out != nullptr) warning_out->clear();
	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(app);
	bool reused_existing_branch = false;
	std::string created_branch_id;
	if (!ChatDomainService().CreateBranchFromMessage(app, source_chat_id, message_index, replacement_content, operation_id, &reused_existing_branch, retained_attachment_ids, prepared_source, prepared_existing_branch, prepared_originals, retained_markdown_store_files, select_chat, &created_branch_id))
	{
		if (error_out != nullptr)
		{
			*error_out = app.status_line;
		}
		return false;
	}

	const ChatSession* branch = ChatDomainService().FindChatById(app, created_branch_id);
	if (branch == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Branch was created but could not be found.";
		}
		return false;
	}
	const std::string branch_id = branch->id;
	if (branch_id_out != nullptr)
	{
		*branch_id_out = branch_id;
	}
	if (!regenerate) return true;
	if (reused_existing_branch)
	{
		if (branch->branch_retry_error.empty()) return true;
		app.status_line = "Branch exists, but regeneration did not start: " + branch->branch_retry_error;
		if (warning_out != nullptr) *warning_out = app.status_line;
		if (error_out != nullptr) *error_out = app.status_line;
		return false;
	}
	const ChatSession branch_snapshot = *branch;
	std::string retry_error;
	if (RetryLastAcpPrompt(app, branch_id, &retry_error))
	{
		return true;
	}
	if (operation_id.has_value() && !uam::strings::Trim(*operation_id).empty())
	{
		const std::string failure = uam::strings::NonEmptyOrFallback(retry_error, "Regeneration could not start.");
		if (ChatSession* failed_branch = ChatDomainService().FindChatById(app, branch_id); failed_branch != nullptr)
		{
			failed_branch->branch_retry_error = failure;
			(void)SaveChatHistories(app, {*failed_branch});
		}
		app.status_line = "Branch was created, but regeneration could not start: " + failure;
		if (warning_out != nullptr) *warning_out = app.status_line;
		if (error_out != nullptr) *error_out = app.status_line;
		return false;
	}

	if (!StopChatRuntimes(app, branch_id))
	{
		app.status_line = "Branch cleanup is waiting for its runtime to stop.";
		if (error_out != nullptr) *error_out = app.status_line;
		return false;
	}
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

	if (select_chat) ChatDomainService().SelectChatById(app, previous_selected_chat_id);
	if (!PersistenceCoordinator().SaveSettings(app))
	{
		const bool history_restored = SaveChatHistories(app, {branch_snapshot});
		if (select_chat) ChatDomainService().SelectChatById(app, branch_id);
		ChatDomainService().RefreshRememberedSelection(app);
		app.status_line = retry_error + " Branch selection rollback failed, so the branch was kept." +
		                  (history_restored ? "" : " Its local history could not be restored.");
		if (error_out != nullptr)
		{
			*error_out = app.status_line;
		}
		return false;
	}
	std::string worktree_cleanup_error;
	if (!uam::GitWorktreeService().RemoveUnusedBranchWorktree(app, branch_snapshot, &worktree_cleanup_error))
	{
		const bool history_restored = SaveChatHistories(app, {branch_snapshot});
		if (select_chat) ChatDomainService().SelectChatById(app, branch_id);
		(void)PersistenceCoordinator().SaveSettings(app);
		app.status_line = retry_error + " Branch worktree contains changes or could not be removed, so the branch was kept: " +
		                  worktree_cleanup_error + (history_restored ? "" : " Its local history could not be restored.");
		if (error_out != nullptr) *error_out = app.status_line;
		return false;
	}

	std::erase_if(app.chats, [&branch_id](const ChatSession& chat) { return chat.id == branch_id; });
	if (select_chat) ChatDomainService().SelectChatById(app, previous_selected_chat_id);
	else
	{
		app.selected_chat_index = ChatDomainService().FindChatIndexById(app, previous_selected_chat_id);
		ChatDomainService().RefreshRememberedSelection(app);
	}
	app.status_line = retry_error;
	if (error_out != nullptr)
	{
		*error_out = retry_error;
	}
	return false;
}

bool RemoveChatsByIds(uam::AppState& app, const std::vector<std::string>& chat_ids)
{
	uam::WorkspaceDeletionTask task;
	if (!uam::PrepareChatDeletion(app, chat_ids, task)) return false;
	if (!uam::StageWorkspaceDeletion(task))
	{
		app.status_line = task.snapshot.status_line.empty()
		    ? "Failed to create a durable chat deletion transaction."
		    : task.snapshot.status_line;
		return false;
	}
	uam::CommitWorkspaceDeletion(app, task);
	if (task.commit_rejected) return false;
	(void)uam::CleanupWorkspaceDeletion(task);
	app.status_line = task.snapshot.status_line;
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
	return DeleteFoldersByIds(app, {folder_id});
}

bool uam::PrepareWorkspaceDeletion(AppState& app, const std::vector<std::string>& folder_ids, WorkspaceDeletionTask& task)
{
	if (app.startup_native_import_service != nullptr && app.startup_native_import_service->ImportPending())
	{
		app.status_line = "Wait for native history loading to finish.";
		return false;
	}

	std::unordered_set<std::string> target_folder_ids;
	for (const std::string& requested_id : folder_ids)
	{
		const std::string id = uam::strings::Trim(requested_id);
		if (id.empty())
		{
			app.status_line = "Folder id is required.";
			return false;
		}
		target_folder_ids.insert(id);
	}
	if (target_folder_ids.empty())
	{
		app.status_line = "Folder id is required.";
		return false;
	}
	std::unordered_set<std::string> existing_folder_ids;
	for (const ChatFolder& folder : app.folders) existing_folder_ids.insert(uam::strings::Trim(folder.id));
	for (const std::string& id : target_folder_ids)
	{
		if (!existing_folder_ids.contains(id))
		{
			app.status_line = "Folder no longer exists.";
			return false;
		}
	}
	DeletedChatsSelection deleted = CollectChatsInFolder(app, target_folder_ids);
	if (DeletionRewritesUnconfirmedInitialRoot(app, deleted.ids))
	{
		app.status_line = "Wait for the saved request to be confirmed before deleting its ancestor.";
		return false;
	}
	for (const std::string& id : deleted.ids)
	{
		CancelWorktreeLaunchPreparation(app, id);
		CancelGitPortRequest(app, id);
		if (app.worktree_operation_chat_ids.contains(id))
		{
			app.status_line = "Wait for the chat worktree operation to finish.";
			return false;
		}
		if (app.pending_chat_save_at_by_chat_id.contains(id))
		{
			app.status_line = "Wait for chat history to finish saving.";
			return false;
		}
	}
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
	for (const ChatSession& chat : deleted.chats)
	{
		const std::string host_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.execution_host_id), "local");
		const std::unordered_map<std::string, std::weak_ptr<void>>::const_iterator lease = app.remote_vcs_operation_leases_by_host_id.find(host_id);
		if (lease != app.remote_vcs_operation_leases_by_host_id.end() && !lease->second.expired())
		{
			app.status_line = "Wait for the workspace operation to finish.";
			return false;
		}
		remote_stop_started = BeginIdleRemoteStopForDeletion(app, chat.id, &chat) || remote_stop_started;
	}
	if (remote_stop_started)
	{
		app.status_line = "Remote runtimes in this folder are stopping. Retry deletion after they finish.";
		return false;
	}
	if (std::ranges::any_of(deleted.chats, [&app](const ChatSession& chat)
	    { return ChatHasDeletionBlockingRuntime(app, chat.id, &chat); }))
	{
		app.status_line = "Cannot delete a folder while one of its chats has a running runtime.";
		return false;
	}
	StopChatRuntimes(app, deleted.chats);
	task.folder_ids.assign(target_folder_ids.begin(), target_folder_ids.end());
	task.deleted_ids = std::move(deleted.ids);
	task.snapshot.data_root = app.data_root;
	task.snapshot.settings = app.settings;
	task.snapshot.provider_profiles = app.provider_profiles;
	task.snapshot.folders = app.folders;
	task.snapshot.chats = std::move(deleted.chats);
	const DeletionIntent intent{{task.deleted_ids.begin(), task.deleted_ids.end()}, task.folder_ids};
	CollectRetainedDeletionOwnership(app, intent, task.retained_native_keys, task.retained_workspace_keys, &task.retained_native_owners);
	return true;
}

bool uam::PrepareChatDeletion(AppState& app, const std::vector<std::string>& chat_ids, WorkspaceDeletionTask& task)
{
	if (app.startup_native_import_service != nullptr && app.startup_native_import_service->ImportPending())
	{
		app.status_line = "Wait for native history loading to finish.";
		return false;
	}

	std::vector<std::string> target_chat_ids;
	std::unordered_set<std::string> deleted_chat_ids;
	for (const std::string& requested_id : chat_ids)
	{
		const std::string id = uam::strings::Trim(requested_id);
		if (!id.empty() && deleted_chat_ids.insert(id).second) target_chat_ids.push_back(id);
	}
	ExpandDependentChatIds(app, target_chat_ids, deleted_chat_ids);
	if (target_chat_ids.empty())
	{
		app.status_line = "Chat id is required.";
		return false;
	}
	if (DeletionRewritesUnconfirmedInitialRoot(app, deleted_chat_ids))
	{
		app.status_line = "Wait for the saved request to be confirmed before deleting its ancestor.";
		return false;
	}
	if (HasActiveDependentAgentRun(app, deleted_chat_ids))
	{
		app.status_line = "Wait for the selected managed agent run to finish or cancel it first.";
		return false;
	}
	for (const std::string& id : target_chat_ids)
	{
		const ChatSession* chat = ChatDomainService().FindChatById(app, id);
		if (chat == nullptr)
		{
			app.status_line = "A selected chat no longer exists.";
			return false;
		}
		CancelWorktreeLaunchPreparation(app, id);
		CancelGitPortRequest(app, id);
		if (app.worktree_operation_chat_ids.contains(id))
		{
			app.status_line = "Wait for the chat worktree operation to finish.";
			return false;
		}
		if (app.pending_chat_save_at_by_chat_id.contains(id))
		{
			app.status_line = "Wait for chat history to finish saving.";
			return false;
		}
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(app, id);
		if (session != nullptr && session->remote_stop_unconfirmed)
		{
			app.status_line = uam::strings::NonEmptyOrFallback(session->last_error, "Remote stop could not be confirmed.");
			return false;
		}
		if (BeginIdleRemoteStopForDeletion(app, id, chat))
		{
			app.status_line = "The remote runtime is stopping. Retry deletion after it finishes.";
			return false;
		}
		if (ChatHasDeletionBlockingRuntime(app, id, chat))
		{
			app.status_line = "Cannot delete chats while a selected runtime is still running.";
			return false;
		}
		const std::string host_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat->execution_host_id), "local");
		const std::unordered_map<std::string, std::weak_ptr<void>>::const_iterator lease = app.remote_vcs_operation_leases_by_host_id.find(host_id);
		if (lease != app.remote_vcs_operation_leases_by_host_id.end() && !lease->second.expired())
		{
			app.status_line = "Wait for the workspace operation to finish.";
			return false;
		}
	}

	std::vector<ChatSession> targets;
	targets.reserve(deleted_chat_ids.size());
	for (const ChatSession& chat : app.chats)
	{
		if (!deleted_chat_ids.contains(chat.id)) continue;
		ChatSession snapshot = chat;
		CaptureDeletionChatLocation(app, snapshot);
		if (!chat.messages_loaded)
		{
			snapshot.messages.clear();
			snapshot.messages_loaded = false;
		}
		targets.push_back(std::move(snapshot));
	}
	if (!StopChatRuntimes(app, targets))
	{
		app.status_line = "Deletion is waiting for its runtime to stop. Retry after it finishes.";
		return false;
	}
	task.deleting_chats = true;
	task.deleted_ids = std::move(deleted_chat_ids);
	task.snapshot.data_root = app.data_root;
	task.snapshot.settings = app.settings;
	task.snapshot.provider_profiles = app.provider_profiles;
	task.snapshot.folders = app.folders;
	task.snapshot.chats = std::move(targets);
	const DeletionIntent intent{{task.deleted_ids.begin(), task.deleted_ids.end()}, task.folder_ids};
	CollectRetainedDeletionOwnership(app, intent, task.retained_native_keys, task.retained_workspace_keys, &task.retained_native_owners);
	return true;
}

bool uam::StageWorkspaceDeletion(WorkspaceDeletionTask& task)
{
	ProtectSharedNativeDeletionKeys(task.snapshot, task.snapshot.chats, task.retained_native_owners, task.retained_native_keys, task.native_stores_by_directory);
	if (!BeginDeletionTransaction(task.snapshot, task.snapshot.chats, task.folder_ids, task.retained_native_keys, task.retained_native_owners))
	{
		task.snapshot.status_line = task.deleting_chats
		    ? "Failed to create a durable chat deletion transaction."
		    : "Failed to create a durable workspace deletion transaction.";
		return false;
	}
	return true;
}

void uam::CommitWorkspaceDeletion(AppState& app, WorkspaceDeletionTask& task)
{
	if (DeletionRewritesUnconfirmedInitialRoot(app, task.deleted_ids))
	{
		// A real protected survivor may have appeared while the worker staged selected ancestors.
		// Preserve the existing transaction and commit-rejected recovery boundary; do not reparent or save it.
		task.commit_rejected = true;
		app.worktree_operation_chat_ids.insert(task.deleted_ids.begin(), task.deleted_ids.end());
		app.status_line = "Deletion is pending because a surviving chat's saved request is still unconfirmed.";
		return;
	}
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	const int previous_selected_chat_index = app.selected_chat_index;
	const DeletionIntent intent{{task.deleted_ids.begin(), task.deleted_ids.end()}, task.folder_ids};
	const std::unordered_set<std::string> staged_retained_keys = std::move(task.retained_native_keys);
	CollectRetainedDeletionOwnership(app, intent, task.retained_native_keys, task.retained_workspace_keys, &task.retained_native_owners);
	task.retained_native_keys.insert(staged_retained_keys.begin(), staged_retained_keys.end());
	ProtectSharedNativeDeletionKeys(task.snapshot, task.snapshot.chats, task.retained_native_owners, task.retained_native_keys, task.native_stores_by_directory, false);
	nlohmann::json encoded = {{"version", 2}, {"complete", true}, {"folderIds", task.folder_ids},
	    {"chatIds", std::vector<std::string>(task.deleted_ids.begin(), task.deleted_ids.end())}};
	EncodeDeletionOwnership(encoded, task.retained_native_keys, task.retained_native_owners, true);
	const std::filesystem::path intent_path = DeletionIntentPath(app.data_root);
	std::error_code backup_error;
	const std::filesystem::file_status backup_status = std::filesystem::symlink_status(uam::io::MakeBackupPath(intent_path), backup_error);
	const bool invalid_backup = backup_error && backup_error != std::errc::no_such_file_or_directory;
	if (invalid_backup || (std::filesystem::exists(backup_status) && !std::filesystem::is_regular_file(backup_status)) ||
	    !uam::io::WriteTextFileWithBackup(intent_path, encoded.dump()))
	{
		task.commit_rejected = true;
		app.worktree_operation_chat_ids.insert(task.deleted_ids.begin(), task.deleted_ids.end());
		app.status_line = "Failed to save deletion ownership. Deletion is pending until restart.";
		return;
	}
	std::vector<ChatFolder> next_folders;
	task.metadata_saved = CommitDeletionMetadataInPlace(app, intent, next_folders, task.snapshot.chats, task.retained_native_keys);
	ApplyDeletedChatsToUiState(app, task.deleted_ids, selected_chat_id, previous_selected_chat_index);
	if (std::ranges::find(task.folder_ids, uam::strings::Trim(app.new_chat_folder_id)) != task.folder_ids.end())
		app.new_chat_folder_id.clear();
	task.settings_saved = PersistenceCoordinator().SaveSettings(app);
	app.status_line = task.deleting_chats ? "Chats deleted. Cleaning up history." : "Workspaces deleted. Cleaning up history.";
}

bool uam::CleanupWorkspaceDeletion(WorkspaceDeletionTask& task)
{
	bool native_cleanup_failed = false;
	const DeletionIntent intent{{task.deleted_ids.begin(), task.deleted_ids.end()}, task.folder_ids};
	const bool completed = task.metadata_saved && CleanupDeletionTransaction(task.snapshot, intent, task.snapshot.chats, native_cleanup_failed, task.retained_native_keys, task.retained_workspace_keys, task.retained_native_owners, task.progress);
	task.snapshot.status_line = completed
	    ? (task.deleting_chats
	        ? std::to_string(task.deleted_ids.size()) + " chat(s) deleted." +
	            (task.settings_saved ? std::string() : " Settings persistence also failed.") +
	            (native_cleanup_failed ? " Some provider-native history could not be removed." : std::string())
	        : StatusAfterFolderDelete(task.deleted_ids.size(), task.settings_saved, native_cleanup_failed))
	    : (task.deleting_chats
	        ? "Chat deletion is committed and hidden. Disk cleanup will finish safely on restart."
	        : "Workspace deletion is committed and hidden. Disk cleanup will finish safely on restart.");
	return completed;
}

bool DeleteFoldersByIds(uam::AppState& app, const std::vector<std::string>& folder_ids)
{
	uam::WorkspaceDeletionTask task;
	if (!uam::PrepareWorkspaceDeletion(app, folder_ids, task)) return false;
	if (!uam::StageWorkspaceDeletion(task))
	{
		app.status_line = task.snapshot.status_line;
		return false;
	}
	uam::CommitWorkspaceDeletion(app, task);
	if (task.commit_rejected) return false;
	(void)uam::CleanupWorkspaceDeletion(task);
	app.status_line = task.snapshot.status_line;
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
	if (HasOtherWorkspaceOwner(app, input))
	{
		app.status_line = "A workspace folder already exists for this directory on the selected computer.";
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
	const bool directory_changed = FolderOwnershipKey(app, original.execution_host_id, original.directory) !=
	                               FolderOwnershipKey(app, input.execution_host_id, input.directory);
	// Existing duplicate records remain editable until an explicit consolidation is approved.
	if (directory_changed && HasOtherWorkspaceOwner(app, input, target_folder_id))
	{
		app.status_line = "A workspace folder already exists for this directory on the selected computer.";
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
				if (original.execution_host_id != uam::execution_hosts::kLocalHostId)
				{
					std::string warning;
					if (!ChatRepository::HydrateChatMessages(app.data_root, chat, &warning) ||
					    !uam::chat::BuildConversationHandoff(chat, chat.provider_handoff_context, &warning))
					{
						app.chats = original_chats;
						app.status_line = "The workspace was kept because its chat context could not be prepared. " + warning;
						return false;
					}
					chat.native_session_id.clear();
					chat.native_session_reset_pending = true;
					chat.provider_handoff_session_id.clear();
				}
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

	if (directory_changed && original.execution_host_id != uam::execution_hosts::kLocalHostId)
	{
		for (const ChatSession& chat : app.chats)
			if (ChatBelongsToFolder(chat, target_folder_id) && chat.native_session_reset_pending)
				app.resolved_native_sessions_by_chat_id.erase(chat.id);
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
