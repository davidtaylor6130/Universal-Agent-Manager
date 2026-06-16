#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "cef/cef_push.h"

namespace uam::acp_detail
{

bool HandleGoalReviewCompletion(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	if (!session.goal_review_turn)
	{
		return false;
	}

	const std::string goal_id = session.goal_review_goal_id;
	const std::string review_user_prompt = session.goal_review_user_prompt;
	const std::string review_assistant_text = session.goal_review_assistant_text;
	const std::string review_text = MessageTextForGoalReview(chat, session.turn_assistant_message_index);
	ClearGoalReviewState(session);

	const std::optional<GoalService::ReviewDecision> parsed = GoalService::ParseReviewDecision(review_text);
	if (!parsed.has_value())
	{
		GoalService::RecordBlocker(app, goal_id, "Goal reviewer returned invalid JSON.");
		SaveChatQuietly(app, chat);
		if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr && active_goal->id == goal_id && active_goal->blocked_turn_count < 2)
		{
			(void)QueueGoalInternalPrompt(session, chat, GoalService::BuildReviewPrompt(*active_goal, review_user_prompt, review_assistant_text), true);
		}
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}

	const GoalService::ReviewDecision& decision = *parsed;
	if (decision.decision == "complete")
	{
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, goal_id); goal != nullptr)
		{
			ApplyGoalProgressUpdate(*goal, decision);
		}
		(void)GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Complete);
		SaveChatQuietly(app, chat);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}
	if (decision.decision == "blocked")
	{
		if (Goal* goal = GoalService::FindGoalById(app, chat.id, goal_id); goal != nullptr)
		{
			ApplyGoalProgressUpdate(*goal, decision);
		}
		GoalService::RecordBlocker(app, goal_id, decision.reason);
		if (GoalBlockerStopsImmediately(decision.blocker_kind))
		{
			(void)GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Blocked);
		}
		SaveChatQuietly(app, chat);
		if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr && active_goal->id == goal_id && !GoalBlockerStopsImmediately(decision.blocker_kind))
		{
			(void)QueueGoalInternalPrompt(session, chat, GoalService::BuildContinuationPrompt(*active_goal, active_goal->tokens_used, active_goal->token_budget), false);
		}
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}

	Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
	if (active_goal == nullptr || active_goal->id != goal_id)
	{
		SaveChatQuietly(app, chat);
		return true;
	}

	ApplyGoalProgressUpdate(*active_goal, decision);
	const std::string follow_up = NormalizeGoalNextPrompt(decision.next_prompt);
	if (active_goal->last_next_prompt == follow_up)
	{
		active_goal->same_next_prompt_count += 1;
	}
	else
	{
		active_goal->last_next_prompt = follow_up;
		active_goal->same_next_prompt_count = 1;
	}
	if (active_goal->same_next_prompt_count >= 3)
	{
		GoalService::RecordBlocker(app, goal_id, "Goal reviewer repeated the same next prompt.");
		SaveChatQuietly(app, chat);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}
	SaveChatQuietly(app, chat);
	(void)QueueGoalInternalPrompt(session, chat, follow_up, false);
	if (browser)
	{
		uam::PushStateUpdateIfChanged(browser, app);
	}
	return true;
}

void ScheduleGoalReviewAfterSuccessfulTurn(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	if (session.goal_review_turn || session.goal_review_scheduled)
	{
		return;
	}
	if (session.goal_turn_kind == kGoalTurnKindReview)
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_after_review_turn", session.goal_review_goal_id);
		return;
	}
	if (session.turn_user_message_index < 0 || session.turn_assistant_message_index < 0)
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_missing_turn_indexes", session.goal_review_goal_id);
		return;
	}

	const std::string recent_user_prompt = MessageTextForGoalReview(chat, session.turn_user_message_index);
	const std::string recent_assistant_text = MessageTextForGoalReview(chat, session.turn_assistant_message_index);
	if (uam::strings::Trim(recent_user_prompt).empty() || uam::strings::Trim(recent_assistant_text).empty())
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_empty_turn_text", session.goal_review_goal_id, recent_assistant_text);
		return;
	}
	if (GoalService::ParseReviewDecision(recent_assistant_text).has_value())
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_review_decision_output", session.goal_review_goal_id, recent_assistant_text);
		return;
	}

	Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
	if (active_goal == nullptr || active_goal->objective.empty())
	{
		return;
	}

	GoalService::RecordTurnCompletion(app, active_goal->id, EstimateGoalTurnTokens(chat, session));
	active_goal = GoalService::FindActiveGoal(app, chat.id);
	SaveChatQuietly(app, chat);
	if (active_goal == nullptr)
	{
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return;
	}

	session.goal_review_scheduled = true;
	session.goal_review_goal_id = active_goal->id;
	session.goal_review_user_prompt = recent_user_prompt;
	session.goal_review_assistant_text = recent_assistant_text;
	const std::string review_prompt = GoalService::BuildReviewPrompt(*active_goal, session.goal_review_user_prompt, session.goal_review_assistant_text);
	AppendGoalLoopDiagnostic(session, "schedule_review", active_goal->id, recent_assistant_text);
	(void)QueueGoalInternalPrompt(session, chat, review_prompt, true);
}

void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser)
{
	const std::string completed_goal_turn_kind = session.goal_turn_kind;
	const bool completed_review_turn = completed_goal_turn_kind == kGoalTurnKindReview || session.goal_review_turn;
	const std::string goal_id = session.goal_review_goal_id;
	CompletePromptTurn(session, lifecycle_state);
	session.crash_restart_attempts = 0;

	if (completed_review_turn)
	{
		AppendGoalLoopDiagnostic(session, "complete_review_turn", goal_id, MessageTextForGoalReview(chat, session.turn_assistant_message_index));
		if (!HandleGoalReviewCompletion(app, session, chat, browser))
		{
			if (!goal_id.empty())
			{
				GoalService::RecordBlocker(app, goal_id, "Goal reviewer turn completed but could not be consumed.");
				SaveChatQuietly(app, chat);
				if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
			}
			ClearGoalReviewState(session);
		}
		if (session.goal_turn_kind == completed_goal_turn_kind)
		{
			session.goal_turn_kind.clear();
		}
		return;
	}

	ScheduleGoalReviewAfterSuccessfulTurn(app, session, chat, browser);
	if (session.goal_turn_kind == completed_goal_turn_kind)
	{
		session.goal_turn_kind.clear();
	}
}

} // namespace uam::acp_detail
