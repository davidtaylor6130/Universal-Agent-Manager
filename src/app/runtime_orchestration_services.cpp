#include "runtime_orchestration_services.h"
#include "runtime_orchestration_internal.h"

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"

#include "common/chat/chat_branching.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/chat_repository.h"
#include "common/constants/app_constants.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/base/gemini_history_loader.h"
#endif
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal_common.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "core/chat_import_utils.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	namespace chat_identity = uam::chat_identity;

	constexpr const char* kDefaultNativeHistoryProviderId = provider_build_config::DefaultNativeHistoryProviderId();
	constexpr const char* kMemoryWorkerPromptPrefix = "You are a non-interactive memory extraction function.";
	constexpr const char* kNativeImportTombstonesFile = "native-import-tombstones.json";

	struct NativeImportTombstones
	{
		bool available = false;
		std::unordered_set<std::string> keys;
	};

	bool ParseNativeImportTombstones(const fs::path& path, std::unordered_set<std::string>& keys)
	{
		std::string text;
		if (!uam::io::TryReadTextFile(path, text)) return false;
		const nlohmann::json values = nlohmann::json::parse(text, nullptr, false);
		if (!values.is_array())
		{
			return false;
		}
		for (const nlohmann::json& value : values)
		{
			if (!value.is_string() || uam::strings::IsBlank(value.get_ref<const std::string&>()))
			{
				keys.clear();
				return false;
			}
			keys.insert(value.get<std::string>());
		}
		return true;
	}

	NativeImportTombstones LoadNativeImportTombstones(const fs::path& data_root)
	{
		NativeImportTombstones result;
		const fs::path path = data_root / kNativeImportTombstonesFile;
		const fs::path backup_path = uam::io::MakeBackupPath(path);
		if (!uam::paths::PathExistsNoThrow(path) && !uam::paths::PathExistsNoThrow(backup_path))
		{
			result.available = true;
			return result;
		}
		result.available = ParseNativeImportTombstones(path, result.keys);
		if (!result.available)
		{
			result.keys.clear();
			result.available = ParseNativeImportTombstones(backup_path, result.keys);
		}
		return result;
	}

	bool SaveNativeImportTombstones(const fs::path& data_root, const std::unordered_set<std::string>& keys)
	{
		std::vector<std::string> ordered(keys.begin(), keys.end());
		std::ranges::sort(ordered);
		const fs::path path = data_root / kNativeImportTombstonesFile;
		return uam::io::WriteTextFileWithBackup(path, nlohmann::json(ordered).dump(2));
	}

	fs::path NormalizeWorkspacePathForComparison(const std::string& workspace_directory)
	{
		const std::string trimmed = uam::strings::Trim(workspace_directory);
		if (trimmed.empty())
		{
			return {};
		}

		const fs::path expanded = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(trimmed);
		return uam::paths::NormalizeExistingPath(expanded);
	}

	bool Utf8WorkspaceDirectoriesMatch(const std::string& lhs, const std::string& rhs)
	{
		return FolderDirectoryMatches(NormalizeWorkspacePathForComparison(lhs), NormalizeWorkspacePathForComparison(rhs));
	}

	bool FolderMatchesWorkspaceRoot(const ChatFolder& folder, const fs::path& workspace_root)
	{
		const fs::path normalized_folder = NormalizeWorkspacePathForComparison(folder.directory);
		const fs::path normalized_workspace = uam::paths::NormalizeExistingPath(workspace_root);
		return !normalized_folder.empty() && FolderDirectoryMatches(normalized_folder, normalized_workspace);
	}

	std::string_view RecentChatTimestamp(const ChatSession& chat)
	{
		// Recency = last message activity (updated_at), not selection time. See issue #49.
		return chat.updated_at.empty() ? std::string_view(chat.created_at) : std::string_view(chat.updated_at);
	}

	std::string MakeCollisionSafeImportedChatId(const ChatSession& chat, const std::unordered_set<std::string>& existing_ids)
	{
		const std::string chat_id = uam::strings::Trim(chat.id);
		const std::string native_session_id = uam::strings::Trim(chat.native_session_id);
		const std::string base_id = uam::strings::NonEmptyOrFallback(chat_id, native_session_id);
		const std::string suffix = chat_identity::NativeIdentityKeyHash(chat_identity::NativeIdentityKeyForHistoryImport(chat));
		std::string candidate = base_id + "--" + suffix;

		while (existing_ids.contains(candidate))
		{
			candidate += "_";
		}

		return candidate;
	}

	struct NativeImportIndex
	{
		std::unordered_set<std::string> existing_ids;
		std::unordered_map<std::string, std::string> existing_id_by_native_key;
		std::unordered_map<std::string, ChatSession> existing_summary_by_native_key;
		bool tombstones_available = false;
		std::unordered_set<std::string> tombstoned_native_keys;
	};

	bool MessagesEquivalent(const std::vector<Message>& lhs, const std::vector<Message>& rhs);
	void OverlayLocalChatState(const ChatSession& local, ChatSession& native);

	NativeImportIndex LoadNativeImportIndex(const fs::path& data_root)
	{
		std::vector<ChatSession> local_chats = ChatRepository::LoadLocalChatSummaries(data_root);
		NativeImportIndex import_index;
		NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
		import_index.tombstones_available = tombstones.available;
		import_index.tombstoned_native_keys = std::move(tombstones.keys);
		import_index.existing_ids.reserve(local_chats.size());
		import_index.existing_id_by_native_key.reserve(local_chats.size());
		import_index.existing_summary_by_native_key.reserve(local_chats.size());

		for (const ChatSession& chat : local_chats)
		{
			import_index.existing_ids.insert(chat.id);
			if (!uam::strings::IsBlank(chat.native_session_id))
			{
				const std::string native_key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
				import_index.existing_id_by_native_key[native_key] = chat.id;
				import_index.existing_summary_by_native_key[native_key] = chat;
			}
		}

		return import_index;
	}

	void TrackImportedNativeChat(NativeImportIndex& import_index, const ChatSession& chat, const std::string& native_key)
	{
		import_index.existing_ids.insert(chat.id);
		import_index.existing_id_by_native_key[native_key] = chat.id;
		import_index.existing_summary_by_native_key[native_key] = chat;
	}

	bool RemoveNativeSessionFileIfPresent(const fs::path& chats_dir, const std::string& native_session_id, std::error_code* error_out = nullptr);

	bool TrimmedIdMatches(std::string_view candidate_id, std::string_view target_id)
	{
		return uam::strings::TrimmedEquals(candidate_id, target_id);
	}

	bool NativeChatMatchesImportTarget(const ChatSession& chat, const std::string& target_chat_id)
	{
		if (target_chat_id.empty())
		{
			return true;
		}

		return TrimmedIdMatches(chat.id, target_chat_id) ||
		       TrimmedIdMatches(chat.native_session_id, target_chat_id);
	}

	void DeleteNativeImportSourceIfRequested(const fs::path& chats_dir, const ChatSession& native_chat, bool delete_native_after_import)
	{
		if (delete_native_after_import && !native_chat.native_session_id.empty())
		{
			RemoveNativeSessionFileIfPresent(chats_dir, native_chat.native_session_id);
		}
	}

	void PreserveEquivalentLocalMessageDetails(const ChatSession& local_chat, ChatSession& native_chat)
	{
		const std::size_t common_count = std::min(local_chat.messages.size(), native_chat.messages.size());
		for (std::size_t index = 0; index < common_count; ++index)
		{
			const Message& local_message = local_chat.messages[index];
			const Message& native_message = native_chat.messages[index];
			if (local_message.role == native_message.role && local_message.content == native_message.content && local_message.created_at == native_message.created_at)
			{
				native_chat.messages[index] = local_message;
			}
		}
	}

	bool ReconcileExistingNativeImport(const fs::path& data_root, const ChatSession& local_summary, ChatSession& native_chat)
	{
		std::optional<ChatSession> local = ChatRepository::LoadLocalChat(data_root, local_summary.id, true);
		if (!local) return false;
		const std::size_t local_count = local->messages.size();
		const std::size_t native_count = native_chat.messages.size();
		const bool native_changed = native_count > local_count ||
		    (native_count == local_count && native_chat.updated_at > local->updated_at && !MessagesEquivalent(local->messages, native_chat.messages));
		OverlayLocalChatState(*local, native_chat);
		if (!native_changed)
		{
			native_chat.messages = local->messages;
			native_chat.updated_at = local->updated_at;
			return false;
		}
		PreserveEquivalentLocalMessageDetails(*local, native_chat);
		return true;
	}

	std::optional<std::string> PrepareNativeChatForImport(const fs::path& data_root, NativeImportIndex& import_index, ChatSession& native_chat, const std::string& target_chat_id)
	{
		if (!import_index.tombstones_available)
		{
			return std::nullopt;
		}
		const std::string native_key = chat_identity::NativeIdentityKeyForHistoryImport(native_chat);
		if (import_index.tombstoned_native_keys.contains(native_key))
		{
			return std::nullopt;
		}
		const auto existing_id_it = import_index.existing_id_by_native_key.find(native_key);
		const bool existing_same_native_identity = existing_id_it != import_index.existing_id_by_native_key.end();

		if (existing_same_native_identity)
		{
			native_chat.id = existing_id_it->second;
			const auto summary = import_index.existing_summary_by_native_key.find(native_key);
			if (target_chat_id.empty() && (summary == import_index.existing_summary_by_native_key.end() || !ReconcileExistingNativeImport(data_root, summary->second, native_chat))) return std::nullopt;
		}
		else if (import_index.existing_ids.contains(native_chat.id))
		{
			native_chat.id = MakeCollisionSafeImportedChatId(native_chat, import_index.existing_ids);
		}

		return native_key;
	}

	void AssignKnownWorkspaceFolderToNewImport(const uam::AppState& app, const NativeImportIndex& import_index, const fs::path& workspace_root, ChatSession& native_chat)
	{
		if (import_index.existing_ids.contains(native_chat.id))
		{
			return;
		}

		for (const ChatFolder& folder : app.folders)
		{
			if (uam::paths::IsControllerLocalWorkspace(folder) &&
			    FolderMatchesWorkspaceRoot(folder, workspace_root))
			{
				native_chat.folder_id = folder.id;
				native_chat.workspace_directory = folder.directory;
				return;
			}
		}
	}

	bool SaveImportedNativeChat(const uam::AppState& app,
	                            NativeImportIndex& import_index,
	                            const ChatSession& native_chat,
	                            const std::string& native_key,
	                            const fs::path& chats_dir,
	                            bool delete_native_after_import)
	{
		if (!ChatRepository::SaveChat(app.data_root, native_chat))
		{
			return false;
		}

		TrackImportedNativeChat(import_index, native_chat, native_key);
		DeleteNativeImportSourceIfRequested(chats_dir, native_chat, delete_native_after_import);
		return true;
	}

	bool IsCodexSyntheticUserMessage(std::string_view content)
	{
		return uam::strings::StartsWith(uam::strings::TrimAsciiView(content), "<environment_context>");
	}

	std::string CodexMessageText(const nlohmann::json& payload)
	{
		const auto content_it = payload.find("content");
		if (content_it == payload.end() || !content_it->is_array())
		{
			return {};
		}

		std::string text;
		for (const nlohmann::json& item : *content_it)
		{
			if (!item.is_object())
			{
				continue;
			}
			const std::string_view type = uam::nlohmann_json::TrimmedStringViewOrEmpty(item, "type");
			if (type != "input_text" && type != "output_text" && type != "text")
			{
				continue;
			}
			const std::string_view part = uam::nlohmann_json::StringViewOrEmpty(item, "text");
			if (uam::strings::TrimAsciiView(part).empty())
			{
				continue;
			}
			if (!text.empty())
			{
				text += '\n';
			}
			text += part;
		}
		return text;
	}

	std::optional<ChatSession> LoadCodexRolloutChat(const fs::path& rollout_file, const ChatFolder& folder, bool* malformed_out = nullptr)
	{
		if (malformed_out != nullptr) *malformed_out = false;
		ChatSession chat;
		bool metadata_seen = false;
		bool include = false;
		bool has_user_message = false;
		bool parse_error = false;

		const bool read_success = uam::io::ForEachTextFileLine(
		    rollout_file,
		    [&](const std::string& line)
		    {
			    try
			    {
				    const nlohmann::json record = nlohmann::json::parse(line);
				    const std::string_view record_type = uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "type");
				    const auto payload_it = record.find("payload");
				    if (payload_it == record.end() || !payload_it->is_object())
				    {
					    return true;
				    }
				    const nlohmann::json& payload = *payload_it;

				    if (record_type == "session_meta")
				    {
					    metadata_seen = true;
					    chat.native_session_id = uam::codex::ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(payload, "id"));
					    if (chat.native_session_id.empty())
					    {
						    chat.native_session_id = uam::codex::ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(payload, "session_id"));
					    }
					    const std::string cwd{uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "cwd")};
					    const bool is_subagent =
					        uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "thread_source") == "subagent" ||
					        (payload.contains("source") && payload["source"].is_object() && payload["source"].contains("subagent"));
					    include = !chat.native_session_id.empty() && !cwd.empty() && !is_subagent &&
					              uam::codex::PathsMatch(cwd, folder.directory);
					    if (!include)
					    {
						    return false;
					    }

					    chat.id = chat.native_session_id;
					    chat.branch_root_chat_id = chat.id;
					    chat.provider_id = uam::provider_ids::kCodexCli;
					    chat.folder_id = folder.id;
					    chat.workspace_directory = folder.directory;
					    chat.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "timestamp")};
					    if (chat.created_at.empty())
					    {
						    chat.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};
					    }
					    chat.updated_at = chat.created_at;
					    return true;
				    }

				    if (!include || record_type != "response_item" ||
				        uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "type") != "message")
				    {
					    return true;
				    }

				    const std::string_view role = uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "role");
				    if (role != "user" && role != "assistant")
				    {
					    return true;
				    }
				    std::string content = CodexMessageText(payload);
				    if (content.empty() || (role == "user" && IsCodexSyntheticUserMessage(content)))
				    {
					    return true;
				    }

				    Message message;
				    message.role = role == "user" ? MessageRole::User : MessageRole::Assistant;
				    message.content = std::move(content);
				    message.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};
				    message.provider = uam::provider_ids::kCodexCli;
				    has_user_message = has_user_message || message.role == MessageRole::User;
				    if (!message.created_at.empty())
				    {
					    chat.updated_at = message.created_at;
				    }
				    chat.messages.push_back(std::move(message));
			    }
			    catch (const nlohmann::json::exception&)
			    {
				    parse_error = true;
			    }
			    return true;
			});
		if (malformed_out != nullptr) *malformed_out = !read_success || (parse_error && !metadata_seen);

		if (!metadata_seen || !include || !has_user_message)
		{
			return std::nullopt;
		}
		chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
		return chat;
	}

	bool IsUamMemoryWorkerNativeChat(const ChatSession& chat)
	{
		for (const Message& message : chat.messages)
		{
			if (message.role != MessageRole::User)
			{
				continue;
			}
			const std::string trimmed_content = uam::strings::Trim(message.content);
			if (uam::strings::StartsWith(trimmed_content, kMemoryWorkerPromptPrefix))
			{
				return true;
			}
		}
		return false;
	}

	bool NativeChatShouldBeImported(const fs::path& chats_dir, const ChatSession& native_chat, const std::string& target_id, bool delete_native_after_import)
	{
		if (IsUamMemoryWorkerNativeChat(native_chat))
		{
			DeleteNativeImportSourceIfRequested(chats_dir, native_chat, delete_native_after_import);
			return false;
		}

		return NativeChatMatchesImportTarget(native_chat, target_id);
	}

	std::string ResolvePersistedImportFolderIdForSource(uam::AppState& app, const ProviderChatSource& source)
	{
		for (const ChatFolder& folder : app.folders)
		{
			if (uam::paths::IsControllerLocalWorkspace(folder) &&
			    Utf8WorkspaceDirectoriesMatch(folder.directory, source.folder_directory))
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

		std::erase_if(app.folders, [&](const ChatFolder& folder) { return folder.id == created_folder_id; });
		app.status_line = "Imported chats without a saved folder because folder metadata could not be saved.";
		return "";
	}

	const ProviderProfile& DefaultNativeHistoryProvider(const uam::AppState& app)
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

	std::vector<fs::path> CollectWorkspaceRootsForNativeHistory(const uam::AppState& app)
	{
		std::vector<fs::path> roots;
		std::unordered_set<std::string> seen;

		for (const ChatFolder& folder : app.folders)
		{
			if (!uam::paths::IsControllerLocalWorkspace(folder))
			{
				continue;
			}
			fs::path root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder.directory);

			if (root.empty())
			{
				continue;
			}

			const fs::path normalized_root = uam::paths::AbsolutePathNoThrow(root);
			const std::string key = uam::paths::Utf8PathString(normalized_root);

			if (key.empty() || seen.contains(key))
			{
				continue;
			}

			seen.insert(key);
			roots.push_back(normalized_root);
		}

		if (roots.empty())
		{
			if (const std::optional<fs::path> current_root = uam::paths::CurrentPathNoThrow())
			{
				roots.push_back(*current_root);
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

	std::size_t EffectiveMessageCount(const ChatSession& chat)
	{
		return chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
	}

	bool LocalMessagesShouldOverrideNative(const ChatSession& local_chat, const ChatSession& native_chat)
	{
		const std::size_t local_count = EffectiveMessageCount(local_chat);
		const std::size_t native_count = EffectiveMessageCount(native_chat);
		return local_count > 0 && (local_count > native_count || (local_count == native_count && local_chat.updated_at > native_chat.updated_at));
	}

	void OverlayLocalChatState(const ChatSession& local, ChatSession& native)
	{
		const NativeSessionLinkService native_session_links;
		const std::string local_session_id = native_session_links.RealNativeSessionId(local);
		if (!local_session_id.empty() && !uam::strings::IsBlank(local.id) && local_session_id == native_session_links.RealNativeSessionId(native))
		{
			native.id = local.id;
		}

		if (!uam::strings::IsBlank(local.provider_id))
		{
			native.provider_id = local.provider_id;
		}

		native.title = local.title;
		native.folder_id = local.folder_id;
		native.pinned = local.pinned;
		native.linked_files = local.linked_files;
		native.parent_chat_id = local.parent_chat_id;
		native.branch_root_chat_id = local.branch_root_chat_id;
		native.branch_from_message_index = local.branch_from_message_index;
		native.branch_message_edited = local.branch_message_edited;
		native.goal_owner_chat_id = local.goal_owner_chat_id;
		native.goal_iteration_goal_id = local.goal_iteration_goal_id;
		native.goal_iteration_turn_kind = local.goal_iteration_turn_kind;
		native.goal_iteration_repair_attempts = local.goal_iteration_repair_attempts;
		native.workspace_directory = local.workspace_directory;
		native.workspace_isolation_kind = local.workspace_isolation_kind;
		native.workspace_source_directory = local.workspace_source_directory;
		native.workspace_base_ref = local.workspace_base_ref;
		native.workspace_branch_name = local.workspace_branch_name;
		native.workspace_worktree_directory = local.workspace_worktree_directory;
		native.imported_read_only = local.imported_read_only;
		native.approval_mode = local.approval_mode;
		native.command_safety_tier = local.command_safety_tier;
		native.model_id = local.model_id;
		native.reviewer_model_id = local.reviewer_model_id;
		native.reasoning_effort = local.reasoning_effort;
		native.service_tier = local.service_tier;
		native.service_tier_explicit = local.service_tier_explicit;
		native.small_model_mode = local.small_model_mode;
		native.extra_flags = local.extra_flags;
		native.memory_enabled = local.memory_enabled;
		native.memory_level = local.memory_level;
		native.memory_last_processed_message_count = local.memory_last_processed_message_count;
		native.memory_last_processed_at = local.memory_last_processed_at;
		native.goals = local.goals;
		native.active_goal_id = local.active_goal_id;
		if (!uam::strings::IsBlank(local.last_opened_at))
		{
			native.last_opened_at = local.last_opened_at;
		}
	}

	std::optional<fs::path> FindNativeSessionFileAcrossDiscoveredSources(const ProviderProfile& provider, const std::string& native_session_id)
	{
		if (uam::strings::IsBlank(native_session_id))
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

			if (session_file)
			{
				if (matched_file && *matched_file != *session_file)
				{
					return std::nullopt;
				}

				matched_file = session_file;
			}
		}

		return matched_file;
	}

	bool RemoveNativeSessionFileIfPresent(const fs::path& chats_dir, const std::string& native_session_id, std::error_code* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}

		if (uam::strings::IsBlank(native_session_id))
		{
			return false;
		}

		const std::optional<fs::path> native_file = ChatHistorySyncService().FindNativeSessionFilePath(chats_dir, native_session_id);
		if (!native_file)
		{
			return false;
		}

		std::error_code ec;
		const bool removed = uam::paths::RemoveFileNoThrow(*native_file, &ec);
		if (error_out != nullptr)
		{
			*error_out = ec;
		}
		return removed;
	}

	std::string NativeSessionFileOperationId(const ChatSession& chat)
	{
		const std::string native_session_id = uam::strings::Trim(chat.native_session_id);
		return native_session_id.empty() ? uam::strings::Trim(chat.id) : native_session_id;
	}

	void ApplyNewerNativeMessagesBeforeWorkspaceMove(const ProviderProfile& provider,
	                                                 const ChatSession& original_chat,
	                                                 const fs::path& old_chats_dir,
	                                                 const std::string& session_id,
	                                                 ChatSession& moved_chat)
	{
		if (old_chats_dir.empty() || session_id.empty())
		{
			return;
		}

		const auto session_file = ChatHistorySyncService().FindNativeSessionFilePath(old_chats_dir, session_id);
		if (!session_file)
		{
			return;
		}

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		GeminiJsonHistoryStoreOptions options;
		options.max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
		options.max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
		const auto parsed = GeminiJsonHistoryStore::ParseFile(*session_file, provider, options);
		if (parsed && !parsed->messages.empty() && !LocalMessagesShouldOverrideNative(original_chat, *parsed))
		{
			moved_chat.messages = parsed->messages;
			if (!parsed->updated_at.empty())
			{
				moved_chat.updated_at = parsed->updated_at;
			}
		}
#else
		(void)provider;
		(void)original_chat;
		(void)moved_chat;
#endif
	}

	void ClearMoveChatState(uam::AppState& app)
	{
		app.move_chat_pending_id.clear();
		app.move_chat_original_folder_id.clear();
		app.move_chat_original_workspace.clear();
		app.move_chat_target_folder_id.clear();
		app.move_chat_target_workspace.clear();
		app.move_chat_show_missing_session_warning = false;
	}

	const ProviderProfile* FindProviderForLocalChat(const uam::AppState& app, const ChatSession& chat)
	{
		const std::string normalized_provider_id = ProviderProfileMigrationService().MapLegacyRuntimeId(chat.provider_id, false);
		return ProviderProfileStore::FindById(app.provider_profiles, normalized_provider_id);
	}

	bool LocalDraftCanInferNativeSessionLink(const uam::AppState& app, const ChatSession& chat)
	{
		if (NativeSessionLinkService().HasRealNativeSessionId(chat) || !NativeSessionLinkService().IsLocalDraftChatId(chat.id))
		{
			return false;
		}

		const ProviderProfile* provider = FindProviderForLocalChat(app, chat);
		return uam::strings::IsBlank(chat.provider_id) || (provider != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*provider));
	}

	bool LocalChatIsRepresentedByNativeOverlay(const uam::AppState& app, const ChatSession& chat)
	{
		if (NativeSessionLinkService().IsLocalDraftChatId(chat.id) || uam::strings::IsBlank(chat.provider_id))
		{
			return false;
		}

		const ProviderProfile* provider = FindProviderForLocalChat(app, chat);
		return (provider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*provider);
	}

	struct LocalChatOverlayIndex
	{
		std::unordered_map<std::string, ChatSession*> by_id;
		std::unordered_map<std::string, ChatSession*> by_native_session_id;
	};

	LocalChatOverlayIndex BuildLocalChatOverlayIndex(std::vector<ChatSession>& local_chats)
	{
		LocalChatOverlayIndex index;
		index.by_id.reserve(local_chats.size());
		index.by_native_session_id.reserve(local_chats.size());

		for (ChatSession& local_chat : local_chats)
		{
			index.by_id[local_chat.id] = &local_chat;

			if (!NativeSessionLinkService().HasRealNativeSessionId(local_chat))
			{
				continue;
			}

			const std::string native_session_id = uam::strings::Trim(local_chat.native_session_id);
			if (!native_session_id.empty())
			{
				index.by_native_session_id[native_session_id] = &local_chat;
			}
		}

		return index;
	}

	ChatSession* FindLocalOverlayMatch(const LocalChatOverlayIndex& index, const ChatSession& native_chat)
	{
		if (const auto local_it = index.by_id.find(native_chat.id); local_it != index.by_id.end())
		{
			return local_it->second;
		}

		const std::string native_session_id = uam::strings::Trim(native_chat.native_session_id);
		if (native_session_id.empty())
		{
			return nullptr;
		}

		const auto native_session_it = index.by_native_session_id.find(native_session_id);
		return native_session_it == index.by_native_session_id.end() ? nullptr : native_session_it->second;
	}

	bool LocalChatHasActiveWork(const uam::AppState& app, const ChatSession& local_chat, const std::string& selected_chat_id)
	{
		return EffectiveMessageCount(local_chat) > 0 ||
		       uam::HasPendingCallForChat(app, local_chat.id) ||
		       uam::ChatHasActiveCliTerminal(app, local_chat.id) ||
		       local_chat.id == selected_chat_id;
	}

	void NormalizeLegacyOpenCodeChatsForSidebar(uam::AppState& app, std::vector<ChatSession>& chats)
	{
		const ProviderProfile* opencode_profile = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kOpenCodeCli);
		if (opencode_profile == nullptr || !ProviderRuntime::IsRuntimeEnabled(*opencode_profile))
		{
			return;
		}

		for (ChatSession& chat : chats)
		{
			if (!chat.provider_id.empty() || chat.native_session_id.empty())
			{
				continue;
			}

			uam::provider_ids::NormalizeLegacyLocalHistoryChatProvider(chat.provider_id, uam::provider_ids::kOpenCodeCli);
		}
	}

	bool IsLaterOpenCodeNativeSessionMatch(const ChatSession& candidate, const ChatSession& existing)
	{
		const std::string_view candidate_recent = RecentChatTimestamp(candidate);
		const std::string_view existing_recent = RecentChatTimestamp(existing);
		if (candidate_recent != existing_recent)
		{
			return candidate_recent > existing_recent;
		}

		if (candidate.updated_at != existing.updated_at)
		{
			return candidate.updated_at > existing.updated_at;
		}

		if (candidate.created_at != existing.created_at)
		{
			return candidate.created_at > existing.created_at;
		}

		return candidate.id > existing.id;
	}
} // namespace

using namespace uam::runtime_orch_impl;

void ChatHistorySyncService::RefreshChatHistory(uam::AppState& app) const
{
	const std::string selected_id = ChatDomainService().SelectedChatId(app);
	uam::SyncChatsFromNative(app, selected_id, true);
	app.status_line = "Chat history refreshed.";
}

bool ChatHistorySyncService::SaveChatWithStatus(uam::AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure) const
{
	if (ChatRepository::SaveChat(app.data_root, chat))
	{
		app.status_line = success;
		return true;
	}

	app.status_line = failure;
	return false;
}

bool ChatHistorySyncService::RenameChat(uam::AppState& app, ChatSession& chat, const std::string& requested_title) const
{
	const std::string previous_title = chat.title;
	const std::string previous_updated_at = chat.updated_at;
	const std::string trimmed_title = uam::strings::Trim(requested_title);

	if (trimmed_title.empty())
	{
		app.status_line = "Chat title is required.";
		return false;
	}

	if (trimmed_title == previous_title)
	{
		return true;
	}

	chat.title = trimmed_title;
	chat.updated_at = uam::time::TimestampNow();

	if (SaveChatWithStatus(app, chat, "Chat title updated.", "Chat title changed in UI, but failed to save."))
	{
		return true;
	}

	chat.title = previous_title;
	chat.updated_at = previous_updated_at;
	app.status_line = "Failed to save renamed chat file: " + uam::paths::Utf8PathString(AppPaths::UamChatFilePath(app.data_root, chat.id));
	return false;
}

std::vector<ChatSession> ChatHistorySyncService::LoadNativeSessionChats(const fs::path& chats_dir, const ProviderProfile& provider, std::stop_token stop_token) const
{
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	if (ProviderRuntime::SupportsGeminiJsonHistory(provider))
	{
		return ChatDomainService().DeduplicateChatsById(LoadGeminiJsonHistoryForRuntime(chats_dir, provider, options, stop_token));
	}
#endif

	return ChatDomainService().DeduplicateChatsById(ProviderRuntime::LoadHistory(provider, fs::path{}, chats_dir, options));
}

std::optional<fs::path> ChatHistorySyncService::ResolveNativeHistoryChatsDirForWorkspace(const fs::path& workspace_root) const
{
	if (workspace_root.empty())
	{
		return std::nullopt;
	}

	const auto tmp_dir = AppPaths::ResolveGeminiProjectTmpDir(workspace_root);

	if (tmp_dir)
	{
		const fs::path chats_dir = *tmp_dir / "chats";
		std::error_code error;
		return uam::paths::CreateDirectoriesNoThrow(chats_dir, &error) ? std::optional<fs::path>(chats_dir) : std::nullopt;
	}

	return std::nullopt;
}

fs::path ChatHistorySyncService::ResolveNativeHistoryChatsDirForChat(const uam::AppState& app, const ChatSession& chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat))
	{
		return {};
	}

	const fs::path workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(app, chat);
	const auto chats_dir = ResolveNativeHistoryChatsDirForWorkspace(workspace_root);
	return chats_dir ? *chats_dir : fs::path{};
}

bool ChatHistorySyncService::AddNativeImportTombstones(
    const fs::path& data_root,
    const std::vector<ChatSession>& chats,
    std::vector<std::string>& added_keys) const
{
	added_keys.clear();
	std::unordered_set<std::string> candidate_keys;
	for (const ChatSession& chat : chats)
	{
		if (!uam::strings::IsBlank(chat.native_session_id))
		{
			candidate_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
		}
	}
	if (candidate_keys.empty())
	{
		return true;
	}
	NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
	if (!tombstones.available)
	{
		return false;
	}
	std::unordered_set<std::string>& keys = tombstones.keys;
	for (const std::string& key : candidate_keys)
	{
		if (keys.insert(key).second)
		{
			added_keys.push_back(key);
		}
	}
	if (added_keys.empty() || SaveNativeImportTombstones(data_root, keys))
	{
		return true;
	}
	added_keys.clear();
	return false;
}

bool ChatHistorySyncService::RemoveNativeImportTombstones(
    const fs::path& data_root,
    const std::vector<std::string>& keys_to_remove) const
{
	if (keys_to_remove.empty())
	{
		return true;
	}
	NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
	if (!tombstones.available) return false;
	std::unordered_set<std::string>& keys = tombstones.keys;
	for (const std::string& key : keys_to_remove)
	{
		keys.erase(key);
	}
	return SaveNativeImportTombstones(data_root, keys);
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsToLocal(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id) const
{
	ImportResult result;
	const std::string target_id = uam::strings::Trim(target_chat_id);
	const ProviderProfile& native_provider = DefaultNativeHistoryProvider(app);
	NativeImportIndex import_index = LoadNativeImportIndex(app.data_root);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; import stopped to avoid restoring deleted chats.");
		return result;
	}

	for (const fs::path& workspace_root : CollectWorkspaceRootsForNativeHistory(app))
	{
		const auto chats_dir = ResolveNativeHistoryChatsDirForWorkspace(workspace_root);
		if (!chats_dir)
		{
			continue;
		}

		std::vector<ChatSession> native_chats = LoadNativeSessionChats(*chats_dir, native_provider);
		ApplyLocalOverrides(app, native_chats);

		for (ChatSession& native_chat : native_chats)
		{
			if (!NativeChatShouldBeImported(*chats_dir, native_chat, target_id, delete_native_after_import))
			{
				continue;
			}

			++result.total_count;

			native_chat.workspace_directory = uam::paths::Utf8PathString(workspace_root);
			AssignKnownWorkspaceFolderToNewImport(app, import_index, workspace_root, native_chat);

			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, native_chat, target_id);
			if (!native_key)
			{
				continue;
			}

			if (SaveImportedNativeChat(app, import_index, native_chat, *native_key, *chats_dir, delete_native_after_import))
			{
				++result.imported_count;
			}
			else
			{
				result.Fail("Failed to save imported " + native_provider.title + " chat " + native_chat.native_session_id + ".");
			}
		}
	}

	return result;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportCodexRolloutChatsForFolder(uam::AppState& app, const std::string& folder_id) const
{
	ImportResult result;
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const ChatFolder* matched_folder = ChatDomainService().FindFolderById(app, uam::strings::Trim(folder_id));
	if (matched_folder == nullptr || uam::strings::IsBlank(matched_folder->directory))
	{
		result.Fail("Codex history scan requires a valid workspace folder.");
		return result;
	}
	if (!uam::paths::IsControllerLocalWorkspace(*matched_folder))
	{
		result.Fail("Remote native-history rescanning is not supported yet.");
		return result;
	}
	const ChatFolder folder = *matched_folder;
	NativeImportIndex import_index = LoadNativeImportIndex(app.data_root);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; Codex import stopped to avoid restoring deleted chats.");
		return result;
	}

	for (const fs::path& root : {uam::codex::CodexHomePath() / "sessions", uam::codex::CodexHomePath() / "archived_sessions"})
	{
		if (!uam::paths::IsDirectoryNoThrow(root))
		{
			continue;
		}
		std::error_code error;
		constexpr auto options = fs::directory_options::skip_permission_denied;
		for (fs::recursive_directory_iterator it(root, options, error), end; !error && it != end; it.increment(error))
		{
			if (!uam::paths::IsRegularFileEntryNoThrow(*it) || it->path().extension() != ".jsonl")
			{
				continue;
			}
			bool malformed = false;
			std::optional<ChatSession> chat = LoadCodexRolloutChat(it->path(), folder, &malformed);
			if (malformed) result.Fail("Could not parse Codex history file " + uam::paths::Utf8PathString(it->path()) + ".");
			if (!chat)
			{
				continue;
			}
			++result.total_count;
			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, *chat, "");
			if (native_key && SaveImportedNativeChat(app, import_index, *chat, *native_key, it->path().parent_path(), false))
			{
				++result.imported_count;
			}
			else if (native_key)
			{
				result.Fail("Failed to save imported Codex chat " + chat->native_session_id + ".");
			}
		}
		if (error) result.Fail("Could not finish scanning Codex history: " + error.message());
	}
#else
	(void)app;
	(void)folder_id;
#endif
	return result;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportProviderChatsForFolder(uam::AppState& app, const std::string& folder_id) const
{
	const ChatFolder* matched_folder = ChatDomainService().FindFolderById(app, uam::strings::Trim(folder_id));
	if (matched_folder != nullptr && uam::strings::NonEmptyOrFallback(
	        matched_folder->execution_host_id, "local") != uam::execution_hosts::kLocalHostId)
	{
		ImportResult result;
		result.Fail("Remote native-history rescanning is not supported yet.");
		return result;
	}
	ImportResult result = ImportCodexRolloutChatsForFolder(app, folder_id);
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (matched_folder == nullptr || uam::strings::IsBlank(matched_folder->directory))
	{
		if (result.success) result.Fail("Copilot history scan requires a valid workspace folder.");
		return result;
	}
	const ChatFolder folder = *matched_folder;
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	std::string copilot_error;
	std::vector<ChatSession> copilot_chats = LoadCopilotSessionStateChats(
	    CopilotSessionStatePath(),
	    folder.directory,
	    options,
	    &copilot_error);
	if (!copilot_error.empty()) result.Fail(copilot_error);
	NativeImportIndex import_index = LoadNativeImportIndex(app.data_root);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; Copilot import stopped to avoid restoring deleted chats.");
		return result;
	}

	for (ChatSession& chat : copilot_chats)
	{
		++result.total_count;
		chat.folder_id = folder.id;
		chat.workspace_directory = folder.directory;
		const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, chat, "");
		if (native_key && SaveImportedNativeChat(app, import_index, chat, *native_key, CopilotSessionStatePath(), false))
		{
			++result.imported_count;
		}
		else if (native_key)
		{
			result.Fail("Failed to save imported Copilot chat " + chat.native_session_id + ".");
		}
	}
#else
	(void)app;
	(void)folder_id;
#endif
	return result;
}

void ChatHistorySyncService::LoadSidebarChats(uam::AppState& app) const
{
	std::string warning;
	std::vector<ChatSession> chats = ChatRepository::LoadLocalChatSummaries(app.data_root, &warning);
	std::erase_if(chats, [](const ChatSession& chat) { return !chat.agent_run_id.empty(); });
	NormalizeLegacyOpenCodeChatsForSidebar(app, chats);
	ReplaceAppChatsWithNormalized(app, std::move(chats));
	if (!warning.empty())
	{
		app.status_line = warning;
	}
}

void ChatHistorySyncService::MergeSidebarChatsPreservingCurrent(uam::AppState& app) const
{
	std::unordered_map<std::string, ChatSession> current_by_id;
	current_by_id.reserve(app.chats.size());
	for (ChatSession& chat : app.chats)
	{
		current_by_id.emplace(chat.id, std::move(chat));
	}

	std::string warning;
	std::vector<ChatSession> chats = ChatRepository::LoadLocalChatSummaries(app.data_root, &warning);
	std::erase_if(chats, [](const ChatSession& chat) { return !chat.agent_run_id.empty(); });
	for (ChatSession& chat : chats)
	{
		const auto current = current_by_id.find(chat.id);
		if (current == current_by_id.end())
		{
			continue;
		}
		chat = std::move(current->second);
		current_by_id.erase(current);
	}
	for (auto& [id, chat] : current_by_id)
	{
		(void)id;
		chats.push_back(std::move(chat));
	}
	ReplaceAppChatsWithNormalized(app, std::move(chats));
	if (!warning.empty())
	{
		app.status_line = warning;
	}
}

void ChatHistorySyncService::ReconcileUnresolvedDraftLinksByDiscovery(uam::AppState& app) const
{
	const ProviderProfile& native_provider = DefaultNativeHistoryProvider(app);
	const ProviderDiscoveryResult discovery = ProviderRuntime::DiscoverChatSources(native_provider);

	if (!discovery.error.empty())
	{
		return;
	}

	for (const ProviderChatSource& source : discovery.sources)
	{
		std::vector<ChatSession> native_chats = LoadNativeSessionChats(source.chats_dir, native_provider);

		NativeImportIndex import_index = LoadNativeImportIndex(app.data_root);
		for (ChatSession& native_chat : native_chats)
		{
			native_chat.workspace_directory = source.folder_directory;
			AssignKnownWorkspaceFolderToNewImport(app, import_index, uam::paths::PathFromUtf8(source.folder_directory), native_chat);
		}

		ApplyLocalOverrides(app, native_chats);
	}
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsByDiscovery(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id) const
{
	ImportResult result;
	const std::string target_id = uam::strings::Trim(target_chat_id);
	const ProviderProfile& native_provider = DefaultNativeHistoryProvider(app);

	const ProviderDiscoveryResult discovery = ProviderRuntime::DiscoverChatSources(native_provider);
	if (!discovery.error.empty())
	{
		result.Fail(native_provider.title + " history discovery failed: " + discovery.error);
		return result;
	}

	NativeImportIndex import_index = LoadNativeImportIndex(app.data_root);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; import stopped to avoid restoring deleted chats.");
		return result;
	}

	for (const ProviderChatSource& source : discovery.sources)
	{
		std::vector<ChatSession> native_chats = LoadNativeSessionChats(source.chats_dir, native_provider);
		ApplyLocalOverrides(app, native_chats);
		std::optional<std::string> import_folder_id;

		for (ChatSession& native_chat : native_chats)
		{
			if (!NativeChatShouldBeImported(source.chats_dir, native_chat, target_id, delete_native_after_import))
			{
				continue;
			}

			++result.total_count;

			native_chat.workspace_directory = source.folder_directory;
			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, native_chat, target_id);
			if (!native_key)
			{
				continue;
			}

			AssignKnownWorkspaceFolderToNewImport(app, import_index, uam::paths::PathFromUtf8(source.folder_directory), native_chat);

			if (native_chat.folder_id.empty())
			{
				if (!import_folder_id)
				{
					import_folder_id = ResolvePersistedImportFolderIdForSource(app, source);
					if (import_folder_id->empty()) result.Fail("Imported history from " + source.folder_directory + ", but could not save its workspace folder.");
				}

				native_chat.folder_id = *import_folder_id;
				native_chat.workspace_directory = source.folder_directory;
			}

			if (SaveImportedNativeChat(app, import_index, native_chat, *native_key, source.chats_dir, delete_native_after_import))
			{
				++result.imported_count;
			}
			else
			{
				result.Fail("Failed to save imported " + native_provider.title + " chat " + native_chat.native_session_id + ".");
			}
		}
	}

	return result;
}

bool ChatHistorySyncService::StartAsyncNativeChatLoad(uam::AppState& app, const ProviderProfile& provider, const fs::path& chats_dir) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) || chats_dir.empty())
	{
		return false;
	}

	const fs::path chats_dir_snapshot = chats_dir;
	const ProviderProfile provider_snapshot = provider;
	const auto load_chats = [this, chats_dir_snapshot, provider_snapshot](std::stop_token stop_token) { return LoadNativeSessionChats(chats_dir_snapshot, provider_snapshot, stop_token); };
	const auto build_digest = [](const std::vector<ChatSession>&) { return std::string(); };
	return uam::platform::StartAsyncNativeChatLoadTask(app.native_chat_load_task, provider.id, chats_dir, load_chats, build_digest);
}

bool ChatHistorySyncService::TryConsumeAsyncNativeChatLoad(uam::AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const
{
	if (!PlatformServicesFactory::Instance().terminal_runtime.SupportsAsyncNativeGeminiHistoryRefresh())
	{
		return false;
	}

	if (!app.native_chat_load_task.running)
	{
		return false;
	}

	return uam::platform::TryConsumeAsyncNativeChatLoadTask(app.native_chat_load_task, chats_out, nullptr, error_out);
}

std::vector<std::string> ChatHistorySyncService::SessionIdsFromChats(const std::vector<ChatSession>& chats) const
{
	std::vector<std::string> ids;
	ids.reserve(chats.size());

	const NativeSessionLinkService native_session_links;
	for (const ChatSession& chat : chats)
	{
		const std::string session_id = native_session_links.RealNativeSessionId(chat);
		if (!session_id.empty())
		{
			ids.push_back(session_id);
		}
	}

	return ids;
}

std::optional<fs::path> ChatHistorySyncService::FindNativeSessionFilePath(const fs::path& chats_dir, const std::string& session_id) const
{
	const std::string target_session_id = uam::strings::Trim(session_id);
	if (!uam::chat_ids::IsSafeStorageChatId(target_session_id) || chats_dir.empty() || !uam::paths::IsDirectoryNoThrow(chats_dir))
	{
		return std::nullopt;
	}

	const fs::path canonical_name = chats_dir / (target_session_id + ".json");
	if (uam::paths::PathExistsNoThrow(canonical_name))
	{
		return canonical_name;
	}

	std::error_code error;
	for (fs::directory_iterator it(chats_dir, error), end; !error && it != end; it.increment(error))
	{
		const fs::directory_entry& item = *it;
		if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".json"))
		{
			continue;
		}

		const std::string text = uam::io::ReadTextFile(item.path());
		const auto json = ParseJson(text);
		if (!json || json->type != JsonValue::Type::Object)
		{
			continue;
		}

		if (JsonStringOrEmpty(json->Find("sessionId")) != target_session_id)
		{
			continue;
		}

		return item.path();
	}

	return std::nullopt;
}

bool ChatHistorySyncService::DeleteNativeSessionFileForChat(const uam::AppState& app, const ChatSession& chat, std::error_code* error_out) const
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::UsesNativeOverlayHistory(provider))
	{
		return false;
	}

	const std::string session_id = NativeSessionFileOperationId(chat);
	if (session_id.empty())
	{
		return false;
	}

	const fs::path chats_dir = ResolveNativeHistoryChatsDirForChat(app, chat);
	auto session_file = FindNativeSessionFilePath(chats_dir, session_id);

	if (!session_file)
	{
		session_file = FindNativeSessionFileAcrossDiscoveredSources(provider, session_id);
	}

	if (!session_file)
	{
		return false;
	}

	std::error_code error;
	const bool removed = RemoveNativeSessionFileIfPresent(session_file->parent_path(), session_id, &error);

	if (error_out != nullptr)
	{
		*error_out = error;
	}

	return removed && !error;
}

bool ChatHistorySyncService::DeleteNativeWorkspaceHistoryForFolder(const uam::AppState& app, const ChatFolder& folder, std::error_code* error_out) const
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}
	if (!uam::paths::IsControllerLocalWorkspace(folder))
	{
		return false;
	}

	const std::string folder_directory = uam::strings::Trim(folder.directory);

	if (folder_directory.empty())
	{
		return false;
	}

	const ProviderProfile& provider = DefaultNativeHistoryProvider(app);

	if (!ProviderRuntime::UsesNativeOverlayHistory(provider))
	{
		return false;
	}

	const fs::path workspace_root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder_directory);
	const auto tmp_dir = AppPaths::ResolveGeminiProjectTmpDir(workspace_root);

	if (!tmp_dir)
	{
		return false;
	}

	const fs::path project_root_file = *tmp_dir / ".project_root";
	const std::string recorded_project_root = uam::strings::Trim(uam::io::ReadTextFile(project_root_file));

	if (recorded_project_root.empty() || !FolderDirectoryMatches(workspace_root, uam::paths::PathFromUtf8(recorded_project_root)))
	{
		return false;
	}

	std::error_code error;
	uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(*tmp_dir, &error);

	if (error_out != nullptr)
	{
		*error_out = error;
	}

	return !error;
}

bool ChatHistorySyncService::PersistLocalDraftNativeSessionLink(const uam::AppState& app, ChatSession& local_chat, const std::string& native_session_id) const
{
	const NativeSessionLinkService native_session_links;
	const std::string session_id = uam::strings::Trim(native_session_id);

	if (session_id.empty() || native_session_links.IsLocalDraftChatId(session_id) || !native_session_links.IsLocalDraftChatId(local_chat.id))
	{
		return false;
	}

	if (native_session_links.RealNativeSessionId(local_chat) == session_id)
	{
		return true;
	}

	local_chat.native_session_id = session_id;
	return ChatRepository::SaveChat(app.data_root, local_chat);
}

bool ChatHistorySyncService::MoveChatToFolder(uam::AppState& app, ChatSession& chat, const std::string& new_folder_id) const
{
	const std::string target_folder_id = uam::strings::Trim(new_folder_id);
	if (target_folder_id.empty())
	{
		return false;
	}

	const ChatFolder* new_folder = ChatDomainService().FindFolderById(app, target_folder_id);
	if (new_folder == nullptr)
	{
		return false;
	}
	if (target_folder_id == chat.folder_id)
	{
		return true;
	}
	const std::string current_host = uam::strings::NonEmptyOrFallback(
	    uam::strings::Trim(chat.execution_host_id), "local");
	const std::string target_host = uam::strings::NonEmptyOrFallback(
	    uam::strings::Trim(new_folder->execution_host_id), "local");
	if (current_host != target_host)
	{
		app.status_line = "A chat cannot be moved to a workspace on another computer.";
		return false;
	}
	if (current_host != uam::execution_hosts::kLocalHostId)
	{
		app.status_line = "Moving remote chats between workspace directories is not supported yet.";
		return false;
	}

	const ChatSession original_chat = chat;
	const std::string old_workspace = chat.workspace_directory;
	const std::string old_folder_id = chat.folder_id;
	app.move_chat_original_folder_id = old_folder_id;
	app.move_chat_original_workspace = old_workspace;
	app.move_chat_target_folder_id = target_folder_id;
	app.move_chat_target_workspace = new_folder->directory;

	ChatSession moved_chat = chat;
	moved_chat.folder_id = target_folder_id;
	moved_chat.workspace_directory = new_folder->directory;
	moved_chat.updated_at = uam::time::TimestampNow();

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const std::string session_id = NativeSessionLinkService().RealNativeSessionId(chat);

	const fs::path normalized_old_workspace = NormalizeWorkspacePathForComparison(old_workspace);
	const fs::path normalized_new_workspace = NormalizeWorkspacePathForComparison(new_folder->directory);
	const bool workspaces_different = !session_id.empty() && !old_workspace.empty() && normalized_old_workspace != normalized_new_workspace;
	const bool reset_opencode_session = workspaces_different && uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kOpenCodeCli);
	std::optional<fs::path> old_chats_dir;

	if (workspaces_different)
	{
		old_chats_dir = ResolveNativeHistoryChatsDirForWorkspace(normalized_old_workspace);
		if (old_chats_dir)
		{
			ApplyNewerNativeMessagesBeforeWorkspaceMove(provider, original_chat, *old_chats_dir, session_id, moved_chat);
		}

		if (!ProviderRuntime::RebuildNativeSessionFile(provider, moved_chat, new_folder->directory))
		{
			app.move_chat_pending_id = chat.id;
			app.move_chat_show_missing_session_warning = true;
			return true;
		}
		if (reset_opencode_session)
		{
			// OpenCode ACP cannot safely load a session under a different cwd: replies
			// may be routed to the old workspace and hang. Preserve UAM history but
			// start a fresh provider-native session in the destination workspace.
			moved_chat.native_session_id.clear();
		}
	}

	chat = moved_chat;
	if (!ChatRepository::SaveChat(app.data_root, chat))
	{
		if (workspaces_different)
		{
			if (const auto target_chats_dir = ResolveNativeHistoryChatsDirForWorkspace(normalized_new_workspace))
			{
				RemoveNativeSessionFileIfPresent(*target_chats_dir, session_id);
			}
		}

		chat = original_chat;
		ClearMoveChatState(app);
		return false;
	}

	if (workspaces_different && old_chats_dir)
	{
		RemoveNativeSessionFileIfPresent(*old_chats_dir, session_id);
	}
	if (reset_opencode_session)
	{
		app.resolved_native_sessions_by_chat_id.erase(chat.id);
	}

	uam::StopAndEraseCliTerminalForChat(app, chat.id);

	ClearMoveChatState(app);
	return true;
}

std::string ChatHistorySyncService::ResolveResumeSessionIdForChat(const uam::AppState& app, const ChatSession& chat) const
{
	if (!ProviderResolutionService().ChatUsesNativeOverlayHistory(app, chat))
	{
		return "";
	}

	const fs::path chats_dir = ResolveNativeHistoryChatsDirForChat(app, chat);

	if (chats_dir.empty())
	{
		return "";
	}

	std::string candidate_id;
	const NativeSessionLinkService native_session_links;
	const auto resolved_session_it = app.resolved_native_sessions_by_chat_id.find(chat.id);
	const std::string resolved_session_id = resolved_session_it == app.resolved_native_sessions_by_chat_id.end() ? std::string{} : uam::strings::Trim(resolved_session_it->second);
	const std::string linked_session_id = native_session_links.RealNativeSessionId(chat);

	if (!resolved_session_id.empty())
	{
		candidate_id = resolved_session_id;
	}
	else if (!linked_session_id.empty())
	{
		candidate_id = linked_session_id;
	}
	else if (!chat.messages.empty() && !chat.id.empty() && !native_session_links.IsLocalDraftChatId(chat.id))
	{
		candidate_id = chat.id;
	}

	if (candidate_id.empty())
	{
		return "";
	}

	if (FindNativeSessionFilePath(chats_dir, candidate_id))
	{
		return candidate_id;
	}

	const fs::path current_workspace = uam::paths::ResolveControllerWorkspaceRootPath(app, chat);
	for (const fs::path& workspace_root : CollectWorkspaceRootsForNativeHistory(app))
	{
		if (workspace_root == current_workspace)
		{
			continue;
		}

		const auto other_chats_dir = ResolveNativeHistoryChatsDirForWorkspace(workspace_root);
		if (!other_chats_dir || other_chats_dir->empty())
		{
			continue;
		}

		const auto source_session_file = FindNativeSessionFilePath(*other_chats_dir, candidate_id);
		if (!source_session_file)
		{
			continue;
		}

		std::error_code error;
		if (!uam::paths::CreateDirectoriesNoThrow(chats_dir, &error))
		{
			continue;
		}

		const fs::path destination = chats_dir / (candidate_id + ".json");
		fs::copy_file(*source_session_file, destination, fs::copy_options::overwrite_existing, error);
		if (!error)
		{
			return candidate_id;
		}
	}

	return "";
}

void ChatHistorySyncService::ForgetResolvedNativeSessionForChat(uam::AppState& app, const std::string& chat_id) const
{
	app.resolved_native_sessions_by_chat_id.erase(uam::strings::Trim(chat_id));
}

void ChatHistorySyncService::RollbackOpenNativeSessionChatImport(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id, bool delete_storage) const
{
	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
	if (chat_index >= 0)
	{
		app.chats.erase(app.chats.begin() + chat_index);
	}

	if (delete_storage)
	{
		ChatRepository::DeleteChatStorageFiles(app.data_root, chat_id);
	}

	ForgetResolvedNativeSessionForChat(app, chat_id);
	ChatDomainService().SelectChatById(app, previous_selected_chat_id);
}

void ChatHistorySyncService::RestoreOpenNativeSessionResolvedMapping(uam::AppState& app,
                                                                     const std::string& chat_id,
                                                                     bool had_previous_resolved_native_session,
                                                                     const std::string& previous_resolved_native_session_id) const
{
	if (had_previous_resolved_native_session)
	{
		app.resolved_native_sessions_by_chat_id[chat_id] = previous_resolved_native_session_id;
	}
	else
	{
		ForgetResolvedNativeSessionForChat(app, chat_id);
	}
}

void ChatHistorySyncService::RestoreOpenNativeSessionChatMetadata(ChatSession& chat,
                                                                  const std::string& previous_provider_id,
                                                                  const std::string& previous_native_session_id,
                                                                  const std::string& previous_updated_at) const
{
	chat.provider_id = previous_provider_id;
	chat.native_session_id = previous_native_session_id;
	chat.updated_at = previous_updated_at;
}

ChatSession* ChatHistorySyncService::FindInMemoryNativeSessionChatForOpen(uam::AppState& app,
                                                                         const ChatSession& source_chat,
                                                                         const ProviderProfile& provider,
                                                                         const std::string& native_session_id,
                                                                         bool persist_resolved_mapping) const
{
	const std::string target_native_session_id = uam::strings::Trim(native_session_id);
	if (target_native_session_id.empty())
	{
		return nullptr;
	}

	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);
	const std::string source_workspace_directory = uam::strings::Trim(source_chat.workspace_directory);
	ChatSession* best_match = nullptr;
	int best_priority = 4;

	for (ChatSession& chat : app.chats)
	{
		const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
		if (!chat_provider_id.empty() && chat_provider_id != source_provider_id)
		{
			continue;
		}

		if (!source_workspace_directory.empty() && !Utf8WorkspaceDirectoriesMatch(chat.workspace_directory, source_workspace_directory))
		{
			continue;
		}

		const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
		const std::string resolved_native_session_id = resolved == app.resolved_native_sessions_by_chat_id.end() ? std::string{} : uam::strings::Trim(resolved->second);
		const std::string raw_native_session_id = uam::strings::Trim(chat.native_session_id);
		int priority = -1;
		if (uam::strings::Trim(chat.id) == target_native_session_id)
		{
			priority = 0;
		}
		else if (!raw_native_session_id.empty() && raw_native_session_id == target_native_session_id)
		{
			priority = 1;
		}
		else if (!resolved_native_session_id.empty() && resolved_native_session_id == target_native_session_id)
		{
			priority = 2;
		}

		if (priority >= 0 && (priority < best_priority || (priority == best_priority && best_match != nullptr && IsLaterOpenCodeNativeSessionMatch(chat, *best_match))))
		{
			best_match = &chat;
			best_priority = priority;
			if (best_priority == 0)
			{
				break;
			}
		}
	}

	if (best_match != nullptr)
	{
		if (persist_resolved_mapping && best_match->provider_id.empty())
		{
			best_match->provider_id = source_provider_id;
		}

		if (persist_resolved_mapping)
		{
			app.resolved_native_sessions_by_chat_id[best_match->id] = target_native_session_id;
		}
	}

	return best_match;
}

ChatSession* ChatHistorySyncService::FindOrImportNativeSessionChatForOpen(uam::AppState& app,
                                                                         const ChatSession& source_chat_reference,
                                                                         const ProviderProfile& provider,
                                                                         const std::string& native_session_id,
                                                                         bool persist_provider_normalization) const
{
	const std::string target_native_session_id = uam::strings::Trim(native_session_id);
	if (target_native_session_id.empty())
	{
		return nullptr;
	}

	const ChatSession source_chat = source_chat_reference;
	const std::string source_workspace_directory = uam::strings::Trim(source_chat.workspace_directory);
	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);
	auto existing_matches_source_workspace = [&source_workspace_directory](const ChatSession& chat) {
		if (source_workspace_directory.empty())
		{
			return true;
		}

		return Utf8WorkspaceDirectoriesMatch(chat.workspace_directory, source_workspace_directory);
	};

	std::vector<ChatSession> candidate_chats;
	if (ProviderRuntime::UsesNativeOverlayHistory(provider))
	{
		const fs::path chats_dir = ResolveNativeHistoryChatsDirForChat(app, source_chat);
		if (chats_dir.empty())
		{
			return nullptr;
		}

		candidate_chats = LoadNativeSessionChats(chats_dir, provider);
	}
	else if (ProviderRuntime::UsesLocalHistory(provider))
	{
		candidate_chats = ChatRepository::LoadLocalChats(app.data_root);
	}
	else
	{
		return nullptr;
	}

	if (!source_workspace_directory.empty())
	{
		std::vector<ChatSession> workspace_filtered;
		workspace_filtered.reserve(candidate_chats.size());
		for (const ChatSession& chat : candidate_chats)
		{
			if (Utf8WorkspaceDirectoriesMatch(chat.workspace_directory, source_workspace_directory))
			{
				workspace_filtered.push_back(chat);
			}
		}
		candidate_chats = std::move(workspace_filtered);
	}

	const std::vector<ChatSession> original_chats = app.chats;
	const std::unordered_map<std::string, std::string> original_resolved_native_sessions_by_chat_id = app.resolved_native_sessions_by_chat_id;
	ApplyLocalOverrides(app, candidate_chats, false);
	app.chats = original_chats;
	app.resolved_native_sessions_by_chat_id = original_resolved_native_sessions_by_chat_id;

	const auto matched = std::ranges::find_if(candidate_chats, [&](const ChatSession& chat) {
		if (uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id) != source_provider_id && !uam::strings::IsBlank(chat.provider_id))
		{
			return false;
		}

		const std::string session_id = uam::strings::Trim(uam::strings::NonEmptyOrFallback(chat.native_session_id, chat.id));
		return session_id == target_native_session_id || uam::strings::Trim(chat.id) == target_native_session_id;
	});

	auto find_loaded_raw_match = [&]() -> ChatSession*
	{
		ChatSession* best_match = nullptr;
		int best_priority = 2;

		for (ChatSession& chat : app.chats)
		{
			const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
			if (!chat_provider_id.empty() && chat_provider_id != source_provider_id)
			{
				continue;
			}

			if (!source_workspace_directory.empty() && !Utf8WorkspaceDirectoriesMatch(chat.workspace_directory, source_workspace_directory))
			{
				continue;
			}

			int priority = -1;
			if (uam::strings::Trim(chat.id) == target_native_session_id)
			{
				priority = 0;
			}
			else if (uam::strings::Trim(chat.native_session_id) == target_native_session_id)
			{
				priority = 1;
			}

			if (priority >= 0 && (priority < best_priority || (priority == best_priority && (best_match == nullptr || IsLaterOpenCodeNativeSessionMatch(chat, *best_match)))))
			{
				best_match = &chat;
				best_priority = priority;
				if (best_priority == 0)
				{
					break;
				}
			}
		}

		return best_match;
	};

	if (matched != candidate_chats.end())
	{
		ChatSession imported_chat = *matched;
		if (!uam::strings::IsBlank(imported_chat.provider_id) && uam::provider_ids::NormalizeCliProviderAliasOrSelf(imported_chat.provider_id) != source_provider_id)
		{
			return nullptr;
		}

			if (ChatSession* loaded_raw_match = find_loaded_raw_match(); loaded_raw_match != nullptr)
			{
				if (persist_provider_normalization && loaded_raw_match->provider_id.empty())
				{
					loaded_raw_match->provider_id = source_provider_id;
				}
				loaded_raw_match->native_session_id = target_native_session_id;
				loaded_raw_match->messages = imported_chat.messages;
				loaded_raw_match->messages_loaded = imported_chat.messages_loaded;
				loaded_raw_match->updated_at = imported_chat.updated_at;
				app.resolved_native_sessions_by_chat_id[loaded_raw_match->id] = target_native_session_id;
				return loaded_raw_match;
			}

			if (ChatSession* resolved_only_match = FindInMemoryNativeSessionChatForOpen(app, source_chat, provider, target_native_session_id, persist_provider_normalization); resolved_only_match != nullptr &&
			    existing_matches_source_workspace(*resolved_only_match) &&
			    (resolved_only_match->provider_id.empty() || uam::provider_ids::NormalizeCliProviderAliasOrSelf(resolved_only_match->provider_id) == source_provider_id) &&
			    (uam::strings::Trim(resolved_only_match->native_session_id).empty() || uam::strings::Trim(resolved_only_match->native_session_id) == target_native_session_id))
			{
				if (persist_provider_normalization && resolved_only_match->provider_id.empty())
				{
					resolved_only_match->provider_id = source_provider_id;
				}
				resolved_only_match->native_session_id = target_native_session_id;
				resolved_only_match->messages = imported_chat.messages;
				resolved_only_match->messages_loaded = imported_chat.messages_loaded;
				resolved_only_match->updated_at = imported_chat.updated_at;
				app.resolved_native_sessions_by_chat_id[resolved_only_match->id] = target_native_session_id;
				return resolved_only_match;
			}

			if (ChatSession* existing = FindInMemoryNativeSessionChatForOpen(app, source_chat, provider, target_native_session_id, persist_provider_normalization); existing != nullptr &&
			    existing_matches_source_workspace(*existing) &&
			    (existing->provider_id.empty() || uam::provider_ids::NormalizeCliProviderAliasOrSelf(existing->provider_id) == source_provider_id) &&
			    uam::strings::Trim(existing->id) == uam::strings::Trim(matched->id))
			{
				if (persist_provider_normalization && existing->provider_id.empty())
				{
					existing->provider_id = source_provider_id;
				}
				existing->native_session_id = target_native_session_id;
				existing->messages = imported_chat.messages;
				existing->messages_loaded = imported_chat.messages_loaded;
				existing->updated_at = imported_chat.updated_at;
				app.resolved_native_sessions_by_chat_id[existing->id] = target_native_session_id;
				return existing;
			}

		std::unordered_set<std::string> existing_ids;
		existing_ids.reserve(app.chats.size());
		for (const ChatSession& chat : app.chats)
		{
			existing_ids.insert(chat.id);
		}

		if (imported_chat.id.empty() || existing_ids.contains(imported_chat.id))
		{
			imported_chat.id = MakeCollisionSafeImportedChatId(imported_chat, existing_ids);
		}

		if (persist_provider_normalization && imported_chat.provider_id.empty())
		{
			imported_chat.provider_id = provider.id;
		}
		if (imported_chat.folder_id.empty())
		{
			imported_chat.folder_id = source_chat.folder_id;
		}
		if (imported_chat.workspace_directory.empty())
		{
			imported_chat.workspace_directory = source_chat.workspace_directory;
		}
		if (imported_chat.title.empty())
		{
			imported_chat.title = target_native_session_id;
		}
		imported_chat.last_opened_at = uam::time::TimestampNow();

		app.chats.push_back(std::move(imported_chat));
		app.resolved_native_sessions_by_chat_id[app.chats.back().id] = target_native_session_id;
		ChatBranching::Normalize(app.chats);
		return &app.chats.back();
	}
	else
	{
		if (ChatSession* existing = FindInMemoryNativeSessionChatForOpen(app, source_chat, provider, target_native_session_id, persist_provider_normalization); existing != nullptr &&
		    existing_matches_source_workspace(*existing) &&
		    (existing->provider_id.empty() || uam::provider_ids::NormalizeCliProviderAliasOrSelf(existing->provider_id) == source_provider_id))
		{
			if (persist_provider_normalization && existing->provider_id.empty())
			{
				existing->provider_id = source_provider_id;
			}
			existing->native_session_id = target_native_session_id;
			existing->updated_at = uam::time::TimestampNow();
			app.resolved_native_sessions_by_chat_id[existing->id] = target_native_session_id;
			return existing;
		}

		return nullptr;
	}
}

void ChatHistorySyncService::ApplyLocalOverrides(uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links) const
{
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	native_chats = ChatDomainService().DeduplicateChatsById(std::move(native_chats));
	std::vector<ChatSession> local_chats = ChatRepository::LoadLocalChatSummaries(app.data_root);

	for (ChatSession& local_chat : local_chats)
	{
		if (!LocalDraftCanInferNativeSessionLink(app, local_chat))
		{
			continue;
		}

		ChatRepository::HydrateChatMessages(app.data_root, local_chat);
		const auto inferred_session_id = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(local_chat, native_chats);

		if (inferred_session_id)
		{
			if (persist_local_draft_links && PersistLocalDraftNativeSessionLink(app, local_chat, *inferred_session_id))
			{
				local_chat.native_session_id = *inferred_session_id;
			}
		}
	}

	local_chats = ChatDomainService().DeduplicateChatsById(std::move(local_chats));
	const LocalChatOverlayIndex local_index = BuildLocalChatOverlayIndex(local_chats);
	std::unordered_set<std::string> native_ids;

	for (ChatSession& native_chat : native_chats)
	{
		native_ids.insert(native_chat.id);
		ChatSession* local_match = FindLocalOverlayMatch(local_index, native_chat);
		if (local_match == nullptr)
		{
			continue;
		}

		ChatSession& local_chat = *local_match;
		OverlayLocalChatState(local_chat, native_chat);

		bool should_copy_local_messages = LocalMessagesShouldOverrideNative(local_chat, native_chat);
		const bool matching_loaded_transcript = native_chat.messages_loaded &&
		    EffectiveMessageCount(local_chat) > 0 &&
		    EffectiveMessageCount(local_chat) == EffectiveMessageCount(native_chat);
		if ((should_copy_local_messages || matching_loaded_transcript) && !local_chat.messages_loaded)
		{
			ChatRepository::HydrateChatMessages(app.data_root, local_chat);
			should_copy_local_messages = LocalMessagesShouldOverrideNative(local_chat, native_chat);
		}
		const bool local_update_is_newer = !local_chat.updated_at.empty() && (native_chat.updated_at.empty() || local_chat.updated_at > native_chat.updated_at);

		if (should_copy_local_messages)
		{
			native_chat.messages = local_chat.messages;

			if (!local_chat.updated_at.empty())
			{
				native_chat.updated_at = local_chat.updated_at;
			}

			if (native_chat.created_at.empty() && !local_chat.created_at.empty())
			{
				native_chat.created_at = local_chat.created_at;
			}
		}
		else if (local_chat.messages_loaded && native_chat.messages_loaded &&
		         MessagesEquivalent(local_chat.messages, native_chat.messages))
		{
			for (std::size_t i = 0; i < local_chat.messages.size(); ++i)
			{
				native_chat.messages[i].markdown_store_files = local_chat.messages[i].markdown_store_files;
				native_chat.messages[i].markdown_store_prompt_blocks = local_chat.messages[i].markdown_store_prompt_blocks;
				native_chat.messages[i].attachments = local_chat.messages[i].attachments;
			}
			if (local_update_is_newer)
			{
				native_chat.updated_at = local_chat.updated_at;
			}
		}
	}

	std::vector<ChatSession> merged_chats = native_chats;

	for (const ChatSession& local_chat : local_chats)
	{
		if (native_ids.contains(local_chat.id))
		{
			continue;
		}

		if (NativeSessionLinkService().HasRealNativeSessionId(local_chat))
		{
			continue;
		}

		if (LocalChatIsRepresentedByNativeOverlay(app, local_chat))
		{
			continue;
		}

		if (!LocalChatHasActiveWork(app, local_chat, selected_chat_id))
		{
			continue;
		}

		merged_chats.push_back(local_chat);
	}

	ReplaceAppChatsWithNormalized(app, std::move(merged_chats));
}

void ChatHistorySyncService::RefreshNativeSessionDirectory(uam::AppState& app) const
{
	const ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected != nullptr)
	{
		const auto selected_chats_dir = ResolveNativeHistoryChatsDirForWorkspace(
		    uam::paths::ResolveControllerWorkspaceRootPath(app, *selected));

		if (selected_chats_dir)
		{
			app.native_history_chats_dir = *selected_chats_dir;
			return;
		}
	}

	std::optional<fs::path> fallback_chats_dir;
	if (const std::optional<fs::path> current_directory = uam::paths::CurrentPathNoThrow())
	{
		fallback_chats_dir = ResolveNativeHistoryChatsDirForWorkspace(*current_directory);
	}

	if (fallback_chats_dir)
	{
		app.native_history_chats_dir = *fallback_chats_dir;
	}
	else
	{
		app.native_history_chats_dir.clear();
	}
}

bool ChatHistorySyncService::ExportChatToNative(const uam::AppState& app, const ChatSession& chat) const
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);

	if (!ProviderRuntime::SupportsGeminiJsonHistory(provider))
	{
		return false;
	}

	const fs::path chats_dir = ResolveNativeHistoryChatsDirForChat(app, chat);
	if (chats_dir.empty())
	{
		return false;
	}

	const std::string linked_session_id = NativeSessionLinkService().RealNativeSessionId(chat);
	const std::string session_id = uam::strings::NonEmptyOrFallback(linked_session_id, chat.id);
	if (!uam::chat_ids::IsSafeStorageChatId(session_id))
	{
		return false;
	}
	const fs::path destination_file = chats_dir / (session_id + ".json");

	return GeminiJsonHistoryStore::SaveFile(destination_file, chat);
#else
	(void)app;
	(void)chat;
	return false;
#endif
}

bool ChatHistorySyncService::TruncateNativeSessionFromDisplayedMessage(const uam::AppState& app, const ChatSession& chat, int displayed_message_index, std::string* error_out) const
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	const auto fail = [error_out](const std::string& message)
	{
		if (error_out != nullptr)
		{
			*error_out = message;
		}
		return false;
	};

	const std::string session_id = NativeSessionLinkService().RealNativeSessionId(chat);
	if (session_id.empty())
	{
		return fail("Chat is not linked to a native runtime session.");
	}

	const fs::path chats_dir = ResolveNativeHistoryChatsDirForChat(app, chat);
	const auto session_file = FindNativeSessionFilePath(chats_dir, session_id);

	if (!session_file)
	{
		return fail("Native runtime session file not found.");
	}

	const std::string file_text = uam::io::ReadTextFile(*session_file);
	const std::optional<JsonValue> parsed_root = ParseJson(file_text);

	if (!parsed_root)
	{
		return fail("Failed to parse native runtime session file.");
	}

	JsonValue root = *parsed_root;

	JsonValue* contents = root.Find("contents");
	if (contents == nullptr || contents->type != JsonValue::Type::Array)
	{
		return fail("Native runtime session does not contain a contents array.");
	}

	const int keep_messages = std::max(0, displayed_message_index + 1);
	int visible_messages = 0;
	std::size_t truncate_index = contents->array_value.size();

	for (std::size_t index = 0; index < contents->array_value.size(); ++index)
	{
		JsonValue& item = contents->array_value[index];
		JsonValue* role = item.Find("role");

		if (role == nullptr || role->type != JsonValue::Type::String)
		{
			continue;
		}

		if (visible_messages >= keep_messages)
		{
			truncate_index = index;
			break;
		}

		++visible_messages;
	}

	contents->array_value.erase(contents->array_value.begin() + static_cast<std::ptrdiff_t>(truncate_index), contents->array_value.end());

	if (!uam::io::WriteTextFile(*session_file, SerializeJson(root)))
	{
		return fail("Failed to write updated native runtime session.");
	}

	return true;
}
