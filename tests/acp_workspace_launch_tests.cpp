#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/paths/app_paths.h"

using namespace uam_test;

namespace
{
	struct WorkspacePromptFixture
	{
		TempDir temp{"uam-acp-workspace-launch"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		WorkspacePromptFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			ChatSession chat;
			chat.id = "private-workspace-launch";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			chat.messages.push_back({.role = MessageRole::User, .content = "Saved private history"});
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = app.chats.front().id;
			owned->provider_id = app.chats.front().provider_id;
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
		}
	};
}

namespace
{
	std::shared_ptr<uam::WorktreeLaunchPreparationTask> SeedOwnedAcpWorkspaceTask(WorkspacePromptFixture& fixture)
	{
		uam::AppState& app = fixture.app;
		ChatSession& chat = app.chats.front();
		fixture.session->running = false;
		chat.svn_update_pending = true;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = std::make_shared<uam::WorktreeLaunchPreparationTask>();
		task->data_root = app.data_root;
		task->original = chat;
		task->resolved_source = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
		task->base_branch = app.settings.git_default_base_branch;
		task->pull_before_chat = app.settings.git_pull_before_chat;
		task->git_enabled = app.settings.git_enabled;
		task->automatic_worktree = app.settings.git_auto_worktree;
		task->svn_enabled = app.settings.svn_enabled;
		task->svn_update_before_chat = app.settings.svn_update_before_chat;
		task->update_only = true;
		app.worktree_launch_preparations.emplace(chat.id, task);
		return task;
	}
}

UAM_TEST(AcpWorkspacePendingFirstDispatchRetainsDurableFifoHead)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = fixture.app.chats.front();
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "private-workspace-fifo";
	prompt.text = "Accepted workspace input";
	session.queued_user_prompts.push_back(prompt);
	chat.acp_queued_prompts = {prompt};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, session, chat));
	UAM_ASSERT(session.worktree_launch_pending && session.worktree_launch_cancellation == task->cancellation);
	UAM_ASSERT(!session.processing && !session.running && session.prompt_request_id == 0);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.reconnect_attempts, 0);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->acp_queued_prompts.size() == 1 && saved->svn_update_pending);
}

UAM_TEST(AcpWorkspaceDiscoveryCannotLaunchBeforeOwnedPreparation)
{
	WorkspacePromptFixture fixture;
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
	fixture.session->model_discovery_only = true;
	const std::shared_ptr<const char> incarnation = fixture.session->permission_review_incarnation;
	UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(fixture.app, *fixture.session, fixture.app.chats.front()));
	UAM_ASSERT(!fixture.session->running && fixture.session->worktree_launch_pending);
	UAM_ASSERT(fixture.session->permission_review_incarnation == incarnation);
	UAM_ASSERT(fixture.session->worktree_launch_cancellation == task->cancellation);
}

UAM_TEST(AcpWorkspaceFailureOrRevocationHoldsInputWithoutReplacement)
{
	for (const bool revoked : {false, true})
	{
		WorkspacePromptFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = SeedOwnedAcpWorkspaceTask(fixture);
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = fixture.app.chats.front();
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "private-held-workspace";
		prompt.text = "Retain failed workspace input";
		session.queued_user_prompts.push_back(prompt);
		chat.acp_queued_prompts = {prompt};
		session.worktree_launch_pending = true;
		session.worktree_launch_cancellation = task->cancellation;
		session.reconnect_pending = true;
		if (revoked) task->cancellation->request_stop();
		else task->error = "Private workspace preparation failed";
		UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, session, chat), uam::WorktreeLaunchPreparationState::Failed);
		UAM_ASSERT(session.queue_held && chat.acp_queue_held && !session.reconnect_pending);
		UAM_ASSERT(!session.worktree_launch_pending && !session.running && !session.processing);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(!session.last_error.empty());
		UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
	}
}
