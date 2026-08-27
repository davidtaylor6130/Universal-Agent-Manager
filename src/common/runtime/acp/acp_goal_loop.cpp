#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/chat_domain_service.h"
#include "app/git_worktree_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/config/provider_chat_defaults.h"
#include "common/paths/workspace_root.h"
#include "common/config/settings_normalization.h"

namespace uam::acp_detail
{
	namespace
	{
		struct TurnCheckpointSnapshotInputs
		{
			std::filesystem::path data_root;
			AppSettings settings;
			std::vector<ChatFolder> folders;
			std::vector<ProviderProfile> provider_profiles;
			std::unordered_map<std::string, CliProviderVersionState> provider_versions;
			ChatSession chat;
		};

		TurnCheckpointSnapshotInputs CaptureTurnCheckpointSnapshot(const AppState& app, const ChatSession& chat)
		{
			return {app.data_root, app.settings, app.folders, app.provider_profiles, app.runtime_cli_versions_by_provider_id, chat};
		}

		AppState BuildTurnCheckpointSnapshot(TurnCheckpointSnapshotInputs inputs)
		{
			AppState snapshot;
			snapshot.data_root = std::move(inputs.data_root);
			snapshot.settings = std::move(inputs.settings);
			snapshot.folders = std::move(inputs.folders);
			snapshot.provider_profiles = std::move(inputs.provider_profiles);
			snapshot.runtime_cli_versions_by_provider_id = std::move(inputs.provider_versions);
			snapshot.chats.push_back(std::move(inputs.chat));
			return snapshot;
		}

		bool HasTurnCheckpointTask(const AppState& app, std::string_view chat_id)
		{
			return std::ranges::any_of(app.turn_checkpoint_tasks, [chat_id](const AsyncTurnCheckpointTask& task) { return task.chat_id == chat_id; });
		}

		std::string GoalOwnerChatId(const ChatSession& chat)
		{
			return uam::strings::NonEmptyOrFallback(chat.goal_owner_chat_id, chat.id);
		}

		ChatSession* GoalOwnerChat(AppState& app, const ChatSession& chat)
		{
			return ChatDomainService().FindChatById(app, GoalOwnerChatId(chat));
		}

		bool ContinueGoalIterationInChat(AppState& app, ChatSession& chat,
		                                 AcpSessionState& session,
		                                 const std::string& prompt,
		                                 std::string_view turn_kind,
		                                 int repair_attempts,
		                                 const std::string& model_id,
		                                 bool fresh_session,
		                                 std::string* error_out = nullptr)
		{
			chat.goal_iteration_turn_kind = std::string(turn_kind);
			chat.goal_iteration_repair_attempts = std::max(0, repair_attempts);
			if (!ChatRepository::SaveChat(app.data_root, chat))
			{
				if (error_out != nullptr) *error_out = "The goal iteration state could not be persisted.";
				return false;
			}
			if (QueueGoalInternalPrompt(app, session, chat, prompt,
			                            turn_kind == kGoalTurnKindReview,
			                            model_id, fresh_session)) return true;
			if (error_out != nullptr)
			{
				*error_out = uam::strings::NonEmptyOrFallback(
				    session.last_error, "The goal iteration prompt could not be queued.");
			}
			return false;
		}

		void StopCompletedGoalIterationRuntime(AppState& app, const ChatSession& chat,
		                                       const AcpSessionState& session)
		{
			if ((!chat.goal_owner_chat_id.empty() || session.goal_internal_session) &&
			    session.queued_user_prompts.empty())
			{
				(void)StopAcpSession(app, chat.id);
			}
		}

		void ContinueCompletedTurn(AppState& app,
		                           AcpSessionState& session,
		                           ChatSession& chat,
		                           CefRefPtr<CefBrowser> browser,
		                           bool continue_goal_loop,
		                           const std::string& completed_goal_turn_kind,
		                           bool completed_review_turn,
		                           const std::string& goal_id);
	}

	bool ScheduleTurnCheckpointPreflight(AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		session.turn_checkpoint_eligible = false;
		session.turn_checkpoint_preflight_pending = false;
		if (!uam::paths::IsGitWorktreeIsolated(chat) || HasTurnCheckpointTask(app, chat.id))
		{
			return false;
		}

		AsyncTurnCheckpointTask task;
		task.kind = AsyncTurnCheckpointTaskKind::Preflight;
		task.chat_id = chat.id;
		task.turn_serial = session.turn_serial;
		task.state = std::make_shared<AsyncTurnCheckpointState>();
		const std::shared_ptr<AsyncTurnCheckpointState> state = task.state;
		TurnCheckpointSnapshotInputs snapshot_inputs = CaptureTurnCheckpointSnapshot(app, chat);
		try
		{
			task.worker = std::make_unique<std::jthread>([state, snapshot_inputs = std::move(snapshot_inputs)](std::stop_token stop_token) mutable {
				try
				{
					AppState snapshot = BuildTurnCheckpointSnapshot(std::move(snapshot_inputs));
					std::string reason;
					state->eligible = GitWorktreeService().CanCheckpointTurn(snapshot, snapshot.chats.front(), &reason, stop_token);
					state->message = std::move(reason);
				}
				catch (const std::exception& exception)
				{
					state->message = std::string("Automatic checkpoint preflight failed: ") + exception.what();
				}
				catch (...)
				{
					state->message = "Automatic checkpoint preflight failed unexpectedly.";
				}
				state->finished.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception& exception)
		{
			app.status_line = std::string("Could not start automatic checkpoint preflight: ") + exception.what();
			return false;
		}
		session.turn_checkpoint_preflight_pending = true;
		app.worktree_operation_chat_ids.insert(chat.id);
		app.turn_checkpoint_tasks.push_back(std::move(task));
		return true;
	}

	namespace
	{
		bool ScheduleTurnCheckpointCommit(AppState& app,
		                                  AcpSessionState& session,
		                                  ChatSession& chat,
		                                  int assistant_message_index,
		                                  const std::string& completed_goal_turn_kind,
		                                  bool completed_review_turn,
		                                  const std::string& goal_id)
		{
			if (assistant_message_index < 0 || assistant_message_index >= static_cast<int>(chat.messages.size()) ||
		    HasTurnCheckpointTask(app, chat.id))
			{
				return false;
			}

			AsyncTurnCheckpointTask task;
			task.kind = AsyncTurnCheckpointTaskKind::Commit;
			task.chat_id = chat.id;
			task.turn_serial = session.turn_serial;
			task.assistant_message_index = assistant_message_index;
			task.expected_message_created_at = chat.messages[assistant_message_index].created_at;
			task.completed_goal_turn_kind = completed_goal_turn_kind;
			task.completed_review_turn = completed_review_turn;
			task.goal_id = goal_id;
			task.state = std::make_shared<AsyncTurnCheckpointState>();
			const std::shared_ptr<AsyncTurnCheckpointState> state = task.state;
			TurnCheckpointSnapshotInputs snapshot_inputs = CaptureTurnCheckpointSnapshot(app, chat);
			try
			{
				task.worker = std::make_unique<std::jthread>([state, snapshot_inputs = std::move(snapshot_inputs), assistant_message_index](std::stop_token stop_token) mutable {
					try
					{
						AppState snapshot = BuildTurnCheckpointSnapshot(std::move(snapshot_inputs));
						const GitTurnCheckpointResult result = GitWorktreeService().CreateTurnCheckpoint(snapshot, snapshot.chats.front(), assistant_message_index, stop_token);
						state->ok = result.ok;
						state->changed = result.changed;
						state->checkpoint_sha = result.checkpoint_sha;
						state->parent_sha = result.parent_sha;
						state->message = result.message;
					}
					catch (const std::exception& exception)
					{
						state->message = std::string("Automatic checkpoint failed: ") + exception.what();
					}
					catch (...)
					{
						state->message = "Automatic checkpoint failed unexpectedly.";
					}
					state->finished.store(true, std::memory_order_release);
				});
			}
			catch (const std::exception& exception)
			{
				app.status_line = std::string("Could not start automatic checkpoint: ") + exception.what();
				return false;
			}
			app.pending_chat_save_at_by_chat_id.erase(chat.id);
			app.worktree_operation_chat_ids.insert(chat.id);
			session.turn_checkpoint_commit_pending = true;
			session.goal_resume_suppressed = true;
			app.turn_checkpoint_tasks.push_back(std::move(task));
			return true;
		}
	}

bool ResumeGoal(AppState& app, const std::string& chat_id, const std::string& goal_id, std::string* error_out)
{
	ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
	Goal* goal = GoalService::FindGoalById(app, chat_id, goal_id);
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (chat == nullptr || goal == nullptr)
	{
		if (error_out != nullptr) *error_out = "Goal not found.";
		return false;
	}
	if (goal->status == GoalStatus::Complete)
	{
		if (error_out != nullptr) *error_out = "Completed goals cannot be resumed.";
		return false;
	}
	if (session == nullptr && !GoalService::IsProviderManaged(*goal))
	{
		session = &EnsureAcpSessionForChat(app, *chat);
	}
	if (session == nullptr || (GoalService::IsProviderManaged(*goal) && !CanQueueGoalInternalPrompt(*session)))
	{
		if (error_out != nullptr) *error_out = "The provider is not ready to resume this goal.";
		return false;
	}
	std::string prompt = GoalService::IsProviderManaged(*goal)
	                         ? uam::strings::Trim(goal->provider_command + " " + goal->objective)
	                         : uam::strings::Trim(goal->last_next_prompt);
	if (prompt.empty())
	{
		prompt = GoalService::BuildContinuationPrompt(*goal, goal->tokens_used, goal->token_budget);
	}
	if (!GoalService::IsProviderManaged(*goal))
	{
		const ProviderChatDefaults defaults =
		    uam::provider_chat_defaults::ForProvider(app.settings, chat->provider_id);
		goal->worker_model_id = uam::strings::NonEmptyOrFallback(
		    chat->model_id,
		    uam::strings::NonEmptyOrFallback(defaults.model_id, goal->worker_model_id));
		goal->reviewer_model_id = uam::strings::NonEmptyOrFallback(
		    chat->reviewer_model_id,
		    uam::strings::NonEmptyOrFallback(defaults.reviewer_model_id,
		                                     uam::strings::NonEmptyOrFallback(goal->reviewer_model_id,
		                                                                      goal->worker_model_id)));
	}
	const std::string model_id = GoalService::IsProviderManaged(*goal)
	                                 ? chat->model_id
	                                 : GoalService::WorkerModelId(*chat, *goal);
	if (!QueueGoalInternalPrompt(app, *session, *chat, prompt, false, model_id,
	                             !GoalService::IsProviderManaged(*goal)))
	{
		if (error_out != nullptr) *error_out = uam::strings::NonEmptyOrFallback(session->last_error, "Failed to queue the goal continuation.");
		return false;
	}

	session->goal_auto_resume_attempts = 0;
	session->crash_restart_attempts = 0;
	session->goal_resume_suppressed = false;
	return GoalService::SetActiveGoal(app, chat_id, goal_id);
}

bool HandleGoalReviewCompletion(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	const bool iteration_review = !chat.goal_owner_chat_id.empty() &&
	                              chat.goal_iteration_turn_kind == kGoalTurnKindReview;
	if (!session.goal_review_turn && !iteration_review)
	{
		return false;
	}

	const std::string owner_chat_id = GoalOwnerChatId(chat);
	const std::string goal_id = iteration_review ? chat.goal_iteration_goal_id : session.goal_review_goal_id;
	const std::string review_user_prompt = session.goal_review_user_prompt;
	const std::string review_assistant_text = session.goal_review_assistant_text;
	const std::string review_text = MessageTextForGoalReview(chat, session.turn_assistant_message_index);
	const int review_repair_attempts = iteration_review
	                                     ? chat.goal_iteration_repair_attempts
	                                     : session.goal_review_repair_attempts;
	ClearGoalReviewState(session);

	const std::optional<GoalService::ReviewDecision> parsed = GoalService::ParseReviewDecision(review_text);
	if (!parsed.has_value())
	{
		Goal* active_goal = GoalService::FindActiveGoal(app, owner_chat_id);
		if (review_repair_attempts == 0 && active_goal != nullptr && active_goal->id == goal_id)
		{
			if (!iteration_review)
			{
				session.goal_review_scheduled = true;
				session.goal_review_goal_id = goal_id;
				session.goal_review_user_prompt = review_user_prompt;
				session.goal_review_assistant_text = review_assistant_text;
				session.goal_review_repair_attempts = 1;
			}
			const std::string repair_prompt =
			    "Your previous goal review could not be parsed. This is the only repair attempt. "
			    "Do not call tools. Output exactly one valid JSON object and nothing else. "
			    "For continue use {\"decision\":\"continue\",\"reason\":\"...\",\"nextPrompt\":\"...\"}. "
			    "For blocked use {\"decision\":\"blocked\",\"reason\":\"...\",\"blockerKind\":\"...\"}. "
			    "For complete use {\"decision\":\"complete\",\"reason\":\"...\",\"evidence\":[\"one concrete item\"]}.";
			AppendGoalLoopDiagnostic(session, "review_json_invalid_retry", goal_id, review_text);
			const ChatSession* model_chat = GoalOwnerChat(app, chat);
			if (model_chat == nullptr) model_chat = &chat;
			if (iteration_review
				        ? ContinueGoalIterationInChat(app, chat, session, repair_prompt,
				                                      kGoalTurnKindReview, 1,
				                                      GoalService::ReviewerModelId(*model_chat, *active_goal), false)
				        : QueueGoalInternalPrompt(app, session, chat, repair_prompt, true,
				                                  GoalService::ReviewerModelId(chat, *active_goal)))
			{
				return true;
			}
			ClearGoalReviewState(session);
		}

		if (Goal* goal = GoalService::FindGoalById(app, owner_chat_id, goal_id); goal != nullptr)
		{
			goal->last_diagnostic = "goal_blocked_invalid_review";
			GoalService::RecordBlocker(app, owner_chat_id, goal_id, "Goal reviewer did not return valid JSON after one repair attempt.");
			(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Blocked);
		}
		AppendGoalLoopDiagnostic(session, "goal_blocked_invalid_review", goal_id, review_text);
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		StopCompletedGoalIterationRuntime(app, chat, session);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}

	const GoalService::ReviewDecision& decision = *parsed;
	if (decision.decision == "complete")
	{
		if (Goal* goal = GoalService::FindGoalById(app, owner_chat_id, goal_id); goal != nullptr)
		{
			ApplyGoalProgressUpdate(*goal, decision);
		}
		(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Complete);
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		StopCompletedGoalIterationRuntime(app, chat, session);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}
	if (decision.decision == "blocked")
	{
		if (Goal* goal = GoalService::FindGoalById(app, owner_chat_id, goal_id); goal != nullptr)
		{
			ApplyGoalProgressUpdate(*goal, decision);
		}
		GoalService::RecordBlocker(app, owner_chat_id, goal_id, decision.reason);
		if (GoalBlockerStopsImmediately(decision.blocker_kind))
		{
			(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Blocked);
		}
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		if (Goal* active_goal = GoalService::FindActiveGoal(app, owner_chat_id); active_goal != nullptr && active_goal->id == goal_id && !GoalBlockerStopsImmediately(decision.blocker_kind) && session.queued_user_prompts.empty())
		{
			const ChatSession* model_chat = GoalOwnerChat(app, chat);
			if (model_chat == nullptr) model_chat = &chat;
			const std::string worker_prompt = GoalService::BuildContinuationPrompt(*active_goal, active_goal->tokens_used, active_goal->token_budget, model_chat->small_model_mode, active_goal->current_step);
			(void)QueueGoalInternalPrompt(app, session, chat, worker_prompt, false,
			                              GoalService::WorkerModelId(*model_chat, *active_goal), true);
		}
		else
		{
			StopCompletedGoalIterationRuntime(app, chat, session);
		}
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}

	Goal* active_goal = GoalService::FindActiveGoal(app, owner_chat_id);
	if (active_goal == nullptr || active_goal->id != goal_id)
	{
		StopCompletedGoalIterationRuntime(app, chat, session);
		return true;
	}

	ApplyGoalProgressUpdate(*active_goal, decision);
	AppSettings bounded_settings = app.settings;
	uam::settings::ClampGoalSettings(bounded_settings);
	if (active_goal->loop_count >= bounded_settings.goal_max_loop_iterations)
	{
		constexpr const char* blocker = "Maximum goal loop iterations reached";
		active_goal->last_diagnostic = "goal_blocked_max_loop_iterations_reached";
		GoalService::RecordBlocker(app, owner_chat_id, goal_id, blocker);
		(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Blocked);
		AppendGoalLoopDiagnostic(session, "goal_blocked_max_loop_iterations_reached", goal_id, blocker);
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		StopCompletedGoalIterationRuntime(app, chat, session);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}
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
		GoalService::RecordBlocker(app, owner_chat_id, goal_id, "Goal reviewer repeated the same next prompt.");
		(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Blocked);
		AppendGoalLoopDiagnostic(session, "goal_blocked_repeated_next_prompt", goal_id, follow_up);
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		StopCompletedGoalIterationRuntime(app, chat, session);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}
	if (!session.queued_user_prompts.empty())
	{
		(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Paused);
		chat.goal_iteration_turn_kind.clear();
		if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
		SaveChatQuietly(app, chat);
		return true;
	}
	if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
	const ChatSession* model_chat = GoalOwnerChat(app, chat);
	if (model_chat == nullptr) model_chat = &chat;
	const std::string worker_prompt = model_chat->small_model_mode
	                                  ? GoalService::BuildContinuationPrompt(*active_goal, active_goal->tokens_used, active_goal->token_budget, true, follow_up)
	                                  : follow_up;
	(void)QueueGoalInternalPrompt(app, session, chat, worker_prompt, false,
	                              GoalService::WorkerModelId(*model_chat, *active_goal), true);
	if (browser)
	{
		uam::PushStateUpdateIfChanged(browser, app);
	}
	return true;
}

void ScheduleGoalReviewAfterSuccessfulTurn(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
{
	const bool goal_iteration = !chat.goal_owner_chat_id.empty();
	if (session.goal_review_turn || session.goal_review_scheduled)
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_already_in_progress", session.goal_review_goal_id);
		return;
	}
	if (session.goal_turn_kind == kGoalTurnKindReview ||
	    (goal_iteration && chat.goal_iteration_turn_kind == kGoalTurnKindReview))
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_after_review_turn", session.goal_review_goal_id);
		return;
	}
	if (session.turn_assistant_message_index < 0 ||
	    (session.turn_user_message_index < 0 && session.goal_turn_kind != kGoalTurnKindWorkerContinuation))
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_missing_turn_indexes", session.goal_review_goal_id);
		return;
	}

	const std::string recent_user_prompt = session.turn_user_message_index >= 0
	                                       ? MessageTextForGoalReview(chat, session.turn_user_message_index)
	                                       : session.goal_review_user_prompt;
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

	const std::string owner_chat_id = GoalOwnerChatId(chat);
	Goal* active_goal = GoalService::FindActiveGoal(app, owner_chat_id);
	if (active_goal == nullptr || active_goal->objective.empty())
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_no_active_goal", session.goal_review_goal_id);
		return;
	}
	if (GoalService::IsProviderManaged(*active_goal))
	{
		AppendGoalLoopDiagnostic(session, "skip_schedule_provider_managed", active_goal->id);
		return;
	}

	// Local models can degenerate into emitting the same output every turn.
	// Three identical back-to-back worker outputs make the reviewer loop-aware
	// so it changes approach or reports blocked, instead of continuing blindly.
	const std::string worker_text = uam::strings::Trim(recent_assistant_text);
	if (worker_text == active_goal->last_assistant_text)
	{
		active_goal->same_assistant_text_count += 1;
	}
	else
	{
		active_goal->last_assistant_text = worker_text;
		active_goal->same_assistant_text_count = 1;
	}
	if (active_goal->same_assistant_text_count >= 3)
	{
		AppendGoalLoopDiagnostic(session, "goal_loop_detected_review_notified", active_goal->id, worker_text);
	}

	GoalService::RecordTurnCompletion(app, owner_chat_id, active_goal->id, EstimateGoalTurnTokens(chat, session));
	active_goal = GoalService::FindActiveGoal(app, owner_chat_id);
	if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
	if (active_goal == nullptr)
	{
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return;
	}

	const std::string goal_id = active_goal->id;
	const ChatSession* model_chat = GoalOwnerChat(app, chat);
	if (model_chat == nullptr) model_chat = &chat;
	const std::string review_prompt = GoalService::BuildReviewPrompt(
	    *active_goal, recent_user_prompt, recent_assistant_text,
	    active_goal->same_assistant_text_count, model_chat->small_model_mode);
	AppendGoalLoopDiagnostic(session, "schedule_review", goal_id, recent_assistant_text);
	std::string queue_error;
	const bool queued = goal_iteration
	                      ? ContinueGoalIterationInChat(app, chat, session, review_prompt,
	                                                    kGoalTurnKindReview, 0,
	                                                    GoalService::ReviewerModelId(*model_chat, *active_goal),
	                                                    true,
	                                                    &queue_error)
	                      : QueueGoalInternalPrompt(app, session, chat, review_prompt, true,
	                                                GoalService::ReviewerModelId(chat, *active_goal), true);
	if (queued && !goal_iteration)
	{
		session.goal_review_scheduled = true;
		session.goal_review_goal_id = goal_id;
		session.goal_review_user_prompt = recent_user_prompt;
		session.goal_review_assistant_text = recent_assistant_text;
		session.goal_review_repair_attempts = 0;
	}
	if (!queued)
	{
		AppendGoalLoopDiagnostic(session, "queue_review_failed", goal_id, queue_error);
		ClearGoalReviewState(session);
		GoalService::RecordBlocker(app, owner_chat_id, goal_id,
		                           uam::strings::NonEmptyOrFallback(queue_error, "The private goal review could not start."));
		(void)GoalService::UpdateGoalStatus(app, owner_chat_id, goal_id, GoalStatus::Blocked);
		if (ChatSession* owner = ChatDomainService().FindChatById(app, owner_chat_id); owner != nullptr) SaveChatQuietly(app, *owner);
	}
}

bool ResumeStalledGoalLoopIfNeeded(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser, double now_seconds)
{
	if (!chat.goal_owner_chat_id.empty())
	{
		return false;
	}
	if (session.goal_resume_suppressed || !CanQueueGoalInternalPrompt(session))
	{
		return false;
	}

	Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
	if (active_goal == nullptr || active_goal->objective.empty())
	{
		return false;
	}
	if (GoalService::IsProviderManaged(*active_goal))
	{
		return false;
	}
	// Only resume a loop that has already run a turn; a freshly created goal
	// waits for the user's first prompt.
	if (active_goal->loop_count == 0 && active_goal->tokens_used == 0)
	{
		return false;
	}
	if (session.last_runtime_activity_time_s <= 0.0 || now_seconds - session.last_runtime_activity_time_s < kGoalLoopResumeIdleSeconds)
	{
		return false;
	}

	if (session.goal_auto_resume_attempts >= 3)
	{
		const std::string goal_id = active_goal->id;
		GoalService::RecordBlocker(app, chat.id, goal_id, "Goal loop stalled and could not be auto-resumed.");
		(void)GoalService::UpdateGoalStatus(app, chat.id, goal_id, GoalStatus::Blocked);
		AppendGoalLoopDiagnostic(session, "goal_blocked_auto_resume_exhausted", goal_id);
		SaveChatQuietly(app, chat);
		if (browser)
		{
			uam::PushStateUpdateIfChanged(browser, app);
		}
		return true;
	}

	session.goal_auto_resume_attempts += 1;
	// The session is idle, so any remaining goal-turn flags are leftovers of a
	// failed queue or write; clear them before resuming.
	session.goal_turn_kind.clear();
	ClearGoalReviewState(session);
	AppendGoalLoopDiagnostic(session, "auto_resume_stalled_goal_loop", active_goal->id);
	const std::string goal_id = active_goal->id;
	const std::string prompt = GoalService::BuildContinuationPrompt(
	    *active_goal, active_goal->tokens_used, active_goal->token_budget,
	    chat.small_model_mode, active_goal->current_step);
	if (!QueueGoalInternalPrompt(app, session, chat, prompt, false,
	                             GoalService::WorkerModelId(chat, *active_goal), true))
	{
		AppendGoalLoopDiagnostic(session, "auto_resume_goal_iteration_failed", goal_id, session.last_error);
	}
	if (browser)
	{
		uam::PushStateUpdateIfChanged(browser, app);
	}
	return true;
}

namespace
{
	void ContinueCompletedTurn(AppState& app,
	                           AcpSessionState& session,
	                           ChatSession& chat,
	                           CefRefPtr<CefBrowser> browser,
	                           bool continue_goal_loop,
	                           const std::string& completed_goal_turn_kind,
	                           bool completed_review_turn,
	                           const std::string& goal_id)
	{
		const std::string owner_chat_id = GoalOwnerChatId(chat);
		if (!continue_goal_loop)
		{
			ClearGoalReviewState(session);
			session.goal_turn_kind.clear();
			if (!session.queued_user_prompts.empty())
			{
				(void)uam::DrainNextQueuedAcpUserPrompt(app, session, chat);
			}
			return;
		}
		if (Goal* active_goal = GoalService::FindActiveGoal(app, owner_chat_id); active_goal != nullptr && GoalService::IsProviderManaged(*active_goal))
		{
			const std::string provider_goal_id = active_goal->id;
			ClearGoalReviewState(session);
			session.goal_turn_kind.clear();
			(void)GoalService::UpdateGoalStatus(app, owner_chat_id, provider_goal_id, GoalStatus::Complete);
			AppendGoalLoopDiagnostic(session, "provider_managed_complete", provider_goal_id);
			if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
			if (!session.queued_user_prompts.empty())
			{
				(void)uam::DrainNextQueuedAcpUserPrompt(app, session, chat);
			}
			if (browser)
			{
				uam::PushStateUpdateIfChanged(browser, app);
			}
			return;
		}

		if (completed_review_turn)
		{
			AppendGoalLoopDiagnostic(session, "complete_review_turn", goal_id, MessageTextForGoalReview(chat, session.turn_assistant_message_index));
			if (!HandleGoalReviewCompletion(app, session, chat, browser))
			{
				if (!goal_id.empty())
				{
					GoalService::RecordBlocker(app, owner_chat_id, goal_id, "Goal reviewer turn completed but could not be consumed.");
					if (ChatSession* owner = GoalOwnerChat(app, chat); owner != nullptr) SaveChatQuietly(app, *owner);
					if (browser)
					{
						uam::PushStateUpdateIfChanged(browser, app);
					}
				}
				ClearGoalReviewState(session);
			}
			if (!session.processing && session.goal_turn_kind == completed_goal_turn_kind)
			{
				session.goal_turn_kind.clear();
			}
			if (!session.queued_user_prompts.empty())
			{
				(void)uam::DrainNextQueuedAcpUserPrompt(app, session, chat);
			}
			return;
		}

		if (!session.queued_user_prompts.empty())
		{
			if (!session.processing && session.goal_turn_kind == completed_goal_turn_kind)
			{
				session.goal_turn_kind.clear();
			}
			(void)uam::DrainNextQueuedAcpUserPrompt(app, session, chat);
			return;
		}

		ScheduleGoalReviewAfterSuccessfulTurn(app, session, chat, browser);
		if (!session.processing && session.goal_turn_kind == completed_goal_turn_kind)
		{
			session.goal_turn_kind.clear();
		}
	}
}

bool PollTurnCheckpointTasks(AppState& app, CefRefPtr<CefBrowser> browser)
{
	std::vector<AsyncTurnCheckpointTask> completed;
	for (std::size_t index = 0; index < app.turn_checkpoint_tasks.size();)
	{
		AsyncTurnCheckpointTask& task = app.turn_checkpoint_tasks[index];
		if (task.state == nullptr || !task.state->finished.load(std::memory_order_acquire))
		{
			++index;
			continue;
		}
		completed.push_back(std::move(task));
		app.turn_checkpoint_tasks.erase(app.turn_checkpoint_tasks.begin() + static_cast<std::ptrdiff_t>(index));
	}
	if (completed.empty())
	{
		return false;
	}

	for (AsyncTurnCheckpointTask& task : completed)
	{
		task.worker.reset();
		AcpSessionState* session = FindAcpSessionForChat(app, task.chat_id);
		ChatSession* chat = ChatDomainService().FindChatById(app, task.chat_id);
		if (task.kind == AsyncTurnCheckpointTaskKind::Preflight)
		{
			app.worktree_operation_chat_ids.erase(task.chat_id);
			if (session != nullptr)
			{
				session->turn_checkpoint_preflight_pending = false;
				const bool same_turn_waiting = session->turn_serial == task.turn_serial && session->processing && !session->queued_prompt.empty();
				session->turn_checkpoint_eligible = same_turn_waiting && task.state->eligible;
				if (same_turn_waiting && chat != nullptr)
				{
					(void)SendQueuedPromptIfReady(*session, *chat);
				}
			}
			continue;
		}

		app.worktree_operation_chat_ids.erase(task.chat_id);
		if (session == nullptr || chat == nullptr)
		{
			continue;
		}
		session->turn_checkpoint_commit_pending = false;
		if (!task.cancelled)
		{
			session->goal_resume_suppressed = false;
		}
		const bool same_message = task.assistant_message_index >= 0 &&
		                          task.assistant_message_index < static_cast<int>(chat->messages.size()) &&
		                          chat->messages[task.assistant_message_index].created_at == task.expected_message_created_at;
		if (task.state->ok && task.state->changed && same_message)
		{
			Message& message = chat->messages[task.assistant_message_index];
			message.checkpoint_sha = task.state->checkpoint_sha;
			message.checkpoint_parent_sha = task.state->parent_sha;
		}
		else if (!task.state->ok)
		{
			app.status_line = uam::strings::NonEmptyOrFallback(task.state->message, "Automatic turn checkpoint failed.");
		}
		if (task.cancelled)
		{
			continue;
		}
		ContinueCompletedTurn(app,
		                      *session,
		                      *chat,
		                      browser,
		                      true,
		                      task.completed_goal_turn_kind,
		                      task.completed_review_turn,
		                      task.goal_id);
	}
	return true;
}

bool PollPendingGoalIterations(AppState& app)
{
	const bool changed = !app.pending_goal_iterations.empty();
	app.pending_goal_iterations.clear();
	return changed;
}

void CancelTurnCheckpointTasksForChat(AppState& app, std::string_view chat_id)
{
	for (AsyncTurnCheckpointTask& task : app.turn_checkpoint_tasks)
	{
		if (task.chat_id != chat_id)
		{
			continue;
		}
		task.cancelled = true;
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
		}
	}
}

void StopTurnCheckpointTasks(AppState& app)
{
	for (AsyncTurnCheckpointTask& task : app.turn_checkpoint_tasks)
	{
		if (task.worker != nullptr)
		{
			task.worker->request_stop();
		}
	}
	for (AsyncTurnCheckpointTask& task : app.turn_checkpoint_tasks)
	{
		task.worker.reset();
		app.worktree_operation_chat_ids.erase(task.chat_id);
		if (AcpSessionState* session = FindAcpSessionForChat(app, task.chat_id); session != nullptr)
		{
			session->turn_checkpoint_eligible = false;
			session->turn_checkpoint_preflight_pending = false;
			session->turn_checkpoint_commit_pending = false;
		}
	}
	app.turn_checkpoint_tasks.clear();
}

void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser, bool continue_goal_loop)
{
	const std::string completed_goal_turn_kind = session.goal_turn_kind;
	const bool completed_review_turn = completed_goal_turn_kind == kGoalTurnKindReview ||
	                                   session.goal_review_turn ||
	                                   (!chat.goal_owner_chat_id.empty() &&
	                                    chat.goal_iteration_turn_kind == kGoalTurnKindReview);
	const std::string goal_id = !chat.goal_owner_chat_id.empty()
	                              ? chat.goal_iteration_goal_id
	                              : session.goal_review_goal_id;
	const int assistant_index = session.current_assistant_message_index >= 0
	                                ? session.current_assistant_message_index
	                                : session.turn_assistant_message_index;
	const bool checkpoint_eligible = session.turn_checkpoint_eligible;
	session.turn_checkpoint_eligible = false;
	session.turn_checkpoint_preflight_pending = false;
	if (session.turn_started_time_s > 0.0 && assistant_index >= 0 && assistant_index < static_cast<int>(chat.messages.size()))
	{
		const double elapsed_ms = (GetAppTimeSeconds() - session.turn_started_time_s) * 1000.0;
		chat.messages[assistant_index].processing_time_ms = static_cast<int>(std::max(0.0, std::min(elapsed_ms, 2147483647.0)));
	}
	session.turn_started_time_s = 0.0;
	CompletePromptTurn(session, lifecycle_state);
	session.crash_restart_attempts = 0;
	session.reconnect_attempts = 0;
	session.reconnect_not_before_time_s = 0.0;
	session.goal_auto_resume_attempts = 0;
	if (continue_goal_loop && checkpoint_eligible && ScheduleTurnCheckpointCommit(
	        app, session, chat, assistant_index, completed_goal_turn_kind, completed_review_turn, goal_id))
	{
		return;
	}

	ContinueCompletedTurn(app, session, chat, browser, continue_goal_loop, completed_goal_turn_kind, completed_review_turn, goal_id);
}

} // namespace uam::acp_detail
