#include "transcript_retention_service.h"
#include "app/chat_domain_service.h"
#include "app/uam_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include <algorithm>

namespace uam
{
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

	bool TranscriptRetentionService::CompleteRelease(AppState& app, const std::string& chat_id, std::uint64_t generation, const ChatSession& persisted)
	{
		const auto current = m_generations.find(chat_id);
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (current == m_generations.end() || current->second != generation || !chat || !CanRelease(app, *chat) ||
		    persisted.id != chat_id || !persisted.messages_loaded || persisted.messages != chat->messages) return false;
		chat->persisted_message_count = persisted.messages.size();
		chat->persisted_messages_digest = persisted.persisted_messages_digest;
		std::vector<Message>().swap(chat->messages);
		chat->messages_loaded = false;
		return true;
	}
}
