#pragma once

#include "common/models/app_models.h"
#include "common/state/app_state.h"

#include <optional>
#include <string>
#include <vector>

namespace uam
{

/// <summary>
/// Service for managing thread-level goals (Codex /goal equivalent).
/// Goals persist across turns and drive the agent toward an objective.
/// </summary>
class GoalService
{
  public:
	struct ReviewDecision
	{
		std::string decision;
		std::string reason;
		std::string next_prompt;
		std::string blocker_kind;
		std::vector<std::string> evidence;
		std::vector<std::string> completed_items;
		std::vector<std::string> remaining_items;
		std::string current_step;
		std::string last_verification;
	};

	/// <summary>
	/// Create a new goal for a chat session.
	/// Returns true on success, false if chat not found.
	/// </summary>
	static bool CreateGoal(AppState& app, const std::string& chat_id, const std::string& objective,
	                       int64_t token_budget, std::string* created_goal_id = nullptr,
	                       std::string execution_owner = "uam", std::string provider_command = "");
	static bool IsProviderManaged(const Goal& goal);

	/// <summary>
	/// Update the objective of an existing goal.
	/// Returns true on success.
	/// </summary>
	static bool UpdateGoalObjective(AppState& app, const std::string& goal_id, const std::string& objective);

	/// <summary>
	/// Transition a goal to a new status (complete, blocked).
	/// Returns true on success.
	/// </summary>
	static bool UpdateGoalStatus(AppState& app, const std::string& goal_id, GoalStatus status);

	/// <summary>
	/// Set the active goal for a chat session. Only one active goal per chat.
	/// </summary>
	static bool SetActiveGoal(AppState& app, const std::string& chat_id, const std::string& goal_id);

	/// <summary>
	/// Clear the active goal for a chat session.
	/// </summary>
	static bool ClearActiveGoal(AppState& app, const std::string& chat_id);

	/// <summary>
	/// Find the active goal for a chat, or nullptr if none.
	/// </summary>
	static Goal* FindActiveGoal(AppState& app, const std::string& chat_id);
	static const Goal* FindActiveGoal(const AppState& app, const std::string& chat_id);

	/// <summary>
	/// Find a goal by its ID within a chat, or nullptr if not found.
	/// </summary>
	static Goal* FindGoalById(AppState& app, const std::string& chat_id, const std::string& goal_id);
	static const Goal* FindGoalById(const AppState& app, const std::string& chat_id, const std::string& goal_id);
	static ChatSession* FindChatForGoal(AppState& app, const std::string& goal_id);
	static const ChatSession* FindChatForGoal(const AppState& app, const std::string& goal_id);

	/// <summary>
	/// Get all goals for a chat session.
	/// </summary>
	static std::vector<Goal> GetGoalsForChat(const AppState& app, const std::string& chat_id);

	/// <summary>
	/// Remove a goal from a chat session. Returns true on success.
	/// </summary>
	static bool RemoveGoal(AppState& app, const std::string& goal_id);

	/// <summary>
	/// Record that the provider completed a turn while working on this goal.
	/// Updates token usage and checks if goal should be auto-completed.
	/// </summary>
	static void RecordTurnCompletion(AppState& app, const std::string& goal_id, int64_t tokens_used);

	/// <summary>
	/// Record a blocker for the goal. After >= 3 consecutive turns at the same
	/// blocker, the goal is auto-marked as blocked (per Codex behavior).
	/// </summary>
	static void RecordBlocker(AppState& app, const std::string& goal_id, const std::string& blocker);

	/// <summary>
	/// Build the continuation prompt text to inject before the next provider turn.
	/// Returns empty string if no active goal or no objective.
	/// </summary>
	static std::string BuildContinuationPrompt(const Goal& goal, int64_t tokens_used, int64_t token_budget,
	                                           bool small_model_mode = false, const std::string& next_step = "");
	static std::string BuildReviewPrompt(const Goal& goal, const std::string& recent_user_prompt,
	                                     const std::string& recent_assistant_text, int repeated_output_count = 0,
	                                     bool small_model_mode = false);
	static std::optional<ReviewDecision> ParseReviewDecision(const std::string& text);

	/// <summary>
	/// Generate a unique goal ID.
	/// </summary>
	static std::string GenerateGoalId();
};

} // namespace uam
