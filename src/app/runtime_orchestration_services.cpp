#include "common/chat/managed_parent_input_history.h"
#include "runtime_orchestration_services.h"
#include "runtime_orchestration_internal.h"
#include "common/provider/native_agent_history.h"
#include "common/provider/opencode_v2_session.h"

#include "app/chat_domain_service.h"
#include "app/managed_subagent_control_service.h"
#include "app/native_session_link_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_profile_migration_service.h"
#include "app/provider_resolution_service.h"
#include "app/provider_worker_command.h"

#include "common/chat/chat_branching.h"
#include "common/chat/conversation_handoff.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_folder_store.h"
#include "common/chat/native_chat_identity.h"
#include "common/chat/chat_repository.h"
#include "common/chat/chat_message_page_index.h"
#include "common/utils/hash_utils.h"
#include "common/chat/native_media_chat_save.h"
#include "common/chat/message_attachment_sources.h"
#include "common/config/execution_host_config.h"
#include "common/constants/app_constants.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/codex/cli/codex_tool_item.h"
#include "common/provider/claude/cli/claude_tool_projection.h"
#include "common/provider/native_provider_agent_history.h"
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
#include "common/provider/antigravity/cli/antigravity_native_history.h"
#endif
#include "common/provider/copilot_native_history.h"
#include "common/provider/copilot/cli/copilot_session_paths.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/gemini_history_journal.h"
#endif
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal_polling.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "common/utils/history_timestamp_order.h"
#include "core/chat_import_utils.h"
#include "core/native_import_content.h"
#include "remote/runner_client.h"
#include "remote/native_history_snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <array>
#include <sqlite3.h>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <sstream>
#include <thread>
#include <tuple>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace uam
{
	struct TerminalNativeMediaWork
	{
		enum class Phase { Waiting, Preparing, Prepared, Finishing, Finished };
		std::atomic<Phase> phase{Phase::Waiting};
		std::atomic<bool> revoked{false};
		std::stop_source cancellation;
		std::filesystem::path root;
		std::filesystem::path chats_dir;
		ProviderProfile profile;
		ChatSession expected;
		std::string metadata;
		std::string protected_metadata;
		std::string conversation_updated_at;
		std::string summary_revision;
		unsigned metadata_rebases_remaining = 3;
		std::string revision;
		std::string native_key;
		std::string digest;
		std::string selected_before;
		bool preserve_selection = true;
		bool armed = false;
		std::string message_digest;
		bool final = false;
		bool wait_for_stop = false;
		std::shared_ptr<CliTerminalHistoryGeneration> producer;
		std::uint64_t generation = 0;
		std::vector<ChatSession> inputs;
		ChatSession projection;
		ChatSession retired_live;
		std::unique_ptr<PreparedNativeMediaChatSave> prepared;
		std::string error;
		bool acknowledged = false;
		bool no_importable_content = false;
		bool published = false;
		~TerminalNativeMediaWork() noexcept
		{
			// This value is always held by the existing bounded worker-retirement
			// owner, including failed admission/publication and app destruction.
			if (prepared && prepared->PrimaryAcknowledged() && !acknowledged)
			{
				try { std::string ignored; (void)prepared->AcknowledgeOriginals(ignored); }
				catch (...) {}
			}
		}
	};

	struct TerminalNativeMediaCoordinator
	{
		std::vector<std::shared_ptr<TerminalNativeMediaWork>> jobs;
		std::uint64_t next_generation = 0;
		bool closing = false;
	};
}


namespace
{
	namespace chat_identity = uam::chat_identity;

	constexpr const char* kDefaultNativeHistoryProviderId = provider_build_config::DefaultNativeHistoryProviderId();
	constexpr const char* kMemoryWorkerPromptPrefix = "You are a non-interactive memory extraction function.";
	constexpr const char* kNativeImportTombstonesFile = "native-import-tombstones.json";
	constexpr std::size_t kRemoteCodexMaxSessions = 200;
	constexpr std::size_t kRemoteCodexMaxMessages = 10000;

	enum class CodexRpcReadResult
	{
		Waiting,
		Success,
		Failure,
	};

	CodexRpcReadResult TakeCodexRpcResponse(std::string& buffered, int expected_id,
	                                      nlohmann::json& result, std::string& error)
	{
		for (;;)
		{
			const std::size_t newline = buffered.find('\n');
			if (newline == std::string::npos) return CodexRpcReadResult::Waiting;
			std::string line = buffered.substr(0, newline);
			buffered.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (uam::strings::IsBlank(line)) continue;
			const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
			if (!message.is_object())
			{
				error = "Codex app-server returned malformed JSON-RPC output.";
				return CodexRpcReadResult::Failure;
			}
			if (!message.contains("id") || !message["id"].is_number_integer() ||
			    message["id"].get<int>() != expected_id)
				continue;
			if (message.contains("error"))
			{
				const nlohmann::json& rpc_error = message["error"];
				error = rpc_error.is_object()
				    ? rpc_error.value("message", "Codex app-server rejected the history request.")
				    : "Codex app-server rejected the history request.";
				return CodexRpcReadResult::Failure;
			}
			if (!message.contains("result"))
			{
				error = "Codex app-server omitted the history result.";
				return CodexRpcReadResult::Failure;
			}
			result = message["result"];
			return CodexRpcReadResult::Success;
		}
	}

	bool PollCodexRpcResponse(uam::remote::RunnerClient& client,
	                          const std::string& process_id, int expected_id,
	                          std::string& buffered, std::size_t& captured_bytes,
	                          nlohmann::json& result, std::string& error,
	                          const std::chrono::steady_clock::time_point deadline,
	                          std::stop_token stop_token)
	{
		const auto interrupt = [stop_token, deadline]
		{
			return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline;
		};
		while (!interrupt())
		{
			if (const CodexRpcReadResult ready =
			        TakeCodexRpcResponse(buffered, expected_id, result, error);
			    ready != CodexRpcReadResult::Waiting)
				return ready == CodexRpcReadResult::Success;
			uam::remote::ProcessPollResult polled;
			if (!client.PollProcess(process_id, polled, &error, interrupt))
			{
				if (interrupt()) break;
				return false;
			}
			captured_bytes += polled.standard_output.size() + polled.standard_error.size();
			if (captured_bytes > uam::platform::kCapturedCommandMaxOutputBytes)
			{
				error = "Codex remote history exceeds UAM's 4 MiB safety limit.";
				return false;
			}
			buffered += polled.standard_output;
			if (!client.AcknowledgeProcessOutput(process_id, polled, &error)) return false;
			if (const CodexRpcReadResult ready =
			        TakeCodexRpcResponse(buffered, expected_id, result, error);
			    ready != CodexRpcReadResult::Waiting)
				return ready == CodexRpcReadResult::Success;
			if (!polled.running)
			{
				error = "Codex app-server closed before returning remote history";
				if (!uam::strings::IsBlank(polled.standard_error))
					error += ": " + uam::strings::Trim(polled.standard_error);
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		error = stop_token.stop_requested() ? "Codex remote history loading was canceled."
		                                    : "Codex remote history request timed out.";
		return false;
	}

	bool RunRemoteCodexRpc(uam::remote::RunnerClient& client,
	                       const std::filesystem::path& working_directory,
	                       std::string_view method, const nlohmann::json& params,
	                       nlohmann::json& result, std::string& error,
	                       std::stop_token stop_token = {})
	{
		if (stop_token.stop_requested())
		{
			error = "Codex remote history loading was canceled.";
			return false;
		}
		std::string process_id = "codex-history-" +
		    PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (process_id == "codex-history-")
			process_id += uam::time::SteadyEpochNanosecondsTokenNow();
		if (!client.StartProcess(process_id, working_directory,
		                         {"codex", "app-server", "--stdio"}, {}, &error))
			return false;
		const auto cleanup = [&]
		{
			(void)client.StopProcess(process_id);
			(void)client.RemoveProcess(process_id);
		};
		std::string buffered;
		std::size_t captured_bytes = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		const nlohmann::json initialize{
		    {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		    {"params", {{"clientInfo", {{"name", "uam-history"},
		                                   {"version", std::string(uam::constants::kAppVersion).substr(1)}}},
		                {"capabilities", {{"experimentalApi", true}}}}}};
		if (!client.WriteProcess(process_id, initialize.dump() + "\n", &error) ||
		    !PollCodexRpcResponse(client, process_id, 1, buffered, captured_bytes,
		                          result, error, deadline, stop_token))
		{
			cleanup();
			return false;
		}
		const nlohmann::json initialized{
		    {"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nullptr}};
		const nlohmann::json request{
		    {"jsonrpc", "2.0"}, {"id", 2}, {"method", method}, {"params", params}};
		if (!client.WriteProcess(process_id, initialized.dump() + "\n" + request.dump() + "\n",
		                         &error) ||
		    !PollCodexRpcResponse(client, process_id, 2, buffered, captured_bytes,
		                          result, error, deadline, stop_token))
		{
			cleanup();
			return false;
		}
		cleanup();
		return true;
	}

	std::string IsoTimestampFromEpochMilliseconds(std::int64_t milliseconds)
	{
		if (milliseconds <= 0) return uam::time::IsoUtcTimestampNow();
		const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
		return uam::time::detail::FormatTimestamp(
		    uam::time::detail::UtcTimeSnapshot(seconds), uam::time::kIsoUtcTimestampFormat);
	}

	std::string IsoTimestampFromEpochSeconds(std::int64_t seconds)
	{
		if (seconds <= 0) return uam::time::IsoUtcTimestampNow();
		return uam::time::detail::FormatTimestamp(
		    uam::time::detail::UtcTimeSnapshot(static_cast<std::time_t>(seconds)),
		    uam::time::kIsoUtcTimestampFormat);
	}

	std::int64_t JsonIntegerOrZero(const nlohmann::json& value)
	{
		if (!value.is_number_integer()) return 0;
		try
		{
			return value.get<std::int64_t>();
		}
		catch (...)
		{
			return 0;
		}
	}

	int BoundedMessageTokenCount(const nlohmann::json& value)
	{
		const std::int64_t count = JsonIntegerOrZero(value);
		return static_cast<int>(std::clamp<std::int64_t>(
		    count, 0, std::numeric_limits<int>::max()));
	}

	void AppendTranscriptText(std::string& target, std::string_view text)
	{
		if (text.empty()) return;
		if (!target.empty() && target.back() != '\n') target.push_back('\n');
		target.append(text);
	}

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

	/// <summary>Compares remote import ownership without changing persisted native identities or exclusion keys.</summary>
	std::string RemoteImportOwnershipKey(const uam::AppState& app, const ChatSession& chat, bool workspace_only = false)
	{
		const std::string host_id = uam::strings::Trim(chat.execution_host_id);
		if (host_id.empty() || host_id == uam::execution_hosts::kLocalHostId) return {};
		const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, host_id);
		if (host == nullptr || host->transport != "ssh") return {};
		const std::string native_id = workspace_only ? std::string{} : uam::strings::Trim(chat.native_session_id);
		if (!workspace_only && native_id.empty()) return {};
		const std::string_view directory = uam::strings::IsBlank(chat.workspace_worktree_directory)
		    ? std::string_view(chat.workspace_directory) : std::string_view(chat.workspace_worktree_directory);
		const std::string platform = uam::strings::ToLowerAscii(host->platform);
		if (!uam::execution_hosts::IsAbsoluteRemotePath(platform, directory)) return {};
		const bool windows = platform == "windows";
		std::string path = uam::execution_hosts::NormalizeRemotePath(platform, directory);
		if (!windows && uam::strings::TrimAsciiView(directory) == "//") path = "//";
		std::size_t root_length = 0;
		while (root_length < path.size() && path[root_length] == '/') ++root_length;
		if (windows && root_length == 0 && path.size() >= 3 && path[1] == ':' && path[2] == '/') root_length = 3;
		std::string normalized = !windows && root_length > 2 ? "/" : path.substr(0, root_length);
		for (std::size_t position = root_length; position < path.size();)
		{
			const std::size_t separator = path.find('/', position);
			const std::size_t end = separator == std::string::npos ? path.size() : separator;
			const std::string_view component(path.data() + position, end - position);
			// '..' can cross a remote symlink, so only remove '.' and redundant separators.
			if (!component.empty() && component != ".")
			{
				if (!normalized.empty() && normalized.back() != '/') normalized.push_back('/');
				normalized.append(component);
			}
			if (separator == std::string::npos) break;
			position = separator + 1;
		}
		return nlohmann::json::array({uam::provider_ids::CanonicalCliProviderLookupId(chat.provider_id), host_id, normalized, native_id}).dump();
	}

	struct NativeImportIndex
	{
		std::unordered_set<std::string> existing_ids;
		std::unordered_set<std::string> owned_native_keys;
		std::unordered_set<std::string> pending_native_workspace_keys;
		std::unordered_set<std::string> remote_owned_native_keys;
		std::unordered_set<std::string> remote_pending_workspace_keys;
		std::unordered_map<std::string, std::string> existing_id_by_native_key;
		std::unordered_map<std::string, ChatSession> existing_summary_by_native_key;
		bool tombstones_available = false;
		std::unordered_set<std::string> tombstoned_native_keys;
	};

	bool MessagesEquivalent(const std::vector<Message>& lhs, const std::vector<Message>& rhs);
	void OverlayLocalChatState(const ChatSession& local, ChatSession& native);

	NativeImportIndex LoadNativeImportIndex(const uam::AppState& app, const std::vector<ChatSession>* sidebar = nullptr, bool retain_summaries = false)
	{
		const fs::path& data_root = app.data_root;
		const std::vector<ChatSession> loaded = sidebar == nullptr ? ChatRepository::LoadLocalChatSummaries(data_root) : std::vector<ChatSession>{};
		const std::vector<ChatSession>& local_chats = sidebar != nullptr ? *sidebar : loaded;
		NativeImportIndex import_index;
		NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
		import_index.tombstones_available = tombstones.available;
		import_index.tombstoned_native_keys = std::move(tombstones.keys);
		import_index.existing_ids.reserve(local_chats.size());
		import_index.existing_id_by_native_key.reserve(local_chats.size());
		import_index.existing_summary_by_native_key.reserve(local_chats.size());

		std::unordered_map<std::string, const ChatSession*> selected_chats;
		selected_chats.reserve(local_chats.size());
		for (const ChatSession& chat : local_chats)
		{
			import_index.existing_ids.insert(chat.id);
			if (!uam::strings::IsBlank(chat.native_session_id))
			{

				const std::string native_key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
				const std::string remote_key = RemoteImportOwnershipKey(app, chat);
				if (!remote_key.empty()) import_index.remote_owned_native_keys.insert(remote_key);
				if (uam::strings::StartsWith(chat.id, "chat-")) import_index.owned_native_keys.insert(native_key);
				const auto existing = selected_chats.find(native_key);
				if (existing == selected_chats.end() ||
				    ChatDomainService().ShouldReplaceChatForDuplicateId(chat, *existing->second))
				{
					import_index.existing_id_by_native_key[native_key] = chat.id;
					selected_chats[native_key] = &chat;
					if (sidebar != nullptr && !retain_summaries) import_index.existing_summary_by_native_key[native_key].id = chat.id;
					else import_index.existing_summary_by_native_key[native_key] = chat;
				}

			}
		}

		for (const ChatSession& chat : app.excluded_provider_chats)
		{
			import_index.existing_ids.insert(chat.id);
			if (uam::strings::IsBlank(chat.native_session_id)) continue;
			import_index.owned_native_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
			const std::string remote_key = RemoteImportOwnershipKey(app, chat);
			if (!remote_key.empty()) import_index.remote_owned_native_keys.insert(remote_key);
		}
		if (app.hidden_agent_chat_ownership.data_root == data_root)
		{
			for (const ChatSession& chat : app.hidden_agent_chat_ownership.chats)
			{
				import_index.existing_ids.insert(chat.id);
				if (uam::strings::IsBlank(chat.native_session_id)) continue;
				import_index.owned_native_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
				const std::string remote_key = RemoteImportOwnershipKey(app, chat);
				if (!remote_key.empty()) import_index.remote_owned_native_keys.insert(remote_key);
			}
		}

		// Session attachment can precede the deferred chat save. Protect live
		// ownership as well as persisted links before any background import.
		for (const ChatSession& chat : app.chats)
		{
			import_index.existing_ids.insert(chat.id);
			if (!uam::strings::StartsWith(chat.id, "chat-")) continue;
			ChatSession linked;
			linked.id = chat.id;
			linked.provider_id = chat.provider_id;
			linked.native_session_id = chat.native_session_id;
			linked.execution_host_id = chat.execution_host_id;
			linked.workspace_directory = chat.workspace_directory;
			linked.workspace_worktree_directory = chat.workspace_worktree_directory;
			const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
			if (resolved != app.resolved_native_sessions_by_chat_id.end()) linked.native_session_id = resolved->second;
			if (NativeSessionLinkService().HasRealNativeSessionId(linked))
			{
				import_index.owned_native_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(linked));
				const std::string remote_key = RemoteImportOwnershipKey(app, linked);
				if (!remote_key.empty()) import_index.remote_owned_native_keys.insert(remote_key);
			}
			else if (uam::ChatHasActiveCliTerminal(app, chat.id) ||
			    std::ranges::any_of(app.acp_sessions, [&chat](const std::unique_ptr<uam::AcpSessionState>& session)
			    { return session != nullptr && session->chat_id == chat.id && session->running; }))
			{
				// A provider can write its rollout before returning its session ID.
				// Defer imports in this workspace until the live attachment settles.
				linked.native_session_id.clear();
				import_index.pending_native_workspace_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(linked));
				const std::string remote_key = RemoteImportOwnershipKey(app, linked, true);
				if (!remote_key.empty()) import_index.remote_pending_workspace_keys.insert(remote_key);
			}
		}
		return import_index;
	}

	bool StartupCandidateIsOwned(const uam::AppState& app, const NativeImportIndex& index, const ChatSession& chat)
	{
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
		if (index.existing_id_by_native_key.contains(key) || index.owned_native_keys.contains(key)) return true;
		if (index.remote_owned_native_keys.contains(RemoteImportOwnershipKey(app, chat)) ||
		    index.remote_pending_workspace_keys.contains(RemoteImportOwnershipKey(app, chat, true))) return true;
		ChatSession workspace;
		workspace.provider_id = chat.provider_id;
		workspace.execution_host_id = chat.execution_host_id;
		workspace.workspace_directory = chat.workspace_directory;
		workspace.workspace_worktree_directory = chat.workspace_worktree_directory;
		return index.pending_native_workspace_keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(workspace));
	}

	void UnloadPersistedTranscript(ChatSession& chat)
	{
		if (chat.messages_loaded)
		{
			chat.persisted_message_count = chat.messages.size();
			chat.persisted_messages_digest = chat.updated_at + ":" + std::to_string(chat.persisted_message_count);
		}
		std::vector<Message>().swap(chat.messages);
		chat.messages_loaded = false;
	}

	bool HasLocalGeminiPendingOriginals(const ChatSession& chat)
	{
		if (chat.provider_id != uam::provider_ids::kGeminiCli || !uam::paths::IsControllerLocalWorkspace(chat))
			return false;
		const auto pending = [](const MessageAttachment& attachment) { return attachment.source == AttachmentSource::NativeInlinePending || attachment.native_inline_payload != nullptr; };
		return std::ranges::any_of(chat.messages, [&](const Message& message) { return std::ranges::any_of(message.attachments, pending); }) || std::ranges::any_of(chat.acp_queued_prompts, [&](const auto& prompt) { return std::ranges::any_of(prompt.attachments, pending); });
	}

	bool HasLocalDiscoveredPendingOriginals(const ChatSession& chat)
	{
		if (chat.provider_id == uam::provider_ids::kGeminiCli)
			return HasLocalGeminiPendingOriginals(chat);
		if (chat.provider_id != uam::provider_ids::kAntigravityCli || !uam::paths::IsControllerLocalWorkspace(chat))
			return false;
		return std::ranges::any_of(chat.messages, [](const Message& message) { return std::ranges::any_of(message.attachments, [](const MessageAttachment& attachment) { return attachment.source == AttachmentSource::NativeInlinePending || attachment.native_inline_payload != nullptr; }); });
	}

	bool AcknowledgeDiscoveredNativeMedia(ChatHistorySyncService::PreparedHistoryBatch& batch) noexcept
	{
		bool acknowledged = true;
		for (auto& candidate : batch.chats)
		{
			if (!candidate.native_media || !candidate.native_media->PrimaryAcknowledged() || candidate.native_media_acknowledged)
				continue;
			try
			{
				std::string error;
				candidate.native_media_acknowledged = candidate.native_media->AcknowledgeOriginals(error);
				acknowledged = candidate.native_media_acknowledged && acknowledged;
			}
			catch (...)
			{
				acknowledged = false;
			}
		}
		return acknowledged;
	}

	struct WorkerOwnedNativeMediaHistoryBatch final : ChatHistorySyncService::PreparedHistoryBatch
	{
		~WorkerOwnedNativeMediaHistoryBatch() noexcept
		{
			// The bounded lifetime owner invokes this destructor on its cleanup worker
			// even after cancellation, an early app return or a failed sync launch.
			// Only an actually acknowledged primary can acknowledge its originals.
			(void)AcknowledgeDiscoveredNativeMedia(*this);
		}
	};

	void TrackImportedNativeChat(NativeImportIndex& import_index, const ChatSession& chat, const std::string& native_key)
	{
		import_index.existing_ids.insert(chat.id);
		import_index.existing_id_by_native_key[native_key] = chat.id;
		import_index.existing_summary_by_native_key[native_key] = chat;
		UnloadPersistedTranscript(import_index.existing_summary_by_native_key.at(native_key));
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

	// A native export does not own UAM-authored input facts. The existing
	// whole-row merge may retain them only while their canonical prefix stays
	// in place; it cannot relocate facts by matching text, time or native index.
	bool NativeReplacementPreservesAuthoredInputFacts(const std::vector<Message>& saved, const std::vector<Message>& incoming)
	{
		// Native history may omit UAM fields, but it cannot originate them at an
		// appended row or a legacy canonical slot. Matching an ID elsewhere is
		// never a row binding, even when that ID belongs to a saved input.
		for (std::size_t index = 0; index < incoming.size(); ++index)
		{
			const Message& candidate = incoming[index];
			if (!candidate.native_input_id && !candidate.origin_request_id && !candidate.input_write_completed) continue;
			if (index >= saved.size()) return false;
			const Message& prior = saved[index];
			if ((candidate.native_input_id && candidate.native_input_id != prior.native_input_id) ||
			    (candidate.origin_request_id && candidate.origin_request_id != prior.origin_request_id) ||
			    (candidate.input_write_completed && !prior.input_write_completed)) return false;
		}
		bool canonical_prefix_unchanged = true;
		for (std::size_t index = 0; index < saved.size(); ++index)
		{
			const Message& prior = saved[index];
			canonical_prefix_unchanged = canonical_prefix_unchanged && index < incoming.size() &&
			    prior.role == incoming[index].role && prior.content == incoming[index].content &&
			    prior.created_at == incoming[index].created_at;
			if (!prior.native_input_id && !prior.origin_request_id && !prior.input_write_completed) continue;
			if (!canonical_prefix_unchanged) return false;
			const Message& candidate = incoming[index];
			// Missing fields are normal for native exports. A contradictory present
			// value or an external positive write fact must not be hidden by copying
			// the old row over it, or adopted as new UAM write evidence.
			if ((candidate.native_input_id && candidate.native_input_id != prior.native_input_id) ||
			    (candidate.origin_request_id && candidate.origin_request_id != prior.origin_request_id) ||
			    (candidate.input_write_completed && !prior.input_write_completed)) return false;
		}
		return true;
	}

	void RetainCanonicalInputHistory(const ChatSession& local_chat, ChatSession& native_chat)
	{
		// This is a retained full canonical graph, not a best-effort per-row
		// provenance transfer. Its caller's existing chat/host/revision ownership
		// checks remain responsible for selecting this local history.
		native_chat.messages = local_chat.messages;
		native_chat.messages_loaded = local_chat.messages_loaded;
		native_chat.persisted_message_count = local_chat.persisted_message_count;
		native_chat.persisted_messages_digest = local_chat.persisted_messages_digest;
		native_chat.updated_at = local_chat.updated_at;
	}

	void PreserveEquivalentLocalMessageDetails(const ChatSession& local_chat, ChatSession& native_chat)
	{
		if (!NativeReplacementPreservesAuthoredInputFacts(local_chat.messages, native_chat.messages))
		{
			RetainCanonicalInputHistory(local_chat, native_chat);
			return;
		}
		// Native exports may split streamed messages. Only unique tool IDs can retain child links.
		std::unordered_map<std::string_view, const ToolCall*> local_tools;
		for (const Message& message : local_chat.messages)
		{
			for (const ToolCall& tool : message.tool_calls)
			{
				if (tool.id.empty()) continue;
				const std::pair<std::unordered_map<std::string_view, const ToolCall*>::iterator, bool> inserted = local_tools.emplace(tool.id, &tool);
				if (!inserted.second) inserted.first->second = nullptr;
			}
		}
		for (Message& message : native_chat.messages)
		{
			for (ToolCall& tool : message.tool_calls)
			{
				const std::unordered_map<std::string_view, const ToolCall*>::const_iterator found = local_tools.find(tool.id);
				if (found == local_tools.end() || found->second == nullptr) continue;
				const ToolCall& local_tool = *found->second;
				tool.is_sub_agent = tool.is_sub_agent || local_tool.is_sub_agent;
				// Adapters distinguish an omitted collection from an authoritative empty/rejected list.
				if (!tool.sub_agent_children_authoritative && tool.sub_agent_children.empty())
				{
					if (tool.sub_agent_id.empty())
						tool.sub_agent_id = local_tool.sub_agent_id;
					if (!tool.sub_agent_id.empty() && tool.sub_agent_id == local_tool.sub_agent_id)
					{
						tool.sub_agent_children = local_tool.sub_agent_children;
						tool.sub_agent_children_authoritative = local_tool.sub_agent_children_authoritative;
						if (tool.sub_agent_title.empty())
							tool.sub_agent_title = local_tool.sub_agent_title;
					}
				}
				if (tool.kind.empty()) tool.kind = local_tool.kind;
				if (tool.approval_status.empty()) tool.approval_status = local_tool.approval_status;
				if (tool.task_id.empty()) tool.task_id = local_tool.task_id;
				if (tool.child_activity.empty()) tool.child_activity = local_tool.child_activity;
			}
		}
		const std::size_t common_count = std::min(local_chat.messages.size(), native_chat.messages.size());
		for (std::size_t index = 0; index < common_count; ++index)
		{
			const Message& local_message = local_chat.messages[index];
			Message& native_message = native_chat.messages[index];
			if (local_message.role != native_message.role || local_message.content != native_message.content || local_message.created_at != native_message.created_at) continue;
			Message merged = local_message;
			merged.continues_turn = native_message.continues_turn;
			if (!native_message.tool_calls.empty())
			{
				merged.tool_calls = std::move(native_message.tool_calls);
			}
			if (!native_message.blocks.empty()) merged.blocks = std::move(native_message.blocks);
			if (!native_message.thoughts.empty()) merged.thoughts = std::move(native_message.thoughts);
			if (!native_message.plan_summary.empty()) merged.plan_summary = std::move(native_message.plan_summary);
			if (!native_message.plan_entries.empty()) merged.plan_entries = std::move(native_message.plan_entries);
			if (std::ranges::any_of(native_message.attachments, [](const MessageAttachment& attachment) { return attachment.source != AttachmentSource::LegacyPath; }))
			{
				// Keep authored local files while replacing the prior native-media
				// projection with this snapshot's exact recorded associations.
				std::erase_if(merged.attachments, [](const MessageAttachment& attachment) { return attachment.source != AttachmentSource::LegacyPath; });
				for (auto& attachment : native_message.attachments)
					if (attachment.source != AttachmentSource::LegacyPath)
						merged.attachments.push_back(std::move(attachment));
			}
			native_message = std::move(merged);
		}
		// Assistant regrouping changes indexes. Preserve steering only for unique user identities.
		using UserIdentity = std::pair<std::string_view, std::string_view>;
		std::map<UserIdentity, std::pair<const Message*, std::size_t>> users;
		for (const Message& message : local_chat.messages)
		{
			if (message.role != MessageRole::User || message.created_at.empty()) continue;
			const UserIdentity key{message.created_at, message.content};
			const bool inserted = users.emplace(key, std::make_pair(&message, 0)).second;
			if (!inserted) users.at(key).first = nullptr;
		}
		for (const Message& message : native_chat.messages)
		{
			if (message.role != MessageRole::User) continue;
			const UserIdentity key{message.created_at, message.content};
			if (users.contains(key)) ++users.at(key).second;
		}
		for (Message& message : native_chat.messages)
		{
			if (message.role != MessageRole::User) continue;
			message.managed_origin_run_id.reset();
			const UserIdentity key{message.created_at, message.content};
			if (!users.contains(key)) continue;
			const std::pair<const Message*, std::size_t>& match = users.at(key);
			if (match.first != nullptr && match.second == 1)
			{
				message.managed_origin_run_id = match.first->managed_origin_run_id;
				message.continues_turn = message.continues_turn || match.first->continues_turn;
				if (match.first->acp_prompt_discarded)
				{
					message.acp_prompt_discarded = true;
					message.acp_prompt_not_sent = true;
				}
			}
		}
	}

	bool ReconcileExistingNativeImport(const fs::path& data_root, const ChatSession& local_summary, ChatSession& native_chat)
	{
		std::optional<ChatSession> local = ChatRepository::LoadLocalChat(data_root, local_summary.id, true);
		if (!local) return false;
		if (native_chat.provider_id == uam::provider_ids::kCodexCli &&
		    native_chat.managed_parent_input_receipts != local->managed_parent_input_receipts) return false;
		PreserveEquivalentLocalMessageDetails(*local, native_chat);
		const std::size_t local_count = local->messages.size();
		const std::size_t native_count = native_chat.messages.size();
		const auto recency = uam::time::CompareHistoryTimestamps(native_chat.updated_at, local->updated_at);
		const bool native_at_least_as_recent = recency.order == uam::time::HistoryTimestampOrder::After || recency.order == uam::time::HistoryTimestampOrder::Equal;
		const bool native_changed = native_count > local_count ||
		    (native_count == local_count && native_at_least_as_recent && !MessagesEquivalent(local->messages, native_chat.messages));
		OverlayLocalChatState(*local, native_chat);
		if (!native_changed)
		{
			native_chat.messages = local->messages;
			native_chat.updated_at = local->updated_at;
			return false;
		}
		return true;
	}

	struct DiscoveredGeminiOriginalReads
	{
		std::unordered_set<std::string> healthy_keys;
		std::size_t attempts = 0;
		std::size_t bytes = 0;
	};

	bool PendingDiscoveredGeminiOriginalIsConsistent(const MessageAttachment& attachment, std::string_view host)
	{
		const auto& payload = attachment.native_inline_payload;
		return attachment.source == AttachmentSource::NativeInlinePending && payload && attachment.mime_type == payload->MimeType() && attachment.content_digest == payload->Digest() && attachment.size_bytes == payload->DecodedSize() && attachment.path.empty() && !attachment.copied && attachment.preview_key.empty() && attachment.preview_origin_path.empty() && attachment.source_uri.empty() && attachment.native_asset_key.empty() && attachment.origin_execution_host_id == host && !attachment.id.empty() && !attachment.native_part_key.empty() && uam::attachment_sources::BoundedText(attachment.id, 16384) && uam::attachment_sources::BoundedText(attachment.name, 4096) && uam::attachment_sources::BoundedText(attachment.native_part_key, 16384) && uam::attachment_sources::BoundedText(attachment.native_tool_call_id, 1024) && uam::attachment_sources::BoundedText(attachment.native_source_field, 16384) && uam::attachment_sources::BoundedText(attachment.origin_execution_host_id, 1024);
	}

	bool DiscoveredGeminiNativePartEquivalent(const fs::path& root, std::string_view owner, std::string_view host, const MessageAttachment& saved, const MessageAttachment& incoming, DiscoveredGeminiOriginalReads& reads, std::stop_token stop)
	{
		if (stop.stop_requested() || saved.id != incoming.id || saved.name != incoming.name || saved.kind != incoming.kind || saved.mime_type != incoming.mime_type || saved.origin_execution_host_id != incoming.origin_execution_host_id || saved.native_part_key != incoming.native_part_key || saved.native_tool_call_id != incoming.native_tool_call_id || saved.native_source_field != incoming.native_source_field)
			return false;
		if (saved.source == AttachmentSource::ProviderUri || incoming.source == AttachmentSource::ProviderUri)
			return saved.source == AttachmentSource::ProviderUri && incoming.source == AttachmentSource::ProviderUri && uam::attachment_sources::IsPersistable(saved, owner) && uam::attachment_sources::IsPersistable(incoming, owner) && saved.source_uri == incoming.source_uri;
		if (saved.source != AttachmentSource::OwnedNativeAsset || !uam::attachment_sources::IsPersistable(saved, owner) || saved.content_digest != incoming.content_digest || saved.size_bytes != incoming.size_bytes)
			return false;
		if (incoming.source == AttachmentSource::OwnedNativeAsset)
			return uam::attachment_sources::IsPersistable(incoming, owner); // Inherited saved media, not a fresh source original.
		if (!PendingDiscoveredGeminiOriginalIsConsistent(incoming, host))
			return false;
		if (reads.healthy_keys.contains(saved.native_asset_key))
			return true;
		if (reads.attempts >= uam::native_assets::kMaxAssetCount || saved.size_bytes > uam::native_assets::kMaxOriginalBytes || saved.size_bytes > uam::native_assets::kMaxBatchBytes - reads.bytes)
			return false;
		// Metadata equality is not byte-health evidence. A failed owned read lets
		// the admitted fresh original reach the existing bounded repair/staging path.
		++reads.attempts;
		reads.bytes += static_cast<std::size_t>(saved.size_bytes); // Failed reads consume the same operation budget.
		const auto original = uam::native_assets::ReadOwned(root, owner, saved.native_asset_key, std::max<std::size_t>(1, static_cast<std::size_t>(saved.size_bytes)), stop);
		if (!original.ok)
			return false;
		reads.healthy_keys.insert(saved.native_asset_key);
		return true;
	}

	bool DiscoveredGeminiAttachmentsEquivalent(const fs::path& root, const ChatSession& local, const ChatSession& incoming, DiscoveredGeminiOriginalReads& reads, std::stop_token stop)
	{
		if (local.messages.size() != incoming.messages.size())
			return false;
		const std::string_view host = incoming.execution_host_id.empty() ? std::string_view("local") : incoming.execution_host_id;
		const auto next = [](const std::vector<MessageAttachment>& attachments, std::size_t& index, bool legacy) -> const MessageAttachment*
		{
			while (index < attachments.size())
			{
				const auto& attachment = attachments[index++];
				if ((attachment.source == AttachmentSource::LegacyPath) == legacy)
					return &attachment;
			}
			return nullptr;
		};
		for (std::size_t row = 0; row < local.messages.size(); ++row)
		{
			// The existing preservation merge groups authored files before native
			// parts. Compare each stable subsequence without changing either order.
			for (const bool legacy : {true, false})
			{
				std::size_t saved_index = 0, incoming_index = 0;
				for (;;)
				{
					const auto* saved = next(local.messages[row].attachments, saved_index, legacy);
					const auto* source = next(incoming.messages[row].attachments, incoming_index, legacy);
					if (!saved || !source)
					{
						if (saved != source)
							return false;
						break;
					}
					if (legacy ? !(*saved == *source) : !DiscoveredGeminiNativePartEquivalent(root, local.id, host, *saved, *source, reads, stop))
						return false;
				}
			}
		}
		return true;
	}

	bool ReconcileDiscoveredLocalGeminiMedia(const fs::path& root, const ChatSession& local_summary, ChatSession& incoming, DiscoveredGeminiOriginalReads& reads, std::stop_token stop,
	    std::optional<std::string_view> unchanged_conversation_updated_at = std::nullopt)
	{
		// Keep the existing reconciliation's fresh primary read and recency/count
		// rules; only this admitted batch path adds native-source equivalence.
		const auto saved = ChatRepository::LoadLocalChat(root, local_summary.id, true);
		if (!saved)
			return false;
		const ChatSession& local = *saved;
		if (local.imported_read_only || local.native_session_reset_pending || incoming.imported_read_only || incoming.native_session_reset_pending)
			return false;
		PreserveEquivalentLocalMessageDetails(local, incoming);
		const std::size_t local_count = local.messages.size();
		const std::size_t incoming_count = incoming.messages.size();
		// A final receipt may prove the exact original conversation unchanged even
		// when a later model/presentation save advances its metadata timestamp.
		// Ordinary discovery keeps the saved-chat recency rule unchanged.
		const std::string_view conversation_updated_at = unchanged_conversation_updated_at.value_or(local.updated_at);
		const auto recency = uam::time::CompareHistoryTimestamps(incoming.updated_at, conversation_updated_at);
		const bool native_at_least_as_recent = recency.order == uam::time::HistoryTimestampOrder::After || recency.order == uam::time::HistoryTimestampOrder::Equal;
		const bool changed = incoming_count > local_count || (incoming_count == local_count && native_at_least_as_recent && (!MessagesEquivalent(local.messages, incoming.messages) || !DiscoveredGeminiAttachmentsEquivalent(root, local, incoming, reads, stop)));
		OverlayLocalChatState(local, incoming);
		if (!changed)
		{
			incoming.messages = local.messages;
			incoming.updated_at = local.updated_at;
			return false;
		}
		return true;
	}

	std::optional<std::string> PrepareNativeChatForImport(const fs::path& data_root, NativeImportIndex& import_index, ChatSession& native_chat, const std::string& target_chat_id)
	{
		if (native_chat.messages_loaded && !uam::HasImportedConversationContent(native_chat.messages)) return std::nullopt;
		if (!import_index.tombstones_available)
		{
			return std::nullopt;
		}
		const std::string native_key = chat_identity::NativeIdentityKeyForHistoryImport(native_chat);
		if (import_index.tombstoned_native_keys.contains(native_key))
		{
			return std::nullopt;
		}
		if (target_chat_id.empty())
		{
			if (import_index.owned_native_keys.contains(native_key)) return std::nullopt;
			ChatSession workspace_identity;
			workspace_identity.provider_id = native_chat.provider_id;
			workspace_identity.execution_host_id = native_chat.execution_host_id;
			workspace_identity.workspace_directory = native_chat.workspace_directory;
			workspace_identity.workspace_worktree_directory = native_chat.workspace_worktree_directory;
			if (import_index.pending_native_workspace_keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(workspace_identity))) return std::nullopt;
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

		if (const ChatFolder* folder = uam::FindImportedWorkspaceFolder(app.folders, workspace_root))
		{
			native_chat.folder_id = folder->id;
			if (FolderDirectoryMatches(workspace_root, uam::paths::PathFromUtf8(folder->directory))) native_chat.workspace_directory = folder->directory;
		}
	}

	bool SaveImportedNativeChat(const uam::AppState& app,
	                            NativeImportIndex& import_index,
	                            const ChatSession& native_chat,
	                            const std::string& native_key,
	                            const fs::path& chats_dir,
	                            bool delete_native_after_import)
	{
		if (!native_chat.messages_loaded || !uam::HasImportedConversationContent(native_chat.messages)) return false;
		if (!ChatRepository::SaveChat(app.data_root, native_chat))
		{
			return false;
		}

		TrackImportedNativeChat(import_index, native_chat, native_key);
		DeleteNativeImportSourceIfRequested(chats_dir, native_chat, delete_native_after_import);
		return true;
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

	class CodexImportCallIndex
	{
	public:
		CodexImportCallIndex(const fs::path& source) : m_database(nullptr, sqlite3_close), m_records(source, std::ios::binary)
		{
			sqlite3* raw = nullptr;
			const int opened = sqlite3_open_v2("", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE, nullptr);
			m_database.reset(raw);
			if (!m_records || opened != SQLITE_OK || sqlite3_exec(raw, "PRAGMA cache_size=-2048; PRAGMA journal_mode=OFF; CREATE TABLE outputs(id TEXT, ordinal INTEGER, offset INTEGER, bytes INTEGER, PRIMARY KEY(id,ordinal)); CREATE TABLE calls(id TEXT PRIMARY KEY);", nullptr, nullptr, nullptr) != SQLITE_OK)
				throw std::runtime_error("Could not stage Codex call associations.");
		}
		void RecordOutput(std::string_view id, std::size_t ordinal, std::uintmax_t offset, std::size_t bytes)
		{
			Statement statement = Prepare("INSERT OR REPLACE INTO outputs VALUES(?1,?2,?3,?4)");
			Bind(statement.get(), 1, id);
			if (sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(ordinal)) != SQLITE_OK) Fail();
			if (sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(offset)) != SQLITE_OK ||
			    sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(bytes)) != SQLITE_OK) Fail();
			if (sqlite3_step(statement.get()) != SQLITE_DONE) Fail();
		}
		bool AddCall(std::string_view id)
		{
			Statement statement = Prepare("INSERT OR IGNORE INTO calls VALUES(?1)");
			Bind(statement.get(), 1, id);
			if (sqlite3_step(statement.get()) != SQLITE_DONE) Fail();
			return sqlite3_changes(m_database.get()) > 0;
		}
		bool HasCall(std::string_view id)
		{
			Statement statement = Prepare("SELECT 1 FROM calls WHERE id=?1");
			Bind(statement.get(), 1, id);
			const int step = sqlite3_step(statement.get());
			if (step != SQLITE_ROW && step != SQLITE_DONE) Fail();
			return step == SQLITE_ROW;
		}
		void VisitOutputs(std::string_view id, std::size_t call_ordinal,
		    const std::function<void(const nlohmann::json&)>& visit)
		{
			Statement statement = Prepare("SELECT offset,bytes FROM outputs WHERE id=?1 AND ordinal>?2 ORDER BY ordinal");
			Bind(statement.get(), 1, id);
			if (sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(call_ordinal)) != SQLITE_OK) Fail();
			for (;;)
			{
				const int step = sqlite3_step(statement.get());
				if (step == SQLITE_DONE) return;
				if (step != SQLITE_ROW) Fail();
				const std::streamoff offset = static_cast<std::streamoff>(sqlite3_column_int64(statement.get(), 0));
				const std::size_t bytes = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 1));
				std::string text(bytes, '\0');
				m_records.clear();
				m_records.seekg(offset);
				m_records.read(text.data(), static_cast<std::streamsize>(bytes));
				if (m_records.gcount() != static_cast<std::streamsize>(bytes)) Fail();
				visit(nlohmann::json::parse(text));
			}
		}
	private:
		using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
		[[noreturn]] static void Fail() { throw std::runtime_error("Could not read or write Codex call associations."); }
		Statement Prepare(const char* sql)
		{
			sqlite3_stmt* raw = nullptr;
			const int status = sqlite3_prepare_v2(m_database.get(), sql, -1, &raw, nullptr);
			Statement statement(raw, sqlite3_finalize);
			if (status != SQLITE_OK) Fail();
			return statement;
		}
		static void Bind(sqlite3_stmt* statement, int parameter, std::string_view text)
		{
			if (sqlite3_bind_text64(statement, parameter, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) Fail();
		}
		std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_database;
		std::ifstream m_records;
	};

	bool VisitCodexRolloutRecords(std::ifstream& records, std::uintmax_t snapshot_bytes,
	    const std::function<bool(const std::string&)>& visit, std::stop_token stop_token)
	{
		std::array<char, 64 * 1024> buffer;
		std::string line;
		std::uintmax_t remaining = snapshot_bytes;
		bool reading = true;
		while (reading && remaining > 0 && !stop_token.stop_requested())
		{
			const std::streamsize requested = static_cast<std::streamsize>(std::min<std::uintmax_t>(remaining, buffer.size()));
			records.read(buffer.data(), requested);
			const std::streamsize received = records.gcount();
			if (received != requested) return false;
			remaining -= static_cast<std::uintmax_t>(received);
			std::size_t start = 0;
			for (std::size_t offset = 0; offset < static_cast<std::size_t>(received); ++offset)
			{
				if (buffer[offset] != '\n') continue;
				line.append(buffer.data() + start, offset - start);
				if (!visit(line)) { reading = false; break; }
				line.clear();
				start = offset + 1;
			}
			if (reading) line.append(buffer.data() + start, static_cast<std::size_t>(received) - start);
		}
		if (reading && !line.empty() && !stop_token.stop_requested()) visit(line);
		return !records.bad();
	}

	std::optional<std::string> RawCodexSingleTextInput(const nlohmann::json& item, bool remote = false)
	{
		const auto* parts = uam::nlohmann_json::FindArrayField(item, "content");
		if (!parts || parts->size() != 1 || !parts->front().is_object()) return std::nullopt;
		const auto& part = parts->front();
		const auto type = uam::nlohmann_json::StringViewOrEmpty(part, "type");
		const auto text = part.find("text");
		if (type != (remote ? "text" : "input_text") || text == part.end() || !text->is_string()) return std::nullopt;
		return text->get<std::string>();
	}

	struct CodexUserTurnProvenance
	{
		std::string started, context;
		static std::string StrictId(const nlohmann::json& object, const char* name)
		{
			const auto value = object.find(name);
			if (value == object.end() || !value->is_string()) return {};
			const auto& id = value->get_ref<const std::string&>();
			return uam::managed_parent_history::Token(id) ? id : std::string{};
		}
		void Observe(const nlohmann::json& record)
		{
			const auto payload = record.find("payload");
			if (payload == record.end() || !payload->is_object()) { started.clear(); context.clear(); return; }
			const auto type = uam::nlohmann_json::StringViewOrEmpty(record, "type");
			if (type == "session_meta") { started.clear(); context.clear(); }
			else if (type == "turn_context")
			{
				const auto id = StrictId(*payload, "turn_id"); // Pinned TurnContextItem; NOT "id".
				if (started.empty() || id != started) { started.clear(); context.clear(); }
				else context = id;
			}
			else if (type == "event_msg")
			{
				const auto event = uam::nlohmann_json::StringViewOrEmpty(*payload, "type");
				if (event == "task_started" || event == "turn_started")
				{
					started = StrictId(*payload, "turn_id"); context.clear();
				}
				else if (event == "task_complete" || event == "task_completed" || event == "turn_completed" || event == "turn_aborted")
				{
					started.clear(); context.clear();
				}
			}
		}
		std::string_view Turn() const { return !started.empty() && started == context ? std::string_view(context) : std::string_view{}; }
	};

	std::optional<ChatSession> LoadCodexRolloutChat(const fs::path& rollout_file, const ChatFolder& folder, bool* malformed_out = nullptr, bool include_subagents = false, std::stop_token stop_token = {}, bool metadata_only = false, const std::function<bool(const Message&)>& emit = {}, const ChatSession* input_receipt_owner = nullptr)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli);
		if (malformed_out != nullptr) *malformed_out = false;
		// Snapshot the byte boundary so a live append cannot keep an import running indefinitely.
		std::error_code size_error;
		const std::uintmax_t snapshot_bytes = fs::file_size(rollout_file, size_error);
		std::ifstream records(rollout_file, std::ios::binary);
		if (size_error || !records)
		{
			if (malformed_out != nullptr) *malformed_out = true;
			return std::nullopt;
		}
		ChatSession chat;
		bool metadata_seen = false;
		bool include = false;
		bool has_user_message = false;
		bool parse_error = false;
		std::unordered_map<std::string, std::size_t> tool_messages;
		std::unique_ptr<CodexImportCallIndex> call_index;
		std::size_t message_count = 0;
		std::size_t ordinal = 0;
		std::string first_user_title;
		const auto add_message = [&](Message message)
		{
			if (first_user_title.empty() && message.role == MessageRole::User) first_user_title = uam::BuildImportedChatTitle(std::vector<Message>{message}, chat.created_at);
			if (emit)
			{
				if (!emit(message)) { parse_error = true; return; }
			}
			else chat.messages.push_back(std::move(message));
			++message_count;
		};
		std::error_code modified_error;
		const fs::file_time_type source_modified = fs::last_write_time(rollout_file, modified_error);
		if (emit)
		{
			call_index = std::make_unique<CodexImportCallIndex>(rollout_file);
			std::uintmax_t record_offset = 0;
			const bool indexed = VisitCodexRolloutRecords(records, snapshot_bytes, [&](const std::string& line)
			{
				++ordinal;
				const std::uintmax_t offset = record_offset;
				record_offset += line.size() + 1;
				if (uam::strings::TrimAsciiView(line).empty()) return true;
				try
				{
					const nlohmann::json record = nlohmann::json::parse(line);
					const auto payload = record.find("payload");
					if (uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "type") != "response_item" || payload == record.end() || !payload->is_object()) return true;
					const std::string_view type = uam::nlohmann_json::TrimmedStringViewOrEmpty(*payload, "type");
					if (type == "function_call_output" || type == "custom_tool_call_output")
						call_index->RecordOutput(uam::nlohmann_json::StringViewOrEmpty(*payload, "call_id"), ordinal, offset, line.size());
					return true;
				}
				catch (const nlohmann::json::exception&) { parse_error = true; return false; }
			}, stop_token);
			if (!indexed || parse_error || stop_token.stop_requested())
			{
				if (malformed_out != nullptr) *malformed_out = !indexed || parse_error;
				return std::nullopt;
			}
			records.clear();
			records.seekg(0);
			ordinal = 0;
		}


		// Raw per-item provenance is private to this decoder. No Message field or body-prefix authority.
		std::unordered_map<std::string, std::size_t> receipt_candidates;
		if (input_receipt_owner && !metadata_only &&
		    uam::managed_parent_history::ValidCollection(input_receipt_owner->managed_parent_input_receipts))
		{
			CodexUserTurnProvenance provenance;
			bool same_scope = false;
			const bool counted = VisitCodexRolloutRecords(records, snapshot_bytes, [&](const std::string& line)
			{
				if (uam::strings::TrimAsciiView(line).empty()) return true;
				try
				{
					const auto record = nlohmann::json::parse(line);
					provenance.Observe(record);
					const auto type = uam::nlohmann_json::StringViewOrEmpty(record, "type");
					const auto payload = record.find("payload");
					if (payload == record.end() || !payload->is_object()) return true;
					if (type == "session_meta")
					{
						const auto native = uam::codex::ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(*payload, "id"));
						const auto workspace = uam::nlohmann_json::StringViewOrEmpty(*payload, "cwd");
						same_scope = native == input_receipt_owner->native_session_id && workspace == input_receipt_owner->workspace_directory &&
						    input_receipt_owner->provider_id == uam::provider_ids::kCodexCli && input_receipt_owner->execution_host_id == "local";
					}
					if (same_scope && type == "response_item" &&
					    uam::nlohmann_json::StringViewOrEmpty(*payload, "type") == "message" &&
					    uam::nlohmann_json::StringViewOrEmpty(*payload, "role") == "user")
						if (const auto raw = RawCodexSingleTextInput(*payload))
							if (const auto* receipt = uam::managed_parent_history::FindBound(*input_receipt_owner,
							    input_receipt_owner->native_session_id, input_receipt_owner->workspace_directory,
							    provenance.Turn(), *raw))
								++receipt_candidates[receipt->id];
					return true;
				}
				catch (const nlohmann::json::exception&) { return false; }
			}, stop_token);
			if (!counted || stop_token.stop_requested()) { if (malformed_out) *malformed_out = true; return std::nullopt; }
			records.clear(); records.seekg(0);
		}
		CodexUserTurnProvenance user_provenance;
		const auto visit = [&](const std::string& line)
		    {
			    if (stop_token.stop_requested()) return false;
			    ++ordinal;
			    if (uam::strings::TrimAsciiView(line).empty()) return true;
			    try
			    {
				    const nlohmann::json record = nlohmann::json::parse(line);
				    user_provenance.Observe(record);
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
					    include = !chat.native_session_id.empty() && !cwd.empty() && (include_subagents || !is_subagent) &&
					              (folder.directory.empty() || uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(cwd), uam::paths::PathFromUtf8(folder.directory)));
					    if (!include)
					    {
						    return false;
					    }

					    chat.id = chat.native_session_id;
					    if (input_receipt_owner && chat.native_session_id == input_receipt_owner->native_session_id &&
					        cwd == input_receipt_owner->workspace_directory && input_receipt_owner->execution_host_id == "local")
					    {
						    chat.managed_parent_input_receipts = input_receipt_owner->managed_parent_input_receipts;
						    chat.managed_parent_input_receipts_present = input_receipt_owner->managed_parent_input_receipts_present;
					    }
					    chat.branch_root_chat_id = chat.id;
					    chat.provider_id = uam::provider_ids::kCodexCli;
					    chat.folder_id = folder.id;
					    chat.workspace_directory = cwd;
					    chat.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "timestamp")};
					    if (chat.created_at.empty())
					    {
						    chat.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};
					    }
					    chat.updated_at = chat.created_at;
					    chat.messages_loaded = !metadata_only;
					    return true;
				    }

				    if (!include || record_type != "response_item") return true;
				    const std::string_view item_type = uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "type");
				    if (metadata_only && (item_type != "message" || uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "role") != "user")) return true;
				    const bool builtin_tool = item_type == "local_shell_call" || item_type == "web_search_call";
				    if (item_type == "function_call" || item_type == "custom_tool_call" || builtin_tool)
				    {
					    std::string call_id = builtin_tool ? std::string{uam::nlohmann_json::StringViewOrEmpty(payload, "call_id")}
					        : payload.value("call_id", "");
					    if (builtin_tool && call_id.empty())
					    {
						    call_id = uam::nlohmann_json::StringViewOrEmpty(payload, "id");
						    // Built-in records can omit both IDs; retain an addressable row in the transcript.
						    if (call_id.empty()) call_id = "rollout-tool-" + std::to_string(message_count);
					    }
					    if (call_id.empty() || (call_index ? !call_index->AddCall(call_id) : tool_messages.contains(call_id))) return true;
					    ToolCall tool;
					    tool.id = call_id;
					    if (builtin_tool)
					    {
						    tool.name = item_type == "local_shell_call" ? "shell" : "web_search";
						    const nlohmann::json::const_iterator action = payload.find("action");
						    if (action != payload.end() && !action->is_null()) tool.args_json = action->dump();
						    tool.status = uam::nlohmann_json::StringViewOrEmpty(payload, "status");
					    }
					    else
					    {
						    tool.name = payload.value("name", "");
						    tool.args_json = item_type == "function_call" ? payload.value("arguments", "")
						        : nlohmann::json{{"input", payload.value("input", "")}}.dump();
						    tool.status = "pending";
					    }
					    runtime.ApplyNativeToolMetadata(tool, payload);
					    if (call_index)
					    {
						    call_index->VisitOutputs(call_id, ordinal, [&](const nlohmann::json& output_record)
						    {
							    const nlohmann::json& output_payload = output_record.at("payload");
							    const nlohmann::json& output = output_payload.at("output");
							    tool.result_text = output.is_string() ? output.get<std::string>() : output.dump();
							    tool.status = "completed";
							    runtime.ApplyNativeToolMetadata(tool, output_payload);
						    });
					    }
					    Message message;
					    message.role = MessageRole::Assistant;
					    message.provider = uam::provider_ids::kCodexCli;
					    message.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};
					    message.blocks.push_back({"tool_call", "", call_id, ""});
					    message.tool_calls.push_back(std::move(tool));
					    if (!message.created_at.empty()) chat.updated_at = message.created_at;
					    if (!call_index) tool_messages.emplace(call_id, chat.messages.size());
					    add_message(std::move(message));
					    return !parse_error;
				    }
				    if (item_type == "function_call_output" || item_type == "custom_tool_call_output")
				    {
					    if (call_index)
					    {
						    if (call_index->HasCall(uam::nlohmann_json::StringViewOrEmpty(payload, "call_id")))
						    {
							    const std::string_view timestamp = uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp");
							    if (!timestamp.empty()) chat.updated_at = timestamp;
						    }
						    return true;
					    }
					    const auto found = tool_messages.find(std::string{uam::nlohmann_json::StringViewOrEmpty(payload, "call_id")});
					    if (found == tool_messages.end()) return true;
					    ToolCall& tool = chat.messages[found->second].tool_calls.front();
					    const nlohmann::json& output = payload.at("output");
					    tool.result_text = output.is_string() ? output.get<std::string>() : output.dump();
					    tool.status = "completed";
					    runtime.ApplyNativeToolMetadata(tool, payload);
					    const std::string_view timestamp = uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp");
					    if (!timestamp.empty()) chat.updated_at = timestamp;
					    return true;
				    }
				    if (item_type == "reasoning")
				    {
					    Message message;
					    message.role = MessageRole::Assistant;
					    message.provider = uam::provider_ids::kCodexCli;
					    message.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};
					    const auto summary = payload.find("summary");
					    if (summary == payload.end() || !summary->is_array()) return true;
					    for (const nlohmann::json& part : *summary)
					    {
						    if (uam::nlohmann_json::StringViewOrEmpty(part, "type") != "summary_text") continue;
						    const std::string_view text = uam::nlohmann_json::StringViewOrEmpty(part, "text");
						    if (uam::strings::TrimAsciiView(text).empty()) continue;
						    if (!message.thoughts.empty()) message.thoughts += '\n';
						    message.thoughts += text;
					    }
					    if (message.thoughts.empty()) return true;
					    message.blocks.push_back({"thought", message.thoughts, "", ""});
					    if (!message.created_at.empty()) chat.updated_at = message.created_at;
					    add_message(std::move(message));
					    return !parse_error;
				    }
				    if (item_type != "message") return true;

				    const std::string_view role = uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "role");
				    if (role != "user" && role != "assistant")
				    {
					    return true;
				    }
				    std::string content = CodexMessageText(payload);
				    if (role == "user" && input_receipt_owner && chat.native_session_id == input_receipt_owner->native_session_id &&
				        chat.workspace_directory == input_receipt_owner->workspace_directory)
				    {
					    if (const auto raw = RawCodexSingleTextInput(payload))
						    if (const auto* receipt = uam::managed_parent_history::FindBound(*input_receipt_owner,
						        chat.native_session_id, chat.workspace_directory, user_provenance.Turn(), *raw);
						        receipt && receipt_candidates[receipt->id] == 1)
							    return true; // Exact raw bytes and one bound native item; keep its assistant output.
				    }
				    if (role == "user") content = uam::CodexVisibleUserMessage(content);
				    if (content.empty())
				    {
					    return true;
				    }
				    if (metadata_only)
				    {
					    // Discovery requires authored history without retaining the transcript.
					    has_user_message = true;
					    return false;
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
				    add_message(std::move(message));
			    }
			    catch (const nlohmann::json::exception&)
			    {
				    parse_error = true;
				    return false;
			    }
			    return !parse_error;
			};
		const bool read_success = VisitCodexRolloutRecords(records, snapshot_bytes, visit, stop_token);
		if (emit && (modified_error || fs::file_size(rollout_file, size_error) != snapshot_bytes || size_error ||
		    fs::last_write_time(rollout_file, modified_error) != source_modified || modified_error)) parse_error = true;
		if (malformed_out != nullptr) *malformed_out = !read_success || parse_error;

		if (stop_token.stop_requested()) return std::nullopt;
		if (!read_success || parse_error || !metadata_seen || !include || !has_user_message)
		{
			return std::nullopt;
		}
		if (emit)
		{
			chat.title = first_user_title.empty() ? uam::BuildImportedChatTitle({}, chat.created_at) : first_user_title;
			chat.messages_loaded = false;
			chat.persisted_message_count = message_count;
			chat.persisted_messages_digest = chat.updated_at + ":" + std::to_string(message_count);
		}
		else chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
		if (metadata_only) chat.messages_loaded = false;
		return chat;
	}

	std::optional<ChatSession> CanonicalCodexInputReceiptOwner(const fs::path& root, const NativeImportIndex& index, const ChatSession& native)
	{
		const auto found = index.existing_id_by_native_key.find(chat_identity::NativeIdentityKeyForHistoryImport(native));
		if (found == index.existing_id_by_native_key.end()) return std::nullopt;
		auto owner = ChatRepository::LoadLocalChat(root, found->second, false);
		if (owner && !owner->managed_parent_input_receipts_present && owner->managed_parent_result &&
		    !owner->managed_parent_result->accepted_turn_id.empty())
			owner = ChatRepository::LoadLocalChat(root, found->second, true); // Only canonical full primary can normalize old proof.
		if (!owner || chat_identity::NativeIdentityKeyForHistoryImport(*owner) != chat_identity::NativeIdentityKeyForHistoryImport(native))
			return std::nullopt;
		return owner;
	}

	std::shared_ptr<PreparedChatSave> PrepareCodexRolloutImport(const fs::path& root, const fs::path& rollout,
	    ChatSession& metadata, bool* malformed = nullptr, std::string_view native_title = {})
	{
		const ChatSession identity = metadata;
		return ChatRepository::PrepareImportedChatSave(root, metadata,
		    [&](const std::function<bool(const Message&)>& emit, ChatSession& final_metadata)
		{
			ChatFolder source;
			source.directory = identity.workspace_directory;
			std::optional<ChatSession> loaded = LoadCodexRolloutChat(rollout, source, malformed, false, {}, false, emit);
			if (!loaded || loaded->native_session_id != identity.native_session_id ||
			    !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(loaded->workspace_directory), uam::paths::PathFromUtf8(identity.workspace_directory)) ||
			    !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(identity.workspace_directory), uam::paths::PathFromUtf8(loaded->workspace_directory))) return false;
			loaded->id = identity.id;
			loaded->folder_id = identity.folder_id;
			loaded->branch_root_chat_id = identity.branch_root_chat_id;
			loaded->execution_host_id = identity.execution_host_id;
			if (!native_title.empty()) loaded->title = native_title;
			final_metadata = std::move(*loaded);
			return true;
		});
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

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	std::shared_ptr<PreparedChatSave> PrepareOpenCodeImport(const fs::path& root, const uam::opencode_import::Spool& source, ChatSession& chat, std::string& error)
	{
		return ChatRepository::PrepareImportedChatSave(root, chat,
		    [&](const std::function<bool(const Message&)>& emit, ChatSession& metadata)
		{
			bool has_content = false;
			const bool complete = uam::opencode_import::Read(*source, metadata, [&](const Message& message)
			{
				has_content = has_content || uam::HasImportedConversationContent(message);
				return emit(message);
			}, {}, error);
			return complete && has_content;
		});
	}
#endif

	bool NativeChatShouldBeImported(const fs::path& chats_dir, const ChatSession& native_chat, const std::string& target_id, bool delete_native_after_import)
	{
		if (native_chat.messages_loaded && !uam::HasImportedConversationContent(native_chat.messages)) return false;
		if (IsUamMemoryWorkerNativeChat(native_chat))
		{
			DeleteNativeImportSourceIfRequested(chats_dir, native_chat, delete_native_after_import);
			return false;
		}

		return NativeChatMatchesImportTarget(native_chat, target_id);
	}

	std::string ResolvePersistedImportFolderIdForSource(uam::AppState& app, const ProviderChatSource& source, bool save = true)
	{
		const fs::path workspace = uam::paths::PathFromUtf8(source.folder_directory);
		if (const ChatFolder* folder = uam::FindImportedWorkspaceFolder(app.folders, workspace)) return folder->id;
		const fs::path directory = uam::ResolveImportedWorkspaceFolderDirectory(workspace);

		ChatFolder new_folder;
		new_folder.id = ChatFolderStore::AllocateId(app.folders);
		if (new_folder.id.empty()) return {};
		new_folder.title = FolderDirectoryMatches(directory, workspace) ? source.folder_title : uam::BuildFolderTitleFromProjectRoot(directory);
		new_folder.directory = uam::paths::Utf8PathString(directory);
		new_folder.collapsed = false;

		app.folders.push_back(std::move(new_folder));
		const std::string created_folder_id = app.folders.back().id;

		if (!save || ChatFolderStore::Save(app.data_root, app.folders))
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
			if (lhs[i].managed_origin_run_id != rhs[i].managed_origin_run_id || lhs[i].role != rhs[i].role || lhs[i].content != rhs[i].content || lhs[i].created_at != rhs[i].created_at ||
			    lhs[i].tool_calls != rhs[i].tool_calls || lhs[i].thoughts != rhs[i].thoughts ||
			    lhs[i].blocks != rhs[i].blocks || lhs[i].plan_summary != rhs[i].plan_summary || lhs[i].plan_entries != rhs[i].plan_entries)
			{
				return false;
			}
		}

		return true;
	}

	bool UnknownHistoryCanRetainAuthoritativeNativeTools(const ChatSession& local, const ChatSession& native)
	{
		if (!local.messages_loaded || !native.messages_loaded || local.messages.size() != native.messages.size()) return false;
		// Compare every persisted message field except its invocation records.
		// Unknown age cannot authorize any other authored/presentation replacement.
		const auto message_fields = [](const Message& message)
		{
			return std::tie(message.role, message.content, message.created_at, message.provider,
			    message.tokens_input, message.tokens_output, message.estimated_cost_usd, message.time_to_first_token_ms,
			    message.processing_time_ms, message.interrupted, message.stop_reason, message.priority_steer,
			    message.checkpoint_sha, message.checkpoint_parent_sha, message.thoughts, message.plan_summary,
			    message.plan_entries, message.blocks, message.markdown_store_files, message.markdown_store_prompt_blocks,
			    message.attachments, message.model_id, message.continues_turn, message.acp_prompt_not_sent,
			    message.acp_prompt_discarded, message.provider_event_ids, message.managed_origin_run_id,
			    message.native_input_id, message.origin_request_id, message.input_write_completed);
		};
		std::unordered_set<std::string_view> local_ids, native_ids;
		bool authoritative_change = false;
		const auto preserves_recorded = [](const std::string& saved, const std::string& incoming)
		{
			return saved.empty() || saved == incoming;
		};
		for (std::size_t row = 0; row < local.messages.size(); ++row)
		{
			const Message& saved = local.messages[row];
			const Message& incoming = native.messages[row];
			if (message_fields(saved) != message_fields(incoming) || saved.tool_calls.size() != incoming.tool_calls.size()) return false;
			for (std::size_t index = 0; index < saved.tool_calls.size(); ++index)
			{
				const ToolCall& old_tool = saved.tool_calls[index];
				const ToolCall& new_tool = incoming.tool_calls[index];
				if (old_tool.id.empty() || old_tool.id != new_tool.id || !local_ids.insert(old_tool.id).second || !native_ids.insert(new_tool.id).second) return false;
				if (old_tool == new_tool) continue;
				// This authority describes normalized receivers, including rejected
				// or empty lists. It is not authority to replace recorded tool input,
				// output or UAM task/approval facts under unknown chronology.
				if (!new_tool.is_sub_agent || !new_tool.sub_agent_children_authoritative ||
				    !uam::codex::HistoryToolOperationEquivalent(local.provider_id, old_tool.name, new_tool.name) ||
				    !preserves_recorded(old_tool.args_json, new_tool.args_json) ||
				    !preserves_recorded(old_tool.result_text, new_tool.result_text) ||
				    !preserves_recorded(old_tool.kind, new_tool.kind) ||
				    !preserves_recorded(old_tool.approval_status, new_tool.approval_status) ||
				    !preserves_recorded(old_tool.task_id, new_tool.task_id) ||
				    !preserves_recorded(old_tool.child_activity, new_tool.child_activity)) return false;
				authoritative_change = true;
			}
		}
		return authoritative_change;
	}

	std::string SelectHistoryMetadataTimestamp(const std::string& saved, const std::string& observed)
	{
		if (observed.empty()) return saved;
		const auto recency = uam::time::CompareHistoryTimestamps(observed, saved.empty() ? observed : saved);
		if ((saved.empty() && recency.order == uam::time::HistoryTimestampOrder::Equal) ||
		    (!saved.empty() && recency.order == uam::time::HistoryTimestampOrder::After)) return observed;
		return saved;
	}

	std::size_t EffectiveMessageCount(const ChatSession& chat)
	{
		return chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
	}

	/// <summary>Session-list metadata cannot establish conversation content for either remote provider.</summary>
	bool LoadRemoteImportHistory(const ExecutionHost& host, const ProviderProfile& profile,
	    ChatSession& chat, std::string& error, std::stop_token stop = {})
	{
		error.clear();
		if (stop.stop_requested()) { error = "Remote history import was canceled."; return false; }
		if (chat.messages_loaded && uam::HasImportedConversationContent(chat.messages)) return true;
		std::vector<Message> messages;
		bool verified_empty = false;
		if (chat.provider_id == uam::provider_ids::kOpenCodeCli)
		{
			auto history = ChatHistorySyncService().LoadRemoteOpenCodeTranscript(host, chat, profile, stop);
			if (!history.success) { error = uam::strings::NonEmptyOrFallback(std::move(history.error), "Remote OpenCode history could not be verified. Retry the import."); return false; }
			verified_empty = history.verified_empty;
			messages = std::move(history.messages);
		}
		else if (chat.provider_id == uam::provider_ids::kCodexCli)
		{
			auto history = ChatHistorySyncService().LoadRemoteCodexTranscript(host, chat, stop);
			if (!history.success) { error = uam::strings::NonEmptyOrFallback(std::move(history.error), "Remote Codex history could not be verified. Retry the import."); return false; }
			verified_empty = !uam::HasImportedConversationContent(history.messages);
			messages = std::move(history.messages);
		}
		else { error = "This provider does not support remote native-history import."; return false; }
		if (stop.stop_requested()) { error = "Remote history import was canceled."; return false; }
		if (!uam::HasImportedConversationContent(messages))
		{
			if (!verified_empty) error = "Provider history contains entries that could not be imported. Retry after updating the provider.";
			return false;
		}
		chat.messages = std::move(messages);
		chat.messages_loaded = true;
		chat.persisted_message_count = chat.messages.size();
		return true;
	}

	bool LocalMessagesShouldOverrideNative(const ChatSession& local_chat, const ChatSession& native_chat)
	{
		const std::size_t local_count = EffectiveMessageCount(local_chat);
		const std::size_t native_count = EffectiveMessageCount(native_chat);
		const auto recency = uam::time::CompareHistoryTimestamps(local_chat.updated_at, native_chat.updated_at);
		// Unknown age preserves a loaded saved graph, never an empty summary vector.
		return local_count > 0 && (local_count > native_count || (local_count == native_count && (recency.order == uam::time::HistoryTimestampOrder::After || (local_chat.messages_loaded && recency.order == uam::time::HistoryTimestampOrder::Unknown))));
	}

	void OverlayLocalChatState(const ChatSession& local, ChatSession& native)
	{
		// A passive legacy/custom export cannot erase a same-owner V2 refusal.
		// Check native identity before the general overlay changes provider/workspace.
		// Presence includes malformed-present data; absence must retain a new V2 observation.
		if (local.opencode_v2_session_id.has_value() &&
		    uam::provider_ids::IsCliProviderAliasOf(local.provider_id, uam::provider_ids::kOpenCodeCli) &&
		    uam::provider_ids::IsCliProviderAliasOf(native.provider_id, uam::provider_ids::kOpenCodeCli) &&
		    !local.native_session_id.empty() && local.native_session_id == native.native_session_id &&
		    uam::strings::NonEmptyOrFallback(uam::strings::Trim(local.execution_host_id), "local") ==
		        uam::strings::NonEmptyOrFallback(uam::strings::Trim(native.execution_host_id), "local") &&
		    !chat_identity::NativeWorkspaceForHistoryImport(local).empty() &&
		    chat_identity::NativeIdentityKeyForHistoryImport(local) == chat_identity::NativeIdentityKeyForHistoryImport(native))
			native.opencode_v2_session_id = local.opencode_v2_session_id;
		const NativeSessionLinkService native_session_links;
		const std::string local_session_id = native_session_links.RealNativeSessionId(local);
		if (!local_session_id.empty() && !uam::strings::IsBlank(local.id) && local_session_id == native_session_links.RealNativeSessionId(native))
		{
			native.id = local.id;
			if (!uam::strings::IsBlank(local.created_at))
			{
				native.created_at = local.created_at;
			}
		}

		if (!uam::strings::IsBlank(local.provider_id))
		{
			native.provider_id = local.provider_id;
		}

		native.title = local.title;
		native.folder_id = local.folder_id;
		native.native_session_reset_pending = local.native_session_reset_pending;
		native.provider_handoff_context = local.provider_handoff_context;
		native.provider_handoff_session_id = local.provider_handoff_session_id;
		native.remote_recovery_enabled = local.remote_recovery_enabled;
		native.remote_recovery_state = local.remote_recovery_state;
		native.pinned = local.pinned;
		native.linked_files = local.linked_files;
		native.temporary_parent_chat_id = local.temporary_parent_chat_id;
		native.provider_handoff_context = local.provider_handoff_context;
		native.provider_handoff_session_id = local.provider_handoff_session_id;
		native.provider_handoff_cli_contexts = local.provider_handoff_cli_contexts;
		native.side_cleanup_requested = local.side_cleanup_requested;
		native.side_cleanup_retry_time_s = local.side_cleanup_retry_time_s;
		native.parent_chat_id = local.parent_chat_id;
		native.branch_root_chat_id = local.branch_root_chat_id;
		native.subagent_parent_chat_id = local.subagent_parent_chat_id;
		native.subagent_parent_created_at = local.subagent_parent_created_at;
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
		native.svn_update_pending = local.svn_update_pending;
		native.imported_read_only = local.imported_read_only;
		native.approval_mode = local.approval_mode;
		native.command_safety_tier = local.command_safety_tier;
		native.model_id = local.model_id;
		native.reviewer_model_id = local.reviewer_model_id;
		native.reasoning_effort = local.reasoning_effort;
		native.service_tier = local.service_tier;
		native.service_tier_explicit = local.service_tier_explicit;
		native.provider_handoff_context = local.provider_handoff_context;
		native.provider_handoff_session_id = local.provider_handoff_session_id;
		native.provider_handoff_cli_contexts = local.provider_handoff_cli_contexts;
		native.small_model_mode = local.small_model_mode;
		native.extra_flags = local.extra_flags;
		native.memory_enabled = local.memory_enabled;
		native.memory_level = local.memory_level;
		native.memory_last_processed_message_count = local.memory_last_processed_message_count;
		native.memory_last_processed_at = local.memory_last_processed_at;
		native.managed_parent_result = local.managed_parent_result;
		native.managed_parent_input_receipts = local.managed_parent_input_receipts;
		native.managed_parent_input_receipts_present = local.managed_parent_input_receipts_present;
		native.goals = local.goals;
		native.active_goal_id = local.active_goal_id;
		native.interaction_at = local.interaction_at;
		if (!uam::strings::IsBlank(local.last_opened_at))
		{
			native.last_opened_at = local.last_opened_at;
			native.attention_revision = local.attention_revision;
			native.last_stop_reason = local.last_stop_reason;
			native.goal_command_revision = local.goal_command_revision;
			native.goal_pending_continuation_id = local.goal_pending_continuation_id;
		}
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
		if (!uam::paths::IsControllerLocalWorkspace(chat) ||
		    NativeSessionLinkService().IsLocalDraftChatId(chat.id) ||
		    uam::strings::IsBlank(chat.provider_id))
		{
			return false;
		}

		const ProviderProfile* provider = FindProviderForLocalChat(app, chat);
		return (provider == nullptr) ? true : ProviderRuntime::UsesNativeOverlayHistory(*provider);
	}

	bool NativeHistoryWorkspacesMatch(const uam::AppState& app, const ChatSession& source, const ChatSession& candidate)
	{
		if (uam::paths::IsControllerLocalWorkspace(source))
		{
			const std::string workspace = chat_identity::NativeWorkspaceForHistoryImport(source);
			return uam::paths::IsControllerLocalWorkspace(candidate) &&
			    (workspace.empty() || Utf8WorkspaceDirectoriesMatch(workspace, chat_identity::NativeWorkspaceForHistoryImport(candidate)));
		}
		const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, source.execution_host_id);
		return host != nullptr && candidate.execution_host_id == source.execution_host_id &&
		    uam::execution_hosts::IsAbsoluteRemotePath(host->platform, source.workspace_directory) &&
		    uam::execution_hosts::RemotePathsMatch(host->platform, source.workspace_directory, candidate.workspace_directory);
	}

	struct LocalChatOverlayIndex
	{
		std::unordered_map<std::string, ChatSession*> by_id;
		std::unordered_map<std::string, ChatSession*> by_native_identity;
	};

	LocalChatOverlayIndex BuildLocalChatOverlayIndex(std::vector<ChatSession>& local_chats)
	{
		LocalChatOverlayIndex index;
		index.by_id.reserve(local_chats.size());
		index.by_native_identity.reserve(local_chats.size());

		for (ChatSession& local_chat : local_chats)
		{
			if (!uam::paths::IsControllerLocalWorkspace(local_chat))
			{
				continue;
			}
			index.by_id[local_chat.id] = &local_chat;

			if (!NativeSessionLinkService().HasRealNativeSessionId(local_chat))
			{
				continue;
			}

			const std::string native_session_id = uam::strings::Trim(local_chat.native_session_id);
			if (!native_session_id.empty())
			{
				index.by_native_identity[chat_identity::NativeIdentityKeyForHistoryImport(local_chat)] = &local_chat;
			}
		}

		return index;
	}

	ChatSession* FindLocalOverlayMatch(const uam::AppState& app, const LocalChatOverlayIndex& index, const ChatSession& native_chat)
	{
		if (const auto local_it = index.by_id.find(native_chat.id); local_it != index.by_id.end())
		{
			const ChatSession& local = *local_it->second;
			if (NativeHistoryWorkspacesMatch(app, native_chat, local) &&
			    (uam::strings::IsBlank(local.provider_id) ||
			     uam::provider_ids::CanonicalCliProviderLookupId(local.provider_id) == uam::provider_ids::CanonicalCliProviderLookupId(native_chat.provider_id)) &&
			    (uam::strings::IsBlank(local.native_session_id) || uam::strings::IsBlank(native_chat.native_session_id) ||
			     uam::strings::TrimmedEquals(local.native_session_id, native_chat.native_session_id)))
				return local_it->second;
		}

		const std::string native_session_id = uam::strings::Trim(native_chat.native_session_id);
		if (native_session_id.empty())
		{
			return nullptr;
		}

		const auto native_session_it = index.by_native_identity.find(chat_identity::NativeIdentityKeyForHistoryImport(native_chat));
		return native_session_it == index.by_native_identity.end() ? nullptr : native_session_it->second;
	}

	bool LocalChatHasActiveWork(const uam::AppState& app, const ChatSession& local_chat, const std::string& selected_chat_id)
	{
		return EffectiveMessageCount(local_chat) > 0 ||
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

	bool OverlayPartialNativeImport(const ChatHistorySyncService& service, const uam::AppState& app, std::vector<ChatSession>& native_chats, std::string& error)
	{
		if (!service.FilterDeletedNativeHistory(app.data_root, native_chats, &error))
			return false;
		std::unordered_set<std::string> attested_keys;
		for (const ChatSession& chat : native_chats)
			attested_keys.insert(chat_identity::NativeIdentityKeyForHistoryImport(chat));
		// A partial source is not a replacement catalogue and cannot infer links.
		(void)service.OverlayLocalHistory(app, native_chats, false, nullptr, &error);
		if (!error.empty()) return false;
		if (std::ranges::any_of(native_chats, [&](const ChatSession& chat) { return !attested_keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(chat)); }))
		{
			error = "Native history ownership changed during local import reconciliation.";
			native_chats.clear();
			return false;
		}
		return true;
	}

	const ChatSession* VerifiedFinalCliTerminalChat(const uam::AppState& app, const uam::CliTerminalState& terminal, const ProviderProfile& provider)
	{
		// A target ID alone cannot authorize overwriting a running terminal's history.
		if (std::ranges::none_of(app.cli_terminals, [&](const auto& current) { return current.get() == &terminal; }) || terminal.native_session_discovery_ambiguous || terminal.native_identity_query_phase != 0 || terminal.worktree_launch_pending || terminal.context_preparation != nullptr || terminal.native_session_setup_cancel != nullptr)
			return nullptr;
		const std::string chat_id = uam::CliTerminalAttachedChatId(terminal);
		const std::string native_id = uam::CliTerminalAttachedSessionId(terminal);
		if (uam::strings::IsBlank(terminal.terminal_id) || chat_id.empty() || native_id.empty() || !uam::ChatSyncIdsMatch(terminal.frontend_chat_id, chat_id))
			return nullptr;
		const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr || chat->imported_read_only || chat->native_session_reset_pending || chat->execution_host_id != uam::execution_hosts::kLocalHostId || !uam::paths::IsControllerLocalWorkspace(*chat) || !uam::ChatSyncIdsMatch(chat->native_session_id, native_id) || uam::provider_ids::CanonicalCliProviderLookupId(chat->provider_id) != uam::provider_ids::CanonicalCliProviderLookupId(provider.id) || !ProviderRuntime::UsesNativeOverlayHistory(provider))
			return nullptr;
		if (!terminal.native_activity_provider_id.empty() && uam::provider_ids::CanonicalCliProviderLookupId(terminal.native_activity_provider_id) != uam::provider_ids::CanonicalCliProviderLookupId(provider.id))
			return nullptr;
		const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat_id);
		if (resolved != app.resolved_native_sessions_by_chat_id.end() && !uam::ChatSyncIdsMatch(resolved->second, native_id))
			return nullptr;
		const std::string workspace = chat_identity::NativeWorkspaceForHistoryImport(*chat);
		if (workspace.empty() || !uam::paths::PathFromUtf8(workspace).is_absolute())
			return nullptr;
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(*chat);
		if (std::ranges::count_if(app.chats, [&](const auto& current) { return current.id == chat_id || chat_identity::NativeIdentityKeyForHistoryImport(current) == key; }) != 1)
			return nullptr;
		if (std::ranges::any_of(app.cli_terminals,
		                        [&](const auto& other)
		                        {
			                        if (other == nullptr || other.get() == &terminal || !other->running)
				                        return false;
			                        if (uam::ChatSyncIdsMatch(other->frontend_chat_id, chat_id) || uam::ChatSyncIdsMatch(other->attached_chat_id, chat_id))
				                        return true;
			                        const ChatSession* other_owner = ChatDomainService().FindChatById(app, uam::CliTerminalAttachedChatId(*other));
			                        return other_owner != nullptr && chat_identity::NativeIdentityKeyForHistoryImport(*other_owner) == key;
		                        }))
			return nullptr;
		return chat;
	}

	struct FinalCliTerminalImport
	{
		const uam::CliTerminalState* terminal = nullptr;
		std::string chat_id;
		std::string native_key;
		std::shared_ptr<const std::uint8_t> incarnation;
		fs::path workspace;
		std::string terminal_id;
		double startup_time_s = 0.0;
	};

	bool PartialNativeImportPreservesLiveWork(const uam::AppState& app, const ChatSession& candidate, const FinalCliTerminalImport* final_import = nullptr)
	{
		const ChatSession* live = ChatDomainService().FindChatById(app, candidate.id);
		if (live == nullptr)
			return final_import == nullptr;
		if (final_import != nullptr)
		{
			const auto* verified = VerifiedFinalCliTerminalChat(app, *final_import->terminal, DefaultNativeHistoryProvider(app));
			if (verified != live || live->id != final_import->chat_id || live->metadata_incarnation != final_import->incarnation || final_import->terminal->terminal_id != final_import->terminal_id || final_import->terminal->startup_time_s != final_import->startup_time_s || chat_identity::NativeIdentityKeyForHistoryImport(candidate) != final_import->native_key)
				return false;
		}
		if (chat_identity::NativeIdentityKeyForHistoryImport(*live) != chat_identity::NativeIdentityKeyForHistoryImport(candidate) || uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(app, *live) || (final_import == nullptr && uam::ChatHasActiveCliTerminal(app, live->id)))
			return false;
		auto persisted = ChatRepository::LoadLocalChat(app.data_root, live->id, live->messages_loaded);
		if (!persisted || (live->messages_loaded && live->messages != persisted->messages))
			return false;
		// Cache bookkeeping can lag a successful loaded save; authored messages and metadata cannot.
		if (live->messages_loaded)
		{
			persisted->persisted_message_count = live->persisted_message_count;
			persisted->persisted_messages_digest = live->persisted_messages_digest;
		}
		// Sidebar normalization names an unparented native root by its UAM owner ID.
		// Align only that exact self-root alias on the detached saved comparison;
		// parent, foreign-root and branch-point edits must still defer publication.
		if (persisted->id == live->id && !persisted->native_session_id.empty() && persisted->native_session_id == live->native_session_id && chat_identity::NativeIdentityKeyForHistoryImport(*persisted) == chat_identity::NativeIdentityKeyForHistoryImport(*live) && persisted->parent_chat_id.empty() && live->parent_chat_id.empty() && persisted->branch_from_message_index == -1 && live->branch_from_message_index == -1 && persisted->branch_root_chat_id == persisted->native_session_id && live->branch_root_chat_id == live->id)
		{
			persisted->branch_root_chat_id = persisted->id;
		}
		const std::string metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
		return !metadata.empty() && metadata == ChatRepository::ChatMetadataFingerprint(app.data_root, *persisted);
	}
	using TerminalMediaWork = uam::TerminalNativeMediaWork;
	using TerminalMediaPhase = TerminalMediaWork::Phase;

	void TerminalMediaError(uam::AppState& app, std::string_view target, std::string_view error)
	{
		if (!error.empty())
		{
			app.native_chat_refresh_error_target_id = target;
			app.native_chat_refresh_error_status = "Native chat refresh failed: " + std::string(error);
			app.status_line = app.native_chat_refresh_error_status;
		}
		else if (app.native_chat_refresh_error_target_id == target)
		{
			if (app.status_line == app.native_chat_refresh_error_status) app.status_line.clear();
			app.native_chat_refresh_error_target_id.clear();
			app.native_chat_refresh_error_status.clear();
		}
	}

	std::string TerminalMediaMessageDigest(const ChatSession& chat)
	{
		std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;
		for (const auto& message : chat.messages) uam::chat_pages::AppendMessageDigest(hash, message);
		return uam::chat_pages::FinishMessageDigest(hash, chat.messages.size());
	}

	std::string TerminalMediaProtectedMetadata(std::string_view metadata)
	{
		// The repository fingerprint is metadata-only canonical JSON. Everything
		// except these explicit non-authoring fields remains part of the receipt.
		auto value = nlohmann::json::parse(metadata, nullptr, false);
		if (!value.is_object()) return {};
		for (const char* key : {"model_id", "reasoning_effort", "service_tier", "service_tier_explicit", "updated_at",
		                       "last_opened_at", "interaction_at", "attention_revision", "settled_at", "snoozed_until_ms",
		                       "title", "pinned", "custom_icon", "folder_id"})
			value.erase(key);
		return value.dump();
	}

	bool TerminalMediaStillOwned(const uam::AppState& app, const TerminalMediaWork& work, bool publication_locked = false)
	{
		if (work.revoked.load(std::memory_order_acquire) || work.cancellation.stop_requested() || app.data_root != work.root ||
		    (work.producer && work.producer->revoked.load(std::memory_order_acquire))) return false;
		const ChatSession* live = ChatDomainService().FindChatById(app, work.expected.id);
		if (!live || live->metadata_incarnation != work.expected.metadata_incarnation ||
		    live->imported_read_only || live->native_session_reset_pending ||
		    live->execution_host_id != uam::execution_hosts::kLocalHostId || !uam::paths::IsControllerLocalWorkspace(*live) ||
		    live->provider_id != uam::provider_ids::kGeminiCli ||
		    chat_identity::NativeIdentityKeyForHistoryImport(*live) != work.native_key ||
		    chat_identity::NativeWorkspaceForHistoryImport(*live) != chat_identity::NativeWorkspaceForHistoryImport(work.expected) ||
		    live->messages_loaded != work.expected.messages_loaded ||
		    (live->messages_loaded && TerminalMediaMessageDigest(*live) != work.message_digest) ||
		    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(app, *live) ||
		    ChatRepository::ChatMetadataFingerprint(app.data_root, *live) != work.metadata ||
		    ChatRepository::ChatStorageRevision(app.data_root, live->id) != work.revision ||
		    ChatRepository::ChatSummaryStorageRevision(app.data_root, live->id) != work.summary_revision) return false;
		const auto resolved = app.resolved_native_sessions_by_chat_id.find(live->id);
		if (resolved != app.resolved_native_sessions_by_chat_id.end() && !TrimmedIdMatches(resolved->second, live->native_session_id)) return false;
		const auto index = LoadNativeImportIndex(app, publication_locked ? &app.chats : nullptr);
		const auto owner = index.existing_id_by_native_key.find(work.native_key);
		if (!index.tombstones_available || index.tombstoned_native_keys.contains(work.native_key) ||
		    owner == index.existing_id_by_native_key.end() || owner->second != live->id ||
		    std::ranges::count_if(app.chats, [&](const auto& chat) { return chat.id == live->id || chat_identity::NativeIdentityKeyForHistoryImport(chat) == work.native_key; }) != 1) return false;
		if (!work.final) return !uam::ChatHasActiveCliTerminal(app, live->id);
		bool current_launch = false;
		for (const auto& terminal : app.cli_terminals)
		{
			if (!terminal) continue;
			if (terminal->native_history_generation == work.producer)
			{
				current_launch = true;
				// Cleared attachments are permitted only after actual process stop.
				if (work.producer->stopped.load(std::memory_order_acquire)) continue;
				if (VerifiedFinalCliTerminalChat(app, *terminal, work.profile) != live) return false;
			}
			else if (terminal->running)
			{
				if (uam::CliTerminalMatchesChatId(*terminal, live->id)) return false;
				const ChatSession* other_owner = ChatDomainService().FindChatById(app, uam::CliTerminalAttachedChatId(*terminal));
				if (other_owner && chat_identity::NativeIdentityKeyForHistoryImport(*other_owner) == work.native_key) return false;
			}
		}
		return work.producer && (current_launch || work.producer->stopped.load(std::memory_order_acquire));
	}

	std::shared_ptr<TerminalMediaWork> ReserveTerminalMediaWork(
	    const std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State>& snapshot_owner)
	{
		if (snapshot_owner && snapshot_owner->worker_retirement_owned)
		{
			if (!snapshot_owner->completed.load(std::memory_order_acquire) || snapshot_owner->retained_processing_payload) return {};
			auto payload = std::make_shared<TerminalMediaWork>();
			snapshot_owner->retained_processing_payload = payload;
			// No back-reference inside payload: State -> payload; alias keeps only
			// State alive, preserving its one already-admitted lifetime quota slot.
			return std::shared_ptr<TerminalMediaWork>(snapshot_owner, payload.get());
		}
		return uam::native_history::TryCreateWorkerOwnedNativeRefresh<TerminalMediaWork>();
	}

	bool CaptureTerminalMediaOwner(uam::AppState& app, TerminalMediaWork& work, const ChatSession& live)
	{
		if (live.provider_id != uam::provider_ids::kGeminiCli || live.execution_host_id != uam::execution_hosts::kLocalHostId ||
		    !uam::paths::IsControllerLocalWorkspace(live) || !app.data_root.is_absolute() ||
		    live.imported_read_only || live.native_session_reset_pending || !live.metadata_incarnation ||
		    live.native_session_id.empty() || !uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(live)).is_absolute()) return false;
		const auto revision = ChatRepository::ChatStorageRevision(app.data_root, live.id);
		const auto summary_revision = ChatRepository::ChatSummaryStorageRevision(app.data_root, live.id);
		if (!revision || *revision == "missing" || !summary_revision) return false;
		work.root = app.data_root;
		// Capture only small immutable identity/cache facts here. The worker reads
		// the complete saved baseline under this captured primary revision.
		work.expected.id = live.id;
		work.expected.provider_id = live.provider_id;
		work.expected.native_session_id = live.native_session_id;
		work.expected.execution_host_id = live.execution_host_id;
		work.expected.workspace_directory = live.workspace_directory;
		work.expected.workspace_worktree_directory = live.workspace_worktree_directory;
		work.expected.metadata_incarnation = live.metadata_incarnation;
		work.expected.created_at = live.created_at;
		work.expected.messages_loaded = live.messages_loaded;
		work.expected.persisted_message_count = live.persisted_message_count;
		work.expected.persisted_messages_digest = live.persisted_messages_digest;
		work.expected.parent_chat_id = live.parent_chat_id;
		work.expected.branch_root_chat_id = live.branch_root_chat_id;
		work.expected.branch_from_message_index = live.branch_from_message_index;
		work.message_digest = live.messages_loaded ? TerminalMediaMessageDigest(live) : std::string{};
		work.metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, live);
		work.protected_metadata = TerminalMediaProtectedMetadata(work.metadata);
		work.conversation_updated_at = live.updated_at;
		work.summary_revision = *summary_revision;
		work.revision = *revision;
		work.native_key = chat_identity::NativeIdentityKeyForHistoryImport(live);
		work.selected_before = ChatDomainService().SelectedChatId(app);
		return !work.metadata.empty() && !work.protected_metadata.empty();
	}

	std::string TerminalMediaColdSaveComparison(std::string_view protected_metadata)
	{
		auto value = nlohmann::json::parse(protected_metadata, nullptr, false);
		if (!value.is_object()) return {};
		// Refusal only: hydration/cache projection changes grant no original
		// message witness and never authorize primary-revision rebasing.
		for (const char* key : {"validationMessagesLoaded", "validationPersistedCount", "validationPersistedDigest"})
			value.erase(key);
		return value.dump();
	}

	bool TerminalMediaColdMetadataSavePending(const uam::AppState& app, const ChatSession& chat)
	{
		if (!app.terminal_native_media || chat.imported_read_only || chat.native_session_reset_pending ||
		    chat.provider_id != uam::provider_ids::kGeminiCli || chat.execution_host_id != uam::execution_hosts::kLocalHostId)
			return false;
		const auto metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, chat);
		const auto comparison = TerminalMediaColdSaveComparison(TerminalMediaProtectedMetadata(metadata));
		if (metadata.empty() || comparison.empty()) return false;
		return std::ranges::any_of(app.terminal_native_media->jobs, [&](const auto& work)
		{
			return work->final && !work->published && !work->expected.messages_loaded &&
			    !work->revoked.load(std::memory_order_acquire) && !work->cancellation.stop_requested() &&
			    work->producer && !work->producer->revoked.load(std::memory_order_acquire) &&
			    work->root == app.data_root && work->expected.id == chat.id &&
			    work->expected.metadata_incarnation == chat.metadata_incarnation &&
			    work->expected.created_at == chat.created_at &&
			    work->native_key == chat_identity::NativeIdentityKeyForHistoryImport(chat) &&
			    chat_identity::NativeWorkspaceForHistoryImport(work->expected) == chat_identity::NativeWorkspaceForHistoryImport(chat) &&
			    metadata != work->metadata && comparison == TerminalMediaColdSaveComparison(work->protected_metadata) &&
			    ChatRepository::ChatStorageRevision(app.data_root, chat.id) == work->revision;
		});
	}

	std::shared_ptr<TerminalMediaWork> RecaptureTerminalMediaMetadata(
	    uam::AppState& app, const TerminalMediaWork& previous)
	{
		// Recapture is exclusively final-history data reconciliation. It grants no
		// process/input authority and cannot change the original authored baseline.
		if (!previous.final || previous.published || previous.revoked.load(std::memory_order_acquire) ||
		    !previous.metadata_rebases_remaining || app.data_root != previous.root) return {};
		const ChatSession* live = ChatDomainService().FindChatById(app, previous.expected.id);
		if (!live) return {};
		auto next = std::make_shared<TerminalMediaWork>(); // Lightweight receipt only.
		if (!CaptureTerminalMediaOwner(app, *next, *live) || next->root != previous.root ||
		    next->native_key != previous.native_key ||
		    next->expected.metadata_incarnation != previous.expected.metadata_incarnation ||
		    next->expected.created_at != previous.expected.created_at ||
		    next->message_digest != previous.message_digest ||
		    next->protected_metadata != previous.protected_metadata ||
		    // Cold owners have only cached transcript metadata, not an actual
		    // original message witness. Never rebase their primary revision.
		    (!previous.expected.messages_loaded && next->revision != previous.revision) ||
		    (next->metadata == previous.metadata && next->revision == previous.revision &&
		     next->summary_revision == previous.summary_revision)) return {};
		next->conversation_updated_at = previous.conversation_updated_at;
		next->chats_dir = previous.chats_dir;
		next->profile = previous.profile;
		next->digest = previous.digest;
		next->selected_before = previous.selected_before;
		next->preserve_selection = previous.preserve_selection;
		next->final = true;
		next->wait_for_stop = previous.wait_for_stop;
		next->producer = previous.producer;
		next->generation = previous.generation;
		next->metadata_rebases_remaining = previous.metadata_rebases_remaining - 1;
		if (!TerminalMediaStillOwned(app, *next)) return {};
		return next;
	}

	void StartTerminalMediaPreparation(const std::shared_ptr<TerminalMediaWork>& work)
	{
		work->phase.store(TerminalMediaPhase::Preparing, std::memory_order_release);
		std::thread worker;
		try
		{
			worker = std::thread([work]
			{
				try
				{
					const auto stop = work->cancellation.get_token();
					if (work->final) work->inputs = ChatHistorySyncService().LoadNativeSessionChats(work->chats_dir, work->profile, stop);
					if (!stop.stop_requested())
					{
						ChatSession* source = nullptr;
						for (auto& candidate : work->inputs)
						{
							if (candidate.workspace_directory.empty() && work->final) candidate.workspace_directory = chat_identity::NativeWorkspaceForHistoryImport(work->expected);
							if (chat_identity::NativeIdentityKeyForHistoryImport(candidate) != work->native_key) continue;
							if (source) { work->error = "Native history has ambiguous target ownership."; source = nullptr; break; }
							source = &candidate;
						}
						// The legacy vector reader cannot distinguish absent/empty from
						// filtered invalid sources. Preserve the old safe non-import
						// disposition, never claim content/healthy-source acknowledgement.
						if (!source && work->error.empty()) work->no_importable_content = true;
						if (source && work->error.empty())
						{
							if (!source->messages_loaded || source->imported_read_only || source->native_session_reset_pending)
								work->error = "The owned native history source is incomplete.";
							else if (!uam::HasImportedConversationContent(source->messages)) work->no_importable_content = true;
							else
							{
								if (ChatRepository::ChatSummaryStorageRevision(work->root, work->expected.id) != work->summary_revision)
									throw std::runtime_error("Saved presentation changed before native preparation.");
								auto baseline = ChatRepository::LoadLocalChat(work->root, work->expected.id, true);
								if (!baseline || ChatRepository::ChatStorageRevision(work->root, work->expected.id) != work->revision ||
								    ChatRepository::ChatSummaryStorageRevision(work->root, work->expected.id) != work->summary_revision)
									throw std::runtime_error("Saved history changed before native preparation.");
								ChatSession comparison = *baseline;
								if (work->expected.messages_loaded)
								{
									if (TerminalMediaMessageDigest(comparison) != work->message_digest) throw std::runtime_error("Unsaved history changed.");
									comparison.persisted_message_count = work->expected.persisted_message_count;
									comparison.persisted_messages_digest = work->expected.persisted_messages_digest;
								}
								else UnloadPersistedTranscript(comparison);
								if (comparison.parent_chat_id.empty() && work->expected.parent_chat_id.empty() &&
								    comparison.branch_from_message_index == -1 && work->expected.branch_from_message_index == -1 &&
								    comparison.branch_root_chat_id == comparison.native_session_id && work->expected.branch_root_chat_id == work->expected.id)
									comparison.branch_root_chat_id = work->expected.id;
								if (ChatRepository::ChatMetadataFingerprint(work->root, comparison) != work->metadata)
									throw std::runtime_error("Saved metadata changed.");
								baseline->branch_root_chat_id = comparison.branch_root_chat_id;
								source->id = work->expected.id;
								DiscoveredGeminiOriginalReads reads;
								(void)ReconcileDiscoveredLocalGeminiMedia(work->root, *baseline, *source, reads, stop,
								    work->final ? std::optional<std::string_view>(work->conversation_updated_at) : std::nullopt);
								OverlayLocalChatState(*baseline, *source);
								// Summary overlays are durable UAM metadata, not native history.
								source->settled_at = baseline->settled_at;
								source->snoozed_until_ms = baseline->snoozed_until_ms;
								source->custom_icon = baseline->custom_icon;
								source->updated_at = SelectHistoryMetadataTimestamp(baseline->updated_at, source->updated_at);
								source->metadata_incarnation = work->expected.metadata_incarnation;
								if (chat_identity::NativeIdentityKeyForHistoryImport(*source) != work->native_key)
									work->error = "Native history ownership changed during preparation.";
								else
									work->prepared = uam::PreparedNativeMediaChatSave::Prepare(work->root, std::move(*source), work->revision, work->error, stop);
								if (work->prepared)
								{
									work->projection = work->prepared->Candidate();
									if (!work->expected.messages_loaded) UnloadPersistedTranscript(work->projection);
								}
							}
						}
					}
					if (stop.stop_requested() && work->error.empty()) work->error = "Native history refresh was cancelled.";
				}
				catch (...) { work->error = "Native history preparation failed; saved history was not acknowledged."; }
				work->phase.store(TerminalMediaPhase::Prepared, std::memory_order_release);
			});
			worker.detach();
		}
		catch (...)
		{
			if (worker.joinable()) { worker.join(); return; }
			work->error = "Could not start native history preparation.";
			work->phase.store(TerminalMediaPhase::Prepared, std::memory_order_release);
		}
	}

	void FinishTerminalMediaPublication(const std::shared_ptr<TerminalMediaWork>& work)
	{
		work->phase.store(TerminalMediaPhase::Finishing, std::memory_order_release);
		std::thread worker;
		try
		{
			worker = std::thread([work]
			{
				try
				{
					std::string acknowledgement_error;
					work->acknowledged = work->prepared->AcknowledgeOriginals(acknowledgement_error);
					if (!work->acknowledged && work->error.empty()) work->error = acknowledgement_error.empty() ? "Native original acknowledgement failed." : acknowledgement_error;
#if defined(__APPLE__)
					uam::io::AtomicWriteResult synced;
					const bool durable = uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatsRootPath(work->root), synced) &&
					    uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatSummariesRootPath(work->root), synced) && uam::io::SyncAtomicWriteDirectory(work->root, synced);
					if (!durable && work->error.empty()) work->error = "Native history was published but directory durability could not be confirmed.";
#endif
				}
				catch (...) { work->error = "Native history was published but original acknowledgement failed."; }
				work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
			});
			worker.detach();
		}
		catch (...)
		{
			if (worker.joinable()) { worker.join(); return; }
			work->error = "Native history was published but durability completion could not start.";
			work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
		}
	}

	void MoveTerminalMediaReceipt(TerminalMediaWork& from, TerminalMediaWork& to)
	{
		to.armed = true;
		to.root = std::move(from.root);
		to.chats_dir = std::move(from.chats_dir);
		to.profile = std::move(from.profile);
		to.expected = std::move(from.expected);
		to.metadata = std::move(from.metadata);
		to.protected_metadata = std::move(from.protected_metadata);
		to.conversation_updated_at = std::move(from.conversation_updated_at);
		to.summary_revision = std::move(from.summary_revision);
		to.metadata_rebases_remaining = from.metadata_rebases_remaining;
		to.message_digest = std::move(from.message_digest);
		to.revision = std::move(from.revision);
		to.native_key = std::move(from.native_key);
		to.digest = std::move(from.digest);
		to.selected_before = std::move(from.selected_before);
		to.preserve_selection = from.preserve_selection;
		to.final = from.final;
		to.wait_for_stop = from.wait_for_stop;
		to.producer = std::move(from.producer);
		to.generation = from.generation;
	}

} // namespace

using namespace uam::runtime_orch_impl;

bool ChatHistorySyncService::BuildNativeTranscriptCandidate(ChatSession& candidate, std::vector<Message> messages) const
{
	if (!candidate.messages_loaded || messages.size() < candidate.messages.size())
		return false;
	// Refuse before moving any canonical state out of the caller's candidate.
	if (!NativeReplacementPreservesAuthoredInputFacts(candidate.messages, messages))
		return false;
	ChatSession local;
	local.messages = std::move(candidate.messages);
	candidate.messages = std::move(messages);
	candidate.messages_loaded = true;
	candidate.persisted_message_count = candidate.messages.size();
	PreserveEquivalentLocalMessageDetails(local, candidate);
	if (!candidate.messages.empty())
	{
		candidate.updated_at = SelectHistoryMetadataTimestamp(candidate.updated_at, candidate.messages.back().created_at);
	}
	return true;
}

bool ChatHistorySyncService::SaveNativeTranscript(uam::AppState& app, ChatSession& current, std::vector<Message> messages) const
{
	if (ChatNeedsTranscriptPreserved(app, current) || messages.size() < EffectiveMessageCount(current))
		return true;
	ChatSession candidate = current;
	if (!ChatRepository::HydrateChatMessages(app.data_root, candidate))
		return false;
	if (!BuildNativeTranscriptCandidate(candidate, std::move(messages)))
		return true;
	if (!ChatRepository::SaveChat(app.data_root, candidate, true)) return false;
	current = std::move(candidate);
	return true;
}

bool ChatHistorySyncService::SaveChatWithStatus(uam::AppState& app, ChatSession& chat, const std::string& success, const std::string& failure) const
{
	if (uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, chat))
	{
		app.status_line = "Wait for the saved request to be confirmed before changing this chat.";
		return false;
	}
	if (TerminalMediaColdMetadataSavePending(app, chat))
	{
		// Model/pin/title handlers restore their attempted metadata on false.
		// Keep the original cold primary intact until final import has its actual
		// message witness; a later explicit retry saves against completed history.
		app.status_line = "Final terminal history is being saved. Retry changing this chat after it finishes.";
		return false;
	}
	// The mutable cold owner must acknowledge the serializer's actual history metadata.
	// Keep the same prepared-save/backup path as SaveChat, without trusting a later read.
	const bool saved = !chat.imported_read_only && !chat.messages_loaded && chat.persisted_message_count > 0
	    ? ChatRepository::PublishPreparedChatSave(ChatRepository::PrepareChatSave(app.data_root, chat), true, &chat)
	    : ChatRepository::SaveChat(app.data_root, chat);
	if (saved)
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

	const bool final_save_deferred = TerminalMediaColdMetadataSavePending(app, chat);
	if (SaveChatWithStatus(app, chat, "Chat title updated.", "Chat title changed in UI, but failed to save."))
	{
		return true;
	}

	chat.title = previous_title;
	chat.updated_at = previous_updated_at;
	if (!final_save_deferred)
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
		std::vector<ChatSession> chats = ChatDomainService().DeduplicateChatsById(LoadGeminiJsonHistoryForRuntime(chats_dir, provider, options, stop_token));
		// Native-profile loads already attest their workspace; only custom legacy
		// callers without that witness may fall back to the adjacent marker.
		if (std::ranges::any_of(chats, [](const ChatSession& chat) { return uam::strings::IsBlank(chat.workspace_directory); }))
		{
			const fs::path source_path = chats_dir.parent_path() / ".project_root";
			const std::optional<std::uintmax_t> source_size = uam::paths::FileSizeNoThrow(source_path);
			const std::string workspace = source_size && *source_size <= 65536 ? uam::strings::Trim(uam::io::ReadTextFile(source_path)) : std::string{};
			if (!workspace.empty())
				for (ChatSession& chat : chats)
					if (uam::strings::IsBlank(chat.workspace_directory))
						chat.workspace_directory = workspace;
		}
		return chats;
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
    std::vector<std::string>& added_keys,
    const std::unordered_set<std::string>& retained_keys) const
{
	added_keys.clear();
	std::unordered_set<std::string> candidate_keys;
	for (const ChatSession& chat : chats)
	{
		if (!uam::strings::IsBlank(chat.native_session_id))
		{
			const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
			if (!retained_keys.contains(key)) candidate_keys.insert(key);
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

std::optional<std::unordered_set<std::string>> ChatHistorySyncService::NativeImportTombstoneKeys(const fs::path& data_root) const
{
	NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
	if (!tombstones.available) return std::nullopt;
	return std::move(tombstones.keys);
}

bool ChatHistorySyncService::FilterDeletedNativeHistory(const fs::path& data_root, std::vector<ChatSession>& chats, std::string* error_out) const
{
	const NativeImportTombstones tombstones = LoadNativeImportTombstones(data_root);
	if (!tombstones.available)
	{
		if (error_out != nullptr) *error_out = "Deletion records could not be read; native history refresh stopped.";
		return false;
	}
	if (!tombstones.keys.empty() && std::ranges::any_of(chats, [](const ChatSession& chat)
	    { return !uam::strings::IsBlank(chat.native_session_id) && chat_identity::NativeWorkspaceForHistoryImport(chat).empty(); }))
	{
		if (error_out != nullptr) *error_out = "Native history has no workspace scope; refresh stopped to preserve deleted chats.";
		return false;
	}
	std::erase_if(chats, [&](const ChatSession& chat)
	{
		return tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(chat));
	});
	return true;
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

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportAllNativeChatsToLocal(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id, const uam::CliTerminalState* final_terminal) const
{
	ImportResult result;
	const std::string target_id = uam::strings::Trim(target_chat_id);
	const ProviderProfile& native_provider = DefaultNativeHistoryProvider(app);
	std::optional<FinalCliTerminalImport> final_import;
	if (final_terminal != nullptr)
	{
		const ChatSession* owner = VerifiedFinalCliTerminalChat(app, *final_terminal, native_provider);
		if (owner == nullptr || delete_native_after_import || !uam::ChatSyncIdsMatch(target_id, final_terminal->attached_session_id))
		{
			result.Fail("Final terminal history ownership could not be verified.");
			return result;
		}
		final_import = FinalCliTerminalImport{final_terminal, owner->id, chat_identity::NativeIdentityKeyForHistoryImport(*owner), owner->metadata_incarnation, uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(*owner)), final_terminal->terminal_id, final_terminal->startup_time_s};
	}
	NativeImportIndex import_index = LoadNativeImportIndex(app);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; import stopped to avoid restoring deleted chats.");
		return result;
	}

	const auto workspace_roots = final_import ? std::vector<fs::path>{final_import->workspace} : CollectWorkspaceRootsForNativeHistory(app);
	for (const fs::path& workspace_root : workspace_roots)
	{
		const auto chats_dir = ResolveNativeHistoryChatsDirForWorkspace(workspace_root);
		if (!chats_dir)
		{
			continue;
		}

		std::vector<ChatSession> native_chats = LoadNativeSessionChats(*chats_dir, native_provider);
		for (ChatSession& native_chat : native_chats)
			if (uam::strings::IsBlank(native_chat.workspace_directory))
				native_chat.workspace_directory = uam::paths::Utf8PathString(workspace_root);
		// Related lookup does not change native ownership. Registered parent or
		// worktree folders may group the chat while its working directory survives.
		std::erase_if(native_chats, [&](const ChatSession& chat) { return (final_import && chat_identity::NativeIdentityKeyForHistoryImport(chat) != final_import->native_key) || !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(chat)), workspace_root); });
		std::string overlay_error;
		if (!OverlayPartialNativeImport(*this, app, native_chats, overlay_error))
		{
			result.Fail(std::move(overlay_error));
			continue;
		}

		for (ChatSession& native_chat : native_chats)
		{
			if (!NativeChatShouldBeImported(*chats_dir, native_chat, target_id, delete_native_after_import))
			{
				continue;
			}

			++result.total_count;

			const std::string attested_key = chat_identity::NativeIdentityKeyForHistoryImport(native_chat);
			AssignKnownWorkspaceFolderToNewImport(app, import_index, uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(native_chat)), native_chat);

			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, native_chat, target_id);
			if (!native_key)
			{
				continue;
			}

			if (*native_key != attested_key || chat_identity::NativeIdentityKeyForHistoryImport(native_chat) != attested_key)
			{
				result.Fail("Native history ownership changed before local import publication.");
				continue;
			}
			if (!PartialNativeImportPreservesLiveWork(app, native_chat, final_import ? &*final_import : nullptr))
				continue;

			if (SaveImportedNativeChat(app, import_index, native_chat, *native_key, *chats_dir, delete_native_after_import))
			{
				++result.imported_count;
				result.published_chat_ids.push_back(native_chat.id);
			}
			else
			{
				result.Fail("Failed to save imported " + native_provider.title + " chat " + native_chat.native_session_id + ".");
			}
		}
	}

	return result;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportLoadedNativeChatsToLocal(uam::AppState& app, std::vector<ChatSession> native_chats, const std::string& target_chat_id) const
{
	ImportResult result;
	const std::string target_id = uam::strings::Trim(target_chat_id);
	if (target_id.empty())
	{
		result.Fail({}); // An unsettled attachment cannot acknowledge a snapshot by ID alone.
		return result;
	}
	NativeImportIndex import_index = LoadNativeImportIndex(app);
	std::string deletion_error;
	if (!import_index.tombstones_available || !FilterDeletedNativeHistory(app.data_root, native_chats, &deletion_error))
	{
		result.Fail(deletion_error.empty() ? "Deletion records could not be read; native history refresh stopped." : std::move(deletion_error));
		return result;
	}

	const ChatSession* requested_owner = ChatDomainService().FindChatById(app, target_id);
	if (requested_owner != nullptr && (requested_owner->imported_read_only || requested_owner->native_session_reset_pending || uam::strings::IsBlank(requested_owner->native_session_id)))
	{
		result.Fail({});
		return result;
	}
	std::vector<ChatSession> candidates;
	for (ChatSession& chat : native_chats)
	{
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
		const auto saved_owner = import_index.existing_id_by_native_key.find(key);
		if (!NativeChatMatchesImportTarget(chat, target_id) && (saved_owner == import_index.existing_id_by_native_key.end() || saved_owner->second != target_id) && (requested_owner == nullptr || chat_identity::NativeIdentityKeyForHistoryImport(*requested_owner) != key))
			continue;
		if (requested_owner != nullptr && chat_identity::NativeIdentityKeyForHistoryImport(*requested_owner) != key)
		{
			result.Fail("Loaded native history does not match the requested chat's ownership.");
			return result;
		}
		candidates.push_back(std::move(chat));
	}
	// A successful complete snapshot can dispose an absent or tombstoned target.
	if (candidates.empty())
		return result;
	if (candidates.size() != 1)
	{
		result.Fail("Loaded native history has ambiguous target ownership.");
		return result;
	}
	ChatSession& candidate = candidates.front();
	++result.total_count;
	if (!candidate.messages_loaded || candidate.imported_read_only || candidate.native_session_reset_pending || uam::strings::IsBlank(candidate.id) || uam::strings::IsBlank(candidate.provider_id) || uam::strings::IsBlank(candidate.native_session_id))
	{
		result.Fail({});
		return result;
	}
	if (!NativeChatShouldBeImported({}, candidate, "", false))
	{
		if (!uam::HasImportedConversationContent(candidate.messages))
			result.confirmed_empty_native_keys.push_back(chat_identity::NativeIdentityKeyForHistoryImport(candidate));
		return result;
	}
	if (uam::paths::PathExistsNoThrow(app.data_root) && !uam::paths::IsDirectoryNoThrow(app.data_root))
	{
		result.Fail("could not save chat history: the data root is not a directory.");
		return result;
	}

	const std::string attested_key = chat_identity::NativeIdentityKeyForHistoryImport(candidate);
	const ChatSession* live_owner = nullptr;
	bool other_native_owner = false;
	for (const ChatSession& live : app.chats)
	{
		if (chat_identity::NativeIdentityKeyForHistoryImport(live) == attested_key)
		{
			if (live_owner != nullptr)
			{
				result.Fail({});
				return result;
			}
			live_owner = &live;
		}
		else if (TrimmedIdMatches(live.native_session_id, candidate.native_session_id))
			other_native_owner = true;
	}
	const auto saved_owner = import_index.existing_id_by_native_key.find(attested_key);
	const std::string expected_owner_id = live_owner != nullptr ? live_owner->id : saved_owner != import_index.existing_id_by_native_key.end() ? saved_owner->second : std::string{};
	if ((live_owner == nullptr && other_native_owner) || (live_owner != nullptr && (live_owner->imported_read_only || live_owner->native_session_reset_pending || saved_owner == import_index.existing_id_by_native_key.end() || saved_owner->second != live_owner->id)))
	{
		result.Fail({});
		return result;
	}
	if (live_owner != nullptr)
	{
		const auto resolved = app.resolved_native_sessions_by_chat_id.find(live_owner->id);
		if (resolved != app.resolved_native_sessions_by_chat_id.end() && !TrimmedIdMatches(resolved->second, candidate.native_session_id))
		{
			result.Fail({});
			return result;
		}
	}

	std::string overlay_error;
	if (!OverlayPartialNativeImport(*this, app, candidates, overlay_error))
	{
		result.Fail(std::move(overlay_error));
		return result;
	}
	if (candidates.empty())
		return result; // A newly recorded tombstone is also a safe disposition.
	ChatSession& prepared = candidates.front();
	if (prepared.imported_read_only || prepared.native_session_reset_pending)
	{
		result.Fail({});
		return result;
	}
	if (uam::paths::IsControllerLocalWorkspace(prepared) && !chat_identity::NativeWorkspaceForHistoryImport(prepared).empty())
		AssignKnownWorkspaceFolderToNewImport(app, import_index, uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(prepared)), prepared);
	const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, prepared, target_id);
	if (!native_key || *native_key != attested_key || chat_identity::NativeIdentityKeyForHistoryImport(prepared) != attested_key || (!expected_owner_id.empty() && prepared.id != expected_owner_id))
	{
		result.Fail("Loaded native history ownership changed before publication.");
		return result;
	}
	// Ordinary snapshots never receive the owned final-terminal bypass.
	if (!PartialNativeImportPreservesLiveWork(app, prepared))
	{
		result.Fail({});
		return result;
	}
	if (!SaveImportedNativeChat(app, import_index, prepared, *native_key, {}, false))
	{
		result.Fail("could not save chat history.");
		return result;
	}
	++result.imported_count;
	result.published_chat_ids.push_back(prepared.id);
	return result;
}

std::optional<ChatHistorySyncService::ImportResult> ChatHistorySyncService::QueueLoadedTerminalNativeMedia(
    uam::AppState& app, std::vector<ChatSession>& input, bool consume_input, std::string_view target,
    bool preserve_selection, const std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State>& snapshot_owner,
    std::string_view digest, const std::shared_ptr<uam::CliTerminalHistoryGeneration>& producer) const
{
	const std::string target_id = uam::NormalizeChatSyncTargetId(target);
	const auto index = LoadNativeImportIndex(app);
	const ChatSession* requested = ChatDomainService().FindChatById(app, target_id);
	const ChatSession* candidate = nullptr;
	bool ambiguous = false;
	bool target_pending_media = false;
	std::vector<const ChatSession*> selected_sources;
	bool pending_media = std::ranges::any_of(input, HasLocalGeminiPendingOriginals);
	for (const auto& source : input)
	{
		const auto key = chat_identity::NativeIdentityKeyForHistoryImport(source);
		const auto saved = index.existing_id_by_native_key.find(key);
		if (!NativeChatMatchesImportTarget(source, target_id) &&
		    (saved == index.existing_id_by_native_key.end() || saved->second != target_id) &&
		    (!requested || chat_identity::NativeIdentityKeyForHistoryImport(*requested) != key)) continue;
		selected_sources.push_back(&source);
		target_pending_media = target_pending_media || HasLocalGeminiPendingOriginals(source);
		if (candidate) ambiguous = true;
		candidate = &source;
	}
	if (!pending_media) return std::nullopt;
	if (!target_pending_media)
	{
		// Unrelated inline media stays in the caller's pre-admitted carrier. The
		// selected non-pending rows keep the existing synchronous disposition and
		// never inherit a different conversation's media authority or timing.
		std::vector<ChatSession> selected;
		for (const auto* source : selected_sources) selected.push_back(*source);
		return ImportLoadedNativeChatsToLocal(app, std::move(selected), target_id);
	}
	ImportResult pending;
	pending.Fail({});
	if (target_id.empty()) return pending;
	if (ambiguous || !candidate)
	{
		pending.Fail("Loaded native history has ambiguous target ownership.");
		return pending;
	}
	if (!index.tombstones_available) return ImportLoadedNativeChatsToLocal(app, {}, target_id);
	const auto key = chat_identity::NativeIdentityKeyForHistoryImport(*candidate);
	const auto saved = index.existing_id_by_native_key.find(key);
	if (index.tombstoned_native_keys.contains(key)) return ImportLoadedNativeChatsToLocal(app, {}, target_id);
	if (saved == index.existing_id_by_native_key.end())
	{
		pending.Fail("Native originals require an existing saved UAM owner before terminal refresh.");
		return pending;
	}
	const ChatSession* live = ChatDomainService().FindChatById(app, saved->second);
	if (!live || (requested && requested != live) ||
	    chat_identity::NativeIdentityKeyForHistoryImport(*live) != key || !PartialNativeImportPreservesLiveWork(app, *live)) return pending;
	if (snapshot_owner && (snapshot_owner->admission_generation != app.terminal_native_history_load_generation ||
	    (snapshot_owner->worker_retirement_owned && snapshot_owner->owner_root != app.data_root))) return pending;
	if (producer && producer->revoked.load(std::memory_order_acquire)) return pending;
	if (!app.terminal_native_media) app.terminal_native_media = std::make_shared<uam::TerminalNativeMediaCoordinator>();
	auto& coordinator = *app.terminal_native_media;
	if (coordinator.closing) return pending;
	// An ordinary parse can never supersede an admitted final capture. The
	// final publication also invalidates parses admitted during its stop window.
	if (std::ranges::any_of(coordinator.jobs, [&](const auto& prior) { return prior->final && prior->expected.id == live->id && !prior->revoked.load(std::memory_order_acquire); })) return pending;
	const std::string signature = digest.empty() ? uam::NativeHistorySnapshotDigest(input) : std::string(digest);
	for (const auto& prior : coordinator.jobs)
		if (!prior->revoked.load() && prior->expected.id == live->id && prior->digest == signature && !prior->final &&
		    prior->phase.load(std::memory_order_acquire) != TerminalMediaPhase::Finished) return pending;
	auto work = ReserveTerminalMediaWork(snapshot_owner);
	if (!work) return pending; // No source move/copy before bounded admission.
	work->armed = true;
	if (!CaptureTerminalMediaOwner(app, *work, *live)) return pending;
	work->producer = producer;
	work->digest = signature;
	work->preserve_selection = preserve_selection;
	if (!TerminalMediaStillOwned(app, *work)) return pending;
	work->generation = ++coordinator.next_generation;
	for (const auto& prior : coordinator.jobs)
		if (prior->expected.id == live->id && !prior->published)
		{
			prior->revoked.store(true, std::memory_order_release);
			prior->cancellation.request_stop();
		}
	if (consume_input) work->inputs = std::move(input);
	else work->inputs = input;
	coordinator.jobs.push_back(work);
	StartTerminalMediaPreparation(work);
	return pending;
}

std::optional<ChatHistorySyncService::ImportResult> ChatHistorySyncService::QueueFinalTerminalNativeMedia(
    uam::AppState& app, const uam::CliTerminalState& terminal, bool wait_for_stop) const
{
	const ChatSession* live = ChatDomainService().FindChatById(app, uam::CliTerminalAttachedChatId(terminal));
	if (!live || live->provider_id != uam::provider_ids::kGeminiCli || !uam::paths::IsControllerLocalWorkspace(*live)) return std::nullopt;
	// Local Gemini eligibility may decline for live work or an invalid final
	// witness. Retire its parser cohort without canceling a valid final receipt.
	uam::InvalidateTerminalNativeChatLoads(app);
	ImportResult pending;
	pending.Fail({});
	const ProviderProfile& profile = ProviderResolutionService().ProviderForChatOrDefault(app, *live);
	if (VerifiedFinalCliTerminalChat(app, terminal, profile) != live || !terminal.native_history_generation) return pending;
	const FinalCliTerminalImport final_import{&terminal, live->id, chat_identity::NativeIdentityKeyForHistoryImport(*live), live->metadata_incarnation,
	    uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(*live)), terminal.terminal_id, terminal.startup_time_s};
	if (!PartialNativeImportPreservesLiveWork(app, *live, &final_import))
	{
		ImportResult deferred; // Finished safe disposition, no work queued or digest granted.
		return deferred;
	}
	if (!app.terminal_native_media) app.terminal_native_media = std::make_shared<uam::TerminalNativeMediaCoordinator>();
	auto& coordinator = *app.terminal_native_media;
	if (coordinator.closing) return pending;
	for (const auto& prior : coordinator.jobs)
		if (!prior->revoked.load() && prior->final && prior->producer == terminal.native_history_generation &&
		    prior->wait_for_stop == wait_for_stop && prior->phase.load(std::memory_order_acquire) != TerminalMediaPhase::Finished) return pending;
	// This is a lightweight receipt only: no native source, copied transcript,
	// inline payload or prepared object is admitted until a worker slot exists.
	auto receipt = std::make_shared<TerminalMediaWork>();
	if (!CaptureTerminalMediaOwner(app, *receipt, *live)) return pending;
	receipt->profile = profile;
	receipt->chats_dir = ResolveNativeHistoryChatsDirForChat(app, *live);
	if (receipt->chats_dir.empty()) return pending;
	receipt->final = true;
	receipt->wait_for_stop = wait_for_stop;
	receipt->producer = terminal.native_history_generation;
	receipt->generation = ++coordinator.next_generation;
	// Loads ordered before final capture cannot later become a newer ordinary
	// request merely because their parser completed after this final request.
	// Periodic cohort was invalidated before all final eligibility exits.
	for (const auto& prior : coordinator.jobs)
		if (prior->expected.id == live->id && !prior->published)
		{
			prior->revoked.store(true, std::memory_order_release);
			prior->cancellation.request_stop();
		}
	coordinator.jobs.push_back(std::move(receipt));
	return pending;
}

void ChatHistorySyncService::CancelTerminalNativeMediaForChat(uam::AppState& app, std::string_view chat_id) const
{
	uam::InvalidateTerminalNativeChatLoads(app);
	if (!app.terminal_native_media) return;
	for (const auto& work : app.terminal_native_media->jobs)
		if (work->expected.id == chat_id && !work->published)
		{
			work->revoked.store(true, std::memory_order_release);
			work->cancellation.request_stop();
		}
}

bool ChatHistorySyncService::PollTerminalNativeMedia(uam::AppState& app) const
{
	// Also runs with no terminal rows or media coordinator.
	uam::SweepTerminalNativeChatLoads(app);
	if (!app.terminal_native_media) return false;
	auto& coordinator = *app.terminal_native_media;
	bool changed = false;
	for (auto& work : coordinator.jobs)
	{
		auto phase = work->phase.load(std::memory_order_acquire);
		if (work->revoked.load(std::memory_order_acquire) && !work->published) continue;
		if (phase == TerminalMediaPhase::Waiting)
		{
			// Process stop cannot advance inside exit settlement. Outside exit,
			// retain the receipt without repeatedly scanning the repository.
			if (work->wait_for_stop && (!work->producer || !work->producer->stopped.load(std::memory_order_acquire)))
			{
				if (coordinator.closing)
				{
					work->error = "Final native process stop was not confirmed; the native source was retained for retry.";
					work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
				}
				continue;
			}
			// Backpressure must not perform owner-index IO on every capacity poll.
			auto armed = ReserveTerminalMediaWork({});
			if (!armed) continue;
			if (!TerminalMediaStillOwned(app, *work))
			{
				auto replacement = RecaptureTerminalMediaMetadata(app, *work);
				if (!replacement)
				{
					work->error = "The chat or terminal owner changed; retry native history refresh.";
					work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
					continue;
				}
				work = std::move(replacement);
			}
			MoveTerminalMediaReceipt(*work, *armed);
			work = std::move(armed);
			StartTerminalMediaPreparation(work);
			continue;
		}
		if (phase == TerminalMediaPhase::Prepared)
		{
			if (!TerminalMediaStillOwned(app, *work))
			{
				if (auto replacement = RecaptureTerminalMediaMetadata(app, *work))
				{
					// Never edit or reuse a staged packet after its saved baseline has
					// changed. The old carrier retires prepared originals worker-side.
					work = std::move(replacement);
					continue;
				}
			}
			if (work->no_importable_content && work->error.empty() && TerminalMediaStillOwned(app, *work))
			{
				if (work->final) uam::InvalidateTerminalNativeChatLoads(app);
				work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
				continue; // Successful safe no-op, no primary/adoption/digest grant.
			}
			if (work->error.empty() && (!work->prepared || !TerminalMediaStillOwned(app, *work)))
				work->error = "The chat or native history owner changed before publication; retry to refresh it.";
			if (work->error.empty() && !work->prepared->Publish(work->error, [&] { return TerminalMediaStillOwned(app, *work, true); }, false))
				if (work->error.empty()) work->error = "Native history publication was refused.";
			if (!work->error.empty())
			{
				// A publication-lock guard may discover a newer safe metadata save.
				// Failed Publish has not acknowledged a primary; retry only a real
				// current receipt difference, never arbitrary preparation/IO failure.
				if ((!work->prepared || !work->prepared->PrimaryAcknowledged()))
					if (auto replacement = RecaptureTerminalMediaMetadata(app, *work))
					{
						work = std::move(replacement);
						continue;
					}
				work->phase.store(TerminalMediaPhase::Finished, std::memory_order_release);
				continue;
			}
			work->published = true;
			if (work->final)
			{
				uam::InvalidateTerminalNativeChatLoads(app);
			}
			work->projection.acp_queue_full_save_revision = work->prepared->Candidate().acp_queue_full_save_revision;
			if (ChatSession* live = ChatDomainService().FindChatById(app, work->expected.id))
				std::swap(*live, work->projection); // Old heavy history retires on the same worker owner.
			const auto selected = ChatDomainService().SelectedChatId(app);
			if (!uam::FinalizeChatSyncSelection(app, selected, work->expected.native_session_id,
			    work->preserve_selection || selected != work->selected_before)) work->error = "Native history was saved, but selected history could not be loaded.";
			if (work->error.empty()) TerminalMediaError(app, work->expected.native_session_id, {});
			changed = true;
			FinishTerminalMediaPublication(work);
			continue;
		}
		if (phase == TerminalMediaPhase::Finished)
		{
			const bool latest = std::ranges::none_of(coordinator.jobs, [&](const auto& other) { return other->expected.id == work->expected.id && other->generation > work->generation; });
			const ChatSession* live = ChatDomainService().FindChatById(app, work->expected.id);
			const bool current_owner = live && live->metadata_incarnation == work->expected.metadata_incarnation && chat_identity::NativeIdentityKeyForHistoryImport(*live) == work->native_key;
			if (latest && app.data_root == work->root && (!work->error.empty() || current_owner))
			{
				TerminalMediaError(app, work->expected.native_session_id, work->error);
				if (work->published && work->error.empty() && !work->digest.empty() && work->producer && !work->producer->revoked.load())
					for (auto& terminal : app.cli_terminals)
						if (terminal && terminal->native_history_generation == work->producer && uam::CliTerminalMatchesChatId(*terminal, live->id))
							terminal->last_native_history_snapshot_digest = work->digest;
				changed = true;
			}
			work->revoked.store(true, std::memory_order_release);
		}
	}
	std::erase_if(coordinator.jobs, [](const auto& work)
	{
		const auto phase = work->phase.load(std::memory_order_acquire);
		return work->revoked.load(std::memory_order_acquire) && phase != TerminalMediaPhase::Preparing && phase != TerminalMediaPhase::Finishing;
	});
	return changed;
}

bool ChatHistorySyncService::TerminalNativeMediaPending(const uam::AppState& app) const
{
	return app.terminal_native_media && !app.terminal_native_media->jobs.empty();
}

bool ChatHistorySyncService::TerminalNativeMediaFinalPending(const uam::AppState& app) const
{
	return app.terminal_native_media && std::ranges::any_of(app.terminal_native_media->jobs, [](const auto& work) { return work->final; });
}

void ChatHistorySyncService::SettleTerminalNativeMediaForExit(uam::AppState& app) const
{
	if (!app.terminal_native_media) return;
	app.terminal_native_media->closing = true;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!app.terminal_native_media->jobs.empty() && std::chrono::steady_clock::now() < deadline)
	{
		(void)PollTerminalNativeMedia(app);
		if (!app.terminal_native_media->jobs.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	for (const auto& work : app.terminal_native_media->jobs)
	{
		work->revoked.store(true, std::memory_order_release);
		work->cancellation.request_stop();
		TerminalMediaError(app, work->expected.native_session_id,
		    work->published ? "Native history was saved; final durability completion is still pending." :
		        "Final native history refresh did not complete before exit; the native source was retained for retry.");
	}
	// Workers own no AppState/CEF pointers. Acknowledged originals are never
	// discarded; cancellation retains prior primary and untouched native source.
	app.terminal_native_media->jobs.clear();
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
	NativeImportIndex import_index = LoadNativeImportIndex(app);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; Codex import stopped to avoid restoring deleted chats.");
		return result;
	}

	const std::unordered_map<std::string, std::string> session_names = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).ReadNativeSessionNames();
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
			std::optional<ChatSession> chat = LoadCodexRolloutChat(it->path(), folder, &malformed, false, {}, true);
			if (chat)
			{
				if (import_index.existing_id_by_native_key.contains(chat_identity::NativeIdentityKeyForHistoryImport(*chat)))
				{
					const auto owner = CanonicalCodexInputReceiptOwner(app.data_root, import_index, *chat);
					chat = LoadCodexRolloutChat(it->path(), folder, &malformed, false, {}, false, {}, owner ? &*owner : nullptr);
				}
				else
					// Preserve direct-import eligibility and counts without retaining the transcript.
					chat = LoadCodexRolloutChat(it->path(), folder, &malformed, false, {}, false, [](const Message&) { return true; });
			}
			if (malformed) result.Fail("Could not parse Codex history file " + uam::paths::Utf8PathString(it->path()) + ".");
			if (!chat)
			{
				continue;
			}
			if (const ChatFolder* owner = uam::FindImportedWorkspaceFolder(app.folders, uam::paths::PathFromUtf8(chat->workspace_directory)); owner != nullptr && owner->id != folder.id) continue;
			const std::unordered_map<std::string, std::string>::const_iterator name = session_names.find(chat->native_session_id);
			if (name != session_names.end() && !uam::IsInjectedChatTitle(name->second)) chat->title = name->second;
			++result.total_count;
			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, *chat, "");
			bool saved = false;
			if (native_key && !chat->messages_loaded)
			{
				const std::string_view title = name != session_names.end() && !uam::IsInjectedChatTitle(name->second) ? std::string_view(name->second) : std::string_view{};
				const std::shared_ptr<PreparedChatSave> prepared = PrepareCodexRolloutImport(app.data_root, it->path(), *chat, &malformed, title);
				saved = prepared && ChatRepository::PublishPreparedChatSave(prepared);
				if (saved) TrackImportedNativeChat(import_index, *chat, *native_key);
			}
			else if (native_key) saved = SaveImportedNativeChat(app, import_index, *chat, *native_key, it->path().parent_path(), false);
			if (saved)
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

ChatHistorySyncService::LocalHistoryDiscovery ChatHistorySyncService::DiscoverProviderChatsForFolder(const ChatFolder& folder, const ProviderProfile* open_code_profile, std::stop_token stop_token, std::size_t open_code_scan_offset, bool open_code_only, std::optional<std::size_t> open_code_remaining, const std::string& open_code_list_fingerprint, const std::string& provider_filter, const std::string& native_session_filter, const fs::path& native_history_root) const
{
	LocalHistoryDiscovery discovery;
	if (!uam::paths::IsControllerLocalWorkspace(folder) || (!folder.id.empty() && uam::strings::IsBlank(folder.directory)))
	{
		discovery.result.Fail("History scan requires a local workspace folder.");
		return discovery;
	}
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	if (!open_code_only && (provider_filter.empty() || provider_filter == uam::provider_ids::kAntigravityCli))
	{
		auto native = uam::antigravity::history::Discover(native_history_root, native_session_filter, stop_token, {},
		    folder.directory.empty() ? uam::antigravity::history::WorkspaceFilter{} : uam::antigravity::history::WorkspaceFilter{[&](const std::string& workspace)
		    { return uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(workspace), uam::paths::PathFromUtf8(folder.directory)); }});
		for (auto& error : native.errors) discovery.result.Fail(std::move(error));
		discovery.result.omitted_error_count += native.omitted_error_count;
		for (auto& chat : native.chats)
		{
			if (!folder.directory.empty() && !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(chat.workspace_directory), uam::paths::PathFromUtf8(folder.directory))) continue;
			chat.folder_id = folder.id;
			chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
			discovery.chats.push_back(std::move(chat));
		}
	}
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	if (!open_code_only && (provider_filter.empty() || provider_filter == uam::provider_ids::kCodexCli))
	{
	for (const fs::path& root : {uam::codex::CodexHomePath() / "sessions", uam::codex::CodexHomePath() / "archived_sessions"})
	{
		std::error_code error;
		if (!fs::exists(root, error) && !error) continue;
		if (error || !fs::is_directory(root, error))
		{
			discovery.result.Fail("Could not inspect Codex history: " + (error ? error.message() : "history path is not a directory."));
			continue;
		}
		for (fs::recursive_directory_iterator it(root, error), end; !error && !stop_token.stop_requested() && it != end; it.increment(error))
		{
			if (!uam::paths::IsRegularFileEntryNoThrow(*it) || it->path().extension() != ".jsonl") continue;
			if (!native_session_filter.empty() && it->path().stem().string() != native_session_filter && provider_filter == uam::provider_ids::kClaudeCli)
				continue;
			bool malformed = false;
			std::optional<ChatSession> chat = LoadCodexRolloutChat(it->path(), folder, &malformed, false, stop_token, true);
			if (malformed) discovery.result.Fail("Could not parse Codex history file " + uam::paths::Utf8PathString(it->path()) + " (unreadable or invalid).");
			if (chat) discovery.codex_rollouts.emplace_back(std::move(*chat), it->path());
		}
		if (error) discovery.result.Fail("Could not finish scanning Codex history: " + error.message());
	}
	}
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (!open_code_only && (provider_filter.empty() || provider_filter == uam::provider_ids::kCopilotCli))
	{
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();

	std::string error;
	std::vector<ChatSession> chats = LoadCopilotSessionStateChats(CopilotSessionStatePath(), folder.directory, options, &error, stop_token);
	if (!error.empty()) discovery.result.Fail(error);
	for (ChatSession& chat : chats)

	{
		chat.folder_id = folder.id;
		discovery.chats.push_back(std::move(chat));
	}
	}
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	if (!open_code_only && (provider_filter.empty() || provider_filter == uam::provider_ids::kGeminiCli))
	{
	const ProviderProfile gemini_profile = ProviderProfileStore::DefaultGeminiProfile();
	// Arm producer cleanup before parsing. Batch leases begin too late, and
	// startup selection may copy payload handles out of this discovery result.
	// Payload aliases carry this independent bounded owner through those copies.
	const auto context = std::make_shared<GeminiHistoryParseContext>();
	ProviderRuntimeHistoryLoadOptions options;
	options.native_max_file_bytes = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxFileBytes();
	options.native_max_messages = PlatformServicesFactory::Instance().process_service.NativeGeminiSessionMaxMessages();
	ProviderDiscoveryResult gemini_sources;
	gemini_sources.sources = DiscoverGeminiTmpChatSources(&gemini_sources.error, stop_token);
	if (!gemini_sources.error.empty()) discovery.result.Fail(gemini_sources.error);
	for (const auto& source : gemini_sources.sources)
	{
		if (!folder.directory.empty() && !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(source.folder_directory), uam::paths::PathFromUtf8(folder.directory))) continue;
		if (stop_token.stop_requested()) break;
		if (!context->payload_retirement)
		{
			context->payload_retirement = uam::native_history::TryCreateWorkerOwnedNativeRefresh<GeminiHistoryParseContext::RetainedPayloads, uam::native_history::NativeRefreshWorkerPool::DiscoveryParser>();
			if (!context->payload_retirement)
			{
				discovery.result.Fail("Gemini history discovery is busy; retry after the pending media scan is released.");
				break;
			}
		}
		for (ChatSession& chat : ChatDomainService().DeduplicateChatsById(LoadGeminiJsonHistoryForRuntime(source.chats_dir, gemini_profile, options, stop_token, context)))
		{
			if (!NativeChatShouldBeImported(source.chats_dir, chat, "", false)) continue;
			if (uam::strings::IsBlank(chat.workspace_directory))
				chat.workspace_directory = source.folder_directory;
			if (!uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(chat)), uam::paths::PathFromUtf8(source.folder_directory)))
			{
				discovery.result.Fail("Gemini history ownership does not match its discovered workspace.");
				continue;
			}
			chat.folder_id = folder.id;
			discovery.chats.push_back(std::move(chat));
		}
		const auto admission_error = context->RetirementError();
		if (!admission_error.empty())
		{
			discovery.result.Fail(admission_error);
			break;
		}
	}
	}
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	if (open_code_profile != nullptr && (provider_filter.empty() || provider_filter == uam::provider_ids::kOpenCodeCli))
	{
		uam::ProviderWorkerInvocation invocation;
		invocation.direct_process = true;
		invocation.structured_stdout = true;
		invocation.argv = ProviderRuntimeRegistry::Resolve(*open_code_profile).BuildLocalNativeDiscoveryArgv(*open_code_profile);
		const auto count_option = std::ranges::find(invocation.argv, "--max-count");
		if (count_option != invocation.argv.end() && std::next(count_option) != invocation.argv.end()) invocation.argv.erase(count_option, std::next(count_option, 2));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
		const auto listed = uam::ExecuteProviderWorkerInvocation(invocation, (folder.directory.empty() ? uam::paths::CurrentPathOrDot() : uam::paths::PathFromUtf8(folder.directory)), uam::opencode_v2::HistoryCommandTimeoutMilliseconds(invocation.argv), stop_token);
		nlohmann::json sessions = nlohmann::json::parse(listed.output, nullptr, false);
		if (!listed.ok || listed.exit_code != 0 || listed.output_truncated || !sessions.is_array())
		{
			discovery.result.Fail("OpenCode history discovery failed: " + uam::strings::NonEmptyOrFallback(listed.error, "invalid session list."));
		}
		else
		{
			std::unordered_set<std::string> identities;
			bool valid = true;
			for (const nlohmann::json& session : sessions)
			{
				if (!session.is_object() || !session.contains("id") || !session["id"].is_string() ||
				    !session.contains("directory") || !session["directory"].is_string()) { valid = false; break; }
				const std::string id = session["id"].get<std::string>();
				if (!uam::execution_hosts::IsPortableId(id) || id.starts_with('-') || !identities.insert(id).second) { valid = false; break; }
			}
			if (!valid) discovery.result.Fail("OpenCode returned malformed or duplicate session metadata.");
			else
			{
				std::sort(sessions.begin(), sessions.end(), [](const nlohmann::json& left, const nlohmann::json& right)
				{
					return left["id"].get_ref<const std::string&>() < right["id"].get_ref<const std::string&>();
				});
				nlohmann::json ids = nlohmann::json::array();
				for (const nlohmann::json& session : sessions) ids.push_back(session["id"]);
				discovery.open_code_session_count = sessions.size();
				discovery.open_code_list_fingerprint = ids.dump();
				if (!open_code_list_fingerprint.empty() && open_code_list_fingerprint != discovery.open_code_list_fingerprint)
					discovery.result.Fail("OpenCode session list changed during discovery; retry the scan.");
				else for (std::size_t scanned = 0; scanned < std::min(sessions.size(), open_code_remaining.value_or(sessions.size())); ++scanned)
		{
			const std::size_t position = (open_code_scan_offset % sessions.size() + scanned) % sessions.size();
			const nlohmann::json& session = sessions[position];
			if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
			{
				discovery.result.Fail("OpenCode history scan stopped before all sessions were read; retry to continue.");
				break;
			}
			discovery.open_code_next_offset = (position + 1) % sessions.size();
			++discovery.open_code_scanned_count;
			if (!session.is_object() || !session.contains("id") || !session["id"].is_string() ||
			    !session.contains("directory") || !session["directory"].is_string())
			{
				discovery.result.Fail("OpenCode returned malformed session metadata.");
				continue;
			}
			const std::string id = session["id"].get<std::string>();
			if (!uam::execution_hosts::IsPortableId(id) || id.starts_with('-') ||
			    (!folder.directory.empty() && !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(session["directory"].get<std::string>()), uam::paths::PathFromUtf8(folder.directory)))) continue;
			ChatSession chat;
			chat.id = id;
			chat.native_session_id = id;
			if (session.value("opencode_v2_session_id", nlohmann::json{}) == id) chat.opencode_v2_session_id = id;
			chat.provider_id = uam::provider_ids::kOpenCodeCli;
			chat.folder_id = folder.id;
			chat.workspace_directory = session["directory"].get<std::string>();
			uam::ProviderWorkerInvocation exported;
			exported.direct_process = true;
			exported.argv = ProviderRuntimeRegistry::Resolve(*open_code_profile).BuildLocalNativeExportArgv(*open_code_profile, chat);
			std::string error;
			const auto source = uam::opencode_import::Export(std::move(exported), uam::paths::PathFromUtf8(chat.workspace_directory), stop_token, error);
			std::size_t message_count = 0;
			bool has_content = false;
			std::string fallback_title;
			if (!source || !uam::opencode_import::Read(*source, chat, [&](const Message& message)
			{
				has_content = has_content || uam::HasImportedConversationContent(message);
				++message_count;
				if (fallback_title.empty() && message.role == MessageRole::User) fallback_title = uam::BuildImportedChatTitle({message}, "");
				return true;
			}, stop_token, error))
			{
				discovery.result.Fail(error);
				continue;
			}
			if (!has_content) continue;
			chat.messages_loaded = false;
			chat.persisted_message_count = message_count;
			chat.title = session.contains("title") && session["title"].is_string() ? session["title"].get<std::string>() : fallback_title;
			discovery.open_code_exports.emplace(chat_identity::NativeIdentityKeyForHistoryImport(chat), source);
			chat.created_at = session.contains("created") && session["created"].is_number_integer() ? IsoTimestampFromEpochMilliseconds(session["created"].get<std::int64_t>()) : "";
			chat.updated_at = session.contains("updated") && session["updated"].is_number_integer() ? IsoTimestampFromEpochMilliseconds(session["updated"].get<std::int64_t>()) : chat.created_at;
			discovery.chats.push_back(std::move(chat));
		}
			}
		}
	}
#else
	(void)open_code_profile;
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	if (!open_code_only && (provider_filter.empty() || provider_filter == uam::provider_ids::kClaudeCli))
	{
	const std::optional<fs::path> configured_claude = uam::env::GetTrimmedPath("CLAUDE_CONFIG_DIR");
	const fs::path claude_root = !native_history_root.empty() ? native_history_root : configured_claude ? *configured_claude : uam::env::GetUserHomePath().value_or(fs::path{}) / ".claude";
	const auto encode_project = [](const std::string& directory)
	{
		std::string encoded = directory;
		for (char& character : encoded)
			if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))) character = '-';
		return encoded;
	};
	const std::string encoded = encode_project(folder.directory);
	std::unordered_set<std::string> project_names{encoded};
	if (!folder.directory.empty() && native_history_root.empty())
	{
		const fs::path location = uam::paths::NormalizeExistingPath(uam::ResolveImportedWorkspaceFolderDirectory(uam::paths::PathFromUtf8(folder.directory)));
		// Include registered linked checkouts without parsing unrelated Claude project histories.
		for (fs::path root = location; !root.empty(); root = root.parent_path())
		{
			if (uam::paths::PathExistsNoThrow(root / ".git"))
			{
				std::error_code error;
				const fs::path registrations = root / ".git" / "worktrees";
				for (fs::directory_iterator it(registrations, error), end; !error && it != end; it.increment(error))
				{
					std::string pointer;
					if (!uam::io::TryReadTextFile(it->path() / "gitdir", pointer, 16384)) continue;
					const fs::path relative = location.lexically_relative(root);
					const fs::path linked_root = uam::paths::PathFromUtf8(uam::strings::Trim(pointer)).parent_path();
					const fs::path linked = relative == "." ? linked_root : linked_root / relative;
					if (uam::ImportedWorkspaceMatchesFolder(linked, uam::paths::PathFromUtf8(folder.directory)))
					{
						project_names.insert(encode_project(uam::paths::Utf8PathString(linked.lexically_normal())));
						const fs::path alias = uam::paths::PathFromUtf8(folder.directory) / linked.lexically_relative(location);
						project_names.insert(encode_project(uam::paths::Utf8PathString(alias.lexically_normal())));
					}
				}
				break;
			}
			if (root == root.parent_path()) break;
		}
	}
	fs::path project;
	std::string project_path_error;
	const bool project_path_available = uam::io::NativeFilePath(claude_root / "projects", project, project_path_error, true);
	if (!project_path_available)
	{
		discovery.result.Fail("Could not resolve Claude history directory: " + project_path_error);
	}
	std::error_code claude_error;
	if (project_path_available && (fs::exists(project, claude_error) || claude_error))
	{
		for (fs::recursive_directory_iterator it(project, claude_error), end; !claude_error && !stop_token.stop_requested() && it != end; it.increment(claude_error))
		{
			if (!folder.directory.empty() && it.depth() == 0 && !project_names.contains(it->path().filename().string())) { it.disable_recursion_pending(); continue; }
			if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
			if (!uam::paths::IsRegularFileEntryNoThrow(*it) || it->path().extension() != ".jsonl") continue;
			if (!native_session_filter.empty() && it->path().stem().string() != native_session_filter && provider_filter == uam::provider_ids::kClaudeCli)
				continue;
			std::error_code size_error;
			if (fs::file_size(it->path(), size_error) > 64 * 1024 * 1024 || size_error)
			{
				discovery.result.Fail("Claude history file exceeds the 64 MiB limit or could not be read.");
				continue;
			}
			std::string text, parse_error;
			if (!uam::io::TryReadTextFile(it->path(), text, 64 * 1024 * 1024))
			{
				discovery.result.Fail("Could not read the bounded Claude native transcript.");
				continue;
			}
			auto parsed = uam::claude::ParseNativeTranscript(text, it->path().stem().string(), folder.directory, &parse_error, stop_token);
			if (parsed)
			{
				parsed->folder_id = folder.id;
				discovery.chats.push_back(std::move(*parsed));
			}
			else if (!parse_error.empty())
				discovery.result.Fail(parse_error);
		}
		if (claude_error) discovery.result.Fail("Could not finish scanning Claude history: " + claude_error.message());
	}
	}
#endif
	if (!native_session_filter.empty())
	{
		std::erase_if(discovery.chats, [&](const ChatSession& chat) { return chat.native_session_id != native_session_filter; });
		std::erase_if(discovery.codex_rollouts, [&](const auto& entry) { return entry.first.native_session_id != native_session_filter; });
	}
	if (stop_token.stop_requested()) discovery.result.Fail("History discovery was canceled.");
	discovery.result.total_count = static_cast<int>(discovery.chats.size() + discovery.codex_rollouts.size());
	return discovery;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportDiscoveredProviderChatsForFolder(uam::AppState& app, const ChatFolder& folder, LocalHistoryDiscovery discovery, bool sidebar_is_complete) const
{
	ImportResult result = std::move(discovery.result);
	if (discovery.open_code_next_offset) app.open_code_history_scan_offset = *discovery.open_code_next_offset;
	const ChatFolder* current = ChatDomainService().FindFolderById(app, folder.id);
	if (!folder.id.empty() && (current == nullptr || current->directory != folder.directory || current->execution_host_id != folder.execution_host_id))
	{
		result.Fail("The workspace changed while history was being scanned.");
		return result;
	}
	NativeImportIndex import_index = LoadNativeImportIndex(app, sidebar_is_complete ? &app.chats : nullptr);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; import stopped to avoid restoring deleted chats.");
		result.total_count = 0;
		return result;
	}
	for (std::size_t index = 0; index < discovery.chats.size() + discovery.codex_rollouts.size(); ++index)
	{
		std::optional<ChatSession> rollout;
		fs::path rollout_file;
		if (index >= discovery.chats.size())
		{
			bool malformed = false;
			rollout_file = discovery.codex_rollouts[index - discovery.chats.size()].second;
			rollout = LoadCodexRolloutChat(rollout_file, folder, &malformed, false, {}, true);
			if (rollout && import_index.existing_id_by_native_key.contains(chat_identity::NativeIdentityKeyForHistoryImport(*rollout)))
			{
				const auto owner = CanonicalCodexInputReceiptOwner(app.data_root, import_index, *rollout);
				rollout = LoadCodexRolloutChat(rollout_file, folder, &malformed, false, {}, false, {}, owner ? &*owner : nullptr);
			}
			if (malformed) result.Fail("Codex history is unreadable, invalid, or exceeds import limits.");
			if (!rollout) continue;
		}
		ChatSession& chat = rollout ? *rollout : discovery.chats[index];
		if (uam::paths::IsControllerLocalWorkspace(chat) && chat.messages_loaded && !uam::HasImportedConversationContent(chat.messages)) continue;
		if (const ChatFolder* owner = uam::FindImportedWorkspaceFolder(app.folders, uam::paths::PathFromUtf8(chat.workspace_directory)))
		{
			if (!folder.id.empty() && owner->id != folder.id) continue;
			chat.folder_id = owner->id;
		}
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
		// An unsaved live transcript owns its tail. A later scan can refresh it after persistence.
		const auto existing = import_index.existing_id_by_native_key.find(key);
		if (existing != import_index.existing_id_by_native_key.end())
		{
			const ChatSession* live = ChatDomainService().FindChatById(app, existing->second);
			if (live != nullptr && live->messages_loaded)
			{
				const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, live->id, true);
				if (!persisted || !MessagesEquivalent(live->messages, persisted->messages)) continue;
			}
		}
		const auto source = discovery.open_code_exports.find(key);
		if (existing == import_index.existing_id_by_native_key.end() && !uam::paths::IsControllerLocalWorkspace(chat) &&
		    (chat.provider_id == uam::provider_ids::kOpenCodeCli || chat.provider_id == uam::provider_ids::kCodexCli))
		{
			const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			const ChatFolder* owner = StartupImportWorkspaceOwner(app, chat);
			if (host == nullptr || owner == nullptr) { result.Fail("The remote workspace is unavailable for import."); continue; }
			chat.folder_id = owner->id;
			const ExecutionHost captured = *host;
			const ChatFolder captured_folder = *owner;
			const fs::path captured_root = app.data_root;
			const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, chat.provider_id);
			const ProviderProfile profile = configured ? *configured : ProviderProfileStore::DefaultOpenCodeProfile();
			std::string error;
			if (!LoadRemoteImportHistory(captured, profile, chat, error))
			{ if (!error.empty()) result.Fail(std::move(error)); continue; }
			const ExecutionHost* current_host = uam::execution_hosts::Find(app.settings.execution_hosts, captured.id);
			const ChatFolder* current_folder = ChatDomainService().FindFolderById(app, captured_folder.id);
			if (app.data_root != captured_root || current_host == nullptr || current_host->runner_status != "ready" ||
			    !uam::execution_hosts::SameConnection(captured, *current_host) || current_folder == nullptr ||
			    current_folder->directory != captured_folder.directory || current_folder->execution_host_id != captured_folder.execution_host_id)
			{ result.Fail("The remote workspace changed while history was being imported."); continue; }
			const NativeImportIndex current_index = LoadNativeImportIndex(app);
			if (!current_index.tombstones_available) { result.Fail("Deletion records changed while history was being imported."); continue; }
			if (current_index.tombstoned_native_keys.contains(key) || current_index.owned_native_keys.contains(key)) continue;
		}
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		if (source != discovery.open_code_exports.end() && existing != import_index.existing_id_by_native_key.end())
		{
			std::string error;
			if (!uam::opencode_import::Read(*source->second, chat, [&](const Message& message) { chat.messages.push_back(message); return true; }, {}, error))
			{ result.Fail(error); continue; }
			chat.messages_loaded = true;
		}
#endif
		const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, chat, "");
		if (!native_key) continue;
		std::string generated_folder_id;
		if (chat.folder_id.empty())
		{
			if (!uam::paths::PathFromUtf8(chat.workspace_directory).is_absolute())
			{
				result.Fail("Provider history has no absolute workspace: " + chat.native_session_id);
				continue;
			}
			ProviderChatSource source;
			source.folder_directory = chat.workspace_directory;
			source.folder_title = uam::paths::PathFromUtf8(chat.workspace_directory).filename().string();
			const std::size_t prior_folder_count = app.folders.size();
			chat.folder_id = ResolvePersistedImportFolderIdForSource(app, source);
			if (chat.folder_id.empty()) { result.Fail("Could not save imported workspace."); continue; }
			if (app.folders.size() > prior_folder_count) generated_folder_id = chat.folder_id;
		}
		if (const ChatSession* live = ChatDomainService().FindChatById(app, chat.id)) OverlayLocalChatState(*live, chat);
		bool saved = false;
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		if (source != discovery.open_code_exports.end() && !chat.messages_loaded)
		{
			std::string error;
			const auto prepared = PrepareOpenCodeImport(app.data_root, source->second, chat, error);
			saved = prepared && ChatRepository::PublishPreparedChatSave(prepared);
			if (!error.empty()) result.Fail(error);
			if (saved) TrackImportedNativeChat(import_index, chat, *native_key);
		}
		else
#endif
		if (!rollout_file.empty() && !chat.messages_loaded)
		{
			const std::shared_ptr<PreparedChatSave> prepared = PrepareCodexRolloutImport(app.data_root, rollout_file, chat);
			saved = prepared && ChatRepository::PublishPreparedChatSave(prepared);
			if (saved) TrackImportedNativeChat(import_index, chat, *native_key);
		}
		else saved = SaveImportedNativeChat(app, import_index, chat, *native_key, {}, false);
		if (saved)
		{
			++result.imported_count;
			if (ChatSession* live = ChatDomainService().FindChatById(app, chat.id))
			{
				OverlayLocalChatState(*live, chat);
				if (!live->messages_loaded) UnloadPersistedTranscript(chat);
				*live = chat;
			}
			else if (sidebar_is_complete)
			{
				UnloadPersistedTranscript(chat);
				app.chats.push_back(std::move(chat));
			}
		}
		else
		{
			result.Fail("Failed to save imported chat " + chat.native_session_id + ".");
			if (!generated_folder_id.empty() &&
			    std::ranges::none_of(app.chats, [&](const ChatSession& saved) { return saved.folder_id == generated_folder_id; }) &&
			    std::ranges::none_of(import_index.existing_summary_by_native_key, [&](const std::pair<const std::string, ChatSession>& saved) { return saved.second.folder_id == generated_folder_id; }))
			{
				std::vector<ChatFolder> retained = app.folders;
				std::erase_if(retained, [&](const ChatFolder& item) { return item.id == generated_folder_id; });
				if (ChatFolderStore::Save(app.data_root, retained)) app.folders = std::move(retained);
				else result.Fail("Failed to remove the empty imported workspace.");
			}
		}
	}
	if (sidebar_is_complete && result.imported_count > 0) ReplaceAppChatsWithNormalized(app, std::move(app.chats));
	return result;
}

void ChatHistorySyncService::LocalHistoryDiscovery::CancelPending()
{
	preparation_stop.request_stop();
	chats.clear();
	open_code_exports.clear();
	codex_rollouts.clear();
	selected_new_folders.clear();
	if (!preparation.valid() && !publication_sync.valid()) return;
	// std::async futures can join on destruction. Let cleanup wait away from the UI.
	std::thread([pending = std::move(preparation), sync = std::move(publication_sync)]()
	{
		if (pending.valid()) pending.wait();
		if (sync.valid()) sync.wait();
	}).detach();
}

const ChatFolder* ChatHistorySyncService::StartupImportWorkspaceOwner(const uam::AppState& app, const ChatSession& chat) const
{
	if (uam::paths::IsControllerLocalWorkspace(chat))
	{
		const fs::path workspace = uam::paths::PathFromUtf8(chat.workspace_directory);
		return workspace.is_absolute() ? uam::FindImportedWorkspaceFolder(app.folders, workspace) : nullptr;
	}
	const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (host == nullptr || host->transport != "ssh" || host->runner_status != "ready" ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(host->platform, chat.workspace_directory)) return nullptr;
	for (const ChatFolder& folder : app.folders)
		if (folder.execution_host_id == chat.execution_host_id && uam::execution_hosts::RemotePathsMatch(host->platform, folder.directory, chat.workspace_directory)) return &folder;
	return nullptr;
}

ChatHistorySyncService::LocalHistoryDiscovery ChatHistorySyncService::DiscoverRemoteProviderChatsForFolder(
    const ChatFolder& folder, const ExecutionHost& host, const ProviderProfile* open_code_profile, std::stop_token stop_token) const
{
	LocalHistoryDiscovery discovery;
	if (host.transport != "ssh" || host.runner_status != "ready" || folder.execution_host_id != host.id ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, folder.directory) || stop_token.stop_requested()) return discovery;
	const auto append = [&](const std::string& id, const std::string& title, const std::string& directory,
	    const std::string& provider, const std::string& created, const std::string& updated)
	{
		if (!uam::execution_hosts::RemotePathsMatch(host.platform, directory, folder.directory)) return;
		ChatSession chat;
		chat.id = id;
		chat.branch_root_chat_id = id;
		chat.title = uam::execution_hosts::Bounded(title, 256);
		if (chat.title.empty()) chat.title = "Session " + id;
		chat.provider_id = provider;
		chat.native_session_id = id;
		chat.folder_id = folder.id;
		chat.execution_host_id = host.id;
		chat.workspace_directory = folder.directory;
		chat.created_at = created;
		chat.updated_at = updated;
		chat.messages_loaded = false;
		discovery.chats.push_back(std::move(chat));
	};
	if (open_code_profile != nullptr && ProviderRuntime::IsRuntimeEnabled(*open_code_profile))
	{
		const RemoteOpenCodeDiscovery remote = DiscoverRemoteOpenCodeSessions(folder, host, *open_code_profile, stop_token);
		if (!remote.error.empty()) discovery.result.Fail(remote.error);
		for (const RemoteOpenCodeSession& session : remote.sessions)
			if (uam::execution_hosts::IsPortableId(session.id)) append(session.id, session.title, session.directory,
			    uam::provider_ids::kOpenCodeCli, IsoTimestampFromEpochMilliseconds(session.created_epoch_ms), IsoTimestampFromEpochMilliseconds(session.updated_epoch_ms));
	}
	if (!stop_token.stop_requested() && ProviderRuntime::IsRuntimeEnabled(uam::provider_ids::kCodexCli))
	{
		const RemoteCodexDiscovery remote = DiscoverRemoteCodexSessions(host, folder, stop_token);
		if (!remote.error.empty()) discovery.result.Fail(remote.error);
		for (const RemoteCodexSession& session : remote.sessions)
		{
			const std::string id = uam::codex::ValidThreadIdOrEmpty(session.id);
			if (!id.empty()) append(id, session.title, session.directory, uam::provider_ids::kCodexCli,
			    IsoTimestampFromEpochSeconds(session.created_epoch_seconds), IsoTimestampFromEpochSeconds(session.updated_epoch_seconds));
		}
	}
	const ProviderProfile profile = open_code_profile ? *open_code_profile : ProviderProfileStore::DefaultOpenCodeProfile();
	std::vector<ChatSession> verified;
	for (ChatSession& chat : discovery.chats)
	{
		if (stop_token.stop_requested()) break;
		std::string error;
		if (LoadRemoteImportHistory(host, profile, chat, error, stop_token))
		{
			UnloadPersistedTranscript(chat);
			verified.push_back(std::move(chat));
		}
		else if (!error.empty()) discovery.result.Fail(std::move(error));
	}
	discovery.chats = std::move(verified);
	return discovery;
}

bool ChatHistorySyncService::FilterStartupImportCandidates(const uam::AppState& app, LocalHistoryDiscovery& discovery, std::string* error_out, bool include_unregistered_local) const
{
	const NativeImportIndex index = LoadNativeImportIndex(app, &app.chats);
	if (!index.tombstones_available)
	{
		if (error_out != nullptr) *error_out = "Deletion and decline records could not be read. History import is unavailable.";
		return false;
	}
	std::unordered_set<std::string> seen;
	const auto reject = [&](ChatSession& chat)
	{
		const ChatFolder* owner = StartupImportWorkspaceOwner(app, chat);
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
		const std::string remote_key = RemoteImportOwnershipKey(app, chat);
		const bool unregistered = include_unregistered_local && uam::paths::IsControllerLocalWorkspace(chat) &&
		    chat.workspace_directory.size() <= 32768 && chat.workspace_directory.find('\0') == std::string::npos &&
		    uam::paths::PathFromUtf8(chat.workspace_directory).is_absolute();
		if ((chat.messages_loaded && !uam::HasImportedConversationContent(chat.messages)) ||
		    (owner == nullptr && !unregistered) || (!uam::paths::IsControllerLocalWorkspace(chat) && (owner == nullptr || owner->id != chat.folder_id)) ||
		    uam::strings::IsBlank(chat.native_session_id) || index.tombstoned_native_keys.contains(key) ||
		    StartupCandidateIsOwned(app, index, chat) || !seen.insert(remote_key.empty() ? key : remote_key).second) return true;
		chat.folder_id = owner != nullptr ? owner->id : "";
		return false;
	};
	std::erase_if(discovery.chats, reject);
	std::erase_if(discovery.open_code_exports, [&](const auto& record)
	{
		return std::ranges::none_of(discovery.chats, [&](const ChatSession& chat) { return chat_identity::NativeIdentityKeyForHistoryImport(chat) == record.first; });
	});
	std::erase_if(discovery.codex_rollouts, [&](std::pair<ChatSession, fs::path>& record) { return reject(record.first); });
	return true;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportDiscoveredProviderChatsBatch(uam::AppState& app, const ChatFolder& folder, LocalHistoryDiscovery& discovery, std::size_t max_chats, ImportPolicy policy) const
{
	ImportResult result;
	// Workspace deletion owns these chats until staging and cleanup finish.
	// Defer publication and further preparation so history cannot recreate them mid-transaction.
	if (!app.worktree_operation_chat_ids.empty()) return result;
	if (discovery.open_code_next_offset) app.open_code_history_scan_offset = *discovery.open_code_next_offset;
	const ChatFolder* current_folder = ChatDomainService().FindFolderById(app, folder.id);
	if (!folder.id.empty() && (current_folder == nullptr || current_folder->directory != folder.directory || current_folder->execution_host_id != folder.execution_host_id))
	{
		discovery.CancelPending();
		result.Fail("The workspace changed while history was being imported.");
		return result;
	}
	if (discovery.publication_sync.valid())
	{
		if (discovery.publication_sync.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return result;
		try { if (!discovery.publication_sync.get()) result.Fail("Imported history could not be fully synced to disk."); }
		catch (const std::exception& error) { result.Fail(std::string("History sync failed: ") + error.what()); }
		discovery.publication_sync = {};
		return result;
	}
	if (discovery.preparation.valid())
	{
		if (discovery.preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return result;
		std::shared_ptr<PreparedHistoryBatch> prepared_owner;
		try
		{
			prepared_owner = discovery.preparation.get();
		}
		catch (const std::exception& error) { result.Fail(std::string("History file preparation failed: ") + error.what()); }
		discovery.preparation = {};
		if (!prepared_owner)
			return result;
		// Preserve ordinary consumption; heavy media candidates instead remain in
		// their armed worker-owned value through every return and rejected row.
		PreparedHistoryBatch ordinary_prepared;
		if (!prepared_owner->native_media_worker_owned)
		{
			ordinary_prepared = std::move(*prepared_owner);
			prepared_owner.reset();
		}
		PreparedHistoryBatch& prepared = prepared_owner ? *prepared_owner : ordinary_prepared;
		if (prepared.expected_data_root != app.data_root || discovery.preparation_stop.stop_requested())
		{ result.Fail("The import owner changed before history could be published."); return result; }
		const NativeImportTombstones tombstones = LoadNativeImportTombstones(app.data_root);
		if (!tombstones.available) { result.Fail("Deletion records could not be read; import stopped."); return result; }
		const NativeImportIndex current_index = LoadNativeImportIndex(app, &app.chats);
		if (!current_index.tombstones_available) { result.Fail("Deletion records changed while import was being published."); return result; }
		for (const PreparedHistoryChat& candidate : prepared.chats)
			if (!candidate.error.empty()) result.Fail(candidate.error);
		for (const PreparedHistoryChat& candidate : prepared.chats)
		{
			if (!candidate.confirmed_empty || !candidate.expected_host || !candidate.expected_folder) continue;
			const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, candidate.expected_host->id);
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, candidate.expected_folder->id);
			if (host != nullptr && host->runner_status == "ready" && uam::execution_hosts::SameConnection(*candidate.expected_host, *host) &&
			    folder != nullptr && folder->directory == candidate.expected_folder->directory && folder->execution_host_id == candidate.expected_folder->execution_host_id)
				result.confirmed_empty_native_keys.push_back(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat));
		}
		// Reviewed new workspaces exist only in this plan until the guarded folder save.
		// Revalidate the captured live catalogue before using that plan as an owner.
		if (policy == ImportPolicy::SelectedNewChatsOnly && !prepared.expected_folders.empty() &&
		    ChatFolderStore::Serialize(app.folders) != prepared.expected_folders)
		{ result.Fail("Workspaces changed while history was being imported; review assignments again."); return result; }
		uam::AppState selected_scope;
		if (policy == ImportPolicy::SelectedNewChatsOnly)
		{
			selected_scope.folders = prepared.expected_folders.empty() ? app.folders : prepared.folders;
			selected_scope.settings.execution_hosts = app.settings.execution_hosts;
		}
		const auto reject_candidate = [&](const PreparedHistoryChat& candidate)
		{
			const ChatFolder* owner = policy == ImportPolicy::SelectedNewChatsOnly ? StartupImportWorkspaceOwner(selected_scope, candidate.chat) : nullptr;
			const ExecutionHost* host = candidate.expected_host ? uam::execution_hosts::Find(app.settings.execution_hosts, candidate.expected_host->id) : nullptr;
			return candidate.skipped || (!candidate.files && !candidate.native_media) || tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat)) || (policy == ImportPolicy::SelectedNewChatsOnly && StartupCandidateIsOwned(app, current_index, candidate.chat)) || (candidate.expected_host && (host == nullptr || host->runner_status != "ready" || !uam::execution_hosts::SameConnection(*candidate.expected_host, *host))) ||
			       (candidate.expected_folder &&
			        [&]()
			        {
				        const ChatFolder* current = ChatDomainService().FindFolderById(policy == ImportPolicy::SelectedNewChatsOnly ? selected_scope : app, candidate.expected_folder->id);
				        return current == nullptr || current->directory != candidate.expected_folder->directory || current->execution_host_id != candidate.expected_folder->execution_host_id;
			        }()) ||
			       (policy == ImportPolicy::SelectedNewChatsOnly && (owner == nullptr || owner->id != candidate.chat.folder_id || (candidate.expected_folder && (owner->directory != candidate.expected_folder->directory || owner->execution_host_id != candidate.expected_folder->execution_host_id)) || (candidate.expected_host && (host == nullptr || !uam::execution_hosts::SameConnection(*candidate.expected_host, *host))))) || (policy == ImportPolicy::RegisteredWorkspacesOnly && (!uam::paths::IsControllerLocalWorkspace(candidate.chat) || !uam::paths::PathFromUtf8(candidate.chat.workspace_directory).is_absolute() || uam::FindImportedWorkspaceFolder(app.folders, uam::paths::PathFromUtf8(candidate.chat.workspace_directory)) == nullptr));
		};
		if (prepared.native_media_worker_owned)
		{
			for (auto& candidate : prepared.chats)
				if (reject_candidate(candidate))
					candidate.skipped = true;
		}
		else
			std::erase_if(prepared.chats, reject_candidate);
		if (std::ranges::none_of(prepared.chats, [](const PreparedHistoryChat& candidate) { return !candidate.skipped; }))
			return result;
		const std::vector<ChatFolder> original_folders = app.folders;
		if (!prepared.expected_folders.empty())
		{
			if (ChatFolderStore::Serialize(app.folders) != prepared.expected_folders) { result.Fail("Workspaces changed while history was being imported; retry to refresh it."); return result; }
			if (!ChatFolderStore::PublishPreparedSave(prepared.folder_files)) { result.Fail("Imported workspaces could not be saved."); return result; }
			app.folders = std::move(prepared.folders);
		}
		for (PreparedHistoryChat& candidate : prepared.chats)
		{
			if (candidate.skipped)
				continue;
			if (tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat))) continue;
			const auto current_identity = current_index.existing_id_by_native_key.find(chat_identity::NativeIdentityKeyForHistoryImport(candidate.chat));
			if (current_identity != current_index.existing_id_by_native_key.end() && current_identity->second != candidate.chat.id) continue;
			const ChatFolder* destination = ChatDomainService().FindFolderById(app, candidate.chat.folder_id);
			if (destination == nullptr || (policy == ImportPolicy::SelectedNewChatsOnly
			    ? StartupImportWorkspaceOwner(app, candidate.chat) != destination
			    : !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(candidate.chat.workspace_directory), uam::paths::PathFromUtf8(destination->directory)))) continue;
			const ChatSession* live = ChatDomainService().FindChatById(app, candidate.chat.id);
			if ((candidate.expected_live.has_value() != (live != nullptr)) ||
			    (live != nullptr && (ChatNeedsTranscriptPreserved(app, *live) || uam::ChatHasActiveCliTerminal(app, live->id) ||
			    live->metadata_incarnation != candidate.expected_live->metadata_incarnation ||
			    live->messages_loaded != candidate.expected_live->messages_loaded ||
			    (live->messages_loaded && live->messages != candidate.expected_live->messages) ||
			    ChatRepository::ChatMetadataFingerprint(app.data_root, *live) != candidate.expected_fingerprint)))
			{
				result.Fail("A chat changed while its history was being imported; retry to refresh it.");
				continue;
			}
			if (candidate.native_media)
			{
				if (candidate.chat.imported_read_only || candidate.chat.native_session_reset_pending || (live != nullptr && (live->imported_read_only || live->native_session_reset_pending)) || !candidate.native_media->Publish(candidate.error, {}, false))
				{
					result.Fail("Failed to save imported native media " + candidate.chat.native_session_id + ".");
					continue;
				}
				// PublishPreparedChatSave changes only this receipt on its committed
				// candidate. Preserve the worker-created full or sidebar projection.
				candidate.chat.acp_queue_full_save_revision = candidate.native_media->Candidate().acp_queue_full_save_revision;
			}
			else if (!ChatRepository::PublishPreparedChatSave(candidate.files, false, &candidate.chat))
			{
				result.Fail("Failed to save imported chat " + candidate.chat.native_session_id + ".");
				continue;
			}
			const std::string imported_id = candidate.chat.id;
			const std::string resolved = NativeSessionLinkService().RealNativeSessionId(candidate.chat);
			if (!resolved.empty()) app.resolved_native_sessions_by_chat_id[imported_id] = resolved;
			if (ChatSession* mutable_live = ChatDomainService().FindChatById(app, imported_id))
			{
				if (candidate.native_media)
					std::swap(*mutable_live, candidate.chat); // Retire old live history on the same bounded worker.
				else
					*mutable_live = std::move(candidate.chat);
			}
			else
			{
				const auto first_agent = std::ranges::find_if(app.chats, [](const ChatSession& item) { return !item.agent_run_id.empty(); });
				app.chats.insert(first_agent, std::move(candidate.chat));
			}
			++result.imported_count;
		}
		if (!prepared.expected_folders.empty())
		{
			std::vector<ChatFolder> retained = app.folders;
			std::erase_if(retained, [&](const ChatFolder& generated)
			{
				return std::ranges::none_of(original_folders, [&](const ChatFolder& original) { return original.id == generated.id; }) &&
				    std::ranges::none_of(app.chats, [&](const ChatSession& chat) { return chat.folder_id == generated.id; });
			});
			if (retained.size() != app.folders.size())
			{
				if (ChatFolderStore::Save(app.data_root, retained)) app.folders = std::move(retained);
				else result.Fail("Failed to remove empty workspaces after history import failed.");
			}
		}
		if (result.imported_count > 0 || !prepared.expected_folders.empty())
		{
			ChatDomainService().NormalizeChatFolderAssignments(app);
			discovery.publication_sync = std::async(std::launch::async,
			                                        [root = app.data_root, prepared_owner]()
			                                        {
				                                        const bool originals_acknowledged = !prepared_owner || AcknowledgeDiscoveredNativeMedia(*prepared_owner);
#if defined(__APPLE__)
				                                        uam::io::AtomicWriteResult synced;
				                                        const bool directories_synced = uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatsRootPath(root), synced) && uam::io::SyncAtomicWriteDirectory(AppPaths::UamChatSummariesRootPath(root), synced) && uam::io::SyncAtomicWriteDirectory(root, synced);
				                                        return originals_acknowledged && directories_synced;
#else
				(void)root;
				return originals_acknowledged;
#endif
			                                        })
			                                 .share();
		}
		return result;
	}
	NativeImportIndex index = LoadNativeImportIndex(app, &app.chats);
	if (!index.tombstones_available) { discovery.CancelPending(); result.Fail("Deletion records could not be read; import stopped."); return result; }
	const bool media_batch = std::ranges::any_of(discovery.chats, HasLocalDiscoveredPendingOriginals);
	std::shared_ptr<PreparedHistoryBatch> candidates;
	if (media_batch)
	{
		// Arm disposal before moving even a subsequently rejected input. A busy
		// bounded owner leaves discovery inputs untouched for the next app poll.
		candidates = uam::native_history::TryCreateWorkerOwnedNativeRefresh<WorkerOwnedNativeMediaHistoryBatch>();
		if (!candidates)
			return result;
		candidates->native_media_worker_owned = true;
	}
	else
		candidates = std::make_shared<PreparedHistoryBatch>();
	candidates->expected_data_root = app.data_root;
	std::size_t admitted_count = 0;
	uam::AppState folder_plan;
	folder_plan.folders = app.folders;
	folder_plan.settings.execution_hosts = app.settings.execution_hosts;
	const std::string expected_folders = ChatFolderStore::Serialize(app.folders);
	if (policy == ImportPolicy::SelectedNewChatsOnly)
	{
		for (const ChatFolder& proposed : discovery.selected_new_folders)
		{
			const ChatFolder* existing = ChatDomainService().FindFolderById(folder_plan, proposed.id);
			if (proposed.id.empty() || proposed.title.empty() || !uam::paths::IsControllerLocalWorkspace(proposed) ||
			    !uam::paths::PathFromUtf8(proposed.directory).is_absolute() ||
			    (existing != nullptr && (existing->directory != proposed.directory || existing->execution_host_id != proposed.execution_host_id)) ||
			    (existing == nullptr && uam::FindImportedWorkspaceFolder(folder_plan.folders, uam::paths::PathFromUtf8(proposed.directory)) != nullptr))
			{
				discovery.CancelPending();
				result.Fail("A reviewed native workspace changed; review assignments again.");
				return result;
			}
			if (existing == nullptr) folder_plan.folders.push_back(proposed);
		}
	}

	if (policy == ImportPolicy::SelectedNewChatsOnly)
	{
		for (const ChatFolder& proposed : discovery.selected_new_folders)
		{
			const ChatFolder* resolved = uam::FindImportedWorkspaceFolder(folder_plan.folders, uam::paths::PathFromUtf8(proposed.directory));
			if (resolved == nullptr || resolved->id != proposed.id)
			{
				discovery.CancelPending();
				result.Fail("Reviewed native workspaces overlap; review assignments again.");
				return result;
			}
		}
	}

	for (std::size_t count = 0; (!discovery.chats.empty() || !discovery.codex_rollouts.empty()) && count < max_chats; ++count)
	{
		PreparedHistoryChat ordinary_candidate;
		ChatSession ordinary_chat;
		PreparedHistoryChat& candidate = media_batch ? candidates->chats.emplace_back() : ordinary_candidate;
		ChatSession& chat = media_batch ? candidates->retired_inputs.emplace_back() : ordinary_chat;
		if (media_batch)
			candidate.skipped = true;
		if (!discovery.chats.empty())
		{
			chat = std::move(discovery.chats.back());
			discovery.chats.pop_back();
			const auto source = discovery.open_code_exports.find(chat_identity::NativeIdentityKeyForHistoryImport(chat));
			if (source != discovery.open_code_exports.end())
			{
				candidate.open_code_export = std::move(source->second);
				discovery.open_code_exports.erase(source);
			}
		}
		else
		{
			chat = std::move(discovery.codex_rollouts.back().first);
			candidate.codex_rollout = std::move(discovery.codex_rollouts.back().second);
			discovery.codex_rollouts.pop_back();
		}
		if (uam::paths::IsControllerLocalWorkspace(chat) && chat.messages_loaded && !uam::HasImportedConversationContent(chat.messages)) continue;
		if (const ChatFolder* owner = policy == ImportPolicy::SelectedNewChatsOnly ? StartupImportWorkspaceOwner(folder_plan, chat) : uam::FindImportedWorkspaceFolder(app.folders, uam::paths::PathFromUtf8(chat.workspace_directory)))
		{
			if (!folder.id.empty() && owner->id != folder.id) continue;
			if (policy == ImportPolicy::SelectedNewChatsOnly)
			{
				if (!chat.folder_id.empty() && owner->id != chat.folder_id) { result.Fail("A selected history resolves to a different workspace; review its assignment again."); continue; }
				const auto selected = discovery.selected_folder_snapshots.find(owner->id);
				if (selected != discovery.selected_folder_snapshots.end() &&
				    (owner->directory != selected->second.directory || owner->execution_host_id != selected->second.execution_host_id)) { result.Fail("A selected history workspace changed; review its assignment again."); continue; }
				candidate.expected_folder = *owner;
			}
			chat.folder_id = owner->id;
		}
		if (policy == ImportPolicy::SelectedNewChatsOnly)
		{
			if (StartupImportWorkspaceOwner(folder_plan, chat) == nullptr) { result.Fail("A selected history has no reviewed workspace; assign it again."); continue; }
			if (!uam::paths::IsControllerLocalWorkspace(chat))
			{
				const auto captured = discovery.expected_hosts.find(chat.execution_host_id);
				const ExecutionHost* current = uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
				if (captured == discovery.expected_hosts.end() || current == nullptr || !uam::execution_hosts::SameConnection(captured->second, *current)) continue;
				candidate.expected_host = captured->second;
			}
		}
		if (policy == ImportPolicy::RegisteredWorkspacesOnly &&
		    (!uam::paths::IsControllerLocalWorkspace(chat) || !uam::paths::PathFromUtf8(chat.workspace_directory).is_absolute() ||
		     uam::FindImportedWorkspaceFolder(app.folders, uam::paths::PathFromUtf8(chat.workspace_directory)) == nullptr)) continue;
		if (!uam::paths::IsControllerLocalWorkspace(chat) &&
		    (chat.provider_id == uam::provider_ids::kOpenCodeCli || chat.provider_id == uam::provider_ids::kCodexCli))
		{
			const ExecutionHost* current = uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			const ChatFolder* owner = StartupImportWorkspaceOwner(app, chat);
			if (current == nullptr || owner == nullptr) continue;
			candidate.expected_host = *current;
			candidate.expected_folder = *owner;
		}
		const std::string key = chat_identity::NativeIdentityKeyForHistoryImport(chat);
		const auto existing = index.existing_id_by_native_key.find(key);
		if (index.tombstoned_native_keys.contains(key)) continue;
		if (policy == ImportPolicy::SelectedNewChatsOnly && StartupCandidateIsOwned(app, index, chat)) continue;
		if (existing != index.existing_id_by_native_key.end())
		{
			chat.id = existing->second;
			const ChatSession* live = ChatDomainService().FindChatById(app, chat.id);
			if (live != nullptr && ChatNeedsTranscriptPreserved(app, *live)) continue;
		}
		else if (index.existing_ids.contains(chat.id)) chat.id = MakeCollisionSafeImportedChatId(chat, index.existing_ids);
		if (chat.folder_id.empty())
		{
			if (!uam::paths::PathFromUtf8(chat.workspace_directory).is_absolute()) { result.Fail("Provider history has no absolute workspace."); continue; }
			ProviderChatSource source;
			source.folder_directory = chat.workspace_directory;
			source.folder_title = uam::paths::PathFromUtf8(chat.workspace_directory).filename().string();
			chat.folder_id = ResolvePersistedImportFolderIdForSource(folder_plan, source, false);
			if (chat.folder_id.empty()) { result.Fail("Could not save imported workspace."); continue; }
		}
		if (const ChatSession* live = ChatDomainService().FindChatById(app, chat.id))
		{
			candidate.expected_live = *live;
			candidate.retain_messages = live->messages_loaded;
			OverlayLocalChatState(*live, chat);
		}
		if (chat.parent_chat_id.empty()) chat.branch_root_chat_id = chat.id;
		candidate.chat = std::move(chat);
		index.existing_ids.insert(candidate.chat.id);
		index.existing_id_by_native_key[key] = candidate.chat.id;
		index.existing_summary_by_native_key[key].id = candidate.chat.id;
		const std::string remote_key = RemoteImportOwnershipKey(app, candidate.chat);
		if (!remote_key.empty()) index.remote_owned_native_keys.insert(remote_key);
		candidate.skipped = false;
		if (!media_batch)
			candidates->chats.push_back(std::move(candidate));
		++admitted_count;
	}
	// Publish only owners represented by admitted chats in this bounded batch.
	std::erase_if(folder_plan.folders, [&](const ChatFolder& proposed)
	{
		return ChatDomainService().FindFolderById(app, proposed.id) == nullptr &&
		    std::ranges::none_of(candidates->chats, [&](const PreparedHistoryChat& candidate) { return !candidate.skipped && candidate.chat.folder_id == proposed.id; });
	});
	if (folder_plan.folders.size() != app.folders.size())
	{
		candidates->expected_folders = expected_folders;
		candidates->folders = std::move(folder_plan.folders);
	}
	if (admitted_count != 0)
	{
		discovery.preparation = std::async(std::launch::async,
		                                   [root = app.data_root, candidates, selected_only = policy == ImportPolicy::SelectedNewChatsOnly, hosts = policy == ImportPolicy::SelectedNewChatsOnly ? app.settings.execution_hosts : std::vector<ExecutionHost>{},
		                                    profile =
		                                        [&]()
		                                    {
			                                    const ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kOpenCodeCli);
			                                    return found ? *found : ProviderProfileStore::DefaultOpenCodeProfile();
		                                    }(),
		                                    stop = discovery.preparation_stop.get_token()]() mutable
		                                   {
			                                   uam::AppState ownership_snapshot;
			                                   std::optional<NativeImportIndex> persisted_ownership;
			                                   if (selected_only)
			                                   {
				                                   ownership_snapshot.data_root = root;
				                                   ownership_snapshot.settings.execution_hosts = std::move(hosts);
				                                   // Persisted summaries are read only by this existing background preparation task.
				                                   persisted_ownership = LoadNativeImportIndex(ownership_snapshot);
			                                   }
			                                   DiscoveredGeminiOriginalReads original_reads;
			                                   for (PreparedHistoryChat& candidate : candidates->chats)
			                                   {
				                                   if (candidate.skipped)
					                                   continue;
				                                   if (stop.stop_requested())
				                                   {
					                                   candidate.skipped = true;
					                                   continue;
				                                   }
				                                   if (persisted_ownership && (!persisted_ownership->tombstones_available || StartupCandidateIsOwned(ownership_snapshot, *persisted_ownership, candidate.chat)))
				                                   {
					                                   candidate.skipped = true;
					                                   if (!persisted_ownership->tombstones_available)
						                                   candidate.error = "Deletion records could not be read; import stopped.";
					                                   continue;
				                                   }
				                                   if (!candidate.expected_live && candidate.expected_host && (candidate.chat.provider_id == uam::provider_ids::kOpenCodeCli || candidate.chat.provider_id == uam::provider_ids::kCodexCli) && !LoadRemoteImportHistory(*candidate.expected_host, profile, candidate.chat, candidate.error, stop))
				                                   {
					                                   candidate.skipped = true;
					                                   candidate.confirmed_empty = candidate.error.empty() && !stop.stop_requested();
					                                   continue;
				                                   }
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
				                                   if (candidate.open_code_export)
				                                   {
					                                   if (!candidate.expected_live && !candidate.retain_messages)
					                                   {
						                                   candidate.files = PrepareOpenCodeImport(root, candidate.open_code_export, candidate.chat, candidate.error);
						                                   if (!candidate.files && candidate.error.empty())
							                                   candidate.error = "Failed to stage complete OpenCode history " + candidate.chat.native_session_id + ".";
						                                   continue;
					                                   }
					                                   if (!uam::opencode_import::Read(
					                                           *candidate.open_code_export, candidate.chat,
					                                           [&](const Message& message)
					                                           {
						                                           candidate.chat.messages.push_back(message);
						                                           return true;
					                                           },
					                                           {}, candidate.error))
					                                   {
						                                   candidate.skipped = true;
						                                   continue;
					                                   }
					                                   candidate.chat.messages_loaded = true;
				                                   }
#endif
				                                   if (!candidate.codex_rollout.empty())
				                                   {
					                                   if (!candidate.expected_live && !candidate.retain_messages)
					                                   {
						                                   bool malformed = false;
						                                   candidate.files = PrepareCodexRolloutImport(root, candidate.codex_rollout, candidate.chat, &malformed);
						                                   if (!candidate.files)
							                                   candidate.error = "Failed to stage complete Codex history " + candidate.chat.native_session_id + "; retry after its source and storage are available.";
						                                   continue;
					                                   }
					                                   ChatFolder source;
					                                   source.directory = candidate.chat.workspace_directory;
					                                   bool malformed = false;
					                                   const auto receipt_owner = candidate.expected_live
					                                       ? ChatRepository::LoadLocalChat(root, candidate.expected_live->id, true) : std::nullopt;
					                                   std::optional<ChatSession> loaded = LoadCodexRolloutChat(candidate.codex_rollout, source, &malformed, false, {}, false, {}, receipt_owner ? &*receipt_owner : nullptr);
					                                   if (!loaded || loaded->native_session_id != candidate.chat.native_session_id)
					                                   {
						                                   candidate.skipped = true;
						                                   if (malformed)
							                                   candidate.error = "Codex history is unreadable, invalid, or exceeds import limits: " + uam::paths::Utf8PathString(candidate.codex_rollout) + ".";
						                                   continue;
					                                   }
					                                   loaded->id = candidate.chat.id;
					                                   loaded->folder_id = candidate.chat.folder_id;
					                                   loaded->branch_root_chat_id = candidate.chat.branch_root_chat_id;
					                                   candidate.chat = std::move(*loaded);
				                                   }
				                                   if (candidate.expected_live)
				                                   {
					                                   const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(root, candidate.chat.id, true);
					                                   const bool local_gemini = candidate.chat.provider_id == uam::provider_ids::kGeminiCli && uam::paths::IsControllerLocalWorkspace(candidate.chat);
					                                   if (!persisted || (candidate.expected_live->messages_loaded && candidate.expected_live->messages != persisted->messages) || !(local_gemini ? ReconcileDiscoveredLocalGeminiMedia(root, *candidate.expected_live, candidate.chat, original_reads, stop) : ReconcileExistingNativeImport(root, *candidate.expected_live, candidate.chat)))
					                                   {
						                                   candidate.skipped = true;
						                                   continue;
					                                   }
					                                   OverlayLocalChatState(*candidate.expected_live, candidate.chat);
					                                   candidate.expected_fingerprint = ChatRepository::ChatMetadataFingerprint(root, *candidate.expected_live);
				                                   }
				                                   if (!candidate.chat.messages_loaded || !uam::HasImportedConversationContent(candidate.chat.messages))
				                                   {
					                                   candidate.skipped = true;
					                                   if (!candidate.chat.messages_loaded)
						                                   candidate.error = "Provider history was not loaded; retry the import.";
					                                   else
						                                   candidate.confirmed_empty = true;
					                                   continue;
				                                   }
				                                   if (candidates->native_media_worker_owned && HasLocalDiscoveredPendingOriginals(candidate.chat))
				                                   {
					                                   if (candidate.chat.imported_read_only || candidate.chat.native_session_reset_pending || !root.is_absolute() || !uam::paths::CreateDirectoriesNoThrow(root))
					                                   {
						                                   candidate.error = "Native media import ownership or storage is unavailable.";
						                                   continue;
					                                   }
					                                   const auto revision = ChatRepository::ChatStorageRevision(root, candidate.chat.id);
					                                   if (!revision)
					                                   {
						                                   candidate.error = "Native media import storage revision is unavailable.";
						                                   continue;
					                                   }
					                                   candidate.native_media = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate.chat), *revision, candidate.error, stop);
					                                   if (candidate.native_media)
						                                   candidate.chat = candidate.native_media->Candidate();
				                                   }
				                                   else
					                                   candidate.files = ChatRepository::PrepareChatSave(root, candidate.chat);
				                                   if (!candidate.files && !candidate.native_media && candidate.error.empty())
					                                   candidate.error = "Failed to prepare imported chat " + candidate.chat.native_session_id + ".";
				                                   if ((candidate.files || candidate.native_media) && !candidate.retain_messages)
				                                   {
					                                   // Keep only sidebar metadata after staging; selection hydrates the complete saved transcript.
					                                   UnloadPersistedTranscript(candidate.chat);
				                                   }
			                                   }
			                                   if (!candidates->expected_folders.empty())
				                                   candidates->folder_files = ChatFolderStore::PrepareSave(root, candidates->folders);
			                                   return candidates;
		                                   })
		                            .share();
	}
	return result;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportProviderChatsForFolder(uam::AppState& app, const std::string& folder_id) const
{
	const ChatFolder* folder = ChatDomainService().FindFolderById(app, uam::strings::Trim(folder_id));
	if (folder == nullptr)
	{
		ImportResult result;
		result.Fail("History scan requires a valid workspace folder.");
		return result;
	}
	const ChatFolder snapshot = *folder;
	const ProviderProfile* open_code = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kOpenCodeCli);
	return ImportDiscoveredProviderChatsForFolder(app, snapshot, DiscoverProviderChatsForFolder(snapshot, open_code, {}, app.open_code_history_scan_offset));
}

ChatHistorySyncService::RemoteOpenCodeDiscovery ChatHistorySyncService::DiscoverRemoteOpenCodeSessions(
	    const ChatFolder& folder, const ExecutionHost& host, const ProviderProfile& profile, std::stop_token stop_token) const
	{
		RemoteOpenCodeDiscovery result;
		uam::remote::RunnerClient client(
		    PlatformServicesFactory::Instance().process_service,
		    host);
		if (stop_token.stop_requested()) return result;
		if (!client.Connect(&result.error)) return result;
		std::string session_id = "history-" +
		    PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (session_id == "history-")
			session_id += uam::time::SteadyEpochNanosecondsTokenNow();
		if (!client.StartProcess(
		        session_id, uam::paths::PathFromUtf8(folder.directory),
		        ProviderRuntimeRegistry::Resolve(profile).BuildNativeDiscoveryArgv(profile),
		        {}, &result.error) || !client.CloseProcessInput(session_id, &result.error))
		{
			(void)client.StopProcess(session_id);
			(void)client.RemoveProcess(session_id);
			return result;
		}
		std::string output;
		std::string diagnostic;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
		{
			uam::remote::ProcessPollResult polled;
			if (!client.PollProcess(session_id, polled, &result.error, [stop_token]() { return stop_token.stop_requested(); })) break;
			if (output.size() + polled.standard_output.size() >
			        uam::platform::kCapturedCommandMaxOutputBytes ||
			    diagnostic.size() + polled.standard_error.size() >
			        uam::platform::kCapturedCommandMaxOutputBytes)
			{
				result.error = "Remote OpenCode session discovery returned too much output.";
				break;
			}
			output += polled.standard_output;
			diagnostic += polled.standard_error;
			if (!client.AcknowledgeProcessOutput(session_id, polled, &result.error)) break;
			if (!polled.running)
			{
				(void)client.RemoveProcess(session_id);
				if (polled.exit_code != 0)
				{
					result.error = "Remote OpenCode session discovery failed (exit " +
					               std::to_string(polled.exit_code) + "): " +
					               uam::strings::NonEmptyOrFallback(
					                   uam::strings::Trim(diagnostic), uam::strings::Trim(output));
					return result;
				}
				const nlohmann::json values = nlohmann::json::parse(output, nullptr, false);
				if (!values.is_array() || values.size() > 200)
				{
					result.error = "Remote OpenCode returned an invalid bounded session list.";
					return result;
				}
				for (const nlohmann::json& value : values)
				{
					if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
					    !value.contains("title") || !value["title"].is_string() ||
					    !value.contains("directory") || !value["directory"].is_string() ||
					    !value.contains("created") || !value["created"].is_number_integer() ||
					    !value.contains("updated") || !value["updated"].is_number_integer())
					{
						result.sessions.clear();
						result.error = "Remote OpenCode returned malformed session metadata.";
						return result;
					}
					result.sessions.push_back({
					    value["id"].get<std::string>(), value["title"].get<std::string>(),
					    value["directory"].get<std::string>(), value["created"].get<std::int64_t>(),
					    value["updated"].get<std::int64_t>()});
				}
				return result;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (result.error.empty()) result.error = stop_token.stop_requested() ? "Remote OpenCode discovery cancelled." : "Remote OpenCode session discovery timed out.";
		(void)client.StopProcess(session_id);
		(void)client.RemoveProcess(session_id);
		return result;
	}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportRemoteOpenCodeChatsForFolder(
    uam::AppState& app, const std::string& folder_id,
    const std::vector<RemoteOpenCodeSession>& sessions) const
{
	ImportResult result;
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const ChatFolder* matched_folder = ChatDomainService().FindFolderById(
	    app, uam::strings::Trim(folder_id));
	if (matched_folder == nullptr || uam::strings::IsBlank(matched_folder->directory) ||
	    uam::paths::IsControllerLocalWorkspace(*matched_folder))
	{
		result.Fail("Remote OpenCode history scan requires a remote workspace folder.");
		return result;
	}
	const ExecutionHost* host = uam::execution_hosts::Find(
	    app.settings.execution_hosts, matched_folder->execution_host_id);
	if (host == nullptr)
	{
		result.Fail("The remote execution host no longer exists.");
		return result;
	}
	const ExecutionHost captured_host = *host;
	const ChatFolder captured_folder = *matched_folder;
	const fs::path captured_root = app.data_root;
	const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kOpenCodeCli);
	const ProviderProfile profile = configured ? *configured : ProviderProfileStore::DefaultOpenCodeProfile();
	NativeImportIndex import_index = LoadNativeImportIndex(app);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; remote import stopped to avoid restoring deleted chats.");
		return result;
	}
	for (const RemoteOpenCodeSession& session : sessions)
	{
		if (!uam::execution_hosts::IsPortableId(session.id) ||
		    !uam::execution_hosts::RemotePathsMatch(host->platform, session.directory,
		                                             matched_folder->directory))
			continue;
		++result.total_count;
		ChatSession chat;
		chat.id = session.id;
		chat.title = uam::execution_hosts::Bounded(session.title, 256);
		if (chat.title.empty()) chat.title = "OpenCode session " + session.id;
		chat.provider_id = uam::provider_ids::kOpenCodeCli;
		chat.native_session_id = session.id;
		chat.folder_id = matched_folder->id;
		chat.execution_host_id = matched_folder->execution_host_id;
		chat.workspace_directory = matched_folder->directory;
		chat.created_at = IsoTimestampFromEpochMilliseconds(session.created_epoch_ms);
		chat.updated_at = IsoTimestampFromEpochMilliseconds(session.updated_epoch_ms);
		chat.messages_loaded = false;
		if (!PrepareNativeChatForImport(app.data_root, import_index, chat, "")) continue;
		std::string error;
		if (!LoadRemoteImportHistory(captured_host, profile, chat, error))
		{
			if (!error.empty()) result.Fail(std::move(error));
			continue;
		}
		const ExecutionHost* current_host = uam::execution_hosts::Find(app.settings.execution_hosts, captured_host.id);
		const ChatFolder* current_folder = ChatDomainService().FindFolderById(app, captured_folder.id);
		if (app.data_root != captured_root || current_host == nullptr || current_host->runner_status != "ready" ||
		    !uam::execution_hosts::SameConnection(captured_host, *current_host) || current_folder == nullptr ||
		    current_folder->directory != captured_folder.directory || current_folder->execution_host_id != captured_folder.execution_host_id)
		{ result.Fail("The remote workspace changed while history was being imported."); break; }
		import_index = LoadNativeImportIndex(app);
		if (!import_index.tombstones_available)
		{ result.Fail("Deletion records changed while history was being imported."); break; }
		const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, chat, "");
		if (native_key && SaveImportedNativeChat(app, import_index, chat, *native_key, {}, false))
			++result.imported_count;
		else if (native_key)
			result.Fail("Failed to save imported OpenCode chat " + chat.native_session_id + ".");
	}
#else
	(void)app;
	(void)folder_id;
	(void)sessions;
	result.Fail("This provider is not included in this build.");
#endif
	return result;
}

ChatHistorySyncService::RemoteOpenCodeTranscript
ChatHistorySyncService::ParseRemoteOpenCodeTranscript(std::string_view output)
try
{
	RemoteOpenCodeTranscript result;
	const std::size_t first = output.find('{');
	const std::size_t last = output.rfind('}');
	if (first == std::string_view::npos || last == std::string_view::npos || last < first)
	{
		result.error = "OpenCode export did not contain JSON.";
		return result;
	}
	const nlohmann::json root = nlohmann::json::parse(
	    output.substr(first, last - first + 1), nullptr, false);
	if (!root.is_object() || !root.contains("info") || !root["info"].is_object())
	{
		result.error = "OpenCode export is missing valid session information.";
		return result;
	}
	if (!root.contains("messages") || !root["messages"].is_array())
	{
		result.error = "OpenCode export is missing a valid messages array.";
		return result;
	}
	for (const char* key : {"nextCursor", "next_cursor"})
		if (root.contains(key) && !root[key].is_null() && (!root[key].is_string() || !root[key].get_ref<const std::string&>().empty()))
		{ result.error = "OpenCode export returned incomplete paginated history. Retry the import."; return result; }
	for (const char* key : {"hasMore", "has_more"})
		if (root.contains(key) && (!root[key].is_boolean() || root[key].get<bool>()))
		{ result.error = "OpenCode export returned incomplete paginated history. Retry the import."; return result; }
	result.verified_empty = root["messages"].empty();
	const nlohmann::json& session_info = root["info"];
	result.session_id = session_info.value("id", "");
	result.directory = session_info.value("directory", "");
	if (result.session_id.empty() || result.directory.empty())
	{
		result.error = "OpenCode export omitted the session identity or workspace.";
		return result;
	}

	bool unmapped = false;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kOpenCodeCli);
	for (const nlohmann::json& entry : root["messages"])
	{
		if (!entry.is_object() || !entry.contains("info") || !entry["info"].is_object() ||
		    !entry.contains("parts") || !entry["parts"].is_array())
		{
			result.error = "OpenCode export contained malformed message data.";
			result.messages.clear();
			return result;
		}
		const nlohmann::json& info = entry["info"];
		const std::string role = info.value("role", "");
		const bool compaction_summary = role == "assistant" && info.value("summary", false);
		if (role == "user" && std::ranges::any_of(entry["parts"], [](const nlohmann::json& part)
		    { return part.is_object() && part.value("type", "") == "compaction"; })) continue;
		Message message;
		if (role == "user") message.role = MessageRole::User;
		else if (role == "assistant") message.role = MessageRole::Assistant;
		else if (role == "system") message.role = MessageRole::System;
		else { unmapped = true; continue; }
		message.provider = uam::provider_ids::kOpenCodeCli;
		if (message.role == MessageRole::Assistant)
		{
			message.interrupted = info.contains("error") && info["error"].is_object() &&
			    info["error"].value("name", "") == "MessageAbortedError";
			message.model_id = info.value("modelID", "");
			const std::string model_provider = info.value("providerID", "");
			if (!message.model_id.empty() && !model_provider.empty()) message.model_id = model_provider + "/" + message.model_id;
		}
		if (info.contains("time") && info["time"].is_object())
		{
			const std::int64_t created = JsonIntegerOrZero(info["time"].value("created", nlohmann::json{}));
			message.created_at = IsoTimestampFromEpochMilliseconds(created);
			const std::int64_t completed = JsonIntegerOrZero(info["time"].value("completed", nlohmann::json{}));
			if (completed > created && created > 0)
				message.processing_time_ms = static_cast<int>(std::min<std::int64_t>(
				    completed - created, std::numeric_limits<int>::max()));
		}
		if (info.contains("tokens") && info["tokens"].is_object())
		{
			message.tokens_input = BoundedMessageTokenCount(
			    info["tokens"].value("input", nlohmann::json{}));
			message.tokens_output = BoundedMessageTokenCount(
			    info["tokens"].value("output", nlohmann::json{}));
		}
		if (info.contains("cost") && info["cost"].is_number())
			message.estimated_cost_usd = info["cost"].get<double>();

		for (const nlohmann::json& part : entry["parts"])
		{
			if (!part.is_object())
			{
				result.error = "OpenCode export contained malformed message parts.";
				result.messages.clear();
				return result;
			}
			const std::string type = part.value("type", "");
			if (type == "text")
			{
				const std::string text = part.at("text").get<std::string>();
				AppendTranscriptText(message.content, text);
				if (message.role == MessageRole::Assistant && !text.empty())
					message.blocks.push_back({"assistant_text", text, "", ""});
			}
			else if (type == "reasoning")
			{
				const std::string text = part.at("text").get<std::string>();
				AppendTranscriptText(message.thoughts, text);
				if (!text.empty()) message.blocks.push_back({"thought", text, "", ""});
			}
			else if (type == "tool")
			{
				ToolCall tool;
				tool.id = uam::strings::NonEmptyOrFallback(
				    part.value("callID", ""), part.value("id", ""));
				tool.name = part.value("tool", "tool");
				if (part.contains("state") && part["state"].is_object())
				{
					const nlohmann::json& state = part["state"];
					if (state.contains("input")) tool.args_json = state["input"].dump();
					if (state.contains("output") && state["output"].is_string())
						tool.result_text = state["output"].get<std::string>();
					tool.status = state.value("status", "completed");
					if (tool.status == "error") tool.result_text = state.value("error", tool.result_text);
				}
				if (tool.id.empty()) tool.id = "opencode-tool-" + std::to_string(message.tool_calls.size());
				runtime.ApplyNativeToolMetadata(tool, part);
				message.blocks.push_back({"tool_call", "", tool.id, ""});
				message.tool_calls.push_back(std::move(tool));
			}
			else if (type == "file")
			{
				AppendTranscriptText(message.content,
				    "[Attachment: " + uam::strings::NonEmptyOrFallback(
				        part.value("filename", ""), part.value("name", "file")) + "]");
			}
			else if (type != "step-start" && type != "step-finish" && type != "compaction" && type != "snapshot" && type != "patch" && type != "retry") unmapped = true;
		}
		if (compaction_summary)
		{
			std::string summary = std::move(message.content);
			message.content.clear();
			message.thoughts.clear();
			message.blocks = {{"context_compaction", std::move(summary), "", info.value("id", "")}};
			message.tool_calls.clear();
		}
		if (compaction_summary || message.interrupted || !message.content.empty() || !message.thoughts.empty() || !message.tool_calls.empty())
			result.messages.push_back(std::move(message));
	}
	result.verified_empty = !unmapped && !uam::HasImportedConversationContent(result.messages);
	result.success = true;
	return result;
}
catch (const nlohmann::json::exception&)
{
	RemoteOpenCodeTranscript rejected;
	rejected.error = "OpenCode returned malformed transcript field types.";
	return rejected;
}

std::optional<ChatSession> ChatHistorySyncService::ParseRemoteNativeHistorySnapshot(const ExecutionHost& host, const ChatSession& chat, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token) const
{
	if (error)
		error->clear();
	const auto fail = [&](const std::string& message) -> std::optional<ChatSession>
	{
		if (error)
			*error = message;
		return std::nullopt;
	};
	const std::string provider = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
	if (stop_token.stop_requested())
		return fail("Native history refresh was cancelled.");
	if (host.id == "local" || host.id != chat.execution_host_id || host.transport != "ssh" || !snapshot.is_object() || !uam::remote::native_history::SafeId(chat.native_session_id) || !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, chat.workspace_directory) || uam::remote::native_history::String(snapshot, "providerId") != provider || uam::remote::native_history::String(snapshot, "sessionId") != chat.native_session_id)
		return fail("The native history snapshot belongs to a different provider, session or host.");
	const std::string workspace = uam::remote::native_history::String(snapshot, "workspace");
	if (!uam::execution_hosts::RemotePathsMatch(host.platform, workspace, chat.workspace_directory))
		return fail("The native history snapshot belongs to a different workspace.");
	const auto text = snapshot.find("text");
	const auto yaml = snapshot.find("workspaceYaml");
	if (text == snapshot.end() || !text->is_string() || text->get_ref<const std::string&>().empty() || text->get_ref<const std::string&>().size() > uam::remote::native_history::kMaxSnapshotBytes || (yaml != snapshot.end() && (!yaml->is_string() || yaml->get_ref<const std::string&>().size() > 16384)))
		return fail("The native history snapshot is empty, malformed or exceeds its size limit.");
	if (text->get_ref<const std::string&>().size() + (yaml == snapshot.end() ? 0 : yaml->get_ref<const std::string&>().size()) > uam::remote::native_history::kMaxSnapshotBytes)
		return fail("The native history snapshot exceeds its aggregate size limit.");
	std::string recorded_workspace;
	if ((provider == uam::provider_ids::kClaudeCli && !uam::remote::native_history::ClaudeIdentity(text->get_ref<const std::string&>(), chat.native_session_id, workspace, recorded_workspace)) || (provider == uam::provider_ids::kCopilotCli && !uam::remote::native_history::CopilotIdentity(text->get_ref<const std::string&>(), chat.native_session_id, workspace, recorded_workspace)))
		return fail("The native transcript does not attest the requested session and workspace.");
	const auto temporary = uam::paths::TempDirectoryPathNoThrow();
	const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (!temporary || !uam::remote::native_history::SafeId(uuid))
		return fail("Could not allocate an isolated native history reader.");
	const fs::path root = *temporary / ("uam-native-history-" + uuid);
	std::error_code directory_error;
	if (!fs::create_directory(root, directory_error) || directory_error)
		return fail("Could not create the isolated native history reader.");
	struct RemoveSnapshot
	{
		fs::path root;
		~RemoveSnapshot()
		{
			std::error_code ignored;
			fs::remove_all(root, ignored);
		}
	} cleanup{root};
#if !defined(_WIN32)
	fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, directory_error);
	if (directory_error)
		return fail("Could not protect the isolated native history reader.");
#endif
	std::optional<ChatSession> loaded;
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (provider == uam::provider_ids::kCopilotCli)
	{
		const fs::path session = root / chat.native_session_id;
		if (!uam::io::WriteTextFile(session / "events.jsonl", text->get_ref<const std::string&>()) || (yaml != snapshot.end() && !yaml->get_ref<const std::string&>().empty() && !uam::io::WriteTextFile(session / "workspace.yaml", yaml->get_ref<const std::string&>())))
			return fail("Could not stage the isolated Copilot native transcript.");
		loaded = LoadCopilotSessionStateChat(session, uam::paths::PathFromUtf8(workspace), {}, error, stop_token, host.id);
	}
	else
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    if (provider == uam::provider_ids::kClaudeCli)
	{
		std::string encoded = workspace;
		for (char& character : encoded)
			if (!uam::strings::IsAsciiAlnum(static_cast<unsigned char>(character)))
				character = '-';
		if (!uam::io::WriteTextFile(root / "projects" / encoded / (chat.native_session_id + ".jsonl"), text->get_ref<const std::string&>()))
			return fail("Could not stage the isolated Claude native transcript.");
		ChatFolder folder;
		folder.directory = workspace;
		auto discovery = DiscoverProviderChatsForFolder(folder, nullptr, stop_token, 0, false, {}, {}, provider, chat.native_session_id, root);
		if (discovery.chats.size() == 1)
			loaded = std::move(discovery.chats.front());
	}
	else
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    if (provider == uam::provider_ids::kGeminiCli)
	{
		const fs::path file = root / "gemini.json";
		if (!uam::io::WriteTextFile(file, text->get_ref<const std::string&>()))
			return fail("Could not stage the isolated Gemini native transcript.");
		loaded = GeminiJsonHistoryStore::ParseFile(file, ProviderProfileStore::DefaultGeminiProfile(), {uam::remote::native_history::kMaxSnapshotBytes, 0, host.id}, stop_token);
		if (loaded)
			loaded->workspace_directory = workspace;
	}
	else
#endif
		return fail("This provider does not expose a native file transcript reader.");
	if (stop_token.stop_requested())
		return fail("Native history refresh was cancelled.");
	if (!loaded || loaded->native_session_id != chat.native_session_id || loaded->provider_id != provider || !uam::HasImportedConversationContent(loaded->messages) || !uam::execution_hosts::RemotePathsMatch(host.platform, loaded->workspace_directory, workspace))
		return fail("The exact native transcript is unavailable or does not contain a readable conversation.");
	loaded->execution_host_id = host.id;
	loaded->workspace_directory = chat.workspace_directory;
	if (provider == uam::provider_ids::kClaudeCli)
		for (Message& message : loaded->messages)
			for (ToolCall& tool : message.tool_calls)
			{
				const auto child = uam::native_agents::ParseHistoryId(tool.sub_agent_id);
				if (child && child->provider_id == uam::provider_ids::kClaudeCli && child->parent_session_id == loaded->native_session_id && child->execution_host_id == "local")
					tool.sub_agent_id = uam::native_agents::BuildHistoryId(child->provider_id, child->parent_session_id, child->agent_id, host.id);
			}
	return loaded;
}

std::optional<ChatSession> ChatHistorySyncService::LoadRemoteNativeTranscript(const ExecutionHost& host, const ChatSession& chat, std::string* error, std::stop_token stop_token) const
{
	if (error)
		error->clear();
	if (host.id == "local" || host.id != chat.execution_host_id || host.transport != "ssh" || host.runner_status != "ready" || !uam::remote::native_history::SafeId(chat.native_session_id) || !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, chat.workspace_directory))
	{
		if (error)
			*error = "The exact remote native session or its helper is not ready.";
		return std::nullopt;
	}
	uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service, host);
	nlohmann::json snapshot;
	if (!client.ReadNativeHistory(uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id), chat.native_session_id, chat.workspace_directory, snapshot, error, stop_token))
		return std::nullopt;
	return ParseRemoteNativeHistorySnapshot(host, chat, snapshot, error, stop_token);
}

std::optional<ChatSession> ChatHistorySyncService::LoadLocalNativeTranscript(const ChatSession& chat, std::string* error, std::stop_token stop_token) const
{
	if (error != nullptr)
		error->clear();
	const std::string provider = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
	if (!uam::paths::IsControllerLocalWorkspace(chat) || chat.native_session_id.empty() || !uam::chat_ids::IsSafeStorageChatId(chat.native_session_id) || uam::strings::IsBlank(chat.workspace_directory))
	{
		if (error != nullptr)
			*error = "Native history refresh requires an exact local session and workspace.";
		return std::nullopt;
	}
	if (provider != uam::provider_ids::kClaudeCli && provider != uam::provider_ids::kCopilotCli && provider != uam::provider_ids::kGeminiCli && provider != uam::provider_ids::kAntigravityCli)
	{
		if (error != nullptr)
			*error = "This provider does not expose a local native transcript reader.";
		return std::nullopt;
	}
	ChatFolder folder;
	folder.directory = chat.workspace_directory;
	folder.execution_host_id = "local";
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (provider == uam::provider_ids::kCopilotCli)
	{
		return LoadCopilotSessionStateChat(CopilotSessionStatePath() / chat.native_session_id, uam::paths::PathFromUtf8(chat.workspace_directory), {}, error, stop_token);
	}
#endif
	LocalHistoryDiscovery discovered = DiscoverProviderChatsForFolder(folder, nullptr, stop_token, 0, false, {}, {}, provider, chat.native_session_id);
	if (stop_token.stop_requested())
		return std::nullopt;
	const auto found = std::ranges::find_if(discovered.chats, [&](const ChatSession& candidate) { return candidate.native_session_id == chat.native_session_id && candidate.provider_id == provider && uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(candidate.workspace_directory), uam::paths::PathFromUtf8(chat.workspace_directory)); });
	if (found != discovered.chats.end())
		return std::move(*found);
	if (error != nullptr)
		*error = "The exact native transcript is unavailable or contains no readable conversation.";
	return std::nullopt;
}

std::optional<ChatSession> ChatHistorySyncService::LoadLocalCodexChildChat(const ChatSession& chat, std::string* error) const
{
	if (error != nullptr) error->clear();
	if (!uam::paths::IsControllerLocalWorkspace(chat) ||
	    !uam::paths::PathFromUtf8(chat.workspace_directory).is_absolute() ||
	    uam::codex::ValidThreadIdOrEmpty(chat.native_session_id).empty())
	{
		if (error != nullptr) *error = "The local Codex session is not ready to load.";
		return std::nullopt;
	}
	const std::optional<fs::path> rollout = uam::codex::FindRolloutFileForSession(chat.native_session_id, uam::codex::CodexHomePath(), true);
	if (!rollout) return std::nullopt;
	ChatFolder folder;
	folder.id = chat.folder_id;
	folder.directory = chat.workspace_directory;
	bool malformed = false;
	std::optional<ChatSession> loaded = LoadCodexRolloutChat(*rollout, folder, &malformed, true, {}, false, {}, &chat);
	if (malformed)
	{
		if (error != nullptr) *error = "Codex history is incomplete or could not be read. Try again.";
		return std::nullopt;
	}
	if (loaded && loaded->native_session_id != chat.native_session_id)
	{
		if (error != nullptr) *error = "Codex returned a different session.";
		return std::nullopt;
	}
	return loaded;
}

ChatHistorySyncService::RemoteOpenCodeTranscript ChatHistorySyncService::LoadLocalOpenCodeTranscript(const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token) const
{
	RemoteOpenCodeTranscript result;
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const fs::path workspace = uam::paths::PathFromUtf8(chat.workspace_directory);
	if (!uam::paths::IsControllerLocalWorkspace(chat) || !workspace.is_absolute() ||
	    !uam::execution_hosts::IsPortableId(chat.native_session_id) || chat.native_session_id.starts_with('-'))
	{
		result.error = "The local OpenCode session is not ready to load.";
		return result;
	}

	uam::ProviderWorkerInvocation invocation;
	invocation.direct_process = true;
	invocation.argv = ProviderRuntimeRegistry::Resolve(profile).BuildLocalNativeExportArgv(profile, chat);
	std::string error;
	const auto source = uam::opencode_import::Export(std::move(invocation), workspace, stop_token, error);
	if (!source || !uam::opencode_import::Read(*source, chat, [&](const Message& message)
	{
		result.messages.push_back(message);
		return true;
	}, stop_token, error))
	{
		result = {};
		result.error = error;
		return result;
	}
	result.success = true;
	result.session_id = chat.native_session_id;
	result.directory = chat.workspace_directory;
#else
	(void)chat;
	(void)profile;
	(void)stop_token;
#endif
	return result;
}

ChatHistorySyncService::RemoteOpenCodeTranscript
ChatHistorySyncService::LoadRemoteOpenCodeTranscript(
    const ExecutionHost& host, const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token) const
{
	RemoteOpenCodeTranscript result;
	if (stop_token.stop_requested())
	{
		result.error = "Remote OpenCode transcript loading was canceled.";
		return result;
	}
	if (host.id == uam::execution_hosts::kLocalHostId || host.runner_status != "ready" ||
	    !uam::execution_hosts::IsPortableId(chat.native_session_id) || chat.native_session_id.starts_with('-') ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, chat.workspace_directory))
	{
		result.error = "The remote OpenCode session is not ready to load.";
		return result;
	}
	uam::remote::RunnerClient client(
	    PlatformServicesFactory::Instance().process_service,
	    host);
	if (!client.Connect(&result.error)) return result;
	if (stop_token.stop_requested())
	{
		result.error = "Remote OpenCode transcript loading was canceled.";
		return result;
	}
	const std::string process_id = "history-export-" +
	    PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (!client.StartProcess(process_id, uam::paths::PathFromUtf8(chat.workspace_directory),
	                         ProviderRuntimeRegistry::Resolve(profile).BuildNativeExportArgv(profile, chat), {},
	                         &result.error) || !client.CloseProcessInput(process_id, &result.error))
	{
		(void)client.StopProcess(process_id);
		(void)client.RemoveProcess(process_id);
		return result;
	}
	std::string output;
	std::string diagnostic;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	const auto interrupt = [stop_token, deadline]
	{
		return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline;
	};
	while (!interrupt())
	{
		uam::remote::ProcessPollResult polled;
		if (!client.PollProcess(process_id, polled, &result.error, interrupt)) break;
		if (output.size() + polled.standard_output.size() >
		        uam::platform::kCapturedCommandMaxOutputBytes ||
		    diagnostic.size() + polled.standard_error.size() >
		        uam::platform::kCapturedCommandMaxOutputBytes)
		{
			result.error = "The remote OpenCode transcript exceeds UAM's 4 MiB safety limit.";
			break;
		}
		output += polled.standard_output;
		diagnostic += polled.standard_error;
		if (!client.AcknowledgeProcessOutput(process_id, polled, &result.error)) break;
		if (!polled.running)
		{
			(void)client.RemoveProcess(process_id);
			if (polled.exit_code != 0)
			{
				result.error = "OpenCode could not export the remote session (exit " +
				               std::to_string(polled.exit_code) + "): " +
				               uam::strings::NonEmptyOrFallback(
				                   uam::strings::Trim(diagnostic), uam::strings::Trim(output));
				return result;
			}
			result = ParseRemoteOpenCodeTranscript(output);
			if (result.success &&
			    (result.session_id != chat.native_session_id ||
			     !uam::execution_hosts::RemotePathsMatch(
			         host.platform, result.directory, chat.workspace_directory)))
			{
				result = {};
				result.error = "OpenCode exported a different session or workspace.";
			}
			return result;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (stop_token.stop_requested()) result.error = "Remote OpenCode transcript loading was canceled.";
	else if (result.error.empty() || std::chrono::steady_clock::now() >= deadline)
		result.error = "Remote OpenCode transcript loading timed out.";
	(void)client.StopProcess(process_id);
	(void)client.RemoveProcess(process_id);
	return result;
}

bool ChatHistorySyncService::AppendRemoteCodexSessions(
    const nlohmann::json& result, std::vector<RemoteCodexSession>& sessions,
    std::string* error)
{
	const nlohmann::json* data = uam::nlohmann_json::FindArrayField(result, "data");
	if (data == nullptr || sessions.size() + data->size() > kRemoteCodexMaxSessions)
	{
		if (error != nullptr) *error = "Codex returned an invalid or oversized thread list.";
		return false;
	}
	for (const nlohmann::json& value : *data)
	{
		if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
		    !value.contains("cwd") || !value["cwd"].is_string() ||
		    !value.contains("preview") || !value["preview"].is_string() ||
		    !value.contains("createdAt") || !value["createdAt"].is_number_integer() ||
		    !value.contains("updatedAt") || !value["updatedAt"].is_number_integer())
		{
			if (error != nullptr) *error = "Codex returned malformed thread metadata.";
			return false;
		}
		const std::string id = uam::codex::ValidThreadIdOrEmpty(value["id"].get<std::string>());
		const std::string directory = value["cwd"].get<std::string>();
		if (id.empty() || directory.empty() || directory.size() > 4096)
		{
			if (error != nullptr) *error = "Codex returned an invalid thread identity or workspace.";
			return false;
		}
		if ((value.contains("ephemeral") && value["ephemeral"].is_boolean() &&
		     value["ephemeral"].get<bool>()) ||
		    (value.contains("parentThreadId") && value["parentThreadId"].is_string() &&
		     !uam::strings::IsBlank(value["parentThreadId"].get_ref<const std::string&>())))
			continue;
		if (std::ranges::any_of(sessions, [&id](const RemoteCodexSession& session)
		    { return session.id == id; }))
			continue;
		RemoteCodexSession session;
		session.id = id;
		session.directory = directory;
		if (value.contains("name") && value["name"].is_string())
			session.title = value["name"].get<std::string>();
		if (uam::strings::IsBlank(session.title))
		{
			Message prompt;
			prompt.role = MessageRole::User;
			prompt.content = value["preview"].get<std::string>();
			session.title = uam::IsCodexSyntheticUserMessage(prompt.content) ? "Codex session " + id :
			    uam::BuildImportedChatTitle({prompt}, "");
		}
		try
		{
			session.created_epoch_seconds = value["createdAt"].get<std::int64_t>();
			session.updated_epoch_seconds = value["updatedAt"].get<std::int64_t>();
		}
		catch (const nlohmann::json::exception&)
		{
			if (error != nullptr) *error = "Codex returned an invalid thread timestamp.";
			return false;
		}
		sessions.push_back(std::move(session));
	}
	return true;
}

ChatHistorySyncService::RemoteCodexDiscovery
ChatHistorySyncService::DiscoverRemoteCodexSessions(
    const ExecutionHost& host, const ChatFolder& folder, std::stop_token stop_token) const
{
	RemoteCodexDiscovery discovery;
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	if (host.id == uam::execution_hosts::kLocalHostId || host.runner_status != "ready" ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, folder.directory))
	{
		discovery.error = "The remote Codex workspace is not ready to scan.";
		return discovery;
	}
	uam::remote::RunnerClient client(
	    PlatformServicesFactory::Instance().process_service,
	    host);
	if (!client.Connect(&discovery.error)) return discovery;
	for (const bool archived : {false, true})
	{
		const std::size_t remaining = kRemoteCodexMaxSessions - discovery.sessions.size();
		if (remaining == 0) break;
		nlohmann::json list_result;
		const nlohmann::json params{
		    {"archived", archived}, {"cwd", folder.directory},
		    {"limit", remaining}, {"sortKey", "updated_at"},
		    {"sortDirection", "desc"}};
		if (!RunRemoteCodexRpc(client, uam::paths::PathFromUtf8(folder.directory),
		                       "thread/list", params, list_result, discovery.error, stop_token) ||
		    !AppendRemoteCodexSessions(list_result, discovery.sessions, &discovery.error))
			return discovery;
	}
#else
	(void)host;
	(void)folder;
	(void)stop_token;
	discovery.error = "This provider is not included in this build.";
#endif
	return discovery;
}

ChatHistorySyncService::ImportResult ChatHistorySyncService::ImportRemoteCodexChatsForFolder(
    uam::AppState& app, const std::string& folder_id,
    const std::vector<RemoteCodexSession>& sessions) const
{
	ImportResult result;
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const ChatFolder* matched_folder = ChatDomainService().FindFolderById(
	    app, uam::strings::Trim(folder_id));
	if (matched_folder == nullptr || uam::strings::IsBlank(matched_folder->directory) ||
	    uam::paths::IsControllerLocalWorkspace(*matched_folder))
	{
		result.Fail("Remote Codex history scan requires a remote workspace folder.");
		return result;
	}
	const ExecutionHost* host = uam::execution_hosts::Find(
	    app.settings.execution_hosts, matched_folder->execution_host_id);
	if (host == nullptr)
	{
		result.Fail("The remote execution host no longer exists.");
		return result;
	}
	const ExecutionHost captured_host = *host;
	const ChatFolder captured_folder = *matched_folder;
	const fs::path captured_root = app.data_root;
	const ProviderProfile* configured = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kCodexCli);
	const ProviderProfile profile = configured ? *configured : ProviderProfileStore::DefaultCodexProfile();
	NativeImportIndex import_index = LoadNativeImportIndex(app);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; remote import stopped to avoid restoring deleted chats.");
		return result;
	}
	for (const RemoteCodexSession& session : sessions)
	{
		const std::string valid_id = uam::codex::ValidThreadIdOrEmpty(session.id);
		if (valid_id.empty() ||
		    !uam::execution_hosts::RemotePathsMatch(host->platform, session.directory,
		                                             matched_folder->directory))
			continue;
		++result.total_count;
		ChatSession chat;
		chat.id = valid_id;
		chat.title = uam::execution_hosts::Bounded(session.title, 256);
		if (chat.title.empty()) chat.title = "Codex session " + valid_id;
		chat.provider_id = uam::provider_ids::kCodexCli;
		chat.native_session_id = valid_id;
		chat.folder_id = matched_folder->id;
		chat.execution_host_id = matched_folder->execution_host_id;
		chat.workspace_directory = matched_folder->directory;
		chat.branch_root_chat_id = chat.id;
		chat.created_at = IsoTimestampFromEpochSeconds(session.created_epoch_seconds);
		chat.updated_at = IsoTimestampFromEpochSeconds(session.updated_epoch_seconds);
		chat.messages_loaded = false;
		if (!PrepareNativeChatForImport(app.data_root, import_index, chat, "")) continue;
		std::string error;
		if (!LoadRemoteImportHistory(captured_host, profile, chat, error))
		{
			if (!error.empty()) result.Fail(std::move(error));
			continue;
		}
		const ExecutionHost* current_host = uam::execution_hosts::Find(app.settings.execution_hosts, captured_host.id);
		const ChatFolder* current_folder = ChatDomainService().FindFolderById(app, captured_folder.id);
		if (app.data_root != captured_root || current_host == nullptr || current_host->runner_status != "ready" ||
		    !uam::execution_hosts::SameConnection(captured_host, *current_host) || current_folder == nullptr ||
		    current_folder->directory != captured_folder.directory || current_folder->execution_host_id != captured_folder.execution_host_id)
		{ result.Fail("The remote workspace changed while history was being imported."); break; }
		import_index = LoadNativeImportIndex(app);
		if (!import_index.tombstones_available)
		{ result.Fail("Deletion records changed while history was being imported."); break; }
		const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, chat, "");
		if (native_key) chat.branch_root_chat_id = chat.id;
		if (native_key && SaveImportedNativeChat(app, import_index, chat, *native_key, {}, false))
			++result.imported_count;
		else if (native_key)
			result.Fail("Failed to save imported Codex chat " + chat.native_session_id + ".");
	}
#else
	(void)app;
	(void)folder_id;
	(void)sessions;
	result.Fail("This provider is not included in this build.");
#endif
	return result;
}

ChatHistorySyncService::RemoteCodexTranscript
ChatHistorySyncService::ParseRemoteCodexTranscript(const nlohmann::json& result, const ChatSession* input_receipt_owner)
try
{
	RemoteCodexTranscript transcript;
	const nlohmann::json* thread = uam::nlohmann_json::FindObjectField(result, "thread");
	if (thread == nullptr || !thread->contains("id") || !(*thread)["id"].is_string() ||
	    !thread->contains("cwd") || !(*thread)["cwd"].is_string() ||
	    !thread->contains("turns") || !(*thread)["turns"].is_array())
	{
		transcript.error = "Codex returned malformed thread history.";
		return transcript;
	}
	for (const nlohmann::json* scope : {&result, thread})
	{
		for (const char* key : {"nextCursor", "next_cursor"})
			if (scope->contains(key) && !(*scope)[key].is_null() && (!(*scope)[key].is_string() || !(*scope)[key].get_ref<const std::string&>().empty()))
			{ transcript.error = "Codex returned incomplete paginated history. Retry the import."; return transcript; }
		for (const char* key : {"hasMore", "has_more"})
			if (scope->contains(key) && (!(*scope)[key].is_boolean() || (*scope)[key].get<bool>()))
			{ transcript.error = "Codex returned incomplete paginated history. Retry the import."; return transcript; }
	}
	transcript.session_id = uam::codex::ValidThreadIdOrEmpty((*thread)["id"].get<std::string>());
	transcript.directory = (*thread)["cwd"].get<std::string>();
	if (transcript.session_id.empty() || transcript.directory.empty() ||
	    (*thread)["turns"].size() > kRemoteCodexMaxMessages)
	{
		transcript.error = "Codex returned invalid or oversized thread history.";
		return transcript;
	}
	bool unmapped = false;
	const auto reject_rich_item = []
	{
		RemoteCodexTranscript rejected;
		rejected.error = "Codex returned malformed reasoning, plan or compaction history.";
		return rejected;
	};
	const auto reasoning_section = [](const nlohmann::json& item, const char* field) -> std::optional<std::string>
	{
		const nlohmann::json* section = uam::nlohmann_json::FindField(item, field);
		if (section == nullptr || section->is_null())
			return std::string{};
		if (section->is_string())
			return section->get<std::string>();
		if (!section->is_array())
			return std::nullopt;
		std::string text;
		for (const auto& part : *section)
		{
			if (part.is_null())
				continue;
			if (part.is_string())
				AppendTranscriptText(text, part.get_ref<const std::string&>());
			else if (part.is_object())
			{
				const auto* value = uam::nlohmann_json::FindField(part, "text");
				if (value == nullptr || !value->is_string())
					return std::nullopt;
				AppendTranscriptText(text, value->get_ref<const std::string&>());
			}
			else
				return std::nullopt;
		}
		return text;
	};
	std::unordered_map<std::string, std::size_t> receipt_candidates;
	if (input_receipt_owner && uam::managed_parent_history::ValidCollection(input_receipt_owner->managed_parent_input_receipts) &&
	    transcript.session_id == input_receipt_owner->native_session_id &&
	    transcript.directory == input_receipt_owner->workspace_directory)
		for (const auto& candidate_turn : (*thread)["turns"])
		{
			if (!candidate_turn.is_object()) continue;
			const auto turn_id = CodexUserTurnProvenance::StrictId(candidate_turn, "id");
			const auto* items = uam::nlohmann_json::FindArrayField(candidate_turn, "items");
			if (!items) continue;
			for (const auto& item : *items)
				if (item.is_object() && uam::nlohmann_json::StringViewOrEmpty(item, "type") == "userMessage")
					if (const auto raw = RawCodexSingleTextInput(item, true))
						if (const auto* receipt = uam::managed_parent_history::FindBound(*input_receipt_owner,
						    transcript.session_id, transcript.directory, turn_id, *raw))
							++receipt_candidates[receipt->id];
		}
	for (const nlohmann::json& turn : (*thread)["turns"])
	{
		const nlohmann::json* items = uam::nlohmann_json::FindArrayField(turn, "items");
		if (items == nullptr)
		{
			transcript.error = "Codex returned malformed turn history.";
			transcript.messages.clear();
			return transcript;
		}
		if (transcript.messages.size() + items->size() > kRemoteCodexMaxMessages)
		{
			transcript.error = "Codex thread history contains too many items.";
			transcript.messages.clear();
			return transcript;
		}
		Message user_message;
		user_message.role = MessageRole::User;
		Message assistant_message;
		assistant_message.role = MessageRole::Assistant;
		for (const nlohmann::json& item : *items)
		{
			if (!item.is_object()) { unmapped = true; continue; }
			const std::string type = item.value("type", "");
			if (type == "userMessage")
			{
				if (input_receipt_owner && transcript.session_id == input_receipt_owner->native_session_id &&
				    transcript.directory == input_receipt_owner->workspace_directory)
					if (const auto raw = RawCodexSingleTextInput(item, true))
						if (const auto* receipt = uam::managed_parent_history::FindBound(*input_receipt_owner,
						    transcript.session_id, transcript.directory, CodexUserTurnProvenance::StrictId(turn, "id"), *raw);
						    receipt && receipt_candidates[receipt->id] == 1) continue;
				std::string text;
				if (const nlohmann::json* content =
				        uam::nlohmann_json::FindArrayField(item, "content"))
				{
					for (const nlohmann::json& part : *content)
					{
						if (part.is_object() && part.value("type", "") == "text" &&
						    part.contains("text") && part["text"].is_string())
							AppendTranscriptText(text, part["text"].get<std::string>());
						else unmapped = true;
					}
				}
				if (uam::nlohmann_json::FindArrayField(item, "content") == nullptr) unmapped = true;
				AppendTranscriptText(user_message.content, uam::CodexVisibleUserMessage(text));
			}
			else if (type == "agentMessage" && item.contains("text") &&
			         item["text"].is_string())
			{
				const std::string text = item["text"].get<std::string>();
				if (!text.empty())
				{
					AppendTranscriptText(assistant_message.content, text);
					assistant_message.blocks.push_back({"assistant_text", text, "", ""});
				}
			}
			else if (type == uam::acp_tool_items::kReasoning)
			{
				const auto content = reasoning_section(item, "content");
				const auto summary = reasoning_section(item, "summary");
				if (!content || !summary)
					return reject_rich_item();
				std::string thought;
				if (!content->empty())
					thought = "### Reasoning\n" + *content;
				if (!summary->empty())
					thought += (thought.empty() ? "" : "\n\n") + std::string("### Summary\n") + *summary;
				if (!thought.empty())
				{
					AppendTranscriptText(assistant_message.thoughts, thought);
					assistant_message.blocks.push_back({"thought", std::move(thought), "", ""});
				}
			}
			else if (type == uam::acp_tool_items::kPlan)
			{
				const auto* text = uam::nlohmann_json::FindField(item, "text");
				if (text == nullptr || !text->is_string())
					return reject_rich_item();
				assistant_message.plan_summary = text->get<std::string>();
				if (!assistant_message.plan_summary.empty())
					assistant_message.blocks.push_back({"plan", assistant_message.plan_summary, "", ""});
			}
			else if (type == "contextCompaction")
			{
				const auto* summary = uam::nlohmann_json::FindField(item, "summary");
				if (summary != nullptr && !summary->is_null() && !summary->is_string())
					return reject_rich_item();
				const std::string text = summary == nullptr || summary->is_null() ? "" : summary->get<std::string>();
				const std::string id = item.value("id", "");
				const std::string identity = "codex-compaction:" + turn.value("id", "") + ":" + id;
				const auto existing = std::ranges::find_if(assistant_message.blocks, [&](const MessageBlock& block) { return !id.empty() && block.type == "context_compaction" && block.request_id_json == identity; });
				if (existing == assistant_message.blocks.end())
					assistant_message.blocks.push_back({"context_compaction", text, "", identity});
				else if (!text.empty())
					existing->text = text;
			}
			else if (uam::acp_tool_items::IsCodexToolItemType(type) && item.contains("id") && item["id"].is_string())
			{
				for (const char* field : {"command", "status"})
				{
					const nlohmann::json* value = uam::nlohmann_json::FindField(item, field);
					if (value != nullptr && !value->is_null() && !value->is_string())
					{
						transcript.error = "Codex returned malformed tool history.";
						transcript.messages.clear();
						return transcript;
					}
				}
				ToolCall tool;
				tool.id = item["id"].get<std::string>();
				tool.name = uam::codex::ToolItemTitle(item);
				tool.args_json = uam::codex::ToolItemArguments(item);
				tool.status = uam::nlohmann_json::TrimmedStringValueOr(item, "status", "completed");
				tool.result_text = uam::codex::ToolItemContent(item);
				ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).ApplyNativeToolMetadata(tool, item);
				assistant_message.tool_calls.push_back(std::move(tool));
				assistant_message.blocks.push_back({"tool_call", "", assistant_message.tool_calls.back().id, ""});
			}
			else unmapped = true;
		}
		if (!uam::strings::IsBlank(user_message.content))
		{
			user_message.provider = uam::provider_ids::kCodexCli;
			transcript.messages.push_back(std::move(user_message));
		}
		if (!uam::strings::IsBlank(assistant_message.content) || !uam::strings::IsBlank(assistant_message.thoughts) || !assistant_message.tool_calls.empty() || !assistant_message.blocks.empty())
		{
			assistant_message.provider = uam::provider_ids::kCodexCli;
			transcript.messages.push_back(std::move(assistant_message));
		}
	}
	if (unmapped && !uam::HasImportedConversationContent(transcript.messages))
	{ transcript.error = "Codex history contains entries that could not be imported. Retry after updating the provider."; return transcript; }
	transcript.success = true;
	return transcript;
}
catch (const nlohmann::json::exception&)
{
	RemoteCodexTranscript rejected;
	rejected.error = "Codex returned malformed transcript field types.";
	return rejected;
}

ChatHistorySyncService::RemoteCodexTranscript
ChatHistorySyncService::LoadRemoteCodexTranscript(
    const ExecutionHost& host, const ChatSession& chat, std::stop_token stop_token) const
{
	RemoteCodexTranscript transcript;
	if (stop_token.stop_requested())
	{
		transcript.error = "Codex remote history loading was canceled.";
		return transcript;
	}
	const std::string valid_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	if (host.id == uam::execution_hosts::kLocalHostId || host.runner_status != "ready" ||
	    valid_id.empty() ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(host.platform, chat.workspace_directory))
	{
		transcript.error = "The remote Codex session is not ready to load.";
		return transcript;
	}
	uam::remote::RunnerClient client(
	    PlatformServicesFactory::Instance().process_service,
	    host);
	if (!client.Connect(&transcript.error)) return transcript;
	nlohmann::json read_result;
	if (!RunRemoteCodexRpc(client, uam::paths::PathFromUtf8(chat.workspace_directory),
	                       "thread/read", {{"threadId", valid_id}, {"includeTurns", true}},
	                       read_result, transcript.error, stop_token))
		return transcript;
	transcript = ParseRemoteCodexTranscript(read_result, &chat);
	if (transcript.success &&
	    (transcript.session_id != valid_id ||
	     !uam::execution_hosts::RemotePathsMatch(
	         host.platform, transcript.directory, chat.workspace_directory)))
	{
		transcript = {};
		transcript.error = "Codex returned a different thread or workspace.";
	}
	return transcript;
}

void ChatHistorySyncService::RetainHiddenAgentChatOwnership(uam::AppState& app, const ChatSession& chat) const
{
	if (chat.agent_run_id.empty()) return;
	uam::HiddenAgentChatOwnership& ownership = app.hidden_agent_chat_ownership;
	if (ownership.data_root != app.data_root)
	{
		ownership.data_root = app.data_root;
		ownership.chats.clear();
	}
	const std::vector<ChatSession>::iterator previous = std::ranges::find(ownership.chats, chat.id, &ChatSession::id);
	ChatSession metadata;
	metadata.id = chat.id;
	metadata.provider_id = chat.provider_id;
	metadata.model_id = chat.model_id;
	metadata.native_session_id = chat.native_session_id;
	metadata.folder_id = chat.folder_id;
	metadata.execution_host_id = chat.execution_host_id;
	metadata.workspace_directory = chat.workspace_directory;
	metadata.workspace_worktree_directory = chat.workspace_worktree_directory;
	metadata.agent_run_id = chat.agent_run_id;
	metadata.managed_followup_incarnation = chat.managed_followup_incarnation;
	metadata.title = chat.title;
	metadata.created_at = chat.created_at;
	metadata.updated_at = chat.updated_at;
	metadata.messages_loaded = false;
	metadata.persisted_message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
	metadata.persisted_messages_digest = chat.persisted_messages_digest;
	const bool draft_native_id = uam::strings::IsBlank(chat.native_session_id) || NativeSessionLinkService().IsLocalDraftChatId(chat.native_session_id);
	const std::unordered_map<std::string, std::string>::const_iterator resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
	if (draft_native_id && resolved != app.resolved_native_sessions_by_chat_id.end() && !uam::strings::IsBlank(resolved->second))
	{
		metadata.native_session_id = resolved->second;
	}
	else if (draft_native_id && previous != ownership.chats.end() &&
	    previous->agent_run_id == chat.agent_run_id &&
	    uam::provider_ids::CanonicalCliProviderLookupId(previous->provider_id) == uam::provider_ids::CanonicalCliProviderLookupId(chat.provider_id) &&
	    chat_identity::NativeWorkspaceForHistoryImport(*previous) == chat_identity::NativeWorkspaceForHistoryImport(chat))
	{
		metadata.native_session_id = previous->native_session_id;
	}
	// Only a unique current terminal run can bind a retirement or carry its existing proof.
	// Inspection metadata keeps its own fresh incarnation; it never becomes the wait's birth.
	if (app.chats.size() <= uam::ManagedSubagentControlService::MaxChatMetadata &&
	    app.agent_runs.size() <= uam::ManagedSubagentControlService::MaxLedgerRuns &&
	    ownership.chats.size() <= uam::ManagedSubagentControlService::MaxChatMetadata &&
	    std::ranges::count(app.agent_runs, chat.agent_run_id, &AgentRun::id) == 1 &&
	    std::ranges::count(app.agent_runs, chat.id, &AgentRun::transcript_chat_id) == 1)
	{
		const auto run = std::ranges::find(app.agent_runs, chat.agent_run_id, &AgentRun::id);
		const ChatSession* root = ChatDomainService().FindChatById(app, run->root_chat_id);
		const bool terminal = run->status == "completed" || run->status == "failed" ||
		                      run->status == "cancelled" || run->status == "interrupted";
		const auto live_count = std::ranges::count(app.chats, chat.id, &ChatSession::id);
		if (terminal && root && root->execution_host_id == chat.execution_host_id &&
		    run->transcript_chat_id == chat.id && run->provider_id == chat.provider_id)
		{
			if (live_count == 1 && ChatDomainService().FindChatById(app, chat.id) == &chat && chat.metadata_incarnation)
			{
				auto proof = std::make_shared<ManagedTerminalRetirementWitness>();
				proof->original_birth = chat.metadata_incarnation;
				proof->data_root = app.data_root;
				proof->run = ManagedTerminalRetirementWitness::RunIdentity::Capture(*run);
				proof->provider_id = chat.provider_id; proof->host_id = chat.execution_host_id;
				proof->workspace = chat.workspace_directory; proof->worktree = chat.workspace_worktree_directory;
				proof->native_session_id = chat.native_session_id; proof->created_at = chat.created_at;
				metadata.managed_terminal_retirement_witness = std::move(proof);
			}
			else if (live_count == 0 && previous != ownership.chats.end() &&
			    std::ranges::count(ownership.chats, chat.id, &ChatSession::id) == 1)
			{
				const auto proof = previous->managed_terminal_retirement_witness;
				const auto matches = [&](const ChatSession& source)
				{
					return proof && proof->original_birth && proof->data_root == app.data_root &&
					    proof->run == ManagedTerminalRetirementWitness::RunIdentity::Capture(*run) &&
					    source.id == proof->run.transcript_id && source.agent_run_id == proof->run.id &&
					    source.provider_id == proof->provider_id && source.execution_host_id == proof->host_id &&
					    source.workspace_directory == proof->workspace && source.workspace_worktree_directory == proof->worktree &&
					    source.native_session_id == proof->native_session_id && source.created_at == proof->created_at;
				};
				// Both the prior trusted record and incoming raw summary must match; native-ID
				// inference/aliases above cannot create or repair retirement provenance.
				if (matches(*previous) && matches(chat) && matches(metadata))
					metadata.managed_terminal_retirement_witness = proof;
			}
		}
	}
	if (previous == ownership.chats.end()) ownership.chats.push_back(std::move(metadata));
	else *previous = std::move(metadata);
}

namespace
{
	void RetainHiddenAgentChats(uam::AppState& app, std::vector<ChatSession>& chats)
	{
		std::vector<ChatSession> previous;
		if (app.hidden_agent_chat_ownership.data_root == app.data_root)
			previous = std::move(app.hidden_agent_chat_ownership.chats);
		app.hidden_agent_chat_ownership = {app.data_root, {}};
		std::erase_if(chats, [&](const ChatSession& chat)
		{
			if (chat.agent_run_id.empty()) return false;
			const std::vector<ChatSession>::const_iterator captured = std::ranges::find(previous, chat.id, &ChatSession::id);
			if (captured != previous.end() && std::ranges::count(previous, chat.id, &ChatSession::id) == 1)
				app.hidden_agent_chat_ownership.chats.push_back(*captured);
			ChatHistorySyncService().RetainHiddenAgentChatOwnership(app, chat);
			return true;
		});
	}
}

void ChatHistorySyncService::LoadSidebarChats(uam::AppState& app) const
{
	std::string warning;
	std::vector<ChatSession> chats = ChatRepository::LoadLocalChatSummaries(app.data_root, &warning);
	RetainHiddenAgentChats(app, chats);
	NormalizeLegacyOpenCodeChatsForSidebar(app, chats);
	app.excluded_provider_chats.clear(); // A full reload, possibly of another data folder.
	ReplaceAppChatsWithNormalized(app, std::move(chats));
	uam::ManagedSubagentControlService::LoadRestartInspection(app);
	if (!warning.empty())
	{
		app.status_line = warning;
	}
}

void ChatHistorySyncService::MergeSidebarChatsPreservingCurrent(uam::AppState& app, const std::vector<std::string>& published_chat_ids) const
{
	const std::unordered_set<std::string> published(published_chat_ids.begin(), published_chat_ids.end());
	std::string warning;
	std::vector<ChatSession> chats = ChatRepository::LoadLocalChatSummaries(app.data_root, &warning);
	// Validate retained retirement proofs against the still-visible original live owners.
	RetainHiddenAgentChats(app, chats);
	std::unordered_map<std::string, ChatSession> current_by_id;
	current_by_id.reserve(app.chats.size());
	for (ChatSession& chat : app.chats)
	{
		current_by_id.emplace(chat.id, std::move(chat));
	}

	for (ChatSession& chat : chats)
	{
		const auto current = current_by_id.find(chat.id);
		if (current == current_by_id.end())
		{
			continue;
		}
		if (!published.contains(chat.id))
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

	NativeImportIndex import_index = LoadNativeImportIndex(app);
	if (!import_index.tombstones_available)
	{
		result.Fail("Native-history deletion records could not be read; import stopped to avoid restoring deleted chats.");
		return result;
	}

	for (const ProviderChatSource& source : discovery.sources)
	{
		std::vector<ChatSession> native_chats = LoadNativeSessionChats(source.chats_dir, native_provider);
		for (ChatSession& native_chat : native_chats)
			if (uam::strings::IsBlank(native_chat.workspace_directory))
				native_chat.workspace_directory = source.folder_directory;
		std::erase_if(native_chats,
		              [&](const ChatSession& chat)
		              {
			              if (uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(chat)), uam::paths::PathFromUtf8(source.folder_directory)))
				              return false;
			              result.Fail("Native history ownership does not match its discovered workspace.");
			              return true;
		              });
		std::string overlay_error;
		if (!OverlayPartialNativeImport(*this, app, native_chats, overlay_error))
		{
			result.Fail(std::move(overlay_error));
			continue;
		}
		std::optional<std::string> import_folder_id;

		for (ChatSession& native_chat : native_chats)
		{
			if (!NativeChatShouldBeImported(source.chats_dir, native_chat, target_id, delete_native_after_import))
			{
				continue;
			}

			++result.total_count;

			const std::string attested_key = chat_identity::NativeIdentityKeyForHistoryImport(native_chat);
			const std::optional<std::string> native_key = PrepareNativeChatForImport(app.data_root, import_index, native_chat, target_id);
			if (!native_key)
			{
				continue;
			}

			if (*native_key != attested_key || chat_identity::NativeIdentityKeyForHistoryImport(native_chat) != attested_key)
			{
				result.Fail("Native history ownership changed before discovery import publication.");
				continue;
			}
			if (!PartialNativeImportPreservesLiveWork(app, native_chat))
				continue;
			const fs::path owner = uam::paths::PathFromUtf8(chat_identity::NativeWorkspaceForHistoryImport(native_chat));
			AssignKnownWorkspaceFolderToNewImport(app, import_index, owner, native_chat);

			if (native_chat.folder_id.empty())
			{
				if (!import_folder_id)
				{
					ProviderChatSource owned_source = source;
					owned_source.folder_directory = uam::paths::Utf8PathString(owner);
					import_folder_id = ResolvePersistedImportFolderIdForSource(app, owned_source);
					if (import_folder_id->empty()) result.Fail("Imported history from " + source.folder_directory + ", but could not save its workspace folder.");
				}

				native_chat.folder_id = *import_folder_id;
			}

			if (chat_identity::NativeIdentityKeyForHistoryImport(native_chat) != attested_key)
			{
				result.Fail("Native history ownership changed during import folder assignment.");
				continue;
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

std::optional<fs::path> ChatHistorySyncService::FindNativeSessionFilePath(const fs::path& chats_dir, const std::string& session_id, std::stop_token stop_token) const
{
	const std::string target_session_id = uam::strings::Trim(session_id);
	if (stop_token.stop_requested() || !uam::chat_ids::IsSafeStorageChatId(target_session_id) || chats_dir.empty() || !uam::paths::IsDirectoryNoThrow(chats_dir))
	{
		return std::nullopt;
	}

	const fs::path journal_name = chats_dir / (target_session_id + ".jsonl");
	if (uam::paths::PathExistsNoThrow(journal_name))
	{
		const auto metadata = uam::gemini::ReadHistoryMetadataFile(journal_name, stop_token);
		if (metadata && metadata->contains("sessionId") && (*metadata)["sessionId"].is_string() && (*metadata)["sessionId"] == target_session_id)
			return journal_name;
		return std::nullopt;
	}
	const fs::path canonical_name = chats_dir / (target_session_id + ".json");
	if (uam::paths::PathExistsNoThrow(canonical_name))
	{
		return canonical_name;
	}

	std::error_code error;
	for (fs::directory_iterator it(chats_dir, error), end; !error && !stop_token.stop_requested() && it != end; it.increment(error))
	{
		const fs::directory_entry& item = *it;
		if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".json") && !uam::paths::IsRegularFileWithExtensionNoThrow(item, ".jsonl"))
		{
			continue;
		}

		fs::path current_sibling = item.path();
		current_sibling += "l";
		if (item.path().extension() == ".json" && uam::paths::PathExistsNoThrow(current_sibling))
			continue;
		const auto metadata = uam::gemini::ReadHistoryMetadataFile(item.path(), stop_token);
		if (stop_token.stop_requested())
			return std::nullopt;
		if (!metadata || !metadata->contains("sessionId") || !(*metadata)["sessionId"].is_string() || (*metadata)["sessionId"] != target_session_id)
			continue;

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

	if (!uam::paths::IsControllerLocalWorkspace(chat) || !ProviderRuntime::UsesNativeOverlayHistory(provider))
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
	const fs::path journal = app.data_root / ".deletion-transaction" / "native-workspace.json";
	nlohmann::json workspace_records = nlohmann::json::object();
	bool legacy_record = false;
	const std::string workspace_key = folder.id.empty() ? uam::paths::Utf8PathString(workspace_root) : folder.id;
	fs::path source;
	fs::path staged;
	bool already_staged = false;
	std::error_code error;
	if (uam::paths::PathExistsNoThrow(journal))
	{
		const nlohmann::json recorded = nlohmann::json::parse(uam::io::ReadTextFile(journal), nullptr, false);
		if (!recorded.is_object() || (recorded.contains("workspaces") && !recorded["workspaces"].is_object()))
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::invalid_argument);
			return false;
		}
		// Legacy transactions contained exactly one workspace. Retain that record
		// under its owner when the batch journal is first updated.
		if (recorded.contains("workspaces")) workspace_records = recorded["workspaces"];
		else
		{
			workspace_records[workspace_key] = recorded;
			legacy_record = true;
		}
	}
	if (workspace_records.contains(workspace_key))
	{
		const nlohmann::json& recorded = workspace_records[workspace_key];
		if (!recorded.is_object() || !recorded.contains("source") || !recorded["source"].is_string() ||
		    (recorded.contains("staged") && !recorded["staged"].is_boolean()))
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::invalid_argument);
			return false;
		}
		source = uam::paths::PathFromUtf8(recorded["source"].get<std::string>());
		already_staged = recorded.value("staged", false);
		if (source.parent_path() != AppPaths::GeminiHomePath() / "tmp" || source.filename().empty() || source.filename() == "." || source.filename() == "..")
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::invalid_argument);
			return false;
		}
		if (legacy_record && !uam::io::WriteTextFileWithBackup(journal, nlohmann::json{{"workspaces", workspace_records}}.dump()))
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::io_error);
			return false;
		}
	}
	else
	{
		const auto tmp_dir = AppPaths::ResolveGeminiProjectTmpDir(workspace_root);
		if (!tmp_dir) return false;
		source = *tmp_dir;
		const std::string recorded_project_root = uam::strings::Trim(uam::io::ReadTextFile(source / ".project_root"));
		if (recorded_project_root.empty() || !FolderDirectoryMatches(workspace_root, uam::paths::PathFromUtf8(recorded_project_root))) return false;
		staged = source.parent_path().parent_path() / (fs::path(".uam-deleted-") += source.filename());
		if (uam::paths::PathExistsNoThrow(staged))
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::file_exists);
			return false;
		}
		workspace_records[workspace_key] = nlohmann::json{{"source", uam::paths::Utf8PathString(source)}};
		if (!uam::io::WriteTextFileWithBackup(journal, nlohmann::json{{"workspaces", workspace_records}}.dump()))
		{
			if (error_out != nullptr) *error_out = std::make_error_code(std::errc::io_error);
			return false;
		}
	}
	// Stage outside Gemini's tmp discovery root; interrupted cleanup cannot be imported.
	staged = source.parent_path().parent_path() / (fs::path(".uam-deleted-") += source.filename());
	if (!already_staged && !uam::paths::PathExistsNoThrow(staged) && uam::paths::PathExistsNoThrow(source))
	{
#if defined(_WIN32)
		if (!MoveFileExW(source.c_str(), staged.c_str(), MOVEFILE_WRITE_THROUGH))
			error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
		fs::rename(source, staged, error);
#endif
	}
#if !defined(_WIN32)
	if (!error && !already_staged)
	{
		uam::io::AtomicWriteResult sync_result;
		if (!uam::io::SyncAtomicWriteDirectory(source.parent_path(), sync_result) ||
		    !uam::io::SyncAtomicWriteDirectory(staged.parent_path(), sync_result)) error = std::make_error_code(std::errc::io_error);
	}
#endif
	if (!error && !already_staged)
	{
		workspace_records[workspace_key] = nlohmann::json{{"source", uam::paths::Utf8PathString(source)}, {"staged", true}};
		if (!uam::io::WriteTextFileWithBackup(journal, nlohmann::json{{"workspaces", workspace_records}}.dump()))
			error = std::make_error_code(std::errc::io_error);
	}
	if (!error && uam::paths::PathExistsNoThrow(staged))
	{
		if (uam::paths::IsLinkOrReparsePointNoThrow(staged)) error = std::make_error_code(std::errc::invalid_argument);
		else uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(staged, &error);
	}
#if !defined(_WIN32)
	if (!error)
	{
		uam::io::AtomicWriteResult sync_result;
		if (!uam::io::SyncAtomicWriteDirectory(staged.parent_path(), sync_result)) error = std::make_error_code(std::errc::io_error);
	}
#endif
	if (error_out != nullptr) *error_out = error;

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
	if (uam::AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, chat))
	{
		app.status_line = "Wait for the saved request to be confirmed before moving this chat.";
		return false;
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
		if (ChatNeedsTranscriptPreserved(app, chat) || uam::ChatHasActiveCliTerminal(app, chat.id))
		{
			app.status_line = "Stop the chat before moving its workspace.";
			return false;
		}
		for (const std::unique_ptr<uam::AcpSessionState>& session : app.acp_sessions)
		{
			if (session != nullptr && session->chat_id == chat.id && session->running)
			{
				app.status_line = "Stop the chat before moving its workspace.";
				return false;
			}
		}
		ChatSession moved_chat = chat;
		if (!ChatRepository::HydrateChatMessages(app.data_root, moved_chat)) return false;
		std::string error;
		if (!uam::chat::BuildConversationHandoff(moved_chat, moved_chat.provider_handoff_context, &error))
		{
			app.status_line = error;
			return false;
		}
		moved_chat.folder_id = target_folder_id;
		moved_chat.workspace_directory = new_folder->directory;
		moved_chat.native_session_id.clear();
		moved_chat.native_session_reset_pending = true;
		moved_chat.provider_handoff_session_id.clear();
		moved_chat.updated_at = uam::time::TimestampNow();
		if (!ChatRepository::SaveChat(app.data_root, moved_chat)) return false;
		chat = std::move(moved_chat);
		app.resolved_native_sessions_by_chat_id.erase(chat.id);
		return true;
	}

	if (!uam::StopAndEraseCliTerminalForChat(app, chat.id))
	{
		app.status_line = "The terminal is still stopping. Retry moving the chat after it exits.";
		return false;
	}
	// A workspace move changes the native ownership key. Let the captured final
	// finish under its original workspace before any moved-chat mutation/save.
	if (app.terminal_native_media && std::ranges::any_of(app.terminal_native_media->jobs, [&](const auto& work)
	    { return work->final && work->expected.id == chat.id; }))
	{
		app.status_line = "Final terminal history is being saved. Retry moving this chat after it finishes.";
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

void ChatHistorySyncService::RollbackOpenNativeSessionChatImport(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id) const
{
	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
	if (chat_index >= 0)
	{
		app.chats.erase(app.chats.begin() + chat_index);
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
	ChatSession* best_match = nullptr;
	int best_priority = 4;

	for (ChatSession& chat : app.chats)
	{
		if (!NativeHistoryWorkspacesMatch(app, source_chat, chat)) continue;
		const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
		if (!chat_provider_id.empty() && chat_provider_id != source_provider_id)
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
                                                                         bool persist_provider_normalization,
                                                                         const ChatSession* native_snapshot,
                                                                         bool native_snapshot_prepared, std::stop_token stop_token, const std::vector<ChatSession>* prepared_local_summaries) const
{
	if (stop_token.stop_requested()) return nullptr;
	if (!uam::paths::IsControllerLocalWorkspace(source_chat_reference) && native_snapshot == nullptr)
	{
		return nullptr;
	}
	const std::string target_native_session_id = uam::strings::Trim(native_session_id);
	if (target_native_session_id.empty())
	{
		return nullptr;
	}

	const ChatSession& source_chat = source_chat_reference;
	const std::string source_workspace_directory = uam::strings::Trim(source_chat.workspace_directory);
	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);
	if (native_snapshot != nullptr && native_snapshot->imported_read_only && native_snapshot->native_session_id.empty())
	{
		// A parent-owned agent journal is a read-only UAM transcript, never a
		// resumable native session. Keep its reference out of native UUID mappings.
		const auto reference = uam::native_agents::ParseHistoryId(target_native_session_id);
		if (!reference || reference->provider_id != source_provider_id || reference->parent_session_id != source_chat.native_session_id || reference->execution_host_id != uam::strings::NonEmptyOrFallback(source_chat.execution_host_id, "local") || native_snapshot->id != target_native_session_id || native_snapshot->parent_chat_id != reference->parent_session_id || uam::provider_ids::NormalizeCliProviderAliasOrSelf(native_snapshot->provider_id) != source_provider_id || !NativeHistoryWorkspacesMatch(app, source_chat, *native_snapshot))
			return nullptr;
		const NativeImportTombstones tombstones = LoadNativeImportTombstones(app.data_root);
		if (!tombstones.available || tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(*native_snapshot)))
			return nullptr;
		for (ChatSession& existing : app.chats)
		{
			if (existing.id != target_native_session_id)
				continue;
			if (!existing.imported_read_only || !existing.native_session_id.empty() || existing.provider_id != source_provider_id || existing.parent_chat_id != reference->parent_session_id || !NativeHistoryWorkspacesMatch(app, source_chat, existing) || ChatNeedsTranscriptPreserved(app, existing) || uam::ChatHasActiveCliTerminal(app, existing.id))
				return nullptr;
			const auto incarnation = existing.metadata_incarnation;
			existing = *native_snapshot;
			existing.metadata_incarnation = incarnation;
			app.resolved_native_sessions_by_chat_id.erase(existing.id);
			return &existing;
		}
		app.chats.push_back(*native_snapshot);
		app.resolved_native_sessions_by_chat_id.erase(app.chats.back().id);
		return &app.chats.back();
	}
	auto existing_matches_source_workspace = [&](const ChatSession& chat) {
		return NativeHistoryWorkspacesMatch(app, source_chat, chat);
	};

	std::vector<ChatSession> candidate_chats;
	if (native_snapshot != nullptr)
	{
		if (!NativeHistoryWorkspacesMatch(app, source_chat, *native_snapshot) ||
		    native_snapshot->native_session_id != target_native_session_id ||
		    uam::provider_ids::NormalizeCliProviderAliasOrSelf(native_snapshot->provider_id) != source_provider_id) return nullptr;
		const NativeImportTombstones tombstones = LoadNativeImportTombstones(app.data_root);
		if (!tombstones.available || tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(*native_snapshot))) return nullptr;
		ChatSession* active = FindInMemoryNativeSessionChatForOpen(app, source_chat, provider, target_native_session_id, false);
		if (active != nullptr && (ChatNeedsTranscriptPreserved(app, *active) || uam::ChatHasActiveCliTerminal(app, active->id))) return active;
		candidate_chats.push_back(*native_snapshot);
	}
	else if (ProviderRuntime::UsesNativeOverlayHistory(provider))
	{
		const fs::path chats_dir = prepared_local_summaries == nullptr
		    ? ResolveNativeHistoryChatsDirForChat(app, source_chat)
		    : ProviderRuntimeRegistry::Resolve(provider).GetNativeSessionDirectory(uam::paths::ResolveControllerWorkspaceRootPath(app, source_chat));
		if (chats_dir.empty())
		{
			return nullptr;
		}

		candidate_chats = LoadNativeSessionChats(chats_dir, provider, stop_token);
		// Gemini records omit workspace metadata; bind them to the captured directory lookup before identity checks.
		for (ChatSession& candidate : candidate_chats)
		{
			if (candidate.workspace_directory.empty()) candidate.workspace_directory = chat_identity::NativeWorkspaceForHistoryImport(source_chat);
		}
	}
	else if (ProviderRuntime::UsesLocalHistory(provider))
	{
		if (source_provider_id == uam::provider_ids::kCodexCli && !source_workspace_directory.empty())
		{
			ChatSession request;
			request.native_session_id = target_native_session_id;
			request.workspace_directory = source_workspace_directory;
			request.folder_id = source_chat.folder_id;
			std::string error;
			std::optional<ChatSession> native_chat = LoadLocalCodexChildChat(request, &error);
			if (!error.empty()) return nullptr;
			if (native_chat)
			{
				const NativeImportTombstones tombstones = LoadNativeImportTombstones(app.data_root);
				if (!tombstones.available || tombstones.keys.contains(chat_identity::NativeIdentityKeyForHistoryImport(*native_chat))) return nullptr;
				candidate_chats.push_back(std::move(*native_chat));
			}
		}
		if (candidate_chats.empty()) candidate_chats = prepared_local_summaries == nullptr ? ChatRepository::LoadLocalChats(app.data_root) : *prepared_local_summaries;
		if (prepared_local_summaries != nullptr)
			for (ChatSession& candidate : candidate_chats)
				if (!ChatRepository::HydrateChatMessages(app.data_root, candidate)) return nullptr;
	}
	else
	{
		return nullptr;
	}

	if (!source_workspace_directory.empty())
	{
		std::erase_if(candidate_chats, [&](const ChatSession& chat)
		{
			return !NativeHistoryWorkspacesMatch(app, source_chat, chat);
		});
	}

	if (native_snapshot == nullptr || !native_snapshot_prepared)
	{
		if (uam::paths::IsControllerLocalWorkspace(source_chat))
		{
			std::string overlay_error;
			(void)OverlayLocalHistory(app, candidate_chats, false, prepared_local_summaries, &overlay_error);
			if (!overlay_error.empty()) return nullptr;
		}
		else
		{
			const NativeImportIndex index = LoadNativeImportIndex(app, prepared_local_summaries, true);
			if (!index.tombstones_available) return nullptr;
			for (ChatSession& candidate : candidate_chats)
			{
				const auto saved = index.existing_summary_by_native_key.find(chat_identity::NativeIdentityKeyForHistoryImport(candidate));
				if (saved == index.existing_summary_by_native_key.end()) continue;
				const std::optional<ChatSession> local = ChatRepository::LoadLocalChat(app.data_root, saved->second.id, true);
				if (!local) return nullptr;
				candidate.id = local->id;
				OverlayLocalChatState(*local, candidate);
				PreserveEquivalentLocalMessageDetails(*local, candidate);
			}
		}

	}
	if (stop_token.stop_requested()) return nullptr;
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
			if (!NativeHistoryWorkspacesMatch(app, source_chat, chat)) continue;
			const std::string chat_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
			if (!chat_provider_id.empty() && chat_provider_id != source_provider_id)
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
				loaded_raw_match->native_session_reset_pending = false;
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
				resolved_only_match->native_session_reset_pending = false;
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
				existing->native_session_reset_pending = false;
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
				existing->native_session_reset_pending = false;
			existing->updated_at = uam::time::TimestampNow();
			app.resolved_native_sessions_by_chat_id[existing->id] = target_native_session_id;
			return existing;
		}

		return nullptr;
	}
}

void ChatHistorySyncService::OverlayPreparedLocalHistory(const ChatSession& local_chat, ChatSession& native_chat) const
{
	const bool same_native_owner = !local_chat.native_session_id.empty() &&
	    chat_identity::NativeIdentityKeyForHistoryImport(local_chat) == chat_identity::NativeIdentityKeyForHistoryImport(native_chat);
	const bool receipt_snapshot_changed = local_chat.provider_id == uam::provider_ids::kCodexCli &&
	    local_chat.managed_parent_input_receipts != native_chat.managed_parent_input_receipts;
	OverlayLocalChatState(local_chat, native_chat);
	if (receipt_snapshot_changed)
	{
		// An unowned/stale external projection cannot replace the retained canonical graph.
		native_chat.messages = local_chat.messages; native_chat.messages_loaded = local_chat.messages_loaded;
		native_chat.persisted_message_count = local_chat.persisted_message_count;
		native_chat.persisted_messages_digest = local_chat.persisted_messages_digest;
		native_chat.updated_at = local_chat.updated_at;
		return;
	}
	const bool should_copy_local_messages = LocalMessagesShouldOverrideNative(local_chat, native_chat);

	const auto recency = uam::time::CompareHistoryTimestamps(local_chat.updated_at, native_chat.updated_at);
	const bool local_update_is_newer = recency.order == uam::time::HistoryTimestampOrder::After;
	const bool retain_authoritative_tools = should_copy_local_messages && same_native_owner && recency.order == uam::time::HistoryTimestampOrder::Unknown &&
	    UnknownHistoryCanRetainAuthoritativeNativeTools(local_chat, native_chat);

	if (should_copy_local_messages && !retain_authoritative_tools)
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
	else if (local_chat.messages_loaded && native_chat.messages_loaded)
	{
		PreserveEquivalentLocalMessageDetails(local_chat, native_chat);
		if (local_update_is_newer || (retain_authoritative_tools && !local_chat.updated_at.empty()))
		{
			native_chat.updated_at = local_chat.updated_at;
		}
	}
}

std::vector<ChatSession> ChatHistorySyncService::OverlayLocalHistory(const uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links, const std::vector<ChatSession>* prepared_local_summaries, std::string* error_out) const
{
	if (error_out != nullptr) error_out->clear();
	const auto fail_hydration = [&native_chats, error_out]() -> std::vector<ChatSession>
	{
		if (error_out != nullptr)
		{
			*error_out = "Saved chat history could not be read. Reopen the chat and try again.";
		}
		native_chats.clear();
		return {};
	};
	native_chats = ChatDomainService().DeduplicateChatsById(std::move(native_chats));
	std::vector<ChatSession> local_chats = prepared_local_summaries == nullptr ? ChatRepository::LoadLocalChatSummaries(app.data_root) : *prepared_local_summaries;

	for (ChatSession& local_chat : local_chats)
	{
		if (!persist_local_draft_links || !LocalDraftCanInferNativeSessionLink(app, local_chat))
		{
			continue;
		}

		if (!ChatRepository::HydrateChatMessages(app.data_root, local_chat))
		{
			return fail_hydration();
		}
		const auto inferred_session_id = NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(local_chat, native_chats);

		if (inferred_session_id && PersistLocalDraftNativeSessionLink(app, local_chat, *inferred_session_id))
		{
			local_chat.native_session_id = *inferred_session_id;
			local_chat.native_session_reset_pending = false;
		}
	}

	local_chats = ChatDomainService().DeduplicateChatsById(std::move(local_chats));
	const LocalChatOverlayIndex local_index = BuildLocalChatOverlayIndex(local_chats);

	for (ChatSession& native_chat : native_chats)
	{
		ChatSession* local_match = FindLocalOverlayMatch(app, local_index, native_chat);
		if (local_match == nullptr)
		{
			continue;
		}

		ChatSession& local_chat = *local_match;
		OverlayLocalChatState(local_chat, native_chat);

		bool should_copy_local_messages = LocalMessagesShouldOverrideNative(local_chat, native_chat);
		const bool has_local_message_details = native_chat.messages_loaded &&
		    EffectiveMessageCount(local_chat) > 0;
		if ((should_copy_local_messages || has_local_message_details) && !local_chat.messages_loaded)
		{
			if (!ChatRepository::HydrateChatMessages(app.data_root, local_chat))
			{
				return fail_hydration();
			}
			should_copy_local_messages = LocalMessagesShouldOverrideNative(local_chat, native_chat);
		}
		OverlayPreparedLocalHistory(local_chat, native_chat);
	}

	return local_chats;
}

void ChatHistorySyncService::ApplyLocalOverrides(uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links) const
{
	std::string deletion_error;
	if (!FilterDeletedNativeHistory(app.data_root, native_chats, &deletion_error))
	{
		native_chats.clear();
		app.status_line = deletion_error;
		return;
	}
	const std::string selected_chat_id = ChatDomainService().SelectedChatId(app);
	std::string overlay_error;
	const std::vector<ChatSession> local_chats = OverlayLocalHistory(app, native_chats, persist_local_draft_links, nullptr, &overlay_error);
	if (!overlay_error.empty())
	{
		app.status_line = overlay_error;
		return;
	}
	std::unordered_set<std::string> native_ids;
	for (const ChatSession& native_chat : native_chats) native_ids.insert(native_chat.id);
	std::vector<ChatSession> merged_chats = native_chats;

	for (const ChatSession& local_chat : local_chats)
	{
		if (!uam::paths::IsControllerLocalWorkspace(local_chat))
		{
			merged_chats.push_back(local_chat);
			continue;
		}
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
