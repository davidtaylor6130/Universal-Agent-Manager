#include "transcript_retention_service.h"
#include "app/chat_domain_service.h"
#include "app/runtime_orchestration_internal.h"
#include "app/uam_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/string_utils.h"
#include <algorithm>
#include "cef/state_serializer.h"
#include "common/paths/app_paths.h"
#include "common/utils/io_utils.h"

namespace uam
{
	bool MessageTailCache::Attach(const std::filesystem::path&, const ChatSession& chat, const std::shared_ptr<PreparedChatSave>& prepared)
	try
	{
		if (!prepared || !chat.messages_loaded || prepared->primary_revision.empty()) return false;
		constexpr std::size_t text_budget = 256 * 1024;
		std::size_t text_bytes = 0;
		std::size_t count = 0;
		for (std::size_t index = chat.messages.size(); index > 0 && count < 50; --index)
		{
			const Message& message = chat.messages[index - 1];
			if (message.content.size() > text_budget || message.thoughts.size() > text_budget - message.content.size()) break;
			std::size_t message_bytes = message.content.size() + message.thoughts.size();
			for (const MessageBlock& block : message.blocks)
			{
				if (block.text.size() > text_budget - message_bytes)
				{
					message_bytes = text_budget + 1;
					break;
				}
				message_bytes += block.text.size();
			}
			if (message_bytes > text_budget - text_bytes) break;
			text_bytes += message_bytes;
			++count;
		}
		if (count == 0 && !chat.messages.empty()) return false;
		std::string text;
		if (!io::TryReadTextFile(prepared->summary_temp, text, 4 * 1024 * 1024)) return false;
		nlohmann::json summary = nlohmann::json::parse(text, nullptr, false);
		if (!summary.is_object() || summary.value("id", "") != chat.id) return false;
		for (;;)
		{
			summary["frontendMessageTail"] = {{"version", 2}, {"sourceRevision", prepared->primary_revision},
			    {"page", StateSerializer::SerializeMessagePage(chat, count, std::nullopt, true)}};
			text = summary.dump();
			if (text.size() <= 512 * 1024) break;
			if (count <= 1) return false;
			// Keep complete messages; metadata can exceed the text-only budget too.
			count /= 2;
		}
		const std::filesystem::path replacement = io::MakeTempWritePath(prepared->summary_path);
		io::AtomicWriteResult result;
		if (!io::WriteAndSyncAtomicTemp(replacement, text, {}, result)) { io::RemoveAtomicTempNoThrow(replacement); return false; }
		io::RemoveAtomicTempNoThrow(prepared->summary_temp);
		prepared->summary_temp = replacement;
		return true;
	}

	catch (const std::exception&) { return false; }

	std::shared_ptr<PreparedChatSave> MessageTailCache::Prepare(const std::filesystem::path& data_root, const ChatSession& chat)
	try
	{
		const std::optional<std::string> revision = ChatRepository::ChatStorageRevision(data_root, chat.id);
		if (!revision || *revision == "missing") return nullptr;
		std::shared_ptr<PreparedChatSave> prepared = std::make_shared<PreparedChatSave>();
		prepared->primary_path = AppPaths::UamChatFilePath(data_root, chat.id);
		prepared->summary_path = AppPaths::UamChatSummaryFilePath(data_root, chat.id);
		prepared->summary_temp = io::MakeTempWritePath(prepared->summary_path);
		prepared->expected_storage_revision = *revision;
		prepared->primary_revision = *revision;
		const std::optional<std::string> summary_revision = ChatRepository::ChatSummaryStorageRevision(data_root, chat.id);
		if (!summary_revision) return nullptr;
		prepared->expected_summary_revision = *summary_revision;
		std::string text;
		if (!io::TryReadTextFile(prepared->summary_path, text, 4 * 1024 * 1024)) return nullptr;
		const nlohmann::json summary = nlohmann::json::parse(text, nullptr, false);
		if (!summary.is_object() || summary.value("persisted_message_count", std::size_t(0)) != chat.messages.size()) return nullptr;
		io::AtomicWriteResult result;
		if (!io::WriteAndSyncAtomicTemp(prepared->summary_temp, text, {}, result) || !Attach(data_root, chat, prepared)) return nullptr;
		return prepared;
	}

	catch (const std::exception&) { return nullptr; }

	std::optional<nlohmann::json> MessageTailCache::Read(const std::filesystem::path& data_root, std::string_view chat_id,
	    std::size_t expected_count, std::string_view expected_digest, std::size_t limit, std::string_view known_digest)
	try
	{
		if (limit == 0 || limit > 50) return std::nullopt;
		const std::optional<std::string> revision = ChatRepository::ChatStorageRevision(data_root, chat_id);
		if (!revision || *revision == "missing") return std::nullopt;
		std::string text;
		if (!io::TryReadTextFile(AppPaths::UamChatSummaryFilePath(data_root, chat_id), text, 4 * 1024 * 1024)) return std::nullopt;
		const nlohmann::json summary = nlohmann::json::parse(text, nullptr, false);
		if (!summary.is_object() || summary.value("id", "") != chat_id ||
		    summary.value("persisted_message_count", std::size_t(0)) != expected_count || summary.value("persisted_messages_digest", "") != expected_digest ||
		    !summary.contains("frontendMessageTail")) return std::nullopt;
		const nlohmann::json& tail = summary["frontendMessageTail"];
		if (!tail.is_object() || tail.value("version", 0) != 2 || tail.value("sourceRevision", "") != *revision || !tail.contains("page")) return std::nullopt;
		nlohmann::json page = tail["page"];
		if (!page.is_object() || !page.contains("messages") || !page["messages"].is_array() ||
		    page.value("totalCount", std::size_t(0)) != expected_count || page["messages"].size() > std::min(expected_count, std::size_t(50)) ||
		    (expected_count > 0 && page["messages"].empty()) ||
		    page.value("startIndex", std::size_t(0)) != expected_count - page["messages"].size() || !page.contains("messagesDigest") || !page["messagesDigest"].is_string()) return std::nullopt;
		for (const nlohmann::json& message : page["messages"])
			if (!message.is_object() || !message.contains("role") || !message["role"].is_string() ||
			    !message.contains("content") || !message["content"].is_string()) return std::nullopt;
		if (ChatRepository::ChatStorageRevision(data_root, chat_id) != revision) return std::nullopt;
		const std::size_t size = std::min(limit, page["messages"].size());
		page["messages"].erase(page["messages"].begin(), page["messages"].end() - static_cast<std::ptrdiff_t>(size));
		page["startIndex"] = expected_count - size;
		page["chatId"] = chat_id;
		page["unchanged"] = !known_digest.empty() && page["messagesDigest"].get_ref<const std::string&>() == known_digest;
		if (page["unchanged"].get<bool>()) page.erase("messages");
		return page;
	}
	catch (const std::exception&) { return std::nullopt; }

	namespace
	{
		bool IsTerminalManagedRun(std::string_view status)
		{
			return status == "completed" || status == "failed" || status == "cancelled" || status == "interrupted";
		}

		std::optional<ExecutionHost> CaptureTranscriptHost(const AppState& app, std::string_view host_id)
		{
			const std::string normalized = uam::strings::NonEmptyOrFallback(uam::strings::Trim(host_id), "local");
			if (normalized == "local") return std::nullopt;
			const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, normalized);
			if (host == nullptr) return std::nullopt;
			return *host;
		}

		bool HostStillMatches(const AppState& app, const std::optional<ExecutionHost>& expected,
		                      std::string_view host_id)
		{
			const std::string normalized = uam::strings::NonEmptyOrFallback(uam::strings::Trim(host_id), "local");
			if (normalized == "local") return !expected.has_value();
			if (!expected) return false;
			const ExecutionHost* current = uam::execution_hosts::Find(app.settings.execution_hosts, normalized);
			return current != nullptr && uam::execution_hosts::SameConnection(*current, *expected);
		}

		void CaptureTranscriptChatIdentity(const AppState& app, ManagedAgentTranscriptReadIdentity& identity, const ChatSession& chat)
		{
			identity.transcript_chat_id = chat.id;
			identity.transcript_provider_id = chat.provider_id;
			identity.transcript_execution_host_id = chat.execution_host_id;
			identity.transcript_native_session_id = chat.native_session_id;
			identity.transcript_workspace_directory = chat.workspace_directory;
			identity.transcript_folder_id = chat.folder_id;
			identity.transcript_updated_at = chat.updated_at;
			identity.transcript_native_identity_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
			identity.transcript_message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
			identity.transcript_messages_digest = chat.persisted_messages_digest;
			identity.transcript_host = CaptureTranscriptHost(app, chat.execution_host_id);
		}

		bool TranscriptIdentityMatches(const ManagedAgentTranscriptReadIdentity& expected,
		                               const ChatSession& chat)
		{
			const std::size_t message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
			return chat.id == expected.transcript_chat_id && chat.agent_run_id == expected.run_id &&
			       chat.provider_id == expected.transcript_provider_id &&
			       chat.execution_host_id == expected.transcript_execution_host_id &&
			       chat.native_session_id == expected.transcript_native_session_id &&
			       chat.workspace_directory == expected.transcript_workspace_directory &&
			       chat.folder_id == expected.transcript_folder_id && chat.updated_at == expected.transcript_updated_at &&
			       uam::chat_identity::NativeIdentityKeyForHistoryImport(chat) == expected.transcript_native_identity_key &&
			       message_count == expected.transcript_message_count &&
			       chat.persisted_messages_digest == expected.transcript_messages_digest;
		}
	}

	std::optional<ManagedAgentTranscriptReadIdentity> CaptureManagedAgentTranscriptRead(
	    const AppState& app, std::string_view root_chat_id, std::string_view transcript_chat_id)
	{
		const ChatSession* root = ChatDomainService().FindChatById(app, std::string(root_chat_id));
		if (root == nullptr || !root->agent_run_id.empty()) return std::nullopt;
		const auto run = std::ranges::find_if(app.agent_runs, [&](const AgentRun& candidate) {
			return candidate.root_chat_id == root_chat_id && candidate.transcript_chat_id == transcript_chat_id;
		});
		if (run == app.agent_runs.end() || !IsTerminalManagedRun(run->status) ||
		    (run->deliver_result_to_root_chat && !run->root_result_delivered)) return std::nullopt;

		ManagedAgentTranscriptReadIdentity identity;
		identity.root_chat_id = root->id;
		identity.root_provider_id = root->provider_id;
		identity.root_execution_host_id = root->execution_host_id;
		identity.root_native_session_id = root->native_session_id;
		identity.root_workspace_directory = root->workspace_directory;
		identity.root_folder_id = root->folder_id;
		identity.root_native_identity_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(*root);
		identity.root_host = CaptureTranscriptHost(app, root->execution_host_id);
		if (uam::strings::NonEmptyOrFallback(uam::strings::Trim(root->execution_host_id), "local") != "local" && !identity.root_host)
			return std::nullopt;
		identity.run_id = run->id;
		identity.run_agent_id = run->agent_id;
		identity.run_provider_id = run->provider_id;
		identity.run_execution_capability = run->execution_capability;
		identity.run_resumed_from_run_id = run->resumed_from_run_id;
		identity.run_status = run->status;
		identity.run_updated_at = run->updated_at;
		identity.run_delivers_result = run->deliver_result_to_root_chat;
		identity.run_result_delivered = run->root_result_delivered;
		identity.transcript_chat_id = std::string(transcript_chat_id);

		const ChatSession* transcript = ChatDomainService().FindChatById(app, std::string(transcript_chat_id));
		if (transcript == nullptr || transcript->agent_run_id != run->id || transcript->provider_id != run->provider_id ||
		    transcript->messages_loaded ||
		    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(app, *transcript) ||
		    ChatHasActiveCliTerminal(app, transcript->id)) return std::nullopt;
		CaptureTranscriptChatIdentity(app, identity, *transcript);
		if (uam::strings::NonEmptyOrFallback(uam::strings::Trim(identity.transcript_execution_host_id), "local") != "local" && !identity.transcript_host)
			return std::nullopt;
		return identity;
	}

	ManagedAgentTranscriptReadResult LoadManagedAgentTranscriptDetached(
	    const std::filesystem::path& data_root, const ManagedAgentTranscriptReadIdentity& expected)
	{
		ManagedAgentTranscriptReadResult result;
		std::string warning;
		std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(data_root, expected.transcript_chat_id, true, &warning);
		if (!loaded)
		{
			result.status = ManagedAgentTranscriptReadStatus::Unavailable;
			result.error = uam::strings::NonEmptyOrFallback(warning, "Managed agent transcript is unavailable.");
			return result;
		}
		if (!loaded->messages_loaded || !TranscriptIdentityMatches(expected, *loaded))
		{
			result.status = ManagedAgentTranscriptReadStatus::Changed;
			result.error = "Managed agent transcript changed while it was loading.";
			return result;
		}
		result.status = ManagedAgentTranscriptReadStatus::Ready;
		result.transcript = std::move(*loaded);
		return result;
	}

	bool MatchesManagedAgentTranscriptRead(const AppState& app,
	                                      const ManagedAgentTranscriptReadIdentity& expected,
	                                      const ChatSession& loaded_transcript)
	{
		const ChatSession* root = ChatDomainService().FindChatById(app, expected.root_chat_id);
		if (root == nullptr || !root->agent_run_id.empty() || root->provider_id != expected.root_provider_id ||
		    root->execution_host_id != expected.root_execution_host_id ||
		    root->native_session_id != expected.root_native_session_id ||
		    root->workspace_directory != expected.root_workspace_directory || root->folder_id != expected.root_folder_id ||
		    uam::chat_identity::NativeIdentityKeyForHistoryImport(*root) != expected.root_native_identity_key ||
		    !HostStillMatches(app, expected.root_host, root->execution_host_id)) return false;

		const auto run = std::ranges::find_if(app.agent_runs, [&](const AgentRun& candidate) {
			return candidate.id == expected.run_id && candidate.root_chat_id == expected.root_chat_id &&
			       candidate.transcript_chat_id == expected.transcript_chat_id;
		});
		if (run == app.agent_runs.end() || run->agent_id != expected.run_agent_id ||
		    run->provider_id != expected.run_provider_id || run->execution_capability != expected.run_execution_capability ||
		    run->resumed_from_run_id != expected.run_resumed_from_run_id ||
		    run->status != expected.run_status || run->updated_at != expected.run_updated_at ||
		    run->deliver_result_to_root_chat != expected.run_delivers_result ||
		    run->root_result_delivered != expected.run_result_delivered || !IsTerminalManagedRun(run->status) ||
		    (run->deliver_result_to_root_chat && !run->root_result_delivered)) return false;

		const ChatSession* current = ChatDomainService().FindChatById(app, expected.transcript_chat_id);
		if (current != nullptr)
		{
			if (current->messages_loaded ||
			    uam::runtime_orch_impl::ChatNeedsTranscriptPreserved(app, *current) ||
			    ChatHasActiveCliTerminal(app, current->id) ||
			    !TranscriptIdentityMatches(expected, *current)) return false;
		}
		else return false;
		return loaded_transcript.id == expected.transcript_chat_id &&
		       loaded_transcript.agent_run_id == expected.run_id &&
		       loaded_transcript.provider_id == expected.transcript_provider_id &&
		       loaded_transcript.execution_host_id == expected.transcript_execution_host_id &&
		       loaded_transcript.workspace_directory == expected.transcript_workspace_directory &&
		       loaded_transcript.folder_id == expected.transcript_folder_id &&
		       HostStillMatches(app, expected.transcript_host, loaded_transcript.execution_host_id) &&
		       TranscriptIdentityMatches(expected, loaded_transcript);
	}

	ToolCallContentIdentity CaptureToolCallContentIdentity(
	    const ChatSession& chat, std::optional<std::size_t> message_index)
	{
		ToolCallContentIdentity identity;
		identity.chat_id = chat.id;
		identity.provider_id = chat.provider_id;
		identity.execution_host_id = chat.execution_host_id;
		identity.native_session_id = chat.native_session_id;
		identity.workspace_directory = chat.workspace_directory;
		identity.workspace_isolation_kind = chat.workspace_isolation_kind;
		identity.workspace_source_directory = chat.workspace_source_directory;
		identity.workspace_base_ref = chat.workspace_base_ref;
		identity.workspace_branch_name = chat.workspace_branch_name;
		identity.workspace_worktree_directory = chat.workspace_worktree_directory;
		identity.updated_at = chat.updated_at;
		identity.message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
		identity.messages_digest = chat.persisted_messages_digest;
		identity.message_index = message_index;
		return identity;
	}

	std::optional<ToolCallContentSnapshot> LoadToolCallContentSnapshot(
	    const std::filesystem::path& data_root, const ToolCallContentIdentity& expected,
	    std::string_view tool_call_id)
	{
		if (expected.chat_id.empty() || tool_call_id.empty() || tool_call_id.size() > 256 ||
		    expected.messages_digest.empty() || (expected.message_index && *expected.message_index >= expected.message_count))
			return std::nullopt;
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(data_root, expected.chat_id, true);
		if (!loaded || loaded->id != expected.chat_id || !loaded->messages_loaded ||
		    loaded->provider_id != expected.provider_id || loaded->execution_host_id != expected.execution_host_id ||
		    loaded->native_session_id != expected.native_session_id ||
		    loaded->workspace_directory != expected.workspace_directory ||
		    loaded->workspace_isolation_kind != expected.workspace_isolation_kind ||
		    loaded->workspace_source_directory != expected.workspace_source_directory ||
		    loaded->workspace_base_ref != expected.workspace_base_ref ||
		    loaded->workspace_branch_name != expected.workspace_branch_name ||
		    loaded->workspace_worktree_directory != expected.workspace_worktree_directory ||
		    loaded->updated_at != expected.updated_at || loaded->messages.size() != expected.message_count ||
		    loaded->persisted_messages_digest != expected.messages_digest)
			return std::nullopt;

		std::optional<ToolCallContentSnapshot> match;
		for (std::size_t message_index = 0; message_index < loaded->messages.size(); ++message_index)
		{
			if (expected.message_index && message_index != *expected.message_index) continue;
			for (const ToolCall& tool_call : loaded->messages[message_index].tool_calls)
			{
				if (tool_call.id != tool_call_id) continue;
				if (match) return std::nullopt;
				match = ToolCallContentSnapshot{expected, message_index, tool_call};
			}
		}
		return match;
	}

	bool MatchesToolCallContentSnapshot(const ChatSession& current, const ToolCallContentSnapshot& snapshot)
	{
		const ToolCallContentIdentity& expected = snapshot.identity;
		const std::size_t message_count = current.messages_loaded ? current.messages.size() : current.persisted_message_count;
		if (current.id != expected.chat_id || current.provider_id != expected.provider_id ||
		    current.execution_host_id != expected.execution_host_id || current.native_session_id != expected.native_session_id ||
		    current.workspace_directory != expected.workspace_directory ||
		    current.workspace_isolation_kind != expected.workspace_isolation_kind ||
		    current.workspace_source_directory != expected.workspace_source_directory ||
		    current.workspace_base_ref != expected.workspace_base_ref ||
		    current.workspace_branch_name != expected.workspace_branch_name ||
		    current.workspace_worktree_directory != expected.workspace_worktree_directory ||
		    current.updated_at != expected.updated_at || message_count != expected.message_count ||
		    current.persisted_messages_digest != expected.messages_digest)
			return false;
		if (!current.messages_loaded) return true;
		if (snapshot.message_index >= current.messages.size()) return false;

		std::size_t matches = 0;
		for (std::size_t message_index = 0; message_index < current.messages.size(); ++message_index)
		{
			if (snapshot.identity.message_index && message_index != snapshot.message_index) continue;
			for (const ToolCall& tool_call : current.messages[message_index].tool_calls)
			{
				if (tool_call.id != snapshot.tool_call.id) continue;
				if (tool_call != snapshot.tool_call) return false;
				++matches;
			}
		}
		return matches == 1;
	}

	bool TranscriptRetentionService::CompleteHistoryLoad(ChatSession& current, ChatSession&& loaded, std::size_t expected_count, std::string_view expected_digest, bool retain_history)
	{
		if (retain_history)
			return ChatRepository::AdoptHydratedMessagesIfUnchanged(current, std::move(loaded), expected_count, expected_digest);
		if (current.messages_loaded) return false;
		ChatSession detached = current;
		return ChatRepository::AdoptHydratedMessagesIfUnchanged(detached, std::move(loaded), expected_count, expected_digest);
	}

	void TranscriptRetentionService::SetVisibleChatIds(const std::vector<std::string>& chat_ids)
	{
		m_visibilityKnown = true;
		m_visibleChatIds = {chat_ids.begin(), chat_ids.end()};
		for (const std::string& id : m_visibleChatIds) Invalidate(id);
	}

	void TranscriptRetentionService::Invalidate(const std::string& chat_id)
	{
		++m_generations[chat_id];
	}

	bool TranscriptRetentionService::CanRelease(const AppState& app, const ChatSession& chat) const
	{
		if (!m_visibilityKnown || !chat.messages_loaded || m_visibleChatIds.contains(chat.id) ||
		    ChatDomainService().SelectedChatId(app) == chat.id || app.pending_chat_save_at_by_chat_id.contains(chat.id) ||
		    app.worktree_operation_chat_ids.contains(chat.id) || ChatHasActiveCliTerminal(app, chat.id) ||
		    app.native_chat_load_task.running || chat.remote_turn_reconnect_pending || chat.remote_restart_pending ||
		    chat.remote_stop_cleanup_pending || chat.remote_source_exit_pending || !chat.remote_prompt_delivery_id.empty() ||
		    !chat.remote_pending_requests.empty() || !chat.remote_interaction_responses.empty() ||
		    !chat.acp_queued_prompts.empty() || !chat.goal_pending_continuation_id.empty() ||
		    UamControlService::PendingApprovalForChat(app, chat.id).has_value()) return false;
		if (std::ranges::any_of(chat.goals, [](const Goal& goal) { return goal.status == GoalStatus::Active; })) return false;
		if (std::ranges::any_of(app.native_chat_load_tasks, [](const auto& entry) { return entry.second.running; })) return false;
		if (std::ranges::any_of(app.acp_sessions, [&chat](const std::unique_ptr<AcpSessionState>& session)
		{
			return session && session->chat_id == chat.id && (session->running || AcpSessionHasBlockingRuntimeWork(*session) ||
			    session->reconnect_pending || session->recovering_remote_turn || session->recovering_remote_process ||
			    !session->pending_user_input.request_id_json.empty() || !session->pending_permission.request_id_json.empty() ||
			    session->turn_checkpoint_preflight_pending || session->turn_checkpoint_commit_pending ||
			    session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed);
		})) return false;
		if (std::ranges::any_of(app.turn_checkpoint_tasks, [&chat](const AsyncTurnCheckpointTask& task) { return task.chat_id == chat.id; }) ||
		    std::ranges::any_of(app.memory_extraction_tasks, [&chat](const AsyncMemoryExtractionTask& task) { return task.chat_id == chat.id; }) ||
		    std::ranges::any_of(app.permission_review_tasks, [&chat](const AsyncPermissionReviewTask& task) { return task.chat_id == chat.id; })) return false;
		if (std::ranges::any_of(app.memory_extraction_queue, [&chat](const QueuedMemoryExtractionTask& task) { return task.chat_id == chat.id; }) ||
		    std::ranges::any_of(app.pending_goal_iterations, [&chat](const PendingGoalIterationState& task) { return task.owner_chat_id == chat.id; }) ||
		    std::ranges::any_of(app.agent_runs, [&chat](const AgentRun& run)
		    {
			    return (run.root_chat_id == chat.id || run.transcript_chat_id == chat.id) &&
			        (run.status == "running" || run.status == "queued" || (run.deliver_result_to_root_chat && !run.root_result_delivered));
		    })) return false;
		return true;
	}

	std::optional<std::uint64_t> TranscriptRetentionService::BeginRelease(const AppState& app, const std::string& chat_id)
	{
		const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (!chat || !CanRelease(app, *chat)) return std::nullopt;
		return ++m_generations[chat_id];
	}

	bool TranscriptRetentionService::CompleteRelease(AppState& app, const std::string& chat_id, std::uint64_t generation, const ChatSession& persisted, std::vector<Message>* released)
	{
		const auto current = m_generations.find(chat_id);
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (current == m_generations.end() || current->second != generation || !chat || !CanRelease(app, *chat) ||
		    persisted.id != chat_id || !persisted.messages_loaded || persisted.messages != chat->messages) return false;
		chat->persisted_message_count = persisted.messages.size();
		chat->persisted_messages_digest = persisted.persisted_messages_digest;
		if (released) released->swap(chat->messages);
		else std::vector<Message>().swap(chat->messages);
		chat->messages_loaded = false;
		return true;
	}
}
