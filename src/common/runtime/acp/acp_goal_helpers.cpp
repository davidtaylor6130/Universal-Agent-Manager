#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/config/approval_modes.h"

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

bool CodexGoalReceiptMatchesOwner(const AcpSessionState& session, const ChatSession& chat,
	const AcpSessionState::CodexGoalOwner& owner, const std::string& thread_id, bool require_active)
{
	if (!session.running || !session.session_ready || session.provider_id != "codex-cli" || chat.provider_id != "codex-cli" ||
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
	AcpSessionState::CodexGoalOwner owner{chat.id, session.session_id, goal.id,
		chat.goal_command_revision, goal.objective, goal.token_budget, goal.tokens_used, true, true, replace_native};
	owner.execution_host_id = chat.execution_host_id.empty() ? "local" : chat.execution_host_id;
	owner.process_id = session.last_process_id;
	const AcpSessionState::CodexGoalOwner& previous = session.codex_goal_owner;
	if (!replace_native && previous.chat_id == chat.id && previous.thread_id == session.session_id &&
		previous.goal_id == goal.id && previous.objective == goal.objective && previous.token_budget == goal.token_budget &&
		previous.execution_host_id == owner.execution_host_id && previous.process_id == owner.process_id &&
		session.codex_goal_created_at > 0)
	{
		owner.resume_existing = true;
		owner.binding_created_at = session.codex_goal_created_at;
		owner.tokens_used_at_start = previous.tokens_used_at_start;
	}
	if (!session.codex_goal_pending_requests.empty())
	{
		for (const std::pair<const std::string, AcpSessionState::CodexGoalOwner>& pending : session.codex_goal_pending_requests)
		{
			if (pending.second.goal_id == goal.id && pending.second.revision == chat.goal_command_revision &&
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
	if (session.provider_id != "codex-cli") return true;
	const bool has_owned_goal = !session.codex_goal_owner.goal_id.empty() &&
		(goal_id.empty() || session.codex_goal_owner.goal_id == goal_id);
	const bool has_pending_goal = std::ranges::any_of(session.codex_goal_pending_requests,
		[&](const std::pair<const std::string, AcpSessionState::CodexGoalOwner>& pending)
		{
			return goal_id.empty() || pending.second.goal_id == goal_id;
		});
	const bool has_queued_goal = session.codex_goal_queued_user_action.has_value() &&
		(goal_id.empty() || session.codex_goal_queued_user_action->goal_id == goal_id);
	if (!has_owned_goal && !has_pending_goal && !has_queued_goal) return true;
	if (session.codex_goal_initial_turn_pending.has_value() &&
		(goal_id.empty() || session.codex_goal_initial_turn_pending->goal_id == goal_id))
		session.codex_goal_initial_turn_pending.reset();
	if (session.codex_goal_queued_user_action.has_value() &&
		(goal_id.empty() || session.codex_goal_queued_user_action->goal_id == goal_id))
		session.codex_goal_queued_user_action.reset();
	AcpSessionState::CodexGoalOwner owner = session.codex_goal_owner;
	bool set_sent = !owner.goal_id.empty() && (goal_id.empty() || owner.goal_id == goal_id);
	for (std::pair<const std::string, AcpSessionState::CodexGoalOwner>& request : session.codex_goal_pending_requests)
	{
		if (!goal_id.empty() && request.second.goal_id != goal_id) continue;
		request.second.start_authorized = false;
		if (request.second.chat_id != chat.id || request.second.thread_id != session.session_id) continue;
		owner = request.second;
		if (request.second.action == AcpSessionState::CodexGoalOwner::Action::StopClear ||
			request.second.action == AcpSessionState::CodexGoalOwner::Action::StopPause) return true;
		if (request.second.action != AcpSessionState::CodexGoalOwner::Action::Probe &&
			request.second.action != AcpSessionState::CodexGoalOwner::Action::ReplaceClear) set_sent = true;
	}
	session.goal_resume_suppressed = true;
	if (!set_sent || (!goal_id.empty() && owner.goal_id != goal_id) || owner.chat_id != chat.id || owner.thread_id != session.session_id) return true;
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
	const AcpSessionState::CodexGoalOwner owner = *session.codex_goal_initial_turn_pending;
	if (!owner.user_origin || !CodexGoalReceiptMatchesOwner(session, chat, owner, owner.thread_id))
	{
		session.codex_goal_initial_turn_pending.reset();
		return;
	}
	if (!session.codex_turn_id.empty() || session.prompt_request_id != 0 || !session.queued_prompt.empty() ||
		!session.queued_user_prompts.empty() || session.turn_checkpoint_commit_pending || session.manual_compaction_pending ||
		AcpSessionIsWaitingForInput(session) || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending) return;
	session.codex_goal_initial_turn_pending.reset();
	int user_index = -1;
	if (!owner.resume_existing)
	{
		user_index = static_cast<int>(chat.messages.size());
		chat.messages.push_back({MessageRole::User, owner.objective, uam::time::TimestampNow()});
	}
	if (!SaveChatQuietly(app, chat))
	{
		if (user_index >= 0) chat.messages.pop_back();
		HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_initial_history_save_failed");
		return;
	}
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
	using Action = AcpSessionState::CodexGoalOwner::Action;
	if (owner.action == Action::StopClear)
	{
		if (owner.thread_id != session.session_id || owner.chat_id != chat.id) return true;
		if (result.is_object() && result.contains("cleared") && result["cleared"].is_boolean())
		{
			session.codex_goal_owner = {};
			session.codex_goal_native_active = false;
			if (session.codex_turn_id.empty()) session.processing = false;
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
		SaveChatQuietly(app, chat);
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
		owner.action = Action::Activate;
		const int next_id = session.next_request_id++;
		if (!SendCodexGoalRequest(session, BuildCodexGoalStatusRequest(next_id, owner.thread_id, "active", owner.user_origin), owner))
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_activation_write_failed");
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
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_invalid_probe");
			return true;
		}
		session.codex_goal_capability = AcpSessionState::CodexGoalCapability::StateApi;
		if (result.contains("goal") && result["goal"].is_object())
		{
			const nlohmann::json& current = result["goal"];
			if (JsonDiagnosticStringValue(current, "threadId") != owner.thread_id)
			{
				HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_invalid_probe_thread");
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
					HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_resume_state_unconfirmed");
					return true;
				}
				owner.action = Action::PreparePaused;
				if (!SendCodexGoalRequest(session, BuildCodexGoalStatusRequest(next_id, owner.thread_id, "paused", owner.user_origin), owner))
					HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_pause_write_failed");
				return true;
			}
			if (!owner.replace_native)
			{
				HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_binding_unavailable_requires_explicit_replace");
				return true;
			}
		}
		owner.resume_existing = false;
		owner.binding_created_at = 0;
		owner.action = Action::ReplaceClear;
		if (!SendCodexGoalRequest(session, BuildCodexGoalClearRequest(next_id, owner.thread_id, owner.user_origin), owner))
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_clear_write_failed");
		return true;
	}
	if (owner.action == Action::ReplaceClear)
	{
		if (!result.is_object() || !result.contains("cleared") || !result["cleared"].is_boolean())
		{
			HoldCodexGoal(app, session, chat, owner.goal_id, "native_goal_clear_unconfirmed");
			return true;
		}
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
		session.codex_goal_initial_turn_pending = owner;
		SaveChatQuietly(app, chat);
		StartPendingCodexNativeGoalTurn(app, session, chat);
	}
	SaveChatQuietly(app, chat);
	return true;
}

bool HandleCodexGoalError(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpFailureDetails& failure)
{
	const std::unordered_map<std::string, AcpSessionState::CodexGoalOwner>::iterator pending = session.codex_goal_pending_requests.find(failure.request_id);
	if (pending == session.codex_goal_pending_requests.end())
		return failure.method == "thread/goal/get" || failure.method == "thread/goal/set" || failure.method == "thread/goal/clear";
	const AcpSessionState::CodexGoalOwner owner = pending->second;
	session.codex_goal_pending_requests.erase(pending);
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
