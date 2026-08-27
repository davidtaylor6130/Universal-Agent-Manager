#include "common/runtime/acp/acp_session_internal.h"

#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace uam::acp_detail
{

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
	return session.session_ready && !session.processing && !session.waiting_for_permission && !session.waiting_for_user_input && !session.cancel_requested && session.prompt_request_id == 0 && session.cancel_request_id == 0 && session.queued_prompt.empty() && session.queued_user_prompts.empty();
}

std::string NormalizeGoalNextPrompt(const std::string& prompt)
{
	return uam::strings::Trim(prompt);
}

bool GoalBlockerStopsImmediately(const std::string& blocker_kind)
{
	return blocker_kind == "needs_user" || blocker_kind == "needs_external_state" || blocker_kind == "invalid_review";
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
