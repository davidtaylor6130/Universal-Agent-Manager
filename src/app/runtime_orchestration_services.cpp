#include "runtime_orchestration_services.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"

#include "common/paths/app_paths.h"
#include "common/chat/chat_branching.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/chat_repository.h"
#include "common/constants/app_constants.h"
#include "common/platform/platform_services.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/terminal_common.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using uam::AppState;

namespace
{
	constexpr const char* kDefaultNativeHistoryProviderId = provider_build_config::DefaultNativeHistoryProviderId();

	std::string NormalizeNativeIdentityWorkspace(const std::string& workspace_directory)
	{
		const std::string trimmed = Trim(workspace_directory);

		if (trimmed.empty())
		{
			return "";
		}

		std::error_code ec;
		const fs::path canonical = fs::weakly_canonical(fs::path(trimmed), ec);
		return (ec ? fs::path(trimmed).lexically_normal() : canonical).generic_string();
	}

	fs::path NormalizeWorkspacePathForComparison(const std::string& workspace_directory)
	{
		const std::string trimmed = Trim(workspace_directory);
		if (trimmed.empty())
		{
			return {};
		}

		const fs::path expanded = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(trimmed);
		std::error_code ec;
		const fs::path canonical = fs::weakly_canonical(expanded, ec);
		return (ec ? expanded : canonical).lexically_normal();
	}

	std::string NativeIdentityKey(const ChatSession& chat)
	{
		return Trim(chat.provider_id) + "|" + NormalizeNativeIdentityWorkspace(chat.workspace_directory) + "|" + Trim(chat.native_session_id);
	}

	std::string HashNativeIdentityKey(const std::string& key)
	{
		std::uint64_t hash = 1469598103934665603ull;

		for (const unsigned char ch : key)
		{
			hash ^= ch;
			hash *= 1099511628211ull;
		}

		std::ostringstream out;
		out << std::hex << hash;
		return out.str();
	}

	std::string MakeCollisionSafeImportedChatId(const ChatSession& chat, const std::unordered_set<std::string>& existing_ids)
	{
		const std::string base_id = Trim(chat.id).empty() ? Trim(chat.native_session_id) : Trim(chat.id);
		const std::string suffix = HashNativeIdentityKey(NativeIdentityKey(chat));
		std::string candidate = base_id + "--" + suffix;

		while (existing_ids.find(candidate) != existing_ids.end())
		{
			candidate += "_";
		}

		return candidate;
	}

	std::string ResolvePersistedImportFolderIdForSource(AppState& app, const ProviderChatSource& source)
	{
		ChatDomainService().EnsureDefaultFolder(app);

		for (const ChatFolder& folder : app.folders)
		{
			if (FolderDirectoryMatches(folder.directory, source.folder_directory))
			{
				return folder.id;
			}
		}

		ChatFolder new_folder;
		new_folder.id = "folder_" + std::to_string(app.folders.size()) + "_" + source.folder_title;
		new_folder.title = source.folder_title;
		new_folder.directory = source.folder_directory;
		new_folder.collapsed = false;

		app.folders.push_back(std::move(new_folder));
		const std::string created_folder_id = app.folders.back().id;

		if (ChatFolderStore::Save(app.data_root, app.folders))
		{
			return created_folder_id;
		}

		app.folders.erase(std::remove_if(app.folders.begin(), app.folders.end(), [&](const ChatFolder& folder) { return folder.id == created_folder_id; }), app.folders.end());
		app.status_line = "Imported chats into General because folder metadata could not be saved.";
		return uam::constants::kDefaultFolderId;
	}

	void ResetAsyncNativeChatLoadTask(uam::platform::AsyncNativeChatLoadTask& task)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
			task.worker.reset();
		}

		task.running = false;
		task.provider_id_snapshot.clear();
		task.chats_dir_snapshot.clear();
		task.state.reset();
	}

	const ProviderProfile& DefaultNativeHistoryProvider(const AppState& app)
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, kDefaultNativeHistoryProviderId); profile != nullptr)
		{
			return *profile;
		}

		for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
		{
			if (profile.id == kDefaultNativeHistoryProviderId)
			{
				static const ProviderProfile result = profile;
				return result;
			}
		}

		if (!app.provider_profiles.empty())
		{
			static const ProviderProfile result = []()
			{
				ProviderProfile profile;
				profile.id = "fallback";
				profile.title = "Fallback";
				return profile;
			}();
			(void)result;
			return app.provider_profiles.front();
		}

		static const ProviderProfile fallback = []()
		{
			ProviderProfile profile;
			profile.id = provider_build_config::FirstEnabledProviderId();
			profile.title = profile.id;
			return profile;
		}();
		return fallback;
	}

	std::vector<fs::path> CollectWorkspaceRootsForNativeHistory(const AppState& app)
	{
		std::vector<fs::path> roots;
		std::unordered_set<std::string> seen;

		for (const ChatFolder& folder : app.folders)
		{
			fs::path root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder.directory);

			if (root.empty())
			{
				continue;
			}

			std::error_code ec;
			const fs::path absolute_root = fs::absolute(root, ec);
			const fs::path normalized_root = (ec ? root : absolute_root).lexically_normal();
			const std::string key = normalized_root.string();

			if (key.empty() || seen.find(key) != seen.end())
			{
				continue;
			}

			seen.insert(key);
			roots.push_back(normalized_root);
		}

		if (roots.empty())
		{
			std::error_code ec;
			const fs::path current_root = fs::current_path(ec);

			if (!ec)
			{
				roots.push_back(current_root.lexically_normal());
			}
		}

		return roots;
	}

	bool MessagesEquivalent(const std::vector<Message>& lhs, const std::vector<Message>& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < lhs.size(); ++i)
		{
			if (lhs[i].role != rhs[i].role || lhs[i].content != rhs[i].content || lhs[i].created_at != rhs[i].created_at)
			{
				return false;
			}
		}

		return true;
	}

	bool LocalMessagesShouldOverrideNative(const ChatSession& local_chat, const ChatSession& native_chat)
	{
		return !local_chat.messages.empty() && (local_chat.messages.size() > native_chat.messages.size() || (local_chat.messages.size() == native_chat.messages.size() && local_chat.updated_at > native_chat.updated_at));
	}

	void OverlayLocalChatState(const ChatSession& local, ChatSession& native)
	{
		if (NativeSessionLinkService().HasRealNativeSessionId(local) && Trim(local.native_session_id) == Trim(native.native_session_id) && !Trim(local.id).empty())
		{
			native.id = local.id;
		}

		if (!Trim(local.provider_id).empty())
		{
			native.provider_id = local.provider_id;
		}

		native.title = local.title;
		native.folder_id = local.folder_id;
		native.linked_files = local.linked_files;
		native.parent_chat_id = local.parent_chat_id;
		native.branch_root_chat_id = local.branch_root_chat_id;
		native.branch_from_message_index = local.branch_from_message_index;
		native.workspace_directory = local.workspace_directory;
		native.approval_mode = local.approval_mode;
		native.model_id = local.model_id;
		native.extra_flags = local.extra_flags;
	}

	std::optional<fs::path> FindNativeSessionFileAcrossDiscoveredSources(const ProviderProfile& provider, const std::string& native_session_id)
	{
		if (Trim(native_session_id).empty())
		{
			return std::nullopt;
		}

		const ProviderDiscoveryResult discovery = ProviderRuntime::DiscoverChatSources(provider);

		if (!discovery.error.empty())
		{
			return std::nullopt;
		}

		std::optional<fs::path> matched_file;

		for (const ProviderChatSource& source : discovery.sources)
		{
			const auto session_file = ChatHistorySyncService().FindNativeSessionFilePath(source.chats_dir, native_session_id);

			if (session_file.has_value())
			{
				if (matched_file.has_value() && matched_file.value() != session_file.value())
				{
					return std::nullopt;
				}

				matched_file = session_file;
			}
		}

		return matched_file;
	}
} // namespace

namespace
{
	const ProviderProfile& ResolvePendingCallProviderOrDefault(const AppState& app, const PendingRuntimeCall& call)
	{
		if (!call.provider_id_snapshot.empty())
		{
			if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, call.provider_id_snapshot); profile != nullptr)
			{
				return *profile;
			}
		}

		return ProviderResolutionService().ActiveProviderOrDefault(app);
	}

	void ResetPendingRuntimeCallWorker(PendingRuntimeCall& call)
	{
		if (call.worker != nullptr)
		{
			call.worker->request_stop();
			call.worker.reset();
		}

		call.state.reset();
	}
} // namespace

bool PollPendingRuntimeCall(AppState& app)
{
	if (app.pending_calls.empty())
	{
		return false;
	}

	bool changed = false;
	std::unordered_set<std::string> claimed_new_session_ids;

	for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
	{
		if (!resolved.second.empty())
		{
			claimed_new_session_ids.insert(resolved.second);
		}
	}

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && !terminal->attached_session_id.empty())
		{
			claimed_new_session_ids.insert(terminal->attached_session_id);
		}
	}

	for (std::size_t i = 0; i < app.pending_calls.size();)
	{
		PendingRuntimeCall& call = app.pending_calls[i];

		if (call.state == nullptr)
		{
			ResetPendingRuntimeCallWorker(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}

		if (!call.state->completed.load(std::memory_order_acquire))
		{
			++i;
			continue;
		}

		const std::string output = call.state->result.output;
		const std::string pending_chat_id = call.chat_id;
		const ChatSession* selected_before = ChatDomainService().SelectedChat(app);
		const std::string selected_before_id = (selected_before != nullptr) ? selected_before->id : "";
		const int pending_chat_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);
		const ProviderProfile& call_provider = ResolvePendingCallProviderOrDefault(app, call);
		ChatSession pending_chat_snapshot;

		if (pending_chat_index >= 0)
		{
			pending_chat_snapshot = app.chats[pending_chat_index];
		}

		if (!ProviderRuntime::UsesNativeOverlayHistory(call_provider))
		{
			if (pending_chat_index >= 0)
			{
				const bool call_failed = call.state->result.exit_code != 0 || call.state->result.timed_out || call.state->result.canceled || !call.state->result.error.empty();
				const MessageRole result_role = call_failed ? MessageRole::System : MessageRole::Assistant;
				const auto completion_time = std::chrono::steady_clock::now();
				const int64_t processing_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(completion_time - call.state->launch_time).count();
				const int64_t output_chars = static_cast<int64_t>(output.size());

				ChatDomainService().AddMessageWithAnalytics(app.chats[pending_chat_index], result_role, output, call.state->provider_id, call.state->estimated_input_tokens, output_chars, 0, processing_time_ms, call.state->result.canceled || call.state->result.timed_out);
				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[pending_chat_index]), app.data_root, app.chats[pending_chat_index]);

				if (pending_chat_id != selected_before_id)
				{
					MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = call_failed ? "Provider command failed." : "Provider response appended to local chat history.";
				app.scroll_to_bottom = true;
			}
			else
			{
				app.status_line = "Provider command completed, but chat no longer exists.";
			}

			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			ResetPendingRuntimeCallWorker(call);
			app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
			changed = true;
			continue;
		}

		const std::filesystem::path native_history_chats_dir = call.native_history_chats_dir_snapshot.empty() ? std::filesystem::path{} : std::filesystem::path(call.native_history_chats_dir_snapshot);
		std::vector<ChatSession> native_after = ChatHistorySyncService().LoadNativeSessionChats(native_history_chats_dir, call_provider);
		ChatHistorySyncService().ApplyLocalOverrides(app, native_after);
		for (ChatSession& chat : native_after)
		{
			ChatRepository::SaveChat(app.data_root, chat);
		}
		app.chats = ChatRepository::LoadLocalChats(app.data_root);
		app.chats = ChatDomainService().DeduplicateChatsById(std::move(app.chats));
		ChatBranching::Normalize(app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(app);

		std::string selected_id = call.resume_session_id;

		if (NativeSessionLinkService().IsLocalDraftChatId(selected_id))
		{
			selected_id.clear();
		}

		if (selected_id.empty())
		{
			const auto resolved_it = app.resolved_native_sessions_by_chat_id.find(pending_chat_id);

			if (resolved_it != app.resolved_native_sessions_by_chat_id.end() && NativeSessionLinkService().SessionIdExistsInLoadedChats(native_after, resolved_it->second))
			{
				selected_id = resolved_it->second;
			}

			if (selected_id.empty())
			{
				if (NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id))
				{
					if (const auto matched = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(pending_chat_snapshot, native_after, claimed_new_session_ids); matched.has_value())
					{
						selected_id = matched.value();
					}
				}
				else
				{
					const std::vector<std::string> candidates = NativeSessionLinkService().CollectNewSessionIds(native_after, call.session_ids_before);
					selected_id = NativeSessionLinkService().PickFirstUnblockedSessionId(candidates, claimed_new_session_ids);
				}
			}
		}

		if (!selected_id.empty())
		{
			claimed_new_session_ids.insert(selected_id);

			if (call.resume_session_id.empty())
			{
				app.resolved_native_sessions_by_chat_id[pending_chat_id] = selected_id;
			}

			const bool should_follow_to_result = selected_before_id == pending_chat_id;
			const int selected_index = ChatDomainService().FindChatIndexById(app, selected_id);
			const int refreshed_pending_chat_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);

			if (refreshed_pending_chat_index >= 0 && selected_id != pending_chat_id && NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id) && !NativeSessionLinkService().HasRealNativeSessionId(app.chats[refreshed_pending_chat_index]))
			{
				changed |= ChatHistorySyncService().PersistLocalDraftNativeSessionLink(app, app.chats[refreshed_pending_chat_index], selected_id);
			}

			const bool transfer_overrides_to_resolved_chat = pending_chat_index >= 0 && selected_index >= 0 && selected_id != pending_chat_id && NativeSessionLinkService().IsLocalDraftChatId(pending_chat_id);

			if (transfer_overrides_to_resolved_chat)
			{
				app.chats[selected_index].linked_files = pending_chat_snapshot.linked_files;
				app.chats[selected_index].parent_chat_id = pending_chat_snapshot.parent_chat_id;
				app.chats[selected_index].branch_root_chat_id = pending_chat_snapshot.branch_root_chat_id;
				app.chats[selected_index].branch_from_message_index = pending_chat_snapshot.branch_from_message_index;

				if (!pending_chat_snapshot.folder_id.empty())
				{
					app.chats[selected_index].folder_id = pending_chat_snapshot.folder_id;
				}

				ProviderRuntime::SaveHistory(ProviderResolutionService().ProviderForChatOrDefault(app, app.chats[selected_index]), app.data_root, app.chats[selected_index]);
			}

			if (selected_id != pending_chat_id)
			{
				app.chats.erase(std::remove_if(app.chats.begin(), app.chats.end(), [&](const ChatSession& chat) { return chat.id == pending_chat_id; }), app.chats.end());
			}

			if (should_follow_to_result)
			{
				ChatDomainService().SelectChatById(app, selected_id);
				app.scroll_to_bottom = true;
			}
			else
			{
				if (!selected_before_id.empty())
				{
					const int keep_index = ChatDomainService().FindChatIndexById(app, selected_before_id);

					if (keep_index >= 0)
					{
						app.selected_chat_index = keep_index;
						app.chats_with_unseen_updates.erase(selected_before_id);
						ChatDomainService().RefreshRememberedSelection(app);
					}
				}

				if (selected_id != selected_before_id)
				{
					MarkChatUnseen(app, selected_id);
				}
			}

			app.status_line = "Provider response synced from native session.";
		}
		else
		{
			app.resolved_native_sessions_by_chat_id.erase(pending_chat_id);
			const int fallback_index = ChatDomainService().FindChatIndexById(app, pending_chat_id);

			if (fallback_index >= 0)
			{
				ChatDomainService().AddMessage(app.chats[fallback_index], MessageRole::System, output);

				if (pending_chat_id != selected_before_id)
				{
					MarkChatUnseen(app, pending_chat_id);
				}

				app.status_line = "Provider command completed, but no native session was detected.";
				app.scroll_to_bottom = true;
			}
			else
			{
				app.status_line = "Provider command completed, but no native session was detected.";
			}
		}

		ChatBranching::Normalize(app.chats);
		ResetPendingRuntimeCallWorker(call);
		app.pending_calls.erase(app.pending_calls.begin() + static_cast<std::ptrdiff_t>(i));
		changed = true;
	}

	return changed;
}

void ChatHistorySyncService::RefreshChatHistory(uam::AppState& p_app) const
{
	const ChatSession* lcp_selected = ChatDomainService().SelectedChat(p_app);
	const std::string l_selectedId = (lcp_selected != nullptr) ? lcp_selected->id : "";
	SyncChatsFromNative(p_app, l_selectedId, true);
	p_app.status_line = "Chat history refreshed.";
}

bool ChatHistorySyncService::SaveChatWithStatus(uam::AppState& p_app, const ChatSession& p_chat, const std::string& p_success, const std::string& p_failure) const
{
	if (ChatRepository::SaveChat(p_app.data_root, p_chat))
	{
		p_app.status_line = p_success;
		return true;
	}

	p_app.status_line = p_failure;
	return false;
}

bool ChatHistorySyncService::RenameChat(AppState& p_app, ChatSession& p_chat, const std::string& p_requestedTitle) const
{
	const std::string l_previousTitle = p_chat.title;
	const std::string l_previousUpdatedAt = p_chat.updated_at;
	const std::string l_trimmedTitle = Trim(p_requestedTitle);

	if (l_trimmedTitle.empty())
	{
		p_app.status_line = "Chat title is required.";
		return false;
	}

	if (l_trimmedTitle == l_previousTitle)
	{
		return true;
	}

	p_chat.title = l_trimmedTitle;
	p_chat.updated_at = TimestampNow();

	if (SaveChatWithStatus(p_app, p_chat, "Chat title updated.", "Chat title changed in UI, but failed to save."))
	{
		return true;
	}

	p_chat.title = l_previousTitle;
	p_chat.updated_at = l_previousUpdatedAt;
	p_app.status_line = "Failed to save renamed chat file: " + AppPaths::UamChatFilePath(p_app.data_root, p_chat.id).string();
	return false;
}

std::vector<ChatSession> ChatHistorySyncService::LoadNativeSessionChats(const fs::path& p_chatsDir, const ProviderProfile& p_provider, std::stop_token p_stopToken) const
{
	ProviderRuntimeHistoryLoadOptions l_options;
	l_options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	l_options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	if (ProviderRuntime::SupportsGeminiJsonHistory(p_provider))
	{
		return ChatDomainService().DeduplicateChatsById(LoadGeminiJsonHistoryForRuntime(p_chatsDir, p_provider, l_options, p_stopToken));
	}

	return ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(p_provider, fs::path{}, p_chatsDir, l_options));
}

std::optional<fs::path> ChatHistorySyncService::ResolveNativeHistoryChatsDirForWorkspace(const fs::path& p_workspaceRoot) const
{
	if (p_workspaceRoot.empty())
	{
		return std::nullopt;
	}

	const auto l_tmpDir = AppPaths::ResolveGeminiProjectTmpDir(p_workspaceRoot);

	if (l_tmpDir.has_value())
	{
		const fs::path l_chatsDir = l_tmpDir.value() / "chats";
		std::error_code l_ec;
		fs::create_directories(l_chatsDir, l_ec);
		return l_ec ? std::nullopt : std::optional<fs::path>(l_chatsDir);
	}

	return std::nullopt;
}

fs::path ChatHistorySyncService::ResolveNativeHistoryChatsDirForChat(const AppState& p_app, const ChatSession& p_chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		return {};
	}

	const fs::path l_workspaceRoot = ResolveWorkspaceRootPath(p_app, p_chat);
	const auto l_chatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
	return l_chatsDir.has_value() ? l_chatsDir.value() : fs::path{};
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsToLocal(AppState& p_app, const bool p_delete_native_after_import, const std::string& p_targetChatId) const
{
	ImportResult result;
	const ProviderProfile& l_nativeProvider = DefaultNativeHistoryProvider(p_app);

	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);
	std::unordered_set<std::string> l_existingIds;
	l_existingIds.reserve(l_localChats.size());
	std::unordered_map<std::string, std::string> l_existingIdByNativeKey;
	l_existingIdByNativeKey.reserve(l_localChats.size());
	for (const ChatSession& chat : l_localChats)
	{
		l_existingIds.insert(chat.id);
		if (!Trim(chat.native_session_id).empty())
		{
			l_existingIdByNativeKey[NativeIdentityKey(chat)] = chat.id;
		}
	}

	for (const fs::path& l_workspaceRoot : CollectWorkspaceRootsForNativeHistory(p_app))
	{
		const auto l_chatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
		if (!l_chatsDir.has_value())
		{
			continue;
		}

		std::vector<ChatSession> l_nativeChats = LoadNativeSessionChats(l_chatsDir.value(), l_nativeProvider);
		ApplyLocalOverrides(p_app, l_nativeChats);

		for (ChatSession& l_nativeChat : l_nativeChats)
		{
			if (!p_targetChatId.empty() && l_nativeChat.id != p_targetChatId && l_nativeChat.native_session_id != p_targetChatId)
			{
				continue;
			}

			++result.total_count;

			const std::string l_nativeKey = NativeIdentityKey(l_nativeChat);
			const auto l_existingIdIt = l_existingIdByNativeKey.find(l_nativeKey);
			const bool existing_same_native_identity = l_existingIdIt != l_existingIdByNativeKey.end();

			if (p_targetChatId.empty() && existing_same_native_identity)
			{
				continue;
			}

			if (existing_same_native_identity)
			{
				l_nativeChat.id = l_existingIdIt->second;
			}
			else if (l_existingIds.contains(l_nativeChat.id))
			{
				l_nativeChat.id = MakeCollisionSafeImportedChatId(l_nativeChat, l_existingIds);
			}

			if (!l_existingIds.contains(l_nativeChat.id))
			{
				for (const ChatFolder& folder : p_app.folders)
				{
					if (FolderDirectoryMatches(folder.directory, l_workspaceRoot))
					{
						l_nativeChat.folder_id = folder.id;
						l_nativeChat.workspace_directory = folder.directory;
						break;
					}
				}
			}

			if (ChatRepository::SaveChat(p_app.data_root, l_nativeChat))
			{
				++result.imported_count;
				l_existingIds.insert(l_nativeChat.id);
				l_existingIdByNativeKey[l_nativeKey] = l_nativeChat.id;

				if (p_delete_native_after_import && !l_nativeChat.native_session_id.empty())
				{
					std::error_code l_ec;
					if (const auto l_nativeFile = FindNativeSessionFilePath(l_chatsDir.value(), l_nativeChat.native_session_id); l_nativeFile.has_value())
					{
						fs::remove(l_nativeFile.value(), l_ec);
					}
				}
			}
		}
	}

	return result;
}

void ChatHistorySyncService::LoadSidebarChats(AppState& p_app) const
{
	std::string warning;
	p_app.chats = ChatRepository::LoadLocalChats(p_app.data_root, &warning);
	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(p_app.chats));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
	if (!warning.empty())
	{
		p_app.status_line = warning;
	}
}

void ChatHistorySyncService::LoadSidebarChatsByDiscovery(AppState& p_app) const
{
	ReconcileUnresolvedDraftLinksByDiscovery(p_app);
	ImportAllNativeChatsByDiscovery(p_app, false);
	LoadSidebarChats(p_app);
}

void ChatHistorySyncService::ReconcileUnresolvedDraftLinksByDiscovery(AppState& p_app) const
{
	const ProviderProfile& l_nativeProvider = DefaultNativeHistoryProvider(p_app);
	const ProviderDiscoveryResult l_discovery = ProviderRuntime::DiscoverChatSources(l_nativeProvider);

	if (!l_discovery.error.empty())
	{
		return;
	}

	for (const ProviderChatSource& l_source : l_discovery.sources)
	{
		std::vector<ChatSession> l_nativeChats = LoadNativeSessionChats(l_source.chats_dir, l_nativeProvider);

		for (ChatSession& l_nativeChat : l_nativeChats)
		{
			l_nativeChat.workspace_directory = l_source.folder_directory;
		}

		ApplyLocalOverrides(p_app, l_nativeChats);
	}
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsByDiscovery(AppState& p_app, const bool p_delete_native_after_import, const std::string& p_targetChatId) const
{
	ImportResult result;
	const ProviderProfile& l_nativeProvider = DefaultNativeHistoryProvider(p_app);

	const ProviderDiscoveryResult l_discovery = ProviderRuntime::DiscoverChatSources(l_nativeProvider);
	if (!l_discovery.error.empty())
	{
		return result;
	}

	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);
	std::unordered_set<std::string> l_existingIds;
	l_existingIds.reserve(l_localChats.size());
	std::unordered_map<std::string, std::string> l_existingIdByNativeKey;
	l_existingIdByNativeKey.reserve(l_localChats.size());
	for (const ChatSession& chat : l_localChats)
	{
		l_existingIds.insert(chat.id);
		if (!Trim(chat.native_session_id).empty())
		{
			l_existingIdByNativeKey[NativeIdentityKey(chat)] = chat.id;
		}
	}

	for (const ProviderChatSource& l_source : l_discovery.sources)
	{
		const std::string import_folder_id = ResolvePersistedImportFolderIdForSource(p_app, l_source);

		std::vector<ChatSession> l_nativeChats = LoadNativeSessionChats(l_source.chats_dir, l_nativeProvider);
		ApplyLocalOverrides(p_app, l_nativeChats);

		for (ChatSession& l_nativeChat : l_nativeChats)
		{
			if (!p_targetChatId.empty() && l_nativeChat.id != p_targetChatId && l_nativeChat.native_session_id != p_targetChatId)
			{
				continue;
			}

			++result.total_count;

			const std::string l_nativeKey = NativeIdentityKey(l_nativeChat);
			const auto l_existingIdIt = l_existingIdByNativeKey.find(l_nativeKey);
			const bool existing_same_native_identity = l_existingIdIt != l_existingIdByNativeKey.end();

			if (p_targetChatId.empty() && existing_same_native_identity)
			{
				continue;
			}

			if (existing_same_native_identity)
			{
				l_nativeChat.id = l_existingIdIt->second;
			}
			else if (l_existingIds.contains(l_nativeChat.id))
			{
				l_nativeChat.id = MakeCollisionSafeImportedChatId(l_nativeChat, l_existingIds);
			}

			l_nativeChat.folder_id = import_folder_id;
			l_nativeChat.workspace_directory = l_source.folder_directory;

			if (ChatRepository::SaveChat(p_app.data_root, l_nativeChat))
			{
				++result.imported_count;
				l_existingIds.insert(l_nativeChat.id);
				l_existingIdByNativeKey[l_nativeKey] = l_nativeChat.id;

				if (p_delete_native_after_import && !l_nativeChat.native_session_id.empty())
				{
					std::error_code l_ec;
					if (const auto l_nativeFile = FindNativeSessionFilePath(l_source.chats_dir, l_nativeChat.native_session_id); l_nativeFile.has_value())
					{
						fs::remove(l_nativeFile.value(), l_ec);
					}
				}
			}
		}
	}

	return result;
}

bool ChatHistorySyncService::StartAsyncNativeChatLoad(AppState& app, const ProviderProfile& p_provider, const fs::path& p_chatsDir) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ProviderRuntime::UsesNativeOverlayHistory(p_provider) || p_chatsDir.empty() || app.native_chat_load_task.running)
	{
		return false;
	}

	ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
	app.native_chat_load_task.running = true;
	app.native_chat_load_task.provider_id_snapshot = p_provider.id;
	app.native_chat_load_task.chats_dir_snapshot = p_chatsDir.string();
	app.native_chat_load_task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
	std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State> state = app.native_chat_load_task.state;
	const fs::path chats_dir = p_chatsDir;
	const ProviderProfile provider = p_provider;
	app.native_chat_load_task.worker = std::make_unique<std::jthread>(
	    [this, chats_dir, provider, state](std::stop_token stop_token)
	    {
		    try
		    {
			    state->chats = LoadNativeSessionChats(chats_dir, provider, stop_token);
		    }
		    catch (const std::exception& ex)
		    {
			    state->error = ex.what();
		    }
		    catch (...)
		    {
			    state->error = "Unknown native chat load failure.";
		    }

		    state->completed.store(true, std::memory_order_release);
	    });
	return true;
}

bool ChatHistorySyncService::TryConsumeAsyncNativeChatLoad(AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!app.native_chat_load_task.running)
	{
		return false;
	}

	if (app.native_chat_load_task.state == nullptr)
	{
		ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
		chats_out.clear();
		error_out.clear();
		return true;
	}

	if (!app.native_chat_load_task.state->completed.load(std::memory_order_acquire))
	{
		return false;
	}

	chats_out = app.native_chat_load_task.state->chats;
	error_out = app.native_chat_load_task.state->error;
	ResetAsyncNativeChatLoadTask(app.native_chat_load_task);
	return true;
}

std::vector<std::string> ChatHistorySyncService::SessionIdsFromChats(const std::vector<ChatSession>& p_chats) const
{
	std::vector<std::string> l_ids;
	l_ids.reserve(p_chats.size());

	for (const ChatSession& l_chat : p_chats)
	{
		if (NativeSessionLinkService().HasRealNativeSessionId(l_chat))
		{
			l_ids.push_back(l_chat.native_session_id);
		}
	}

	return l_ids;
}

std::optional<fs::path> ChatHistorySyncService::FindNativeSessionFilePath(const fs::path& p_chatsDir, const std::string& p_sessionId) const
{
	if (p_sessionId.empty() || p_chatsDir.empty() || !fs::exists(p_chatsDir))
	{
		return std::nullopt;
	}

	std::error_code l_ec;
	const fs::path canonical_name = p_chatsDir / (p_sessionId + ".json");
	if (fs::exists(canonical_name, l_ec) && !l_ec)
	{
		return canonical_name;
	}

	for (const auto& l_item : fs::directory_iterator(p_chatsDir, l_ec))
	{
		if (l_ec || !l_item.is_regular_file() || l_item.path().extension() != ".json")
		{
			continue;
		}

		const std::string l_text = ReadTextFile(l_item.path());
		const auto l_json = ParseJson(l_text);
		if (!l_json.has_value() || l_json->type != JsonValue::Type::Object)
		{
			continue;
		}

		if (JsonStringOrEmpty(l_json->Find("sessionId")) != p_sessionId)
		{
			continue;
		}

		return l_item.path();
	}

	return std::nullopt;
}

bool ChatHistorySyncService::DeleteNativeSessionFileForChat(const AppState& p_app, const ChatSession& p_chat, std::error_code* p_errorOut) const
{
	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (!ProviderRuntime::UsesNativeOverlayHistory(l_provider))
	{
		return false;
	}

	const std::string l_sessionId = !Trim(p_chat.native_session_id).empty() ? Trim(p_chat.native_session_id) : Trim(p_chat.id);

	if (l_sessionId.empty())
	{
		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	auto l_sessionFile = FindNativeSessionFilePath(l_chatsDir, l_sessionId);

	if (!l_sessionFile.has_value())
	{
		l_sessionFile = FindNativeSessionFileAcrossDiscoveredSources(l_provider, l_sessionId);
	}

	if (!l_sessionFile.has_value())
	{
		return false;
	}

	std::error_code l_ec;
	const bool l_removed = fs::remove(l_sessionFile.value(), l_ec);

	if (p_errorOut != nullptr)
	{
		*p_errorOut = l_ec;
	}

	return l_removed && !l_ec;
}

bool ChatHistorySyncService::PersistLocalDraftNativeSessionLink(const AppState& p_app, ChatSession& p_localChat, const std::string& p_nativeSessionId) const
{
	const std::string l_sessionId = Trim(p_nativeSessionId);

	if (l_sessionId.empty() || NativeSessionLinkService().IsLocalDraftChatId(l_sessionId) || !NativeSessionLinkService().IsLocalDraftChatId(p_localChat.id))
	{
		return false;
	}

	if (NativeSessionLinkService().HasRealNativeSessionId(p_localChat) && Trim(p_localChat.native_session_id) == l_sessionId)
	{
		return true;
	}

	p_localChat.native_session_id = l_sessionId;
	return ChatRepository::SaveChat(p_app.data_root, p_localChat);
}

bool ChatHistorySyncService::MoveChatToFolder(AppState& p_app, ChatSession& p_chat, const std::string& p_newFolderId) const
{
	if (p_newFolderId.empty())
	{
		return false;
	}

	const ChatFolder* l_newFolder = ChatDomainService().FindFolderById(p_app, p_newFolderId);
	if (l_newFolder == nullptr)
	{
		return false;
	}

	const ChatSession l_originalChat = p_chat;
	const std::string l_oldWorkspace = p_chat.workspace_directory;
	const std::string l_oldFolderId = p_chat.folder_id;
	p_app.move_chat_original_folder_id = l_oldFolderId;
	p_app.move_chat_original_workspace = l_oldWorkspace;
	p_app.move_chat_target_folder_id = p_newFolderId;
	p_app.move_chat_target_workspace = l_newFolder->directory;

	ChatSession l_movedChat = p_chat;
	l_movedChat.folder_id = p_newFolderId;
	l_movedChat.workspace_directory = l_newFolder->directory;
	l_movedChat.updated_at = TimestampNow();

	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);
	const std::string l_sessionId = NativeSessionLinkService().HasRealNativeSessionId(p_chat) ? p_chat.native_session_id : "";

	const fs::path normalizedOld = NormalizeWorkspacePathForComparison(l_oldWorkspace);
	const fs::path normalizedNew = NormalizeWorkspacePathForComparison(l_newFolder->directory);
	bool l_workspacesDifferent = !l_sessionId.empty() && !l_oldWorkspace.empty() && normalizedOld != normalizedNew;
	std::optional<fs::path> l_oldChatsDir;

	if (l_workspacesDifferent)
	{
		l_oldChatsDir = ResolveNativeHistoryChatsDirForWorkspace(normalizedOld);
		if (l_oldChatsDir.has_value() && !l_sessionId.empty())
		{
			const auto l_sessionFile = FindNativeSessionFilePath(l_oldChatsDir.value(), l_sessionId);
			if (l_sessionFile.has_value())
			{
				GeminiJsonHistoryStoreOptions l_opts;
				l_opts.max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
				l_opts.max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
				const auto l_parsed = GeminiJsonHistoryStore::ParseFile(l_sessionFile.value(), l_provider, l_opts);
				if (l_parsed.has_value() && !l_parsed->messages.empty() && !LocalMessagesShouldOverrideNative(l_originalChat, *l_parsed))
				{
					l_movedChat.messages = l_parsed->messages;
					if (!l_parsed->updated_at.empty())
					{
						l_movedChat.updated_at = l_parsed->updated_at;
					}
				}
			}
		}

		if (!ProviderRuntime::RebuildNativeSessionFile(l_provider, l_movedChat, l_newFolder->directory))
		{
			p_app.move_chat_pending_id = p_chat.id;
			p_app.move_chat_show_missing_session_warning = true;
			return true;
		}
	}

	p_chat = l_movedChat;
	if (!ChatRepository::SaveChat(p_app.data_root, p_chat))
	{
		if (l_workspacesDifferent && !l_sessionId.empty())
		{
			if (const auto l_targetChatsDir = ResolveNativeHistoryChatsDirForWorkspace(normalizedNew); l_targetChatsDir.has_value())
			{
				std::error_code l_removeTargetEc;
				fs::remove(l_targetChatsDir.value() / (l_sessionId + ".json"), l_removeTargetEc);
			}
		}

		p_chat = l_originalChat;
		p_app.move_chat_pending_id.clear();
		p_app.move_chat_original_folder_id.clear();
		p_app.move_chat_original_workspace.clear();
		p_app.move_chat_target_folder_id.clear();
		p_app.move_chat_target_workspace.clear();
		p_app.move_chat_show_missing_session_warning = false;
		return false;
	}

	if (l_workspacesDifferent && l_oldChatsDir.has_value() && !l_sessionId.empty())
	{
		std::error_code l_cleanupEc;
		if (const auto l_oldSessionFile = FindNativeSessionFilePath(l_oldChatsDir.value(), l_sessionId); l_oldSessionFile.has_value())
		{
			fs::remove(l_oldSessionFile.value(), l_cleanupEc);
		}
	}

	StopAndEraseCliTerminalForChat(p_app, p_chat.id);

	p_app.move_chat_pending_id.clear();
	p_app.move_chat_original_folder_id.clear();
	p_app.move_chat_original_workspace.clear();
	p_app.move_chat_target_folder_id.clear();
	p_app.move_chat_target_workspace.clear();
	p_app.move_chat_show_missing_session_warning = false;
	return true;
}

std::string ChatHistorySyncService::ResolveResumeSessionIdForChat(const AppState& p_app, const ChatSession& p_chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(p_app, p_chat))
	{
		return "";
	}

	fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);

	if (l_chatsDir.empty())
	{
		return "";
	}

	std::string l_candidateId;

	if (NativeSessionLinkService().HasRealNativeSessionId(p_chat))
	{
		l_candidateId = p_chat.native_session_id;
	}
	else if (!p_chat.messages.empty() && !p_chat.id.empty() && !NativeSessionLinkService().IsLocalDraftChatId(p_chat.id))
	{
		l_candidateId = p_chat.id;
	}

	if (l_candidateId.empty())
	{
		return "";
	}

	if (FindNativeSessionFilePath(l_chatsDir, l_candidateId).has_value())
	{
		return l_candidateId;
	}

	const fs::path l_currentWorkspace = ResolveWorkspaceRootPath(p_app, p_chat);
	for (const fs::path& l_workspaceRoot : CollectWorkspaceRootsForNativeHistory(p_app))
	{
		if (l_workspaceRoot == l_currentWorkspace)
		{
			continue;
		}

		const auto l_otherChatsDir = ResolveNativeHistoryChatsDirForWorkspace(l_workspaceRoot);
		if (!l_otherChatsDir.has_value() || l_otherChatsDir.value().empty())
		{
			continue;
		}

		const auto l_sourceSessionFile = FindNativeSessionFilePath(l_otherChatsDir.value(), l_candidateId);
		if (!l_sourceSessionFile.has_value())
		{
			continue;
		}

		std::error_code l_ec;
		fs::create_directories(l_chatsDir, l_ec);
		if (l_ec)
		{
			continue;
		}

		const fs::path l_dest = l_chatsDir / (l_candidateId + ".json");
		fs::copy_file(l_sourceSessionFile.value(), l_dest, fs::copy_options::overwrite_existing, l_ec);
		if (!l_ec)
		{
			return l_candidateId;
		}
	}

	return "";
}

void ChatHistorySyncService::ApplyLocalOverrides(AppState& p_app, std::vector<ChatSession>& p_nativeChats) const
{
	const std::string l_selectedChatId = (ChatDomainService().SelectedChat(p_app) != nullptr) ? ChatDomainService().SelectedChat(p_app)->id : "";
	p_nativeChats = ChatDomainService().DeduplicateChatsById(std::move(p_nativeChats));
	std::vector<ChatSession> l_localChats = ChatRepository::LoadLocalChats(p_app.data_root);

	for (ChatSession& l_local : l_localChats)
	{
		if (NativeSessionLinkService().HasRealNativeSessionId(l_local) || !NativeSessionLinkService().IsLocalDraftChatId(l_local.id))
		{
			continue;
		}

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_local.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = Trim(l_local.provider_id).empty() || (lcp_localProvider != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider));

		if (!l_localChatUsesNativeOverlayHistory)
		{
			continue;
		}

		const auto l_inferredSessionId = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(l_local, p_nativeChats);

		if (l_inferredSessionId.has_value())
		{
			if (PersistLocalDraftNativeSessionLink(p_app, l_local, l_inferredSessionId.value()))
			{
				l_local.native_session_id = l_inferredSessionId.value();
			}
		}
	}

	l_localChats = ChatDomainService().DeduplicateChatsById(std::move(l_localChats));
	std::unordered_map<std::string, const ChatSession*> lcp_localMap;
	std::unordered_map<std::string, const ChatSession*> lcp_localByNativeSessionIdMap;

	for (const ChatSession& l_local : l_localChats)
	{
		lcp_localMap[l_local.id] = &l_local;

		if (NativeSessionLinkService().HasRealNativeSessionId(l_local))
		{
			const std::string l_nativeSessionId = Trim(l_local.native_session_id);

			if (!l_nativeSessionId.empty())
			{
				lcp_localByNativeSessionIdMap[l_nativeSessionId] = &l_local;
			}
		}
	}

	std::unordered_set<std::string> l_nativeIds;

	for (ChatSession& l_native : p_nativeChats)
	{
		l_nativeIds.insert(l_native.id);
		const ChatSession* lcp_local = nullptr;

		if (const auto l_it = lcp_localMap.find(l_native.id); l_it != lcp_localMap.end())
		{
			lcp_local = l_it->second;
		}
		else if (!Trim(l_native.native_session_id).empty())
		{
			if (const auto l_nativeSessionIt = lcp_localByNativeSessionIdMap.find(Trim(l_native.native_session_id)); l_nativeSessionIt != lcp_localByNativeSessionIdMap.end())
			{
				lcp_local = l_nativeSessionIt->second;
			}
		}

		if (lcp_local == nullptr)
		{
			continue;
		}

		const ChatSession& l_local = *lcp_local;
		OverlayLocalChatState(l_local, l_native);

		if (LocalMessagesShouldOverrideNative(l_local, l_native))
		{
			l_native.messages = l_local.messages;

			if (!l_local.updated_at.empty())
			{
				l_native.updated_at = l_local.updated_at;
			}

			if (l_native.created_at.empty() && !l_local.created_at.empty())
			{
				l_native.created_at = l_local.created_at;
			}
		}
		else if (MessagesEquivalent(l_local.messages, l_native.messages) && !l_local.updated_at.empty() && (l_native.updated_at.empty() || l_local.updated_at > l_native.updated_at))
		{
			l_native.updated_at = l_local.updated_at;
		}
	}

	std::vector<ChatSession> l_merged = p_nativeChats;

	for (const ChatSession& l_chat : l_localChats)
	{
		if (l_nativeIds.find(l_chat.id) != l_nativeIds.end())
		{
			continue;
		}

		if (NativeSessionLinkService().HasRealNativeSessionId(l_chat))
		{
			continue;
		}

		const std::string l_normalizedProviderId = ProviderProfileMigrationService().MapLegacyRuntimeId(l_chat.provider_id, false);
		const ProviderProfile* lcp_localProvider = ProviderProfileStore::FindById(p_app.provider_profiles, l_normalizedProviderId);
		const bool l_localChatUsesNativeOverlayHistory = (lcp_localProvider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*lcp_localProvider);

		if (l_localChatUsesNativeOverlayHistory && !NativeSessionLinkService().IsLocalDraftChatId(l_chat.id) && !Trim(l_chat.provider_id).empty())
		{
			continue;
		}

		bool l_hasRunningTerminal = false;

		for (const auto& l_terminal : p_app.cli_terminals)
		{
			if (l_terminal != nullptr && l_terminal->running && CliTerminalMatchesChatId(*l_terminal, l_chat.id))
			{
				l_hasRunningTerminal = true;
				break;
			}
		}

		if (l_chat.messages.empty() && !HasPendingCallForChat(p_app, l_chat.id) && !l_hasRunningTerminal && l_chat.id != l_selectedChatId)
		{
			continue;
		}

		l_merged.push_back(l_chat);
	}

	p_app.chats = ChatDomainService().DeduplicateChatsById(std::move(l_merged));
	ChatBranching::Normalize(p_app.chats);
	ChatDomainService().NormalizeChatFolderAssignments(p_app);
}

void ChatHistorySyncService::RefreshNativeSessionDirectory(AppState& p_app) const
{
	const ChatSession* lcp_selected = ChatDomainService().SelectedChat(p_app);

	if (lcp_selected != nullptr)
	{
		const auto l_selectedChatsDir = ResolveNativeHistoryChatsDirForWorkspace(ResolveWorkspaceRootPath(p_app, *lcp_selected));

		if (l_selectedChatsDir.has_value())
		{
			p_app.native_history_chats_dir = l_selectedChatsDir.value();
			return;
		}
	}

	std::error_code l_cwdEc;
	const fs::path l_cwd = fs::current_path(l_cwdEc);
	const auto l_tmpDir = l_cwdEc ? std::nullopt : ResolveNativeHistoryChatsDirForWorkspace(l_cwd);

	if (l_tmpDir.has_value())
	{
		p_app.native_history_chats_dir = l_tmpDir.value();
	}
	else
	{
		p_app.native_history_chats_dir.clear();
	}
}

bool ChatHistorySyncService::ExportChatToNative(const AppState& p_app, const ChatSession& p_chat) const
{
	const ProviderProfile& l_provider = ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat);

	if (!ProviderRuntime::SupportsGeminiJsonHistory(l_provider))
	{
		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	if (l_chatsDir.empty())
	{
		return false;
	}

	const std::string l_sessionId = NativeSessionLinkService().HasRealNativeSessionId(p_chat) ? p_chat.native_session_id : p_chat.id;
	const fs::path l_destFile = l_chatsDir / (l_sessionId + ".json");

	return GeminiJsonHistoryStore::SaveFile(l_destFile, p_chat);
}

bool ChatHistorySyncService::TruncateNativeSessionFromDisplayedMessage(const AppState& p_app, const ChatSession& p_chat, const int p_displayedMessageIndex, std::string* p_errorOut) const
{
	if (p_errorOut != nullptr)
	{
		p_errorOut->clear();
	}

	if (!NativeSessionLinkService().HasRealNativeSessionId(p_chat))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Chat is not linked to a native runtime session.";
		}

		return false;
	}

	const fs::path l_chatsDir = ResolveNativeHistoryChatsDirForChat(p_app, p_chat);
	const auto l_sessionFile = FindNativeSessionFilePath(l_chatsDir, p_chat.native_session_id);

	if (!l_sessionFile.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session file not found.";
		}

		return false;
	}

	JsonValue l_root;

	const std::string l_fileText = ReadTextFile(l_sessionFile.value());
	const std::optional<JsonValue> l_parsedRoot = ParseJson(l_fileText);

	if (!l_parsedRoot.has_value())
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to parse native runtime session file.";
		}

		return false;
	}

	l_root = l_parsedRoot.value();

	JsonValue* lp_contents = nullptr;
	const auto l_contentsIt = l_root.object_value.find("contents");

	if (l_contentsIt != l_root.object_value.end())
	{
		lp_contents = &l_contentsIt->second;
	}

	if (lp_contents == nullptr || lp_contents->type != JsonValue::Type::Array)
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Native runtime session does not contain a contents array.";
		}

		return false;
	}

	const int l_keepMessages = std::max(0, p_displayedMessageIndex + 1);
	int l_visibleMessages = 0;
	std::size_t l_truncateIndex = lp_contents->array_value.size();

	for (std::size_t l_i = 0; l_i < lp_contents->array_value.size(); ++l_i)
	{
		JsonValue* lp_role = nullptr;
		auto& l_item = lp_contents->array_value[l_i];
		const auto l_roleIt = l_item.object_value.find("role");

		if (l_roleIt != l_item.object_value.end())
		{
			lp_role = &l_roleIt->second;
		}

		if (lp_role == nullptr || lp_role->type != JsonValue::Type::String)
		{
			continue;
		}

		const MessageRole l_role = ProviderRuntime::RoleFromNativeType(ProviderResolutionService().ProviderForChatOrDefault(p_app, p_chat), lp_role->string_value);

		if (l_visibleMessages >= l_keepMessages)
		{
			l_truncateIndex = l_i;
			break;
		}

		++l_visibleMessages;
	}

	lp_contents->array_value.erase(lp_contents->array_value.begin() + static_cast<std::ptrdiff_t>(l_truncateIndex), lp_contents->array_value.end());

	if (!WriteTextFile(l_sessionFile.value(), SerializeJson(l_root)))
	{
		if (p_errorOut != nullptr)
		{
			*p_errorOut = "Failed to write updated native runtime session.";
		}

		return false;
	}

	return true;
}
