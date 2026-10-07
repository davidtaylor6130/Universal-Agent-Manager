#include "transcript_retention_service.h"
#include "app/chat_domain_service.h"
#include "app/uam_control_service.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include <algorithm>

namespace uam
{
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
		if (std::ranges::any_of(chat.goals, [](const Goal& goal) { return goal.status != GoalStatus::Complete; })) return false;
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
