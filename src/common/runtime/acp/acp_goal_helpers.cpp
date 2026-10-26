#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/config/approval_modes.h"
#include "app/chat_domain_service.h"

#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <limits>
#include <string>

namespace uam::acp_detail
{

namespace
{
	std::string NativeGoalDeliveryConfiguration(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const Goal& goal)
	{
		return nlohmann::json::array({PromptDeliveryConfiguration(app, session, chat), chat.workspace_directory,
			chat.workspace_source_directory, chat.workspace_worktree_directory, chat.workspace_isolation_kind,
			chat.uam_agent_id, session.active_uam_agent_workspace_access, session.active_uam_agent_execution_capability,
			chat.small_model_mode, goal.worker_model_id, goal.reviewer_model_id}).dump();
	}

	bool NativeGoalDeliveryConfigurationMatches(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
		const AcpSessionState::CodexGoalOwner& owner)
	{
		if (owner.delivery_configuration.empty()) return false;
		for (const Goal& goal : chat.goals)
			if (goal.id == owner.goal_id) return owner.delivery_configuration == NativeGoalDeliveryConfiguration(app, session, chat, goal);
		return false;
	}
}

AcpSessionState::CodexGoalOwner CaptureCodexNativeGoalOwner(const AcpSessionState& session, const ChatSession& chat,
	const Goal& goal, bool replace_native)
{
	AcpSessionState::CodexGoalOwner owner{chat.id, session.session_id, goal.id,
		chat.goal_command_revision, goal.objective, goal.token_budget, goal.tokens_used, true, true, replace_native};
	owner.execution_host_id = chat.execution_host_id.empty() ? "local" : chat.execution_host_id;
	owner.process_id = session.last_process_id;
	owner.parent_incarnation = chat.metadata_incarnation;
	owner.initial_history_incarnation = std::make_shared<const std::uint8_t>(0);
	return owner;
}

AcpSessionState::CodexGoalOwner CaptureCodexNativeGoalOwner(const AppState& app, const AcpSessionState& session,
	const ChatSession& chat, const Goal& goal, bool replace_native)
{
	AcpSessionState::CodexGoalOwner owner = CaptureCodexNativeGoalOwner(session, chat, goal, replace_native);
	owner.delivery_configuration = NativeGoalDeliveryConfiguration(app, session, chat, goal);
	return owner;
}

bool CodexGoalReceiptMatchesOwner(const AcpSessionState& session, const ChatSession& chat,
	const AcpSessionState::CodexGoalOwner& owner, const std::string& thread_id, bool require_active)
{
	if (!session.running || !session.session_ready || session.provider_id != "codex-cli" || chat.provider_id != "codex-cli" ||
		!owner.parent_incarnation || owner.parent_incarnation != chat.metadata_incarnation ||
		owner.chat_id.empty() || owner.goal_id.empty() || owner.revision.empty() || owner.thread_id.empty() ||
		owner.chat_id != chat.id || session.chat_id != chat.id || thread_id != owner.thread_id ||
		owner.execution_host_id != (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) ||
		owner.execution_host_id != (session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id) ||
		(!owner.process_id.empty() && owner.process_id != session.last_process_id) ||
		session.session_id != owner.thread_id || chat.goal_command_revision != owner.revision ||
		(require_active && (chat.active_goal_id != owner.goal_id || chat.imported_read_only ||
		session.active_uam_agent_workspace_access == "read" || session.goal_review_turn || session.goal_internal_session ||
		chat.approval_mode == uam::approval_modes::kPlanApprovalMode || session.current_mode_id == uam::approval_modes::kPlanApprovalMode)))
		return false;
	for (const Goal& goal : chat.goals)
	{
		if (goal.id == owner.goal_id)
			return (!require_active || goal.status == GoalStatus::Active) && goal.execution_owner == "provider" && !goal.provider_command.empty() &&
				goal.objective == owner.objective && goal.token_budget == owner.token_budget;
	}
	return false;
}

bool CodexGoalOwnsNativeTurn(const AcpSessionState& session, const ChatSession& chat, const std::string& thread_id)
{
	if (session.codex_goal_native_active && CodexGoalReceiptMatchesOwner(session, chat, session.codex_goal_owner, thread_id)) return true;
	for (const std::pair<const std::string, AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
	{
		if (!pending.second.start_authorized || !CodexGoalReceiptMatchesOwner(session, chat, pending.second, thread_id)) continue;
		if (pending.second.action == AcpSessionState::CodexGoalOwner::Action::InitialTurn ||
			pending.second.action == AcpSessionState::CodexGoalOwner::Action::Activate) return true;
	}
	return false;
}

namespace
{
	bool SendCodexGoalRequest(AcpSessionState& session, const nlohmann::json& request,
		const AcpSessionState::CodexGoalOwner& owner)
	{
		const int id = request.at("id").get<int>();
		const std::string key = std::to_string(id);
		session.pending_request_methods[id] = request.at("method").get<std::string>();
		session.codex_goal_pending_requests[key] = owner;
		if (WriteAcpMessage(session, request)) return true;
		session.pending_request_methods.erase(id);
		session.codex_goal_pending_requests.erase(key);
		return false;
	}

	void HoldCodexGoal(AppState& app, AcpSessionState& session, ChatSession& chat,
		const std::string& goal_id, const std::string& diagnostic)
	{
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, goal_id); goal != nullptr)
		{
			goal->last_diagnostic = diagnostic;
			(void)GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Paused);
		}
		session.goal_resume_suppressed = true;
		SaveChatQuietly(app, chat);
	}
	void HoldUnconfirmedCodexGoal(AppState& app, AcpSessionState& session, ChatSession& chat,
		const std::string& goal_id, const std::string& diagnostic)
	{
		// Failed fresh reads cannot authorize controls against potentially replaced native state.
		session.codex_goal_owner = {};
		session.codex_goal_native_active = false;
		HoldCodexGoal(app, session, chat, goal_id, diagnostic);
	}

	bool NativeGoalInitialRowMatches(const ChatSession& chat, const Goal& goal, const NativeGoalInitialDelivery& witness)
	{
		if ((witness.state != NativeGoalInitialDelivery::State::Pending && witness.state != NativeGoalInitialDelivery::State::Attempted &&
			witness.state != NativeGoalInitialDelivery::State::Accepted) || !chat.messages_loaded || witness.user_message_index < 0 || witness.user_message_index > std::numeric_limits<int>::max() ||
			static_cast<std::size_t>(witness.user_message_index) >= chat.messages.size() || witness.user_message_created_at.empty()) return false;
		const Message& message = chat.messages[static_cast<std::size_t>(witness.user_message_index)];
		return message.role == MessageRole::User && message.content == goal.objective && message.created_at == witness.user_message_created_at &&
			(witness.state != NativeGoalInitialDelivery::State::Pending || message.acp_prompt_not_sent) &&
			(witness.state != NativeGoalInitialDelivery::State::Accepted || !message.acp_prompt_not_sent) &&
			((witness.state == NativeGoalInitialDelivery::State::Accepted) == !witness.turn_id.empty());
	}

	/// <summary>Persists a current native control receipt without permitting a stale receipt to authorize transport.</summary>
	void PersistCodexControlReceipt(AppState& app, AcpSessionState& session, ChatSession& chat,
		const AcpSessionState::CodexGoalOwner& owner,
		std::function<void(AppState&, AcpSessionState&, ChatSession&)> committed = {})
	{
		const Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
		if (goal == nullptr) return;
		const std::filesystem::path root = app.data_root;
		const std::optional<NativeGoalBinding> binding = goal->native_binding;
		const std::int64_t used = goal->tokens_used;
		const std::int64_t native_created = session.codex_goal_created_at;
		const std::int64_t native_updated = session.codex_goal_updated_at;
		const std::string native_turn = session.codex_turn_id;
		const AcpSessionState::CodexGoalOwner current_owner = session.codex_goal_owner;
		const bool requires_activation = static_cast<bool>(committed);
		const std::function<bool()> actor_owned = [&app, root, owner, binding, used, native_created, native_updated, native_turn, current_owner, requires_activation]()
		{
			const AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			const ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			if (app.data_root != root || current == nullptr || stored == nullptr ||
				!CodexGoalReceiptMatchesOwner(*current, *stored, owner, owner.thread_id, false) ||
				current->codex_goal_created_at != native_created || current->codex_goal_owner.parent_incarnation != current_owner.parent_incarnation ||
				current->codex_goal_owner.goal_id != current_owner.goal_id || current->codex_goal_owner.revision != current_owner.revision ||
				current->codex_goal_owner.binding_created_at != current_owner.binding_created_at ||
				current->codex_goal_owner.initial_history_incarnation != current_owner.initial_history_incarnation) return false;
			if (requires_activation)
			{
				if (!current->codex_goal_initial_turn_pending.has_value()) return false;
				const AcpSessionState::CodexGoalOwner& phase = *current->codex_goal_initial_turn_pending;
				if (phase.action != AcpSessionState::CodexGoalOwner::Action::Activate || phase.parent_incarnation != owner.parent_incarnation ||
					phase.goal_id != owner.goal_id || phase.revision != owner.revision || phase.thread_id != owner.thread_id ||
					phase.process_id != owner.process_id || phase.binding_created_at != owner.binding_created_at ||
					!owner.initial_history_incarnation || phase.initial_history_incarnation != owner.initial_history_incarnation) return false;
			}
			const Goal* saved_goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id);
			if (saved_goal == nullptr || saved_goal->native_binding.has_value() != binding.has_value()) return false;
			if (!binding.has_value()) return true;
			const NativeGoalBinding& actual = *saved_goal->native_binding;
			const NativeGoalBinding& expected = *binding;
			return actual.version == expected.version && actual.provider_id == expected.provider_id && actual.execution_host_id == expected.execution_host_id &&
				actual.thread_id == expected.thread_id && actual.uam_chat_id == expected.uam_chat_id && actual.uam_goal_id == expected.uam_goal_id &&
				actual.objective == expected.objective && actual.created_at == expected.created_at && actual.uam_tokens_at_creation == expected.uam_tokens_at_creation &&
				actual.uam_budget_at_creation == expected.uam_budget_at_creation && actual.native_token_budget == expected.native_token_budget &&
				actual.initial_delivery == expected.initial_delivery;
		};
		const std::function<bool()> owned = [&app, owner, binding, used, native_updated, native_turn, actor_owned, requires_activation]()
		{
			if (!actor_owned()) return false;
			const AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			const ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			const Goal* saved_goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id);
			return current != nullptr && stored != nullptr && saved_goal != nullptr &&
				(!requires_activation || NativeGoalDeliveryConfigurationMatches(app, *current, *stored, owner)) &&
				current->codex_goal_updated_at == native_updated &&
				(!requires_activation || current->codex_turn_id == native_turn) && saved_goal->tokens_used == used && saved_goal->native_binding == binding;
		};
		const std::function<void(DurableAdmissionState)> completion = [&app, owner, owned, actor_owned, committed = std::move(committed)](DurableAdmissionState result)
		{
			if (!actor_owned()) return;
			AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			if (current == nullptr || stored == nullptr) return;
			const bool full_owned = owned();
			if (committed && current->codex_goal_initial_turn_pending.has_value())
			{
				const AcpSessionState::CodexGoalOwner& phase = *current->codex_goal_initial_turn_pending;
				if (phase.action != AcpSessionState::CodexGoalOwner::Action::Activate || phase.parent_incarnation != owner.parent_incarnation ||
					phase.goal_id != owner.goal_id || phase.revision != owner.revision || phase.thread_id != owner.thread_id ||
					phase.process_id != owner.process_id || phase.binding_created_at != owner.binding_created_at ||
					!owner.initial_history_incarnation || phase.initial_history_incarnation != owner.initial_history_incarnation) return;
				current->codex_goal_initial_turn_pending.reset();
			}
			if (result != DurableAdmissionState::Committed || !full_owned)
			{
				if (Goal* saved_goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id); saved_goal != nullptr)
					saved_goal->last_diagnostic = "native_goal_control_receipt_save_failed";
				current->goal_resume_suppressed = true;
				return;
			}
			if (committed && !current->cancel_requested && !current->local_stop_pending && !current->remote_stop_pending &&
				CodexGoalReceiptMatchesOwner(*current, *stored, owner, owner.thread_id) &&
				NativeGoalDeliveryConfigurationMatches(app, *current, *stored, owner)) committed(app, *current, *stored);
		};
		DurableAdmissionState admission = DurableAdmissionState::Failed;
		try { admission = BeginDurableAdmission(app, chat, owned, completion, DurableAdmissionFailurePurpose::BindingOnly); }
		catch (const std::exception&) { }
		if (admission == DurableAdmissionState::Failed) completion(admission);
	}

}

bool StartCodexNativeGoal(AppState& app, AcpSessionState& session, ChatSession& chat, Goal& goal,
	bool user_action, std::string* error_out, bool replace_native)
{
	if (session.provider_id != "codex-cli" || session.session_id.empty() || !session.session_ready ||
		!session.running || chat.imported_read_only || !chat.messages_loaded || session.current_mode_id == uam::approval_modes::kPlanApprovalMode ||
		chat.approval_mode == uam::approval_modes::kPlanApprovalMode || session.cancel_requested ||
		session.active_uam_agent_workspace_access == "read" || session.goal_review_turn || session.goal_internal_session ||
		!chat.goal_owner_chat_id.empty() ||
		session.local_stop_pending || session.remote_stop_pending)
	{
		if (error_out != nullptr) *error_out = "Native goals require a writable, loaded Codex chat outside Plan mode.";
		HoldCodexGoal(app, session, chat, goal.id, "native_goal_not_ready");
		return false;
	}
	if (!user_action)
	{
		if (session.codex_goal_native_active && CodexGoalReceiptMatchesOwner(session, chat, session.codex_goal_owner, session.session_id)) return true;
		if (error_out != nullptr) *error_out = "Resume this native goal explicitly to authorize provider execution.";
		HoldCodexGoal(app, session, chat, goal.id, "native_goal_requires_user_resume");
		return false;
	}
	if (session.codex_goal_initial_turn_pending.has_value() &&
		CodexGoalReceiptMatchesOwner(session, chat, *session.codex_goal_initial_turn_pending, session.session_id)) return true;
	AcpSessionState::CodexGoalOwner owner = CaptureCodexNativeGoalOwner(app, session, chat, goal, replace_native);
	const AcpSessionState::CodexGoalOwner& previous = session.codex_goal_owner;
	if (!replace_native && previous.parent_incarnation == chat.metadata_incarnation && previous.chat_id == chat.id && previous.thread_id == session.session_id &&
		previous.goal_id == goal.id && previous.objective == goal.objective && previous.token_budget == goal.token_budget &&
		previous.execution_host_id == owner.execution_host_id && previous.process_id == owner.process_id &&
		session.codex_goal_created_at > 0)
	{
		owner.resume_existing = true;
		owner.binding_created_at = session.codex_goal_created_at;
		owner.tokens_used_at_start = previous.tokens_used_at_start;
	}
	if (!replace_native && goal.native_binding.has_value())
	{
		const NativeGoalBinding& binding = *goal.native_binding;
		const bool valid = binding.version == 1 && binding.provider_id == session.provider_id &&
			binding.execution_host_id == owner.execution_host_id && binding.thread_id == owner.thread_id &&
			binding.uam_chat_id == chat.id && binding.uam_goal_id == goal.id &&
			binding.objective == goal.objective && binding.uam_budget_at_creation == goal.token_budget &&
			binding.created_at > 0 && binding.last_updated_at >= binding.created_at && binding.uam_tokens_at_creation >= 0 &&
			binding.last_native_tokens_used >= 0 && binding.uam_tokens_at_creation <= goal.tokens_used &&
			binding.last_native_tokens_used <= goal.tokens_used - binding.uam_tokens_at_creation &&
			(!binding.native_token_budget || *binding.native_token_budget >= 0) &&
			(goal.token_budget <= 0 || (binding.uam_tokens_at_creation < goal.token_budget &&
			 binding.native_token_budget == goal.token_budget - binding.uam_tokens_at_creation));
		if (!valid)
		{
			if (error_out != nullptr) *error_out = "The saved native goal binding cannot authorize resume.";
			HoldUnconfirmedCodexGoal(app, session, chat, goal.id, "native_goal_invalid_durable_binding");
			return false;
		}
		if (binding.initial_delivery.has_value())
		{
			const NativeGoalInitialDelivery& witness = *binding.initial_delivery;
			if (!NativeGoalInitialRowMatches(chat, goal, witness) || witness.state == NativeGoalInitialDelivery::State::Attempted)
			{
				goal.last_diagnostic = witness.state == NativeGoalInitialDelivery::State::Attempted
					? "native_goal_initial_delivery_ambiguous" : "native_goal_initial_row_unconfirmed";
				session.goal_resume_suppressed = true;
				if (error_out != nullptr) *error_out = "The saved native goal binding cannot authorize resume.";
				return false;
			}
			owner.initial_history_user_message_index = static_cast<int>(witness.user_message_index);
			owner.initial_history_user_message_created_at = witness.user_message_created_at;
		}
		else if (std::ranges::any_of(chat.messages, [&goal](const Message& message)
		{
			return message.role == MessageRole::User && message.acp_prompt_not_sent && message.content == goal.objective;
		}))
		{
			goal.last_diagnostic = "native_goal_legacy_unsent_row_unconfirmed";
			session.goal_resume_suppressed = true;
			if (error_out != nullptr) *error_out = "The saved native goal binding cannot authorize resume.";
			return false;
		}
		owner.resume_existing = true;
		owner.binding_created_at = binding.created_at;
		owner.tokens_used_at_start = binding.uam_tokens_at_creation;
		// The read-only probe must confirm this witness before any native mutation.
		session.codex_goal_created_at = binding.created_at;
		session.codex_goal_updated_at = binding.last_updated_at;
	}
	if (!session.codex_goal_pending_requests.empty())
	{
		for (const std::pair<const std::string, AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
		{
			if (pending.second.parent_incarnation == chat.metadata_incarnation && pending.second.goal_id == goal.id && pending.second.revision == chat.goal_command_revision &&
				pending.second.start_authorized) return true;
		}
		session.codex_goal_queued_user_action = owner;
		return true;
	}
	if (goal.token_budget > 0 && goal.tokens_used >= goal.token_budget)
	{
		if (error_out != nullptr) *error_out = "The goal token budget is exhausted.";
		HoldCodexGoal(app, session, chat, goal.id, "native_goal_budget_exhausted");
		return false;
	}
	if (goal.tokens_used < 0)
	{
		if (error_out != nullptr) *error_out = "The saved goal usage is invalid.";
		HoldCodexGoal(app, session, chat, goal.id, "native_goal_invalid_saved_usage");
		return false;
	}
	if (session.codex_goal_capability == AcpSessionState::CodexGoalCapability::Unsupported)
	{
		goal.execution_owner = "uam";
		goal.provider_command.clear();
		goal.native_binding.reset();
		goal.last_diagnostic = "native_goal_unsupported_using_uam";
		SaveChatQuietly(app, chat);
		return ResumeGoal(app, chat.id, goal.id, error_out);
	}
	session.goal_command_revision = chat.goal_command_revision;
	session.goal_resume_suppressed = true;
	const int request_id = session.next_request_id++;
	const bool sent = SendCodexGoalRequest(session, BuildCodexGoalGetRequest(request_id, session.session_id), owner);
	if (!sent)
	{
		if (error_out != nullptr) *error_out = session.last_error;
		HoldCodexGoal(app, session, chat, goal.id, "native_goal_probe_write_failed");
	}
	return sent;
}

bool StopCodexNativeGoal(AcpSessionState& session, const ChatSession& chat, bool user_action, const std::string& goal_id, bool clear_native)
{
	if (session.provider_id != "codex-cli" || !chat.metadata_incarnation) return true;
	const bool has_owned_goal = session.codex_goal_owner.parent_incarnation == chat.metadata_incarnation &&
		!session.codex_goal_owner.goal_id.empty() &&
		(goal_id.empty() || session.codex_goal_owner.goal_id == goal_id);
	const bool has_pending_goal = std::ranges::any_of(session.codex_goal_pending_requests,
		[&](const std::pair<const std::string, AcpSessionState::CodexGoalOwner>& pending)
		{
			return pending.second.parent_incarnation == chat.metadata_incarnation &&
				(goal_id.empty() || pending.second.goal_id == goal_id);
		});
	const bool has_queued_goal = session.codex_goal_queued_user_action.has_value() &&
		session.codex_goal_queued_user_action->parent_incarnation == chat.metadata_incarnation &&
		(goal_id.empty() || session.codex_goal_queued_user_action->goal_id == goal_id);
	if (!has_owned_goal && !has_pending_goal && !has_queued_goal) return true;
	if (session.codex_goal_initial_turn_pending.has_value() &&
		session.codex_goal_initial_turn_pending->parent_incarnation == chat.metadata_incarnation &&
		(goal_id.empty() || session.codex_goal_initial_turn_pending->goal_id == goal_id))
		session.codex_goal_initial_turn_pending.reset();
	if (session.codex_goal_queued_user_action.has_value() &&
		session.codex_goal_queued_user_action->parent_incarnation == chat.metadata_incarnation &&
		(goal_id.empty() || session.codex_goal_queued_user_action->goal_id == goal_id))
		session.codex_goal_queued_user_action.reset();
	AcpSessionState::CodexGoalOwner owner = session.codex_goal_owner;
	bool set_sent = owner.parent_incarnation == chat.metadata_incarnation && !owner.goal_id.empty() && (goal_id.empty() || owner.goal_id == goal_id);
	for (std::pair<const std::string, AcpSessionState::CodexGoalOwner>& request : session.codex_goal_pending_requests)
	{
		if (request.second.parent_incarnation != chat.metadata_incarnation ||
			(!goal_id.empty() && request.second.goal_id != goal_id)) continue;
		request.second.start_authorized = false;
		if (request.second.chat_id != chat.id || request.second.thread_id != session.session_id) continue;
		owner = request.second;
		if (request.second.action == AcpSessionState::CodexGoalOwner::Action::StopClear ||
			request.second.action == AcpSessionState::CodexGoalOwner::Action::StopPause) return true;
		if (request.second.action != AcpSessionState::CodexGoalOwner::Action::Probe &&
			request.second.action != AcpSessionState::CodexGoalOwner::Action::ReplaceClear) set_sent = true;
	}
	session.goal_resume_suppressed = true;
	if (!owner.parent_incarnation || owner.parent_incarnation != chat.metadata_incarnation || !set_sent ||
		(!goal_id.empty() && owner.goal_id != goal_id) || owner.chat_id != chat.id || owner.thread_id != session.session_id ||
		(!owner.process_id.empty() && owner.process_id != session.last_process_id) ||
		owner.execution_host_id != (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) ||
		owner.execution_host_id != (session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id)) return true;
	owner.user_origin = user_action;
	owner.start_authorized = false;
	owner.revision = chat.goal_command_revision;
	if (owner.binding_created_at == 0 && session.codex_goal_created_at == 0) clear_native = true;
	owner.action = clear_native ? AcpSessionState::CodexGoalOwner::Action::StopClear : AcpSessionState::CodexGoalOwner::Action::StopPause;
	const int request_id = session.next_request_id++;
	return SendCodexGoalRequest(session, clear_native ? BuildCodexGoalClearRequest(request_id, owner.thread_id, user_action)
		: BuildCodexGoalStatusRequest(request_id, owner.thread_id, "paused", user_action), owner);
}

void DrainQueuedCodexNativeGoal(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (!session.codex_goal_pending_requests.empty() || !session.codex_goal_queued_user_action.has_value()) return;
	const AcpSessionState::CodexGoalOwner owner = *session.codex_goal_queued_user_action;
	session.codex_goal_queued_user_action.reset();
	if (!owner.user_origin || !CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id)) return;
	if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr)
	{
		std::string error;
		(void)StartCodexNativeGoal(app, session, chat, *goal, true, &error, owner.replace_native);
	}
}

/// <summary>Starts one configured user-authorized turn after native paused intent and UAM history are durable.</summary>
void StartPendingCodexNativeGoalTurn(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (!session.codex_goal_initial_turn_pending.has_value()) return;
	AcpSessionState::CodexGoalOwner owner = *session.codex_goal_initial_turn_pending;
	if (!owner.user_origin || !owner.initial_history_incarnation ||
		!NativeGoalDeliveryConfigurationMatches(app, session, chat, owner) ||
		(owner.binding_created_at > 0 && session.codex_goal_created_at != owner.binding_created_at) ||
		!CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id))
	{
		session.codex_goal_initial_turn_pending.reset();
		return;
	}
	if (!session.codex_turn_id.empty() || session.prompt_request_id != 0 || !session.queued_prompt.empty() ||
		!session.queued_user_prompts.empty() || session.turn_checkpoint_commit_pending || session.manual_compaction_pending ||
		AcpSessionIsWaitingForInput(session) || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending) return;
	using HistoryState = AcpSessionState::CodexGoalOwner::InitialHistoryState;
	if (owner.initial_history_state == HistoryState::Pending) return;
	if (owner.initial_history_state == HistoryState::Unprepared)
	{
		Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
		if (goal == nullptr || !goal->native_binding.has_value()) return;
		if (!owner.resume_existing)
		{
			if (goal->native_binding->initial_delivery.has_value())
			{
				const NativeGoalInitialDelivery& witness = *goal->native_binding->initial_delivery;
				if ((witness.state != NativeGoalInitialDelivery::State::Pending &&
					!(witness.state == NativeGoalInitialDelivery::State::Attempted && owner.initial_history_user_message_index == witness.user_message_index &&
					owner.initial_history_user_message_created_at == witness.user_message_created_at)) || !NativeGoalInitialRowMatches(chat, *goal, witness)) return;
				owner.initial_history_user_message_index = static_cast<int>(witness.user_message_index);
				owner.initial_history_user_message_created_at = witness.user_message_created_at;
			}
			else
			{
				owner.initial_history_user_message_index = static_cast<int>(chat.messages.size());
				owner.initial_history_user_message_created_at = uam::time::TimestampNow();
				chat.messages.push_back({MessageRole::User, owner.objective, owner.initial_history_user_message_created_at});
				chat.messages.back().acp_prompt_not_sent = true;
				goal->native_binding->initial_delivery = NativeGoalInitialDelivery{owner.initial_history_user_message_index,
					owner.initial_history_user_message_created_at, NativeGoalInitialDelivery::State::Pending, ""};
			}
		}
		owner.initial_history_state = HistoryState::Pending;
		session.codex_goal_initial_turn_pending = owner;
		const std::filesystem::path root = app.data_root;
		const std::optional<NativeGoalBinding> expected_binding = goal->native_binding;
		const std::function<bool()> identity_owned = [&app, root, owner, expected_binding]()
		{
			const AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			const ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			if (app.data_root != root || current == nullptr || stored == nullptr || current->cancel_requested ||
				current->local_stop_pending || current->remote_stop_pending ||
				!current->codex_goal_initial_turn_pending.has_value() ||
				current->codex_goal_created_at != owner.binding_created_at ||
				!CodexGoalReceiptMatchesOwner(*current, *stored, owner, owner.thread_id)) return false;
			const AcpSessionState::CodexGoalOwner& pending = *current->codex_goal_initial_turn_pending;
			if (pending.initial_history_state != HistoryState::Pending || pending.parent_incarnation != owner.parent_incarnation ||
				pending.initial_history_incarnation != owner.initial_history_incarnation ||
				pending.revision != owner.revision || pending.thread_id != owner.thread_id || pending.process_id != owner.process_id ||
				pending.goal_id != owner.goal_id || pending.binding_created_at != owner.binding_created_at ||
				pending.initial_history_user_message_index != owner.initial_history_user_message_index ||
				pending.initial_history_user_message_created_at != owner.initial_history_user_message_created_at) return false;
			const Goal* saved_goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id);
			if (saved_goal == nullptr || !saved_goal->native_binding.has_value() || !expected_binding.has_value()) return false;
			const NativeGoalBinding& actual = *saved_goal->native_binding;
			const NativeGoalBinding& expected = *expected_binding;
			if (actual.version != expected.version || actual.provider_id != expected.provider_id || actual.execution_host_id != expected.execution_host_id ||
				actual.thread_id != expected.thread_id || actual.uam_chat_id != expected.uam_chat_id || actual.uam_goal_id != expected.uam_goal_id ||
				actual.objective != expected.objective || actual.created_at != expected.created_at || actual.uam_tokens_at_creation != expected.uam_tokens_at_creation ||
				actual.uam_budget_at_creation != expected.uam_budget_at_creation || actual.native_token_budget != expected.native_token_budget ||
				actual.initial_delivery != expected.initial_delivery) return false;
			if (owner.resume_existing) return !saved_goal->native_binding->initial_delivery.has_value() ||
				NativeGoalInitialRowMatches(*stored, *saved_goal, *saved_goal->native_binding->initial_delivery);
			const int index = owner.initial_history_user_message_index;
			if (index < 0 || static_cast<std::size_t>(index) >= stored->messages.size()) return false;
			const Message& message = stored->messages[static_cast<std::size_t>(index)];
			return message.role == MessageRole::User && message.content == owner.objective &&
				message.created_at == owner.initial_history_user_message_created_at && message.acp_prompt_not_sent;
		};
		const std::function<bool()> owned = [&app, owner, expected_binding, identity_owned]()
		{
			if (!identity_owned()) return false;
			const Goal* saved_goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id);
			const AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			const ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			return current != nullptr && stored != nullptr && saved_goal != nullptr && saved_goal->native_binding == expected_binding &&
				NativeGoalDeliveryConfigurationMatches(app, *current, *stored, owner);
		};
		const std::function<void(DurableAdmissionState)> completion = [&app, owner, owned, identity_owned](DurableAdmissionState result)
		{
			if (!identity_owned()) return;
			AcpSessionState* current = FindAcpSessionForChat(app, owner.chat_id);
			ChatSession* stored = ChatDomainService().FindChatById(app, owner.chat_id);
			if (current == nullptr || stored == nullptr) return;
			if (result != DurableAdmissionState::Committed || !owned())
			{
				if (!owner.resume_existing)
					if (Goal* goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id); goal != nullptr &&
						goal->native_binding.has_value() && goal->native_binding->initial_delivery.has_value() &&
						goal->native_binding->initial_delivery->state != NativeGoalInitialDelivery::State::Accepted)
						goal->native_binding->initial_delivery->state = NativeGoalInitialDelivery::State::Pending;
				current->codex_goal_initial_turn_pending.reset();
				current->goal_resume_suppressed = true;
				if (Goal* goal = GoalService::FindGoalById(app, owner.chat_id, owner.goal_id); goal != nullptr)
				{
					goal->last_diagnostic = "native_goal_initial_history_save_failed";
					goal->status = GoalStatus::Paused;
					goal->updated_at = uam::time::TimestampNow();
				}
				if (stored->active_goal_id == owner.goal_id) stored->active_goal_id.clear();
				if (stored->goal_pending_continuation_id == owner.goal_id) stored->goal_pending_continuation_id.clear();
				app.chats_with_unseen_updates.insert(owner.chat_id);
				return;
			}
			current->codex_goal_initial_turn_pending->initial_history_state = HistoryState::Committed;
			StartPendingCodexNativeGoalTurn(app, *current, *stored);
		};
		DurableAdmissionState admission = DurableAdmissionState::Failed;
		try { admission = BeginDurableAdmission(app, chat, owned, completion, DurableAdmissionFailurePurpose::BindingOnly); }
		catch (const std::exception&) { }
		if (admission == DurableAdmissionState::Failed) completion(admission);
		return;
	}
	Goal* current_goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
	if (current_goal == nullptr || !current_goal->native_binding.has_value()) return;
	if (!owner.resume_existing && current_goal->native_binding->initial_delivery.has_value() &&
		current_goal->native_binding->initial_delivery->state == NativeGoalInitialDelivery::State::Pending)
	{
		current_goal->native_binding->initial_delivery->state = NativeGoalInitialDelivery::State::Attempted;
		owner.initial_history_state = HistoryState::Unprepared;
		session.codex_goal_initial_turn_pending = owner;
		// The row is already prepared; the second full receipt records ambiguity before pipe transport.
		StartPendingCodexNativeGoalTurn(app, session, chat);
		return;
	}
	const int user_index = owner.resume_existing ? -1 : owner.initial_history_user_message_index;
	if (!owner.resume_existing)
	{
		if (user_index < 0 || static_cast<std::size_t>(user_index) >= chat.messages.size()) return;
		const Message& message = chat.messages[static_cast<std::size_t>(user_index)];
		if (message.role != MessageRole::User || message.content != owner.objective ||
			message.created_at != owner.initial_history_user_message_created_at || !message.acp_prompt_not_sent) return;
	}
	session.codex_goal_initial_turn_pending.reset();
	const int request_id = session.next_request_id++;
	std::string method;
	nlohmann::json request = ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpBuildPrompt(
		session, request_id, owner.resume_existing ? "" : owner.objective, chat, method);
	if (owner.resume_existing) request["params"]["input"] = nlohmann::json::array();
	AcpSessionState::CodexGoalOwner turn_owner = owner;
	turn_owner.action = AcpSessionState::CodexGoalOwner::Action::InitialTurn;
	if (method != "turn/start" || !SendCodexGoalRequest(session, request, turn_owner))
	{
		HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_initial_turn_write_failed");
		return;
	}
	if (user_index >= 0) chat.messages[static_cast<std::size_t>(user_index)].acp_prompt_not_sent = false;
	ScheduleChatSave(app, chat, 0.0);
	session.codex_goal_initial_turn_request_id = request_id;
	session.current_assistant_message_index = -1;
	session.turn_first_user_message_index = user_index;
	session.turn_user_message_index = user_index;
	session.turn_assistant_message_index = -1;
	session.turn_serial += 1;
	session.turn_started_time_s = GetAppTimeSeconds();
	session.last_runtime_activity_time_s = session.turn_started_time_s;
	session.goal_command_revision = chat.goal_command_revision;
	ResetAcpTurnStreamState(session);
	session.processing = true;
	session.lifecycle_state = kAcpLifecycleProcessing;
}

bool ApplyCodexNativeGoalReceipt(AcpSessionState& session, ChatSession& chat, const nlohmann::json& native_goal, CodexGoalReceiptMode mode)
{
	const AcpSessionState::CodexGoalOwner& owner = session.codex_goal_owner;
	if (!native_goal.is_object() || !CodexGoalReceiptMatchesOwner(session, chat, owner,
		JsonDiagnosticStringValue(native_goal, "threadId"), mode != CodexGoalReceiptMode::Stopping) ||
		JsonDiagnosticStringValue(native_goal, "objective") != owner.objective) return false;
	const nlohmann::json::const_iterator tokens = native_goal.find("tokensUsed");
	const nlohmann::json::const_iterator created = native_goal.find("createdAt");
	const nlohmann::json::const_iterator updated = native_goal.find("updatedAt");
	if (tokens == native_goal.end() || !tokens->is_number_integer() ||
		created == native_goal.end() || !created->is_number_integer() ||
		updated == native_goal.end() || !updated->is_number_integer()) return false;
	const std::int64_t used = tokens->get<std::int64_t>();
	const std::int64_t created_at = created->get<std::int64_t>();
	const std::int64_t updated_at = updated->get<std::int64_t>();
	if (used < 0 || owner.tokens_used_at_start < 0 || created_at <= 0 || updated_at < created_at ||
		(session.codex_goal_created_at != 0 && session.codex_goal_created_at != created_at) ||
		(owner.binding_created_at > 0 && owner.binding_created_at != created_at) ||
		updated_at < session.codex_goal_updated_at ||
		used > std::numeric_limits<std::int64_t>::max() - owner.tokens_used_at_start) return false;
	// Durable JsonValue numbers cannot represent integers beyond 2^53-1 exactly.
	constexpr std::int64_t max_exact = 9'007'199'254'740'991LL;
	const nlohmann::json::const_iterator native_budget = native_goal.find("tokenBudget");
	if (used > max_exact || created_at > max_exact || updated_at > max_exact || owner.tokens_used_at_start > max_exact ||
		owner.token_budget > max_exact || used > max_exact - owner.tokens_used_at_start || (native_budget != native_goal.end() && !native_budget->is_null() &&
		(!native_budget->is_number_integer() || native_budget->get<std::int64_t>() < 0 || native_budget->get<std::int64_t>() > max_exact))) return false;
	const std::string status = JsonDiagnosticStringValue(native_goal, "status");
	if (mode == CodexGoalReceiptMode::Stopping && status != "paused") return false;
	if (owner.token_budget > 0)
	{
		const nlohmann::json::const_iterator budget = native_goal.find("tokenBudget");
		if (budget == native_goal.end() || !budget->is_number_integer() ||
			budget->get<std::int64_t>() != owner.token_budget - owner.tokens_used_at_start) return false;
	}
	if (status != "active" && status != "paused" && status != "blocked" && status != "usageLimited" &&
		status != "budgetLimited" && status != "complete") return false;
	for (Goal& goal : chat.goals)
	{
		if (goal.id != owner.goal_id) continue;
		const std::int64_t total_used = owner.tokens_used_at_start + used;
		if (total_used < goal.tokens_used) return false;
		if (goal.native_binding.has_value())
		{
			NativeGoalBinding& binding = *goal.native_binding;
			const nlohmann::json::const_iterator budget = native_goal.find("tokenBudget");
			const std::optional<std::int64_t> reported_budget = budget != native_goal.end() && budget->is_number_integer()
				? std::optional<std::int64_t>(budget->get<std::int64_t>()) : std::nullopt;
			if (binding.created_at != created_at || binding.native_token_budget != reported_budget ||
				updated_at < binding.last_updated_at || used < binding.last_native_tokens_used) return false;
			binding.last_updated_at = updated_at;
			binding.last_native_tokens_used = used;
		}
		session.codex_goal_created_at = created_at;
		session.codex_goal_updated_at = updated_at;
		goal.tokens_used = total_used;
		goal.updated_at = uam::time::TimestampNow();
		session.codex_goal_native_active = status == "active";
		if (mode == CodexGoalReceiptMode::Stopping) return status == "paused";
		if (((mode == CodexGoalReceiptMode::Preparing && status == "paused") || status == "active") && (goal.token_budget <= 0 || total_used < goal.token_budget)) return true;
		goal.status = status == "blocked" ? GoalStatus::Blocked : GoalStatus::Paused;
		goal.last_diagnostic = status == "complete" ? "native_goal_complete_unverified" : "native_goal_" + status;
		chat.active_goal_id.clear();
		chat.goal_pending_continuation_id.clear();
		session.goal_resume_suppressed = true;
		return true;
	}
	return false;
}

bool HandleCodexGoalResult(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& method,
	const std::string& request_id, const nlohmann::json& result)
{
	const std::unordered_map<std::string, AcpSessionState::CodexGoalOwner>::iterator pending = session.codex_goal_pending_requests.find(request_id);
	if (pending == session.codex_goal_pending_requests.end())
		return method == "thread/goal/get" || method == "thread/goal/set" || method == "thread/goal/clear";
	AcpSessionState::CodexGoalOwner owner = pending->second;
	session.codex_goal_pending_requests.erase(pending);
	if (!owner.parent_incarnation || owner.parent_incarnation != chat.metadata_incarnation ||
		owner.chat_id != chat.id || owner.thread_id != session.session_id ||
		(!owner.process_id.empty() && owner.process_id != session.last_process_id) ||
		owner.execution_host_id != (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) ||
		owner.execution_host_id != (session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id)) return true;
	using Action = AcpSessionState::CodexGoalOwner::Action;
	if (owner.action == Action::StopClear)
	{
		if (owner.thread_id != session.session_id || owner.chat_id != chat.id ||
			owner.revision != chat.goal_command_revision ||
			(!owner.process_id.empty() && owner.process_id != session.last_process_id) ||
			owner.execution_host_id != (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) ||
			owner.execution_host_id != (session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id)) return true;
		if (result.is_object() && result.contains("cleared") && result["cleared"].is_boolean())
		{
			if (chat.goal_command_revision == owner.revision)
				if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr) goal->native_binding.reset();
			session.codex_goal_owner = {};
			session.codex_goal_native_active = false;
			if (session.codex_turn_id.empty()) session.processing = false;
			PersistCodexControlReceipt(app, session, chat, owner);
		}
		else
		{
			session.last_error = "Codex did not confirm that the native goal stopped.";
			if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr)
				goal->last_diagnostic = "native_goal_stop_unconfirmed";
			SaveChatQuietly(app, chat);
		}
		return true;
	}
	if (owner.action == Action::StopPause)
	{
		if (!CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id, false)) return true;
		session.codex_goal_owner = owner;
		if (!result.is_object() || !result.contains("goal") ||
			!ApplyCodexNativeGoalReceipt(session, chat, result["goal"], CodexGoalReceiptMode::Stopping))
		{
			session.last_error = "Codex did not confirm that the native goal paused.";
			if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr)
				goal->last_diagnostic = "native_goal_stop_unconfirmed";
		}
		else if (session.codex_turn_id.empty()) session.processing = false;
		PersistCodexControlReceipt(app, session, chat, owner);
		return true;
	}
	if (owner.action == Action::InitialTurn)
	{
		session.codex_goal_initial_turn_request_id = 0;
		const std::string turn_id = JsonDiagnosticStringValue(JsonObjectValue(result, "turn"), "id");
		if (owner.thread_id == session.session_id && owner.chat_id == chat.id && !turn_id.empty())
		{
			session.codex_goal_initial_turn_id = turn_id;
			session.codex_turn_id = turn_id;
		}
		if (!owner.start_authorized || !CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id)) return true;
		if (turn_id.empty())
		{
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_initial_turn_unconfirmed");
			return true;
		}
		Goal* accepted_goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
		if (accepted_goal == nullptr || !accepted_goal->native_binding.has_value()) return true;
		if (accepted_goal->native_binding->initial_delivery.has_value())
		{
			NativeGoalInitialDelivery& witness = *accepted_goal->native_binding->initial_delivery;
			if (!NativeGoalInitialRowMatches(chat, *accepted_goal, witness) ||
				(!owner.resume_existing && (witness.state != NativeGoalInitialDelivery::State::Attempted ||
				 witness.user_message_index != owner.initial_history_user_message_index ||
				 witness.user_message_created_at != owner.initial_history_user_message_created_at))) return true;
			if (owner.resume_existing)
			{
				if (witness.state != NativeGoalInitialDelivery::State::Accepted) return true;
			}
			else
			{
				witness.state = NativeGoalInitialDelivery::State::Accepted;
				witness.turn_id = turn_id;
			}
		}
		else if (!owner.resume_existing) return true;
		owner.action = Action::Activate;
		owner.initial_history_state = AcpSessionState::CodexGoalOwner::InitialHistoryState::Pending;
		session.codex_goal_initial_turn_pending = owner;
		PersistCodexControlReceipt(app, session, chat, owner, [owner](AppState& current_app, AcpSessionState& current_session, ChatSession& current_chat)
		{
			const int next_id = current_session.next_request_id++;
			if (!SendCodexGoalRequest(current_session, BuildCodexGoalStatusRequest(next_id, owner.thread_id, "active", owner.user_origin), owner))
			{
				current_session.goal_resume_suppressed = true;
				if (Goal* goal = GoalService::FindGoalById(current_app, current_chat.id, owner.goal_id); goal != nullptr)
					goal->last_diagnostic = "native_goal_activation_write_failed";
			}
		});
		return true;
	}
	if (!owner.start_authorized || !CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id))
	{
		if (owner.action == Action::Activate && chat.active_goal_id.empty() &&
			session.codex_goal_owner.goal_id == owner.goal_id && owner.binding_created_at > 0 &&
			session.codex_goal_created_at == owner.binding_created_at)
			(void)StopCodexNativeGoal(session, chat, false, owner.goal_id, false);
		return true;
	}
	const int next_id = session.next_request_id++;
	if (owner.action == Action::Probe)
	{
		if (!result.is_object() || (result.contains("goal") && !result["goal"].is_null() && !result["goal"].is_object()))
		{
			HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_invalid_probe");
			return true;
		}
		session.codex_goal_capability = AcpSessionState::CodexGoalCapability::StateApi;
		if (result.contains("goal") && result["goal"].is_object())
		{
			const nlohmann::json& current = result["goal"];
			if (JsonDiagnosticStringValue(current, "threadId") != owner.thread_id)
			{
				HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_invalid_probe_thread");
				return true;
			}
			const bool bound = owner.resume_existing && current.contains("createdAt") && current["createdAt"].is_number_integer() &&
				current["createdAt"].get<std::int64_t>() == owner.binding_created_at &&
				JsonDiagnosticStringValue(current, "objective") == owner.objective;
			if (bound)
			{
				session.codex_goal_owner = owner;
				if (!ApplyCodexNativeGoalReceipt(session, chat, current, CodexGoalReceiptMode::Preparing))
				{
					HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_resume_state_unconfirmed");
					return true;
				}
				owner.action = Action::PreparePaused;
				if (!SendCodexGoalRequest(session, BuildCodexGoalStatusRequest(next_id, owner.thread_id, "paused", owner.user_origin), owner))
					HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_pause_write_failed");
				return true;
			}
			if (!owner.replace_native)
			{
				HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_binding_unavailable_requires_explicit_replace");
				return true;
			}
		}
		if (!owner.replace_native)
		{
			HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_missing_binding_requires_explicit_replace");
			return true;
		}
		owner.resume_existing = false;
		owner.binding_created_at = 0;
		owner.action = Action::ReplaceClear;
		if (!SendCodexGoalRequest(session, BuildCodexGoalClearRequest(next_id, owner.thread_id, owner.user_origin), owner))
			HoldUnconfirmedCodexGoal(app, session, chat, owner.goal_id, "native_goal_clear_write_failed");
		return true;
	}
	if (owner.action == Action::ReplaceClear)
	{
		if (!result.is_object() || !result.contains("cleared") || !result["cleared"].is_boolean())
		{
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_clear_unconfirmed");
			return true;
		}
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr) goal->native_binding.reset();
		session.codex_goal_owner = {};
		session.codex_goal_created_at = 0;
		session.codex_goal_updated_at = 0;
		session.codex_goal_native_active = false;
		owner.action = Action::PreparePaused;
		const std::int64_t remaining = owner.token_budget > 0 ? owner.token_budget - owner.tokens_used_at_start : 0;
		if (!SendCodexGoalRequest(session, BuildCodexGoalSetRequest(next_id, owner.thread_id, owner.objective, "paused", remaining, owner.user_origin), owner))
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_set_write_failed");
		return true;
	}
	session.codex_goal_owner = owner;
	if (owner.action == Action::PreparePaused && result.is_object() && result.contains("goal") &&
		JsonDiagnosticStringValue(result["goal"], "status") == "active")
	{
		HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_paused_state_unconfirmed");
		return true;
	}
	if (!result.is_object() || !result.contains("goal") || !ApplyCodexNativeGoalReceipt(session, chat, result["goal"],
		owner.action == Action::PreparePaused ? CodexGoalReceiptMode::Preparing : CodexGoalReceiptMode::Running))
	{
		HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_set_unconfirmed");
		return true;
	}
	owner.binding_created_at = session.codex_goal_created_at;
	session.codex_goal_owner = owner;
	if (owner.action == Action::PreparePaused && !chat.active_goal_id.empty())
	{
		Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
		if (goal == nullptr) return true;
		if (!goal->native_binding.has_value())
		{
			const nlohmann::json& receipt = result["goal"];
			const std::optional<std::int64_t> native_budget = receipt.contains("tokenBudget") && receipt["tokenBudget"].is_number_integer()
				? std::optional<std::int64_t>(receipt["tokenBudget"].get<std::int64_t>()) : std::nullopt;
			goal->native_binding = NativeGoalBinding{1, session.provider_id, owner.execution_host_id, owner.thread_id,
				owner.chat_id, owner.goal_id, owner.objective, session.codex_goal_created_at, session.codex_goal_updated_at, owner.tokens_used_at_start,
				owner.token_budget, native_budget, receipt["tokensUsed"].get<std::int64_t>()};
		}

		if (goal->native_binding->initial_delivery.has_value() &&
			goal->native_binding->initial_delivery->state == NativeGoalInitialDelivery::State::Pending)
		{
			owner.resume_existing = false;
			owner.initial_history_user_message_index = static_cast<int>(goal->native_binding->initial_delivery->user_message_index);
			owner.initial_history_user_message_created_at = goal->native_binding->initial_delivery->user_message_created_at;
		}
		session.codex_goal_initial_turn_pending = owner;
		StartPendingCodexNativeGoalTurn(app, session, chat);
		return true;
	}
	if (owner.action == Action::Activate) PersistCodexControlReceipt(app, session, chat, owner);
	else SaveChatQuietly(app, chat);
	return true;
}

bool HandleCodexGoalError(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpFailureDetails& failure)
{
	const std::unordered_map<std::string, AcpSessionState::CodexGoalOwner>::iterator pending = session.codex_goal_pending_requests.find(failure.request_id);
	if (pending == session.codex_goal_pending_requests.end())
		return failure.method == "thread/goal/get" || failure.method == "thread/goal/set" || failure.method == "thread/goal/clear";
	const AcpSessionState::CodexGoalOwner owner = pending->second;
	session.codex_goal_pending_requests.erase(pending);
	if (!owner.parent_incarnation || owner.parent_incarnation != chat.metadata_incarnation ||
		owner.chat_id != chat.id || owner.thread_id != session.session_id ||
		(!owner.process_id.empty() && owner.process_id != session.last_process_id) ||
		owner.execution_host_id != (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) ||
		owner.execution_host_id != (session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id)) return true;
	using Action = AcpSessionState::CodexGoalOwner::Action;
	if (owner.action == Action::InitialTurn) session.codex_goal_initial_turn_request_id = 0;
	if ((owner.action == Action::StopClear || owner.action == Action::StopPause) &&
		owner.chat_id == chat.id && owner.thread_id == session.session_id)
	{
		session.last_error = "Codex did not confirm that the native goal stopped.";
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id);
			goal != nullptr && owner.revision == chat.goal_command_revision)
			goal->last_diagnostic = "native_goal_stop_unconfirmed";
		if (session.codex_goal_queued_user_action.has_value())
		{
			const std::string queued_id = session.codex_goal_queued_user_action->goal_id;
			session.codex_goal_queued_user_action.reset();
			HoldCodexGoal(app, session, chat, queued_id, "native_goal_stop_unconfirmed");
		}
		SaveChatQuietly(app, chat);
		return true;
	}
	if (!CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id)) return true;
	if (owner.action == Action::Probe && failure.has_code && failure.code == -32601)
	{
		session.codex_goal_capability = AcpSessionState::CodexGoalCapability::Unsupported;
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, owner.goal_id); goal != nullptr)
		{
			goal->execution_owner = "uam";
			goal->provider_command.clear();
			goal->last_diagnostic = "native_goal_unsupported_using_uam";
			SaveChatQuietly(app, chat);
			std::string error;
			if (!ResumeGoal(app, chat.id, goal->id, &error)) HoldCodexGoal(app, session, chat, goal->id, error);
		}
		return true;
	}
	if (owner.action == Action::PreparePaused || owner.action == Action::Activate) session.codex_goal_owner = owner;
	HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_control_failed");
	return true;
}

std::string MessageTextForGoalReview(const ChatSession& chat, int index)
{
	if (index < 0 || index >= static_cast<int>(chat.messages.size()))
	{
		return "";
	}
	return chat.messages[static_cast<std::size_t>(index)].content;
}

std::string GoalTextPrefixForDiagnostics(const std::string& text)
{
	std::string flattened = text;
	std::replace(flattened.begin(), flattened.end(), '\n', ' ');
	std::replace(flattened.begin(), flattened.end(), '\r', ' ');
	return CapDiagnosticString(flattened, 160);
}

std::string GoalLoopDiagnosticDetail(const AcpSessionState& session, const std::string& goal_id, const std::string& text)
{
	std::ostringstream detail;
	detail << "goalId=" << goal_id
	       << "\ngoalTurnKind=" << session.goal_turn_kind
	       << "\ngoalReviewTurn=" << (session.goal_review_turn ? "true" : "false")
	       << "\ngoalReviewScheduled=" << (session.goal_review_scheduled ? "true" : "false")
	       << "\nturnUserMessageIndex=" << session.turn_user_message_index
	       << "\nturnAssistantMessageIndex=" << session.turn_assistant_message_index;
	if (!text.empty())
	{
		detail << "\ntextPrefix=" << GoalTextPrefixForDiagnostics(text);
	}
	return detail.str();
}

void AppendGoalLoopDiagnostic(AcpSessionState& session, const std::string& reason, const std::string& goal_id, const std::string& text)
{
	AppendAcpDiagnostic(session, "goal_loop", reason, "", "", false, 0, "", GoalLoopDiagnosticDetail(session, goal_id, text));
}

int64_t EstimateGoalTurnTokens(const ChatSession& chat, const AcpSessionState& session)
{
	int64_t tokens = 0;
	if (session.turn_user_message_index >= 0 && session.turn_user_message_index < static_cast<int>(chat.messages.size()))
	{
		const Message& message = chat.messages[static_cast<std::size_t>(session.turn_user_message_index)];
		tokens += message.tokens_input + message.tokens_output;
		if (message.tokens_input == 0 && message.tokens_output == 0)
		{
			tokens += static_cast<int64_t>(std::max<std::size_t>(1, message.content.size() / 4));
		}
	}
	if (session.turn_assistant_message_index >= 0 && session.turn_assistant_message_index < static_cast<int>(chat.messages.size()))
	{
		const Message& message = chat.messages[static_cast<std::size_t>(session.turn_assistant_message_index)];
		tokens += message.tokens_input + message.tokens_output;
		if (message.tokens_input == 0 && message.tokens_output == 0)
		{
			tokens += static_cast<int64_t>(std::max<std::size_t>(1, message.content.size() / 4));
		}
	}
	return std::max<int64_t>(1, tokens);
}

void BeginGoalReviewUsage(AcpSessionState& session, bool fresh_session)
{
	session.goal_review_usage = {};
	auto& receipt = session.goal_review_usage;
	receipt.turn_serial = session.turn_serial;
	receipt.fresh_session = fresh_session;
	auto& usage = session.provider_usage.token_usage;
	if (fresh_session)
	{
		// The internal review requests a new native context, with no resumed history.
		receipt.cumulative_baseline = 0;
		usage.goal_cumulative_total.reset();
		usage.goal_cumulative_session_id.clear();
	}
	else
	{
		receipt.session_id = session.session_id;
		if (!session.session_id.empty() && usage.goal_cumulative_session_id == session.session_id)
			receipt.cumulative_baseline = usage.goal_cumulative_total;
	}
}

void RecordGoalReviewTurnUsage(AcpSessionState& session, std::int64_t tokens, bool incremental)
{
	auto& receipt = session.goal_review_usage;
	if (!session.processing || !session.goal_review_turn || receipt.turn_serial != session.turn_serial || receipt.consumed || tokens < 0) return;
	if (session.session_id.empty() || (!receipt.session_id.empty() && receipt.session_id != session.session_id))
	{
		receipt.tokens.reset();
		receipt.cumulative_baseline.reset();
		receipt.turn_serial = -1;
		return;
	}
	if (receipt.session_id.empty()) receipt.session_id = session.session_id;
	const std::int64_t previous = incremental ? receipt.tokens.value_or(0) : 0;
	receipt.tokens = tokens > std::numeric_limits<std::int64_t>::max() - previous
	    ? std::numeric_limits<std::int64_t>::max() : previous + tokens;
}

void RecordGoalReviewCumulativeUsage(AcpSessionState& session, std::int64_t tokens)
{
	if (tokens < 0 || session.session_id.empty()) return;
	// Only validated adapter receipts reach this collector. Display telemetry is not a baseline.
	auto& usage = session.provider_usage.token_usage;
	if (usage.goal_cumulative_session_id != session.session_id)
	{
		usage.goal_cumulative_total.reset();
		usage.goal_cumulative_session_id = session.session_id;
	}
	usage.goal_cumulative_total = std::max(usage.goal_cumulative_total.value_or(0), tokens);
	auto& receipt = session.goal_review_usage;
	if (!session.processing || !session.goal_review_turn || receipt.turn_serial != session.turn_serial || receipt.consumed) return;
	if (receipt.fresh_session && receipt.session_id.empty()) receipt.session_id = session.session_id;
	if (session.session_id.empty() || receipt.session_id != session.session_id ||
	    !receipt.cumulative_baseline)
	{
		receipt.tokens.reset();
		receipt.cumulative_baseline.reset();
		return;
	}
	// A late older snapshot cannot erase usage already observed for this same receipt.
	if (tokens < *receipt.cumulative_baseline) return;
	const std::int64_t delta = tokens - *receipt.cumulative_baseline;
	if (receipt.tokens && delta < *receipt.tokens) return;
	RecordGoalReviewTurnUsage(session, delta);
}

bool CanQueueGoalInternalPrompt(const AcpSessionState& session)
{
	return !session.remote_source_exit_reported && session.session_ready && !session.processing && !session.waiting_for_permission && !session.waiting_for_user_input && !session.cancel_requested && session.prompt_request_id == 0 && session.cancel_request_id == 0 && session.queued_prompt.empty() && session.queued_user_prompts.empty();
}

std::string NormalizeGoalNextPrompt(const std::string& prompt)
{
	return uam::strings::Trim(prompt);
}

bool GoalBlockerStopsImmediately(const std::string& blocker_kind)
{
	return blocker_kind != "transient";
}

bool PreservesGoalPlan(const Goal& goal, const GoalService::ReviewDecision& decision)
{
	std::vector<std::string> existing = goal.completed_items;
	existing.insert(existing.end(), goal.remaining_items.begin(), goal.remaining_items.end());
	if (existing.empty()) return true;

	std::vector<std::string> updated = decision.completed_items;
	updated.insert(updated.end(), decision.remaining_items.begin(), decision.remaining_items.end());
	if (existing != updated) return false;

	return std::ranges::all_of(goal.completed_items, [&](const std::string& item)
	{
		return std::ranges::find(decision.completed_items, item) != decision.completed_items.end();
	});
}

void ApplyGoalProgressUpdate(Goal& goal, const GoalService::ReviewDecision& decision)
{
	if (decision.has_progress_update && PreservesGoalPlan(goal, decision))
	{
		goal.completed_items = decision.completed_items;
		goal.remaining_items = decision.remaining_items;
		goal.current_step = decision.current_step;
		if (goal.remaining_items.empty())
		{
			goal.current_step.clear();
		}
		else if (std::ranges::find(goal.remaining_items, decision.current_step) != goal.remaining_items.end())
		{
			goal.current_step = decision.current_step;
		}
		else
		{
			goal.current_step = goal.remaining_items.front();
		}
	}
	if (decision.has_progress_update)
	{
		goal.last_verification = decision.last_verification;
	}
	goal.loop_count += 1;
	goal.updated_at = uam::time::TimestampNow();
}

} // namespace uam::acp_detail
