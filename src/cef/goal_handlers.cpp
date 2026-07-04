#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"

#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// Goal handlers (set, update, set active, remove)
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;

void UamQueryHandler::HandleSetGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if (chat_id.empty())
	{
		cb->Failure(400, "Missing chat id.");
		return;
	}

	const std::string objective = payload.value("objective", "");
	if (objective.empty())
	{
		cb->Failure(400, "Goal requires an objective.");
		return;
	}

	const int64_t token_budget = payload.value("tokenBudget", static_cast<int64_t>(0));

	std::string goal_id;
	if (!uam::GoalService::CreateGoal(m_app, chat_id, objective, token_budget, &goal_id))
	{
		cb->Failure(404, "Chat not found.");
		return;
	}

	// Auto-activate the new goal
	uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id);
	if (ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id); chat != nullptr)
	{
		(void)ChatRepository::SaveChat(m_app.data_root, *chat);
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(R"({"goalId":")" + goal_id + R"("})");
}

void UamQueryHandler::HandleUpdateGoalStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string goal_id = payload.value("goalId", "");
	if (goal_id.empty())
	{
		cb->Failure(400, "Missing goal id.");
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

	if (!uam::GoalService::UpdateGoalStatus(m_app, goal_id, status))
	{
		cb->Failure(404, "Goal not found.");
		return;
	}

	// Find parent chat for push update
	std::string parent_chat_id;
	for (const auto& chat : m_app.chats)
	{
		for (const auto& goal : chat.goals)
		{
			if (goal.id == goal_id)
			{
				parent_chat_id = chat.id;
				break;
			}
		}
		if (!parent_chat_id.empty())
			break;
	}

	if (!parent_chat_id.empty())
	{
		if (ChatSession* chat = ChatDomainService().FindChatById(m_app, parent_chat_id); chat != nullptr)
		{
			(void)ChatRepository::SaveChat(m_app.data_root, *chat);
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
	}

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

	const bool updated = goal_id.empty() ? uam::GoalService::ClearActiveGoal(m_app, chat_id) : uam::GoalService::SetActiveGoal(m_app, chat_id, goal_id);
	if (!updated)
	{
		cb->Failure(404, "Failed to set active goal. Goal may not exist or is not in this chat.");
		return;
	}

	if (ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id); chat != nullptr)
	{
		(void)ChatRepository::SaveChat(m_app.data_root, *chat);
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleRemoveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string goal_id = payload.value("goalId", "");
	if (goal_id.empty())
	{
		cb->Failure(400, "Missing goal id.");
		return;
	}

	// Find parent chat before removing
	std::string parent_chat_id;
	for (const auto& chat : m_app.chats)
	{
		for (const auto& goal : chat.goals)
		{
			if (goal.id == goal_id)
			{
				parent_chat_id = chat.id;
				break;
			}
		}
		if (!parent_chat_id.empty())
			break;
	}

	if (!uam::GoalService::RemoveGoal(m_app, goal_id))
	{
		cb->Failure(404, "Goal not found.");
		return;
	}

	if (!parent_chat_id.empty())
	{
		if (ChatSession* chat = ChatDomainService().FindChatById(m_app, parent_chat_id); chat != nullptr)
		{
			(void)ChatRepository::SaveChat(m_app.data_root, *chat);
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
	}

	cb->Success("{}");
}
