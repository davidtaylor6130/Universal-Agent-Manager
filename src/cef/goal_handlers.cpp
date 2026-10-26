#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "app/provider_resolution_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>

// ---------------------------------------------------------------------------
// Goal handlers (set, update, set active, remove)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	/// Rollback owns only goal metadata; messages and accounting outside it are never copied.
	struct GoalMutationSnapshot
	{
		std::vector<Goal> goals;
		std::string active, revision, pending, iteration_goal, iteration_kind, updated;
		int iteration_attempts;
		explicit GoalMutationSnapshot(const ChatSession& chat) : goals(chat.goals), active(chat.active_goal_id),
		    revision(chat.goal_command_revision), pending(chat.goal_pending_continuation_id),
		    iteration_goal(chat.goal_iteration_goal_id), iteration_kind(chat.goal_iteration_turn_kind),
		    updated(chat.updated_at), iteration_attempts(chat.goal_iteration_repair_attempts) {}
		void Restore(ChatSession& chat) const
		{
			chat.goals = goals; chat.active_goal_id = active; chat.goal_command_revision = revision;
			chat.goal_pending_continuation_id = pending; chat.goal_iteration_goal_id = iteration_goal;
			chat.goal_iteration_turn_kind = iteration_kind; chat.goal_iteration_repair_attempts = iteration_attempts;
			// Leave newer chat activity timestamps intact.
		}
	};

	std::string GoalMutationFingerprint(const ChatSession& chat)
	{
		nlohmann::json goals = nlohmann::json::array();
		for (const Goal& goal : chat.goals)
		{
			nlohmann::json binding = nullptr;
			if (goal.native_binding)
			{
				const NativeGoalBinding& b = *goal.native_binding;
				binding = {b.version, b.provider_id, b.execution_host_id, b.thread_id, b.uam_chat_id, b.uam_goal_id, b.objective, b.uam_budget_at_creation,
				    b.uam_tokens_at_creation, b.created_at, b.last_updated_at, b.native_token_budget ? nlohmann::json(*b.native_token_budget) : nlohmann::json(nullptr), b.last_native_tokens_used};
				if (b.initial_delivery) binding.push_back({b.initial_delivery->user_message_index,
				    b.initial_delivery->user_message_created_at, static_cast<int>(b.initial_delivery->state), b.initial_delivery->turn_id});
			}
			goals.push_back({goal.id, goal.objective, static_cast<int>(goal.status), goal.token_budget, goal.tokens_used,
			    goal.blocked_turn_count, goal.last_blocker, goal.last_blocker_kind, goal.last_diagnostic,
			    goal.completed_items, goal.remaining_items, goal.current_step, goal.last_verification, goal.last_next_prompt,
			    goal.same_next_prompt_count, goal.last_assistant_text, goal.same_assistant_text_count, goal.loop_count,
			    goal.created_at, goal.updated_at, goal.execution_owner, goal.provider_command, binding,
			    goal.worker_model_id, goal.reviewer_model_id, goal.creator, goal.creator_provider_id,
			    goal.creator_agent_id, goal.creator_run_id, goal.creator_request_key_hash});
		}
		return nlohmann::json::array({goals, chat.active_goal_id, chat.goal_command_revision,
		    chat.goal_pending_continuation_id, chat.goal_iteration_goal_id, chat.goal_iteration_turn_kind,
		    chat.goal_iteration_repair_attempts}).dump();
	}

	std::string GoalHandlerConfiguration(const uam::AppState& app, const ChatSession& chat,
	    const uam::AcpSessionState* session)
	{
		return nlohmann::json::array({chat.provider_id, chat.execution_host_id, chat.workspace_directory,
		    chat.workspace_source_directory, chat.workspace_worktree_directory, chat.workspace_isolation_kind,
		    chat.model_id, chat.reviewer_model_id, chat.uam_agent_id, chat.small_model_mode, chat.imported_read_only,
		    session ? uam::acp_detail::PromptDeliveryConfiguration(app, *session, chat) : std::string()}).dump();
	}

}

/// Publishes a FULL goal snapshot before authorizing its captured control continuation.
void UamQueryHandler::PersistGoalMutation(CefRefPtr<CefBrowser> browser, const std::string& chat_id,
    std::function<void()> rollback, std::function<bool(std::string*, std::function<bool()>, std::function<bool()>, std::function<void(bool, std::string)>)> execute,
    CefRefPtr<Callback> callback, std::string success)
{
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat) { callback->Failure(404, "Chat not found."); return; }
	const std::shared_ptr<GoalMutationOperation> operation = std::make_shared<GoalMutationOperation>();
	if (const std::unordered_map<std::string, std::shared_ptr<GoalMutationOperation>>::iterator old = m_goalMutations.find(chat_id); old != m_goalMutations.end()) old->second->cancelled = true;
	m_goalMutations[chat_id] = operation;
	const std::unordered_map<Callback*, ChatIconQueryIdentity>::const_iterator identity = m_goalQueryIdentities.find(callback.get());
	if (identity != m_goalQueryIdentities.end()) m_goalQueryOperations[identity->second] = operation;
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	const std::filesystem::path root = m_app.data_root;
	const std::shared_ptr<const std::uint8_t> incarnation = chat->metadata_incarnation;
	const std::string fingerprint = GoalMutationFingerprint(*chat);
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	const bool had_session = session != nullptr;
	const std::string process = session ? session->last_process_id : "";
	const std::string thread = session ? session->session_id : "";
	const std::string actual_process = session ? uam::acp_detail::AcpProcessHandleLabel(*session) : "";
	const std::string configuration = GoalHandlerConfiguration(m_app, *chat, session);
	const std::string desired_configuration = GoalHandlerConfiguration(m_app, *chat, nullptr);
	const nlohmann::json goal_fingerprint = nlohmann::json::parse(fingerprint);
	const std::string intent = nlohmann::json::array({goal_fingerprint[0], goal_fingerprint[1], goal_fingerprint[2]}).dump();
	const bool previous_suppression = session && session->goal_resume_suppressed;
	if (session) session->goal_resume_suppressed = true;
	const std::function<bool()> owned = [this, lifetime, operation, root, chat_id, incarnation,
	    fingerprint, had_session, process, actual_process, thread, configuration]()
	{
		if (lifetime.expired() || operation->cancelled || operation->finished || m_app.data_root != root) return false;
		const std::unordered_map<std::string, std::shared_ptr<GoalMutationOperation>>::const_iterator current_op = m_goalMutations.find(chat_id);
		if (current_op == m_goalMutations.end() || current_op->second != operation) return false;
		const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		const uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id);
		return current && current->metadata_incarnation == incarnation && had_session == (native != nullptr) &&
		    (!native || (native->last_process_id == process && uam::acp_detail::AcpProcessHandleLabel(*native) == actual_process && native->session_id == thread && !native->cancel_requested)) &&
		    configuration == GoalHandlerConfiguration(m_app, *current, native) && fingerprint == GoalMutationFingerprint(*current);
	};
	const std::function<bool()> continuation_owned = [this, lifetime, operation, root, chat_id, incarnation, desired_configuration, intent]()
	{
		if (lifetime.expired() || operation->cancelled || operation->finished || m_app.data_root != root) return false;
		const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		if (!current || current->metadata_incarnation != incarnation || desired_configuration != GoalHandlerConfiguration(m_app, *current, nullptr)) return false;
		const nlohmann::json fingerprint = nlohmann::json::parse(GoalMutationFingerprint(*current));
		return intent == nlohmann::json::array({fingerprint[0], fingerprint[1], fingerprint[2]}).dump();
	};
	const std::function<void(uam::acp_detail::DurableAdmissionState)> finish = [this, lifetime, operation, owned,
	    chat_id, browser, callback, continuation_owned, rollback = std::move(rollback), execute = std::move(execute),
	    previous_suppression, success = std::move(success)](uam::acp_detail::DurableAdmissionState state)
	{
		if (lifetime.expired() || operation->finished) return;
		const bool current = owned();
		const std::function<void()> retire = [this, operation, chat_id]()
		{
			operation->finished = true;
			const std::unordered_map<std::string, std::shared_ptr<GoalMutationOperation>>::const_iterator current_op = m_goalMutations.find(chat_id);
			if (current_op != m_goalMutations.end() && current_op->second == operation) m_goalMutations.erase(current_op);
			std::erase_if(m_goalQueryOperations, [&](const std::pair<const ChatIconQueryIdentity, std::weak_ptr<GoalMutationOperation>>& entry)
			{ const std::shared_ptr<GoalMutationOperation> pending = entry.second.lock(); return !pending || pending == operation; });
		};
		if (operation->cancelled) { retire(); return; }
		if (!current)
		{
			retire();
			callback->Failure(409, "Goal context changed before saving.");
			return;
		}
		if (state != uam::acp_detail::DurableAdmissionState::Committed)
		{
			rollback();
			if (uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id)) native->goal_resume_suppressed = previous_suppression;
			retire();
			callback->Failure(500, "Failed to persist goal state.");
			return;
		}
		// The continuation can only use this predicate within this committed callback.
		const std::shared_ptr<bool> receipt_active = std::make_shared<bool>(true);
		const std::function<bool()> admitted = [receipt_active, owned]() { return *receipt_active && owned(); };
		const std::function<void(bool, std::string)> completed = [this, lifetime, operation, receipt_active,
		    browser, callback, success, retire](bool executed, std::string error)
		{
			if (lifetime.expired() || operation->finished) return;
			*receipt_active = false;
			if (operation->cancelled) { retire(); return; }
			retire();
			uam::PushStateUpdateIfChanged(browser, m_app);
			if (executed) callback->Success(success);
			else callback->Failure(409, uam::strings::NonEmptyOrFallback(error, "Failed to resume goal."));
		};
		std::string error;
		if (uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id)) native->goal_resume_suppressed = previous_suppression;
		try
		{
			if (!execute) completed(true, "");
			else if (!execute(&error, admitted, continuation_owned, completed)) completed(false, error);
		}
		catch (const std::exception& exception) { completed(false, exception.what()); }
	};
	const uam::acp_detail::DurableAdmissionState admission = uam::acp_detail::BeginDurableAdmission(m_app, *chat, owned,
	    finish, uam::acp_detail::DurableAdmissionFailurePurpose::BindingOnly);
	if (admission == uam::acp_detail::DurableAdmissionState::Failed) finish(admission);
}

void UamQueryHandler::HandleSetGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string objective = uam::strings::Trim(payload.value("objective", ""));
	if (chat_id.empty() || objective.empty()) { cb->Failure(400, "Missing chat id or goal objective."); return; }
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat) { cb->Failure(404, "Chat not found."); return; }
	const GoalMutationSnapshot previous(*chat);
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
	const bool provider_managed = payload.value("executionOwner", "uam") == "provider" && !uam::strings::IsBlank(provider.native_goal_command);
	std::string goal_id, error;
	if (!uam::GoalService::CreateGoal(m_app, chat_id, objective, payload.value("tokenBudget", static_cast<int64_t>(0)),
	    &goal_id, provider_managed ? "provider" : "uam", provider_managed ? provider.native_goal_command : ""))
	{ cb->Failure(409, "Failed to create goal."); return; }
	if (!uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id, &error, nullptr, true))
	{ previous.Restore(*chat); cb->Failure(409, uam::strings::NonEmptyOrFallback(error, "Failed to activate goal.")); return; }
	const bool native_resume = provider_managed && chat->provider_id == "codex-cli";
	PersistGoalMutation(
	    browser, chat_id,
	    [this, chat_id, previous]()
	    {
		    if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id))
			    previous.Restore(*current);
	    },
	    [this, chat_id, goal_id, native_resume, stop_goal_id = previous.active](std::string* failure, std::function<bool()> admitted, std::function<bool()>, std::function<void(bool, std::string)> completion)
	    {
		    if (!admitted())
			    return false;
		    if (native_resume)
		    {
			    if (uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id))
				    native->goal_resume_suppressed = false;
			    const bool resumed = uam::acp_detail::ResumeGoal(m_app, chat_id, goal_id, failure, false, true, true, uam::acp_detail::GoalResumeStage::AfterReceipt, std::move(admitted));
			    completion(resumed, failure ? *failure : std::string());
			    return true;
		    }
		    if (uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id); native && native->codex_goal_owner.goal_id != goal_id)
		    {
			    ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			    if (current && !uam::acp_detail::StopCodexNativeGoal(*native, *current, true, native->codex_goal_owner.goal_id))
			    {
				    if (failure)
					    *failure = native->last_error;
				    return false;
			    }
			    if (current && !stop_goal_id.empty() && stop_goal_id != goal_id)
			    {
				    std::string stop_error;
				    if (!ProviderRuntimeRegistry::ResolveById(current->provider_id).OnAcpGoalControlCommitted(m_app, *native, *current, stop_goal_id, stop_error))
				    {
					    if (failure)
						    *failure = stop_error;
					    return false;
				    }
			    }
		    }
		    completion(true, "");
		    return true;
	    },
	    cb, nlohmann::json{{"goalId", goal_id}, {"executionOwner", provider_managed ? "provider" : "uam"}, {"providerCommand", provider_managed ? provider.native_goal_command : ""}}.dump());
}

void UamQueryHandler::HandleUpdateGoalStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");
	if (chat_id.empty() || goal_id.empty())
	{
		cb->Failure(400, "Missing chat or goal id.");
		return;
	}

	const std::string status_str = payload.value("status", "");
	GoalStatus status = GoalStatus::Active;
	if (status_str == "complete")
	{
		status = GoalStatus::Complete;
	}
	else if (status_str == "blocked")
	{
		status = GoalStatus::Blocked;
	}
	else if (status_str == "paused")
	{
		status = GoalStatus::Paused;
	}
	else if (status_str == "active")
	{
		cb->Failure(400, "Use resume goal to activate a goal.");
		return;
	}
	else
	{
		cb->Failure(400, "Invalid goal status: " + status_str);
		return;
	}

	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const GoalMutationSnapshot previous(*chat);
	if (!uam::GoalService::UpdateGoalStatus(m_app, chat_id, goal_id, status, true, true))
	{ cb->Failure(404, "Goal not found in this chat."); return; }
	PersistGoalMutation(browser, chat_id,
	    [this, chat_id, previous]() { if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id)) previous.Restore(*current); },
	    [this, chat_id, goal_id, status](std::string* error, std::function<bool()> admitted, std::function<bool()>, std::function<void(bool, std::string)> completion)
	    {
		if (!admitted()) return false;
		ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
		if (session && current && !uam::acp_detail::StopCodexNativeGoal(*session, *current, true, goal_id, status == GoalStatus::Complete))
		{ if (error) *error = session->last_error; return false; }
		if (session && current)
		{
			std::string stop_error;
			if (!ProviderRuntimeRegistry::ResolveById(current->provider_id).OnAcpGoalControlCommitted(m_app, *session, *current, goal_id, stop_error))
			{
				if (error)
					*error = stop_error;
				return false;
			}
		}
		completion(current != nullptr, "");
		return true;
	    }, cb);
}

void UamQueryHandler::HandleUpdateGoalObjective(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");
	const std::string objective = uam::strings::Trim(payload.value("objective", ""));
	if (chat_id.empty() || goal_id.empty())
	{
		cb->Failure(400, "Missing chat or goal id.");
		return;
	}
	if (objective.empty())
	{
		cb->Failure(400, "Goal objective is required.");
		return;
	}
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const GoalMutationSnapshot previous(*chat);
	std::string error;
	if (!uam::GoalService::UpdateGoalObjective(m_app, chat_id, goal_id, objective, &error))
	{
		cb->Failure(error == "Goal not found in this chat." ? 404 : 409,
		            uam::strings::NonEmptyOrFallback(error, "Failed to update goal objective."));
		return;
	}
	PersistGoalMutation(browser, chat_id,
	    [this, chat_id, previous]() { if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id)) previous.Restore(*current); },
	    {}, cb);
}

void UamQueryHandler::HandleSetActiveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", ""), goal_id = payload.value("goalId", "");
	if (chat_id.empty()) { cb->Failure(400, "Missing chatId."); return; }
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat) { cb->Failure(404, "Chat not found."); return; }
	const GoalMutationSnapshot previous(*chat);
	std::string error;
	const bool updated = goal_id.empty() ? uam::GoalService::ClearActiveGoal(m_app, chat_id, true, true)
	    : uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id, &error, nullptr, true);
	if (!updated)
	{
		previous.Restore(*chat);
		cb->Failure(error.empty() ? 404 : 409, uam::strings::NonEmptyOrFallback(error, "Failed to set active goal. Goal may not exist or is not in this chat."));
		return;
	}
	// A queued continuation cannot outrun this control's owned FULL publication.
	chat->goal_pending_continuation_id.clear();
	PersistGoalMutation(browser, chat_id,
	    [this, chat_id, previous]() { if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id)) previous.Restore(*current); },
	    [this, chat_id, goal_id, stop_goal_id = previous.active, retry_continuation = previous.pending == goal_id](std::string* failure, std::function<bool()> admitted,
	        std::function<bool()> continuation_owned, std::function<void(bool, std::string)> completion)
	    {
		if (!admitted()) return false;
		ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
		if (session && current && (goal_id.empty() || session->codex_goal_owner.goal_id != goal_id))
		{
			if (!uam::acp_detail::StopCodexNativeGoal(*session, *current, true, goal_id.empty() ? std::string() : stop_goal_id))
			{ if (failure) *failure = session->last_error; return false; }
		}
		if (session && current && stop_goal_id != goal_id)
		{
			std::string stop_error;
			if (!ProviderRuntimeRegistry::ResolveById(current->provider_id).OnAcpGoalControlCommitted(m_app, *session, *current, stop_goal_id, stop_error))
			{
				if (failure)
					*failure = stop_error;
				return false;
			}
		}
		Goal* goal = uam::GoalService::FindGoalById(m_app, chat_id, goal_id);
		if (current && current->provider_id == "codex-cli" && goal && uam::GoalService::IsProviderManaged(*goal))
		{
			if (session) session->goal_resume_suppressed = false;
			const bool resumed = uam::acp_detail::ResumeGoal(m_app, chat_id, goal_id, failure, false, true, false,
			    uam::acp_detail::GoalResumeStage::AfterReceipt, std::move(admitted));
			completion(resumed, failure ? *failure : std::string());
			return true;
		}
		if (current && !goal_id.empty() && session && (uam::AcpSessionHasActiveTurn(*session) || retry_continuation))
		{
			current->goal_pending_continuation_id = goal_id;
			session->goal_queue_admission = uam::AcpSessionState::GoalQueueAdmission::PendingContinuation;
			session->goal_queue_admission_revision = current->goal_command_revision;
			session->goal_resume_suppressed = true;
			const std::string revision = current->goal_command_revision;
			const std::string actual_process = uam::acp_detail::AcpProcessHandleLabel(*session);
			const std::string thread = session->session_id;
			const std::shared_ptr<bool> finished = std::make_shared<bool>(false);
			const std::function<bool()> owned = [this, continuation_owned, finished, chat_id, goal_id, revision, actual_process, thread]()
			{
				if (*finished || !continuation_owned()) return false;
				const ChatSession* stored = ChatDomainService().FindChatById(m_app, chat_id);
				const uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id);
				return stored && native && stored->goal_command_revision == revision && stored->goal_pending_continuation_id == goal_id &&
				    native->goal_queue_admission == uam::AcpSessionState::GoalQueueAdmission::PendingContinuation &&
				    native->goal_queue_admission_revision == revision && !native->cancel_requested && native->session_id == thread &&
				    uam::acp_detail::AcpProcessHandleLabel(*native) == actual_process;
			};
			const std::function<void(uam::acp_detail::DurableAdmissionState)> settle = [this, owned, finished, chat_id, completion](uam::acp_detail::DurableAdmissionState state)
			{
				if (*finished) return;
				const bool current = owned();
				*finished = true;
				if (!current) { completion(false, "Goal context changed before continuation publication."); return; }
				uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id);
				const bool committed = state == uam::acp_detail::DurableAdmissionState::Committed;
				native->goal_queue_admission = committed ? uam::AcpSessionState::GoalQueueAdmission::None : uam::AcpSessionState::GoalQueueAdmission::HeldContinuation;
				native->goal_resume_suppressed = !committed;
				completion(committed, committed ? "" : "Failed to persist the goal continuation.");
			};
			const uam::acp_detail::DurableAdmissionState state = uam::acp_detail::BeginDurableAdmission(m_app, *current, owned,
			    settle, uam::acp_detail::DurableAdmissionFailurePurpose::BindingOnly);
			if (state == uam::acp_detail::DurableAdmissionState::Failed) settle(state);
			return true;
		}
		completion(current != nullptr, "");
		return true;
	    }, cb);
}

void UamQueryHandler::HandleResumeGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", ""), goal_id = payload.value("goalId", "");
	if (chat_id.empty() || goal_id.empty()) { cb->Failure(400, "Missing chat or goal id."); return; }
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat) { cb->Failure(404, "Chat not found."); return; }
	const GoalMutationSnapshot previous(*chat);
	const bool restart = payload.value("restart", false);
	std::string error;
	if (!uam::acp_detail::ResumeGoal(m_app, chat_id, goal_id, &error, restart, true, false,
	    uam::acp_detail::GoalResumeStage::PrepareOnly))
	{ previous.Restore(*chat); cb->Failure(409, uam::strings::NonEmptyOrFallback(error, "Failed to resume goal.")); return; }
	PersistGoalMutation(browser, chat_id,
	    [this, chat_id, previous]() { if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id)) previous.Restore(*current); },
	    [this, chat_id, goal_id, restart](std::string* failure, std::function<bool()> admitted, std::function<bool()> continuation_owned, std::function<void(bool, std::string)> completion)
	    {
		if (!admitted()) return false;
		if (uam::AcpSessionState* native = uam::FindAcpSessionForChat(m_app, chat_id)) native->goal_resume_suppressed = false;
		const Goal* goal = uam::GoalService::FindGoalById(m_app, chat_id, goal_id);
		const bool native = goal && goal->execution_owner == "provider" && ChatDomainService().FindChatById(m_app, chat_id)->provider_id == "codex-cli";
		const uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
		const bool deferred = !native && session && !uam::AcpSessionHasActiveTurn(*session);
		const bool resumed = uam::acp_detail::ResumeGoal(m_app, chat_id, goal_id, failure, restart, true, false,
	        uam::acp_detail::GoalResumeStage::AfterReceipt, std::move(admitted), std::move(continuation_owned), deferred ? completion : std::function<void(bool, std::string)>{});
		if (!deferred || !resumed) completion(resumed, failure ? *failure : std::string());
		return true; }, cb);
}

void UamQueryHandler::HandleRemoveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", ""), goal_id = payload.value("goalId", "");
	if (chat_id.empty() || goal_id.empty()) { cb->Failure(400, "Missing chat or goal id."); return; }
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (!chat) { cb->Failure(404, "Chat not found."); return; }
	const GoalMutationSnapshot previous(*chat);
	const std::shared_ptr<uam::GoalService::GoalRemovalPreparation> preparation = std::make_shared<uam::GoalService::GoalRemovalPreparation>();
	std::string error;
	if (!uam::GoalService::RemoveGoal(m_app, chat_id, goal_id, &error, nullptr,
	    uam::GoalService::GoalRemovalStage::PrepareOnly, preparation.get()))
	{ cb->Failure(error.empty() ? 404 : 409, uam::strings::NonEmptyOrFallback(error, "Goal not found in this chat.")); return; }
	PersistGoalMutation(browser, chat_id,
	    [this, chat_id, previous]() { if (ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id)) previous.Restore(*current); },
	    [this, chat_id, goal_id, preparation](std::string* failure, std::function<bool()> admitted,
	        std::function<bool()>, std::function<void(bool, std::string)> completion)
	    {
		if (!admitted()) return false;
		const bool removed = uam::GoalService::RemoveGoal(m_app, chat_id, goal_id, failure, nullptr,
		    uam::GoalService::GoalRemovalStage::AfterReceipt, preparation.get(), admitted);
		if (removed && admitted() && preparation->native_owner)
		{
			ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
			const uam::AcpSessionState::CodexGoalOwner& captured = *preparation->native_owner;
			if (session && current && session->codex_goal_owner.parent_incarnation == captured.parent_incarnation &&
			    session->codex_goal_owner.goal_id == captured.goal_id && session->codex_goal_owner.revision == captured.revision &&
			    session->codex_goal_owner.thread_id == captured.thread_id && session->codex_goal_owner.process_id == captured.process_id &&
			    session->codex_goal_owner.initial_history_incarnation == captured.initial_history_incarnation &&
			    session->codex_goal_owner.binding_created_at == captured.binding_created_at &&
			    session->codex_goal_created_at == preparation->native_created_at &&
			    uam::acp_detail::AcpProcessHandleLabel(*session) == preparation->native_process_label)
			{
				// Clear ACK ownership does not require the already removed Goal row.
				if (!uam::acp_detail::StopCodexNativeGoal(*session, *current, true, goal_id, true))
				{ completion(false, session->last_error); return true; }
			}
			else { completion(false, "Native goal ownership changed before removal."); return true; }
		}
		completion(removed, failure ? *failure : std::string());
		return true;
	    }, cb);
}
