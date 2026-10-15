#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct GoalWorkspaceFixture
	{
		TempDir temp{"uam-goal-workspace-admission"};
		uam::AppState app;
		std::shared_ptr<uam::WorktreeLaunchPreparationTask> task;
		std::string saved_history;

		GoalWorkspaceFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			ChatSession chat;
			chat.id = "private-goal-workspace";
			chat.created_at = "2026-10-15T00:00:00Z";
			chat.updated_at = chat.created_at;
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.small_model_mode = true;
			chat.memory_enabled = false;
			chat.svn_update_pending = true;
			chat.goal_command_revision = "private-original-revision";
			Goal goal;
			goal.id = "private-blocked-goal";
			goal.objective = "Preserve the blocked goal";
			goal.status = GoalStatus::Blocked;
			goal.blocked_turn_count = 2;
			goal.last_blocker = "Waiting for user approval";
			goal.last_blocker_kind = "user_permission_approval";
			goal.last_next_prompt = "Continue only after workspace readiness";
			goal.worker_model_id = "private-original-worker";
			goal.reviewer_model_id = "private-original-reviewer";
			goal.tokens_used = 17;
			goal.loop_count = 3;
			goal.created_at = chat.created_at;
			goal.updated_at = chat.updated_at;
			chat.goals.push_back(goal);
			goal.id = "private-stale-active-sibling";
			goal.status = GoalStatus::Active;
			chat.goals.push_back(goal);
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			saved_history = ReadFile(AppPaths::UamChatFilePath(app.data_root, app.chats.front().id));
			task = std::make_shared<uam::WorktreeLaunchPreparationTask>();
			task->data_root = app.data_root;
			task->original = app.chats.front();
			task->resolved_source = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, app.chats.front()));
			task->base_branch = app.settings.git_default_base_branch;
			task->pull_before_chat = app.settings.git_pull_before_chat;
			task->git_enabled = app.settings.git_enabled;
			task->automatic_worktree = app.settings.git_auto_worktree;
			task->svn_enabled = app.settings.svn_enabled;
			task->svn_update_before_chat = app.settings.svn_update_before_chat;
			task->update_only = true;
			app.worktree_launch_preparations.emplace(app.chats.front().id, task);
		}

		void AssertBlockedEvidence() const
		{
			const ChatSession& chat = app.chats.front();
			UAM_ASSERT(chat.active_goal_id.empty() && chat.goal_pending_continuation_id.empty());
			UAM_ASSERT_EQ(chat.goal_command_revision, std::string("private-original-revision"));
			UAM_ASSERT_EQ(chat.goals.size(), std::size_t{2});
			UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Blocked);
			UAM_ASSERT_EQ(chat.goals.front().blocked_turn_count, 2);
			UAM_ASSERT_EQ(chat.goals.front().last_blocker, std::string("Waiting for user approval"));
			UAM_ASSERT_EQ(chat.goals.front().last_blocker_kind, std::string("user_permission_approval"));
			UAM_ASSERT_EQ(chat.goals.front().last_next_prompt, std::string("Continue only after workspace readiness"));
			UAM_ASSERT_EQ(chat.goals.front().tokens_used, std::int64_t{17});
			UAM_ASSERT_EQ(chat.goals.front().loop_count, 3);
			UAM_ASSERT_EQ(chat.goals.front().worker_model_id, std::string("private-original-worker"));
			UAM_ASSERT_EQ(chat.goals.front().reviewer_model_id, std::string("private-original-reviewer"));
			UAM_ASSERT_EQ(chat.goals.back().status, GoalStatus::Active);
			UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), saved_history);
		}
	};
}

UAM_TEST(AcpGoalWorkspacePendingResumePreservesBlockedGoalAndSibling)
{
	for (const uam::acp_detail::GoalResumeStage stage : {uam::acp_detail::GoalResumeStage::Normal, uam::acp_detail::GoalResumeStage::PrepareOnly})
	{
		GoalWorkspaceFixture fixture;
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.app.chats.front().id, "private-blocked-goal", &error, false, true, false, stage));
		UAM_ASSERT(error.find("Workspace preparation") != std::string::npos && error.find("Retry") != std::string::npos);
		fixture.AssertBlockedEvidence();
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(AcpGoalWorkspaceFailureRejectsBeforeActivation)
{
	GoalWorkspaceFixture fixture;
	fixture.task->error = "Private goal workspace failure";
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.app.chats.front().id, "private-blocked-goal", &error));
	UAM_ASSERT_EQ(error, std::string("Private goal workspace failure"));
	fixture.AssertBlockedEvidence();
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
}

UAM_TEST(AcpGoalWorkspaceCanceledPreparationCannotResumeGoal)
{
	GoalWorkspaceFixture fixture;
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.app.chats.front().id, "private-blocked-goal", &error));
	UAM_ASSERT(fixture.app.acp_sessions.empty());
	uam::CancelWorktreeLaunchPreparation(fixture.app, fixture.app.chats.front().id);
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	fixture.AssertBlockedEvidence();
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
	UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
}

UAM_TEST(AcpGoalWorkspacePendingRestartKeepsOriginalDurableEvidence)
{
	GoalWorkspaceFixture fixture;
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, fixture.app.chats.front().id, "private-blocked-goal", &error, true));
	fixture.AssertBlockedEvidence();
	uam::AppState reloaded;
	reloaded.data_root = fixture.app.data_root;
	reloaded.provider_profiles = fixture.app.provider_profiles;
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(reloaded.data_root, fixture.app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	reloaded.chats.push_back(*saved);
	UAM_ASSERT(saved->active_goal_id.empty() && saved->goal_pending_continuation_id.empty());
	UAM_ASSERT_EQ(saved->goals.front().status, GoalStatus::Blocked);
	UAM_ASSERT_EQ(saved->goals.front().blocked_turn_count, 2);
	UAM_ASSERT_EQ(saved->goals.front().tokens_used, std::int64_t{17});
	UAM_ASSERT_EQ(saved->goals.back().status, GoalStatus::Active);
	UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(reloaded), std::size_t{0});
	UAM_ASSERT(reloaded.acp_sessions.empty());
}

UAM_TEST(AcpGoalWorkspaceExplicitRetryAcceptsOnlyAfterReady)
{
	GoalWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	std::string error;
	UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, chat.id, "private-blocked-goal", &error));
	fixture.AssertBlockedEvidence();
	chat.svn_update_pending = false;
	fixture.task->prepared = uam::PreparedLaunchWorktree{.operation = {.ok = true}, .metadata = chat};
	fixture.task->merged = true;
	fixture.task->saved = true;
	UAM_ASSERT(uam::acp_detail::ResumeGoal(fixture.app, chat.id, "private-blocked-goal", &error, false, true, false, uam::acp_detail::GoalResumeStage::PrepareOnly));
	UAM_ASSERT_EQ(chat.active_goal_id, std::string("private-blocked-goal"));
	UAM_ASSERT_EQ(chat.goals.front().status, GoalStatus::Active);
	UAM_ASSERT(chat.goals.front().last_blocker.empty());
	UAM_ASSERT_EQ(chat.goals.back().status, GoalStatus::Paused);
	const uam::AcpSessionState* session = uam::FindAcpSessionForChat(fixture.app, chat.id);
	UAM_ASSERT(session && !session->running && !session->processing && session->queued_prompt.empty());
	UAM_ASSERT_EQ(session->turn_serial, 0);
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
}

UAM_TEST(AcpGoalWorkspaceDirectQueuePendingPreservesTurnMetadata)
{
	GoalWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpSessionState& session = uam::acp_detail::EnsureAcpSessionForChat(fixture.app, chat);
	session.session_ready = true;
	session.turn_serial = 9;
	session.goal_turn_kind = "private-old-kind";
	session.goal_turn_model_id = "private-old-model";
	UAM_ASSERT(!uam::acp_detail::QueueGoalInternalPrompt(fixture.app, session, chat, "Private continuation", false, "private-new-model", true));
	UAM_ASSERT(!session.last_error.empty());
	UAM_ASSERT_EQ(session.turn_serial, 9);
	UAM_ASSERT_EQ(session.goal_turn_kind, std::string("private-old-kind"));
	UAM_ASSERT_EQ(session.goal_turn_model_id, std::string("private-old-model"));
	UAM_ASSERT(session.queued_prompt.empty() && !session.processing && !session.running && !session.reconnect_pending);
	UAM_ASSERT(chat.goal_iteration_goal_id.empty() && chat.goal_iteration_turn_kind.empty());
	fixture.AssertBlockedEvidence();
}

UAM_TEST(AcpGoalWorkspaceDirectQueueFailureKeepsHeldAdmission)
{
	GoalWorkspaceFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpSessionState& session = uam::acp_detail::EnsureAcpSessionForChat(fixture.app, chat);
	fixture.task->error = "Private direct goal workspace failure";
	session.session_ready = true;
	session.turn_serial = 9;
	session.queued_prompt = "Private held continuation";
	session.goal_queue_admission = uam::AcpSessionState::GoalQueueAdmission::Held;
	session.goal_queue_admission_turn_serial = 9;
	bool completed = false;
	UAM_ASSERT(!uam::acp_detail::QueueGoalInternalPrompt(fixture.app, session, chat, session.queued_prompt, false, "private-new-model", true,
	    []() { return true; }, [&completed](bool, std::string) { completed = true; }));
	UAM_ASSERT(!completed);
	UAM_ASSERT_EQ(session.last_error, std::string("Private direct goal workspace failure"));
	UAM_ASSERT_EQ(session.turn_serial, 9);
	UAM_ASSERT_EQ(session.queued_prompt, std::string("Private held continuation"));
	UAM_ASSERT_EQ(session.goal_queue_admission, uam::AcpSessionState::GoalQueueAdmission::Held);
	UAM_ASSERT(!session.processing && !session.running && !session.reconnect_pending);
	fixture.AssertBlockedEvidence();
}

UAM_TEST(AcpGoalWorkspaceAuthorityRejectsBeforePreparation)
{
	for (int variant = 0; variant < 4; ++variant)
	{
		GoalWorkspaceFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		fixture.app.worktree_launch_preparations.clear();
		if (variant == 0) chat.imported_read_only = true;
		if (variant == 1) chat.goals.front().status = GoalStatus::Complete;
		if (variant == 2) chat.active_goal_id = "private-other-goal";
		std::string error;
		UAM_ASSERT(!uam::acp_detail::ResumeGoal(fixture.app, chat.id, "private-blocked-goal", &error, false, true, false,
		    variant == 3 ? uam::acp_detail::GoalResumeStage::AfterReceipt : uam::acp_detail::GoalResumeStage::Normal,
		    variant == 3 ? std::function<bool()>([]() { return false; }) : std::function<bool()>{}));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() && fixture.app.worktree_operation_chat_ids.empty());
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
}
