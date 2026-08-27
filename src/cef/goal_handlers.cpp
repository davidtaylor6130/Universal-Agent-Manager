#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "app/provider_resolution_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Goal handlers (set, update, set active, remove)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

namespace
{
	bool EnsureGoalChatWritable(uam::AppState& app, const ChatSession& chat,
	                            CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		if (ChatRepository::SaveChat(app.data_root, chat)) return true;
		cb->Failure(500, "Failed to persist goal state.");
		return false;
	}

	bool SaveGoalMutationOrRestore(uam::AppState& app, const std::string& chat_id,
	                               const ChatSession& previous,
	                               CefRefPtr<CefMessageRouterBrowserSide::Callback> cb)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat != nullptr && ChatRepository::SaveChat(app.data_root, *chat)) return true;
		if (chat != nullptr) *chat = previous;
		cb->Failure(500, "Failed to persist goal state.");
		return false;
	}
}

void UamQueryHandler::HandleSetGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chat id.");
		return;
	}

	const std::string objective = uam::strings::Trim(payload.value("objective", ""));
	if (objective.empty())
	{
		cb->Failure(400, "Goal requires an objective.");
		return;
	}

	const int64_t token_budget = payload.value("tokenBudget", static_cast<int64_t>(0));
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const ChatSession previous_chat = *chat;
	if (!EnsureGoalChatWritable(m_app, *chat, cb)) return;
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *chat);
	const bool provider_managed = payload.value("executionOwner", "uam") == "provider" && !uam::strings::IsBlank(provider.native_goal_command);

	std::string goal_id;
	if (!uam::GoalService::CreateGoal(m_app, chat_id, objective, token_budget, &goal_id, provider_managed ? "provider" : "uam", provider_managed ? provider.native_goal_command : ""))
	{
		cb->Failure(404, "Chat not found.");
		return;
	}

	// Auto-activate the new goal.
	std::string activation_error;
	if (!uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id, &activation_error))
	{
		(void)uam::GoalService::RemoveGoal(m_app, chat_id, goal_id);
		cb->Failure(409, uam::strings::NonEmptyOrFallback(activation_error, "Failed to activate the goal."));
		return;
	}
	if (!SaveGoalMutationOrRestore(m_app, chat_id, previous_chat, cb)) return;

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"goalId", goal_id}, {"executionOwner", provider_managed ? "provider" : "uam"}, {"providerCommand", provider_managed ? provider.native_goal_command : ""}}.dump());
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
		status = GoalStatus::Active;
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
	const ChatSession previous_chat = *chat;
	const bool stops_work = status == GoalStatus::Complete || status == GoalStatus::Blocked || status == GoalStatus::Paused;
	if (stops_work && !EnsureGoalChatWritable(m_app, *chat, cb)) return;
	std::string cancel_error;
	if (stops_work &&
	    !uam::GoalService::CancelGoalWork(m_app, chat_id, goal_id, &cancel_error))
	{
		cb->Failure(409, uam::strings::NonEmptyOrFallback(cancel_error, "Failed to stop goal work."));
		return;
	}
	if (!uam::GoalService::UpdateGoalStatus(m_app, chat_id, goal_id, status))
	{
		cb->Failure(404, "Goal not found in this chat.");
		return;
	}
	if (!SaveGoalMutationOrRestore(m_app, chat_id, previous_chat, cb)) return;
	uam::PushStateUpdateIfChanged(browser, m_app);

	cb->Success("{}");
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
	const ChatSession previous_chat = *chat;
	std::string error;
	if (!uam::GoalService::UpdateGoalObjective(m_app, chat_id, goal_id, objective, &error))
	{
		cb->Failure(error == "Goal not found in this chat." ? 404 : 409,
		            uam::strings::NonEmptyOrFallback(error, "Failed to update goal objective."));
		return;
	}
	if (!SaveGoalMutationOrRestore(m_app, chat_id, previous_chat, cb)) return;
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetActiveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");

	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chatId.");
		return;
	}
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const ChatSession previous_chat = *chat;
	if (!EnsureGoalChatWritable(m_app, *chat, cb)) return;

	bool updated = false;
	if (goal_id.empty())
	{
		const ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
		const std::string active_goal_id = chat == nullptr ? std::string{} : chat->active_goal_id;
		std::string cancel_error;
		updated = chat != nullptr && (active_goal_id.empty() ||
		          uam::GoalService::CancelGoalWork(m_app, chat_id, active_goal_id, &cancel_error)) &&
		          uam::GoalService::ClearActiveGoal(m_app, chat_id);
	}
	else
	{
		std::string activation_error;
		updated = uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id, &activation_error);
		if (!updated && !activation_error.empty())
		{
			cb->Failure(409, activation_error);
			return;
		}
	}
	if (!updated)
	{
		cb->Failure(404, "Failed to set active goal. Goal may not exist or is not in this chat.");
		return;
	}

	if (!SaveGoalMutationOrRestore(m_app, chat_id, previous_chat, cb)) return;
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleResumeGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");
	std::string error;
	if (chat_id.empty() || goal_id.empty())
	{
		cb->Failure(400, "Missing chat or goal id.");
		return;
	}
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const ChatSession previous_chat = *chat;
	if (!EnsureGoalChatWritable(m_app, *chat, cb)) return;
	if (!uam::acp_detail::ResumeGoal(m_app, chat_id, goal_id, &error))
	{
		cb->Failure(409, uam::strings::NonEmptyOrFallback(error, "Failed to resume goal."));
		return;
	}

	chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr || !ChatRepository::SaveChat(m_app.data_root, *chat))
	{
		(void)uam::GoalService::CancelGoalWork(m_app, chat_id, goal_id);
		chat = ChatDomainService().FindChatById(m_app, chat_id);
		if (chat != nullptr) *chat = previous_chat;
		cb->Failure(500, "Failed to persist goal state.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRemoveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string goal_id = payload.value("goalId", "");
	if (chat_id.empty() || goal_id.empty())
	{
		cb->Failure(400, "Missing chat or goal id.");
		return;
	}
	ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
	if (chat == nullptr)
	{
		cb->Failure(404, "Chat not found.");
		return;
	}
	const ChatSession previous_chat = *chat;
	if (!EnsureGoalChatWritable(m_app, *chat, cb)) return;

	std::string remove_error;
	if (!uam::GoalService::RemoveGoal(m_app, chat_id, goal_id, &remove_error))
	{
		cb->Failure(remove_error.empty() ? 404 : 409,
		            uam::strings::NonEmptyOrFallback(remove_error, "Goal not found in this chat."));
		return;
	}
	if (!SaveGoalMutationOrRestore(m_app, chat_id, previous_chat, cb)) return;
	uam::PushStateUpdateIfChanged(browser, m_app);

	cb->Success("{}");
}
