#include "test_harness.h"
#include "common/runtime/acp/acp_goal_loop.h"

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
	goal.objective = " \t ";
	goal.status = GoalStatus::Active;

	const std::string prompt = uam::GoalService::BuildContinuationPrompt(goal, 0, 0);
	UAM_ASSERT(prompt.empty());
}

UAM_TEST(GoalServiceMaintainsOnlyOneActiveGoalPerChat)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-single-active-goal";
	app.chats.push_back(chat);

	std::string first_goal_id;
	std::string second_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "First goal.", 0, &first_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, first_goal_id));
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Second goal.", 0, &second_goal_id));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, chat.id, second_goal_id));

	UAM_ASSERT_EQ(std::ranges::count(app.chats.front().goals, GoalStatus::Active, &Goal::status), static_cast<std::ptrdiff_t>(1));
}

UAM_TEST(GoalServiceSmallModelPromptPlansThenExecutesOneDurableStep)
{
	Goal goal;
	goal.objective = "Implement and verify the parser fix.";

	const std::string planning_prompt = uam::GoalService::BuildContinuationPrompt(goal, 0, 0, true);
	UAM_ASSERT(planning_prompt.find("This is the planning turn") != std::string::npos);
	UAM_ASSERT(planning_prompt.find("do not edit files") != std::string::npos);
	UAM_ASSERT(planning_prompt.find("3-8 ordered atomic, verifiable steps") != std::string::npos);

	goal.loop_count = 1;
	goal.completed_items = {"Located the shared parser"};
	goal.remaining_items = {"Add the parser guard", "Run the parser test"};
	goal.current_step = "Add the parser guard";
	goal.last_verification = "Parser test reproduces the failure";
	const std::string worker_prompt = uam::GoalService::BuildContinuationPrompt(
	    goal, 120, 1000, true, "Add one guard in parser.cpp and run the focused parser test.");
	UAM_ASSERT(worker_prompt.find("Execute exactly one atomic step") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Add one guard in parser.cpp") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Located the shared parser") != std::string::npos);
	UAM_ASSERT(worker_prompt.find("Parser test reproduces the failure") != std::string::npos);

	goal.loop_count = 0;
	const std::string review_prompt = uam::GoalService::BuildReviewPrompt(goal, goal.objective, "1. Inspect parser\n2. Add guard", 0, true);
	UAM_ASSERT(review_prompt.find("mandatory planning turn") != std::string::npos);
	UAM_ASSERT(review_prompt.find("ONLY output must be a single JSON object") != std::string::npos);
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

UAM_TEST(GoalServiceKeepsCompletedGoalsInHistory)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-completed-goal-history";
	app.chats.push_back(chat);

	std::string completed_goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Keep the completed goal.", 0, &completed_goal_id));
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, completed_goal_id, GoalStatus::Complete));
	UAM_ASSERT(!uam::GoalService::RemoveGoal(app, completed_goal_id));
	UAM_ASSERT(uam::GoalService::FindGoalById(app, chat.id, completed_goal_id) != nullptr);
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

UAM_TEST(AcpResumeGoalQueuesWorkBeforePublishingActiveState)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-resume-work";
	std::string goal_id;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Finish the paused work.", 0, &goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Paused;
	goal.last_next_prompt = "Run the focused regression tests.";

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	session->goal_resume_suppressed = true;
	session->goal_auto_resume_attempts = 2;
	session->turn_serial = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(goal.status, GoalStatus::Active);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Run the focused regression tests."));
	UAM_ASSERT_EQ(raw_session->turn_serial, 5);
	UAM_ASSERT(!raw_session->goal_resume_suppressed);
	UAM_ASSERT_EQ(raw_session->goal_auto_resume_attempts, 0);
}

UAM_TEST(AcpResumeGoalQueuesProviderCommandAndKeepsPausedWhenBusy)
{
	uam::AppState app;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-resume-provider";
	std::string goal_id;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::GoalService::CreateGoal(app, chat.id, "Ship the provider task.", 0, &goal_id, "provider", "/goal"));
	Goal& goal = app.chats.front().goals.front();
	goal.status = GoalStatus::Paused;

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(goal.status, GoalStatus::Paused);
	UAM_ASSERT_EQ(raw_session->turn_serial, 0);

	raw_session->processing = false;
	error.clear();
	UAM_ASSERT(uam::acp_detail::ResumeGoal(app, chat.id, goal_id, &error));
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("/goal Ship the provider task."));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, goal_id);
	UAM_ASSERT_EQ(goal.status, GoalStatus::Active);
}

UAM_TEST(ChatRepositoryPersistsPausedGoalProgressState)
{
	TempDir temp("uam-goal-progress-state");
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli);
	chat.id = "chat-goal-progress";
	chat.small_model_mode = true;
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
	UAM_ASSERT(loaded.front().small_model_mode);
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

	raw_session->queued_prompt.clear();
	raw_session->prompt_request_id = 11;
	raw_session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Applied the runtime guard and ran its focused test."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":11,"result":{}})"));
	UAM_ASSERT_EQ(raw_session->goal_turn_kind, std::string("review"));
	UAM_ASSERT(raw_session->goal_review_turn);
	UAM_ASSERT(raw_session->goal_review_scheduled);
	UAM_ASSERT_EQ(raw_session->goal_review_user_prompt, std::string("Apply the runtime guard."));
	UAM_ASSERT_EQ(raw_session->goal_review_assistant_text, std::string("Applied the runtime guard and ran its focused test."));
	UAM_ASSERT(raw_session->queued_prompt.find("You are a goal review agent") != std::string::npos);
}

UAM_TEST(AcpInvalidGoalReviewRetriesOnceThenBlocksInsteadOfCompleting)
{
	TempDir temp("uam-invalid-goal-review");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-invalid-review";
	chat.provider_id = "gemini-cli";
	Goal goal;
	goal.id = "goal-invalid-review";
	goal.objective = "Implement work that must not be marked complete by malformed output.";
	goal.status = GoalStatus::Active;
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-invalid-review";
	session->provider_id = "gemini-cli";
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
	session->goal_turn_kind = "review";
	session->goal_review_turn = true;
	session->goal_review_scheduled = true;
	session->goal_review_goal_id = goal.id;
	session->goal_review_user_prompt = "Implement the work.";
	session->goal_review_assistant_text = "I made partial progress.";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Goal Review COMPLETE"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT_EQ(raw_session->goal_review_repair_attempts, 1);
	UAM_ASSERT(raw_session->goal_review_turn);
	UAM_ASSERT(raw_session->queued_prompt.find("only repair attempt") != std::string::npos);

	raw_session->queued_prompt.clear();
	raw_session->prompt_request_id = 11;
	raw_session->pending_request_methods[11] = uam::acp_methods::kSessionPrompt;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Still not JSON"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":11,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().last_diagnostic, std::string("goal_blocked_invalid_review"));
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("goal_blocked_invalid_review"));
}

UAM_TEST(AcpGoalReviewContinuationHonorsConfiguredLoopCap)
{
	TempDir temp("uam-goal-loop-cap");
	auto run_review = [&](const std::string& chat_id, int max_iterations, int initial_loop_count, bool expect_blocked) {
		uam::AppState app;
		app.data_root = temp.root;
		app.settings.goal_max_loop_iterations = max_iterations;

		ChatSession chat;
		chat.id = chat_id;
		chat.provider_id = "gemini-cli";
		Goal goal;
		goal.id = "goal-1";
		goal.objective = "Finish without an unbounded loop.";
		goal.status = GoalStatus::Active;
		goal.loop_count = initial_loop_count;
		chat.active_goal_id = goal.id;
		chat.goals.push_back(goal);
		app.chats.push_back(std::move(chat));

		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat_id;
		session->session_ready = true;
		session->processing = true;
		session->prompt_request_id = 10;
		session->pending_request_methods[10] = uam::acp_methods::kSessionPrompt;
		session->goal_turn_kind = "review";
		session->goal_review_turn = true;
		session->goal_review_scheduled = true;
		session->goal_review_goal_id = "goal-1";
		uam::AcpSessionState* raw_session = session.get();
		app.acp_sessions.push_back(std::move(session));

		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"{\"decision\":\"continue\",\"reason\":\"More work remains.\",\"nextPrompt\":\"Continue the implementation.\"}"}}}})"));
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
		UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, initial_loop_count + 1);

		if (!expect_blocked)
		{
			UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
			UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Continue the implementation."));
			return;
		}

		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Blocked);
		UAM_ASSERT_EQ(app.chats.front().goals.front().last_blocker, std::string("Maximum goal loop iterations reached"));
		UAM_ASSERT(raw_session->queued_prompt.empty());
		UAM_ASSERT(!raw_session->diagnostics.empty());
		UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("goal_blocked_max_loop_iterations_reached"));
		const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
		const auto saved_chat = std::find_if(saved.begin(), saved.end(), [&](const ChatSession& candidate) { return candidate.id == chat_id; });
		UAM_ASSERT(saved_chat != saved.end());
		UAM_ASSERT_EQ(saved_chat->goals.front().last_blocker, std::string("Maximum goal loop iterations reached"));
		UAM_ASSERT_EQ(saved_chat->goals.front().last_diagnostic, std::string("goal_blocked_max_loop_iterations_reached"));
	};

	run_review("below-cap", 2, 0, false);
	run_review("at-cap", 2, 1, true);
	run_review("unlimited", 0, 200, false);
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

UAM_TEST(AcpCancelledGoalTurnDoesNotQueueAnotherGoalPrompt)
{
	TempDir temp("uam-goal-cancel");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-goal-cancel";
	chat.provider_id = "codex-cli";
	chat.messages.push_back(Message{MessageRole::User, "Do the work"});
	chat.messages.push_back(Message{MessageRole::Assistant, "Partial result"});
	Goal goal;
	goal.id = "goal-cancel";
	goal.objective = "Finish the work.";
	chat.goals.push_back(goal);
	chat.active_goal_id = goal.id;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-goal-cancel";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	session->cancel_requested = true;
	session->goal_resume_suppressed = true;
	session->turn_user_message_index = 0;
	session->turn_assistant_message_index = 1;
	session->current_assistant_message_index = 1;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                     R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turn":{"id":"turn-1","status":"interrupted"}}})"));

	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(!raw_session->processing);
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

UAM_TEST(AcpProviderManagedGoalSkipsUamLoopCompletesAndPersistsOwner)
{
	TempDir temp("uam-provider-managed-goal");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-provider-goal";
	chat.provider_id = "codex-cli";
	chat.messages.push_back({MessageRole::User, "/goal Keep this literal", "now"});
	chat.messages.push_back({MessageRole::Assistant, "Completed by provider.", "now"});
	app.chats.push_back(std::move(chat));

	std::string goal_id;
	UAM_ASSERT(uam::GoalService::CreateGoal(app, app.chats.front().id, "Keep this literal", 100, &goal_id, "provider", "/goal"));
	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, goal_id));
	Goal& goal = app.chats.front().goals.front();
	goal.loop_count = 7;

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = "codex-cli";
	session->session_ready = true;
	session->processing = true;
	session->turn_user_message_index = 0;
	session->turn_assistant_message_index = 1;
	session->last_runtime_activity_time_s = 1.0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!uam::ResumeStalledGoalLoopForTests(app, *raw_session, app.chats.front(), 100.0));
	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw_session, app.chats.front(), "ready", nullptr);
	UAM_ASSERT_EQ(goal.status, GoalStatus::Complete);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(goal.loop_count, 7);

	UAM_ASSERT(uam::GoalService::SetActiveGoal(app, app.chats.front().id, goal_id));
	std::string cancel_error;
	UAM_ASSERT(uam::CancelAcpTurn(app, app.chats.front().id, &cancel_error));
	UAM_ASSERT_EQ(goal.status, GoalStatus::Paused);
	UAM_ASSERT(app.chats.front().active_goal_id.empty());

	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.front().execution_owner, std::string("provider"));
	UAM_ASSERT_EQ(loaded.front().goals.front().provider_command, std::string("/goal"));
}

// NOTE: The former BuildPrompt/BuildCommand goal-context tests were removed with the
// dead one-shot command pipeline (PR-5). Goal-prompt composition is now exercised by the
// GoalServiceBuildContinuationPrompt* tests and the live ACP goal-review tests above.
