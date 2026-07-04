#include "test_harness.h"

using namespace uam_test;

UAM_TEST(GoalServiceBuildContinuationPromptIncludesObjectiveAndBudget)
{
	Goal goal;
	goal.id = "goal_test_1";
	goal.objective = "Refactor the caching layer to reduce cold-start latency by 50%.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 8000;
	goal.tokens_used = 1500;

	const std::string prompt = uam::GoalService::BuildContinuationPrompt(goal, goal.tokens_used, goal.token_budget);

	UAM_ASSERT(!prompt.empty());
	UAM_ASSERT(prompt.find(goal.objective) != std::string::npos);
	UAM_ASSERT(prompt.find("Continue working toward the active thread goal.") != std::string::npos);
	UAM_ASSERT(prompt.find("Tokens used: 1500") != std::string::npos);
	UAM_ASSERT(prompt.find("Token budget: 8000") != std::string::npos);
	UAM_ASSERT(prompt.find("Tokens remaining: 6500") != std::string::npos);
}

UAM_TEST(GoalServiceBuildContinuationPromptReturnsEmptyForBlankObjective)
{
	Goal goal;
	goal.id = "goal_test_2";
	goal.objective = "";
	goal.status = GoalStatus::Active;

	const std::string prompt = uam::GoalService::BuildContinuationPrompt(goal, 0, 0);
	UAM_ASSERT(prompt.empty());
}

UAM_TEST(GoalServiceStatusBlockerAndTokenUpdatesMarkParentAndClearActive)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-service";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish the durable goal mode work.", 10, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	app.chats_with_unseen_updates.clear();

	uam::GoalService::RecordTurnCompletion(app, goal_id, 4);
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, static_cast<int64_t>(4));
	UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);

	uam::GoalService::RecordBlocker(app, goal_id, "Need user credentials.");
	uam::GoalService::RecordBlocker(app, goal_id, "Need user credentials.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	uam::GoalService::RecordBlocker(app, goal_id, "Need user credentials.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	uam::GoalService::RecordTurnCompletion(app, goal_id, 10);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker, std::string("Token budget exceeded."));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
}

UAM_TEST(GoalServiceUpdateStatusAndClearActiveGoalWork)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-clear-active";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Clear active goal correctly.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT(uam::GoalService::ClearActiveGoal(app, chat.id));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Complete));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Complete);
}

UAM_TEST(GoalServiceSetActiveGoalReactivatesBlockedGoal)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kOpenCodeCli);
	chat.id = "chat-resume-goal";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Resume a blocked OpenCode goal.", 0, &goal_id));
	uam::GoalService::RecordBlocker(app, goal_id, "Same blocker.");
	uam::GoalService::RecordBlocker(app, goal_id, "Same blocker.");
	uam::GoalService::RecordBlocker(app, goal_id, "Same blocker.");
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(app.chats.front().goals.front().blocked_turn_count, 0);
	UAM_ASSERT(app.chats.front().goals.front().last_blocker.empty());
}

UAM_TEST(GoalServicePauseClearsActiveGoalAndResumeReactivates)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-pause-goal";
	app.chats.push_back(chat);

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Pause and resume goal mode.", 0, &goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Paused));
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Paused);
	UAM_ASSERT(uam::GoalService::FindActiveGoal(app, chat.id) == nullptr);

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, goal_id));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
}

UAM_TEST(ChatRepositoryPersistsPausedGoalProgressState)
{
	TempDir temp("uam-goal-progress-state");
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-progress";
	Goal goal;
	goal.id = "goal-progress";
	goal.objective = "Track durable progress.";
	goal.status = GoalStatus::Paused;
	goal.token_budget = 100;
	goal.tokens_used = 20;
	goal.completed_items = {"first"};
	goal.remaining_items = {"second"};
	goal.current_step = "second";
	goal.last_verification = "npm test passed";
	goal.last_next_prompt = "Do second";
	goal.same_next_prompt_count = 2;
	goal.loop_count = 3;
	goal.created_at = "2026-01-01T00:00:00.000Z";
	goal.updated_at = "2026-01-01T00:00:01.000Z";
	chat.goals.push_back(goal);

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.size(), static_cast<std::size_t>(1));
	const Goal& loaded_goal = loaded.front().goals.front();
	UAM_ASSERT_EQ(loaded_goal.status, GoalStatus::Paused);
	UAM_ASSERT_EQ(loaded_goal.completed_items.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded_goal.completed_items.front(), std::string("first"));
	UAM_ASSERT_EQ(loaded_goal.remaining_items.front(), std::string("second"));
	UAM_ASSERT_EQ(loaded_goal.current_step, std::string("second"));
	UAM_ASSERT_EQ(loaded_goal.last_verification, std::string("npm test passed"));
	UAM_ASSERT_EQ(loaded_goal.last_next_prompt, std::string("Do second"));
	UAM_ASSERT_EQ(loaded_goal.same_next_prompt_count, 2);
	UAM_ASSERT_EQ(loaded_goal.loop_count, 3);
}

UAM_TEST(GoalServiceParseReviewDecisionAllowsWrappedStrictJson)
{
	const auto parsed = uam::GoalService::ParseReviewDecision("review:\n{\"decision\":\"continue\",\"reason\":\"more work remains\",\"nextPrompt\":\"Run tests\"}\nthanks");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->decision, std::string("continue"));
	UAM_ASSERT_EQ(parsed->reason, std::string("more work remains"));
	UAM_ASSERT_EQ(parsed->next_prompt, std::string("Run tests"));
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision("{\"decision\":\"maybe\",\"reason\":\"x\",\"nextPrompt\":\"y\"}").has_value());
}

UAM_TEST(GoalServiceParseReviewDecisionRejectsContinueWithoutNextPrompt)
{
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"continue","reason":"more work remains","nextPrompt":""})").has_value());
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"continue","reason":"more work remains","nextPrompt":"   "})").has_value());

	const auto blocked = uam::GoalService::ParseReviewDecision(R"({"decision":"blocked","reason":"Need user input.","nextPrompt":""})");
	UAM_ASSERT(blocked.has_value());
	UAM_ASSERT_EQ(blocked->decision, std::string("blocked"));
	UAM_ASSERT_EQ(blocked->next_prompt, std::string(""));
}

UAM_TEST(GoalServiceParseReviewDecisionRequiresEvidenceForCompleteAndReadsProgress)
{
	UAM_ASSERT(!uam::GoalService::ParseReviewDecision(R"({"decision":"complete","reason":"done","nextPrompt":""})").has_value());

	const auto parsed = uam::GoalService::ParseReviewDecision(R"({
		"decision":"continue",
		"reason":"more work remains",
		"nextPrompt":"Run the focused tests",
		"evidence":["diff reviewed"],
		"blockerKind":"transient",
		"progressUpdate":{
			"completed":["parser guard"],
			"remaining":["runtime guard"],
			"currentStep":"runtime guard",
			"lastVerification":"npm test passed"
		}
	})");
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->blocker_kind, std::string("transient"));
	UAM_ASSERT_EQ(parsed->evidence.front(), std::string("diff reviewed"));
	UAM_ASSERT_EQ(parsed->completed_items.front(), std::string("parser guard"));
	UAM_ASSERT_EQ(parsed->remaining_items.front(), std::string("runtime guard"));
	UAM_ASSERT_EQ(parsed->current_step, std::string("runtime guard"));
	UAM_ASSERT_EQ(parsed->last_verification, std::string("npm test passed"));
}

UAM_TEST(AcpGoalReviewTurnQueuesWorkerContinuationWithoutReviewingReviewOutput)
{
	TempDir temp("uam-goal-review-turn-completion");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Finish the work without looping.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = true;
	session->goal_review_scheduled = true;
	session->goal_review_goal_id = "goal-1";
	session->goal_review_user_prompt = "Implement the fix.";
	session->goal_review_assistant_text = "I inspected the runtime.";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More implementation remains.\",\"nextPrompt\":\"Apply the runtime guard.\",\"evidence\":[\"reviewed worker output\"]}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT(!raw_session->goal_review_turn);
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Apply the runtime guard."));
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_next_prompt, std::string("Apply the runtime guard."));
	UAM_ASSERT_EQ(app.chats.front().goals.front().same_next_prompt_count, 1);
}

UAM_TEST(AcpGoalReviewTurnWithLostReviewBoolDoesNotScheduleReviewOfReviewOutput)
{
	TempDir temp("uam-goal-review-lost-bool");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Avoid reviewing review output.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = false;
	session->goal_review_goal_id = "goal-1";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More work remains.\",\"nextPrompt\":\"Do the next worker task.\",\"evidence\":[\"review text\"]}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT(raw_session->goal_turn_kind.empty());
	UAM_ASSERT(!raw_session->goal_review_turn);
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);
	UAM_ASSERT_EQ(app.chats.front().goals.front().blocked_turn_count, 1);
}

UAM_TEST(AcpGoalSchedulerSkipsMissingUserIndexAndReviewDecisionOutput)
{
	TempDir temp("uam-goal-scheduler-guards");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Only review worker turns.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = -1;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Normal worker output."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);

	Message user;
	user.role = MessageRole::User;
	user.content = "Worker prompt";
	app.chats.front().messages.push_back(std::move(user));
	raw_session->processing = true;
	raw_session->prompt_request_id = 11;
	raw_session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	raw_session->turn_user_message_index = static_cast<int>(app.chats.front().messages.size()) - 1;
	raw_session->current_assistant_message_index = -1;
	raw_session->turn_assistant_message_index = -1;

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"Still going.\",\"nextPrompt\":\"Repeat this review.\",\"evidence\":[\"review output\"]}"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":11,"result":{}})"));
	UAM_ASSERT(!raw_session->goal_review_scheduled);
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().tokens_used, 0);
}

UAM_TEST(AcpGoalWorkerTurnSchedulesOneReview)
{
	TempDir temp("uam-goal-worker-schedules-review");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Implement the next fix.";
	chat.messages.push_back(std::move(user));
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Review normal worker turns.";
	goal.status = GoalStatus::Active;
	goal.token_budget = 1000;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"I changed the runtime guard."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));

	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("review"));
	UAM_ASSERT(raw_session->goal_review_turn);
	UAM_ASSERT(raw_session->goal_review_scheduled);
	UAM_ASSERT(!raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(raw_session->goal_review_goal_id, std::string("goal-1"));
	UAM_ASSERT_EQ(raw_session->goal_review_user_prompt, std::string("Implement the next fix."));
	UAM_ASSERT_EQ(raw_session->goal_review_assistant_text, std::string("I changed the runtime guard."));
	UAM_ASSERT(app.chats.front().goals.front().tokens_used > 0);
}

UAM_TEST(AcpGoalRepeatedIdenticalWorkerOutputMakesReviewLoopAware)
{
	TempDir temp("uam-goal-identical-output");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Implement the next fix.";
	chat.messages.push_back(std::move(user));
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Recover when the model loops.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	for (int turn = 1; turn <= 3; ++turn)
	{
		raw_session->processing = true;
		raw_session->prompt_request_id = 10 + turn;
		raw_session->pending_request_methods[10 + turn] = uam::acp_methods::kSessionPrompt;
		raw_session->turn_user_message_index = 0;
		raw_session->current_assistant_message_index = -1;
		raw_session->turn_assistant_message_index = -1;

		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Same worker output."}}}})"));
		const std::string result_line = std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(10 + turn) + R"(,"result":{}})";
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), result_line));

		UAM_ASSERT_EQ(app.chats.front().goals.front().same_assistant_text_count, turn);
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
		UAM_ASSERT(raw_session->goal_review_scheduled);
		// Reviews before the third identical turn carry no loop notice.
		UAM_ASSERT_EQ(raw_session->queued_prompt.find("loopDetection") != std::string::npos, turn >= 3);

		if (turn < 3)
		{
			// Mimic the review turn concluding so the next worker turn can run.
			raw_session->goal_review_scheduled = false;
			raw_session->goal_review_turn = false;
			raw_session->goal_review_goal_id.clear();
			raw_session->goal_turn_kind.clear();
			raw_session->queued_prompt.clear();
			raw_session->processing = false;
		}
	}

	UAM_ASSERT(raw_session->queued_prompt.find("identical output for 3 consecutive turns") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("materially different approach") != std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, std::string("goal-1"));
}

UAM_TEST(AcpGoalWatchdogResumesStalledLoopAndBlocksAfterRepeatedResumes)
{
	TempDir temp("uam-goal-watchdog");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Keep the loop moving.";
	goal.status = GoalStatus::Active;
	goal.loop_count = 1;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->processing = false;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	// User-cancelled sessions must not be auto-resumed.
	raw_session->goal_resume_suppressed = true;
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	raw_session->goal_resume_suppressed = false;

	UAM_ASSERT(uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	UAM_ASSERT(!raw_session->queued_prompt.empty());
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("worker_continuation"));
	UAM_ASSERT_EQ(raw_session->goal_auto_resume_attempts, 1);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);

	// A loop that keeps stalling is blocked visibly instead of retried forever.
	raw_session->processing = false;
	raw_session->queued_prompt.clear();
	raw_session->goal_turn_kind.clear();
	raw_session->goal_auto_resume_attempts = 3;
	raw_session->last_runtime_activity_time_s = 1.0;
	UAM_ASSERT(uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
}

UAM_TEST(AcpGoalWatchdogIgnoresFreshGoalsAndBusySessions)
{
	TempDir temp("uam-goal-watchdog-guards");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	Goal goal;
	goal.id = "goal-1";
	goal.objective = "Wait for the first user prompt.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = false;
	session->session_ready = true;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	// Fresh goal (no turns yet): not resumed.
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));

	// Engaged goal but busy session: not resumed.
	app.chats.front().goals.front().loop_count = 1;
	raw_session->processing = true;
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	raw_session->processing = false;

	// Engaged goal but recent activity: not resumed.
	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 2.0));
}

// NOTE: The former BuildPrompt/BuildCommand goal-context tests were removed with the
// dead one-shot command pipeline (PR-5). Goal-prompt composition is now exercised by the
// GoalServiceBuildContinuationPrompt* tests and the live ACP goal-review tests above.

