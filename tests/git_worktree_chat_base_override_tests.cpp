#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/workspace_vcs_context_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct BaseOverrideFixture
	{
		TempDir temporary{"uam-chat-base-override"};
		uam::AppState app;
		fs::path repository = temporary.root / "repo";
		std::string earlier;
		std::string head;
		BaseOverrideFixture()
		{
			fs::create_directories(repository);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
			UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
			UAM_ASSERT(RunGitForTest(repository, "config core.hooksPath " + ShellQuoteForTest((temporary.root / "no-hooks").string())));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "earlier\n"));
			UAM_ASSERT(RunGitForTest(repository, "add source.txt"));
			UAM_ASSERT(RunGitForTest(repository, "commit -m earlier"));
			UAM_ASSERT(RunGitForTest(repository, "branch feature/earlier"));
			earlier = uam::ReadWorkspaceVcsContext(repository, true, false).current_commit;
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "head survivor\n"));
			UAM_ASSERT(RunGitForTest(repository, "commit -am head"));
			head = uam::ReadWorkspaceVcsContext(repository, true, false).current_commit;
			app.data_root = temporary.root / "data";
			app.settings.git_default_base_branch = "missing-global-default";
		}
	};
}

UAM_TEST(GitChatBaseOverridePersistsInheritanceVersusExplicitHead)
{
	TempDir temp("uam-chat-base-roundtrip");
	for (const std::optional<std::string>& value : {std::optional<std::string>{}, std::optional<std::string>{""}, std::optional<std::string>{"feature/base"}})
	{
		ChatSession chat = ChatDomainService().CreateNewChat("folder", "codex-cli", true, value);
		UAM_ASSERT(chat.git_base_branch_override == value);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT(saved->git_base_branch_override == value);
		const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id);
		UAM_ASSERT(summary.has_value());
		UAM_ASSERT(summary->git_base_branch_override == value);
	}
}

UAM_TEST(GitChatBaseOverrideCreatesFromChosenCommitWithoutSwitchingSource)
{
	if (!GitAvailableForTests()) return;
	BaseOverrideFixture fixture;
	for (const std::string& choice : {std::string("feature/earlier"), std::string()})
	{
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", true, choice);
		chat.workspace_directory = fixture.repository.string();
		UAM_ASSERT(uam::GitWorktreeService().CreateForChat(fixture.app, chat).ok);
		UAM_ASSERT_EQ(chat.workspace_base_ref, choice.empty() ? fixture.head : fixture.earlier);
		UAM_ASSERT_EQ(ReadFile(uam::paths::PathFromUtf8(chat.workspace_worktree_directory) / "source.txt"), choice.empty() ? "head survivor\n" : "earlier\n");
		UAM_ASSERT_EQ(uam::ReadWorkspaceVcsContext(fixture.repository, true, false).current_commit, fixture.head);
		UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), "head survivor\n");
	}
}

UAM_TEST(GitChatBaseOverrideRefusesBeforeAnyOwnedCreationIntent)
{
	if (!GitAvailableForTests()) return;
	BaseOverrideFixture fixture;
	const fs::path plain = fixture.temporary.root / "plain";
	fs::create_directories(plain / ".svn");
	UAM_ASSERT(uam::io::WriteTextFile(plain / "keep.txt", "SVN/plain survivor\n"));
	for (const int variant : {0, 1, 2, 5, 6, 3, 4, 7, 8, 9, 10, 11, 12})
	{
		fixture.app.settings.git_auto_worktree = variant != 2;
		fixture.app.settings.git_enabled = variant != 6;
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", true, variant == 0 ? "missing" : variant == 5 ? "\n" : "");
		chat.workspace_directory = (variant == 1 ? plain : fixture.repository).string();
		if (variant == 3) UAM_ASSERT(RunGitForTest(fixture.repository, "checkout --detach HEAD"));
		if (variant == 4) chat.execution_host_id = "foreign-host";
		if (variant == 7) chat.imported_read_only = true;
		if (variant == 8) chat.temporary_parent_chat_id = "side-owner";
		if (variant == 9) chat.goal_owner_chat_id = "goal-owner";
		if (variant == 10) chat.agent_run_id = "agent-owner";
		if (variant == 11) chat.native_session_id = "existing-native-session";
		if (variant == 12) chat.routing_mode = "automatic";
		if (variant == 2 || variant == 4 || variant == 6 || variant >= 7)
		{
			std::string error;
			UAM_ASSERT(!uam::GitWorktreeService().PrepareForFirstLaunch(fixture.app, chat, &error));
			UAM_ASSERT(!error.empty());
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Failed);
			UAM_ASSERT(fixture.app.worktree_launch_preparations.empty());
			UAM_ASSERT_EQ(chat.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
		}
		unsigned intents = 0;
		const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().PrepareForChat(fixture.app, chat,
		    [&intents](const uam::GitWorktreeCreationIntent&, std::string&) { ++intents; return true; }, {});
		UAM_ASSERT(!result.ok);
		UAM_ASSERT(!result.message.empty());
		UAM_ASSERT_EQ(intents, 0u);
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT(!fs::exists(fixture.app.data_root));
	}
	UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), "head survivor\n");
	UAM_ASSERT_EQ(ReadFile(plain / "keep.txt"), "SVN/plain survivor\n");
}

UAM_TEST(GitChatBaseOverrideAsyncPreparationDurablyKeepsChosenPolicy)
{
	if (!GitAvailableForTests()) return;
	BaseOverrideFixture fixture;
	ChatSession created = ChatDomainService().CreateNewChat("", "codex-cli", true, "feature/earlier");
	created.workspace_directory = fixture.repository.string();
	created.messages.push_back({MessageRole::User, "body survivor"});
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, created));
	fixture.app.chats.push_back(created);
	ChatSession& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
		UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
	chat.title = "newer title survivor";
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	while (!task->saved.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(task->saved.has_value() && *task->saved);
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->git_base_branch_override == std::optional<std::string>{"feature/earlier"});
	UAM_ASSERT_EQ(saved->workspace_base_ref, fixture.earlier);
	UAM_ASSERT_EQ(saved->title, "newer title survivor");
	UAM_ASSERT_EQ(saved->messages.front().content, "body survivor");
	UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), "head survivor\n");
}

UAM_TEST(GitChatBaseOverrideCompletedIsolationStillLaunchesWithAutomaticPolicyDisabled)
{
	TempDir temp("uam-completed-override-policy");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_enabled = false;
	app.settings.git_auto_worktree = false;
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", true, "feature/base");
	chat.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	chat.workspace_directory = (temp.root / "source").string();
	chat.workspace_source_directory = chat.workspace_directory;
	chat.workspace_worktree_directory = (temp.root / "already-owned").string();
	chat.workspace_branch_name = "owned-branch";
	UAM_ASSERT(uam::GitWorktreeService().PrepareForFirstLaunch(app, chat));
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, chat).state, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, (temp.root / "already-owned").string());
	UAM_ASSERT_EQ(chat.workspace_branch_name, "owned-branch");
	UAM_ASSERT(chat.git_base_branch_override == std::optional<std::string>{"feature/base"});
	UAM_ASSERT(!fs::exists(app.data_root));
}

UAM_TEST(GitChatBaseOverrideChangedDuringPreparationCannotPublishOrLaunchOldChoice)
{
	if (!GitAvailableForTests()) return;
	for (const bool after_merge : {false, true})
	{
		BaseOverrideFixture fixture;
		const fs::path foreign = fixture.temporary.root / "foreign-worktree";
		UAM_ASSERT(RunGitForTest(fixture.repository, "worktree add -b foreign-survivor " + ShellQuoteForTest(foreign.string())));
		UAM_ASSERT(uam::io::WriteTextFile(foreign / "keep.txt", "foreign survivor\n"));
		ChatSession created = ChatDomainService().CreateNewChat("", "codex-cli", true, "feature/earlier");
		created.workspace_directory = fixture.repository.string();
		created.messages.push_back({MessageRole::User, "initial body"});
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, created));
		fixture.app.chats.push_back(created);
		ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, chat).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.app.worktree_launch_preparations.at(chat.id);
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		if (after_merge)
		{
			while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			UAM_ASSERT(task->merged && task->saving && !task->saved.has_value());
			UAM_ASSERT_EQ(chat.workspace_base_ref, fixture.earlier);
		}
		chat.git_base_branch_override = std::string(); // New choice is explicit HEAD, not inherited Settings.
		chat.title = "newer choice title";
		chat.messages.push_back({MessageRole::User, "newer choice body"});
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		if (!after_merge)
		{
			while (task->worker.valid() && task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		}
		UAM_ASSERT(task->cancellation->stop_requested());
		UAM_ASSERT(!task->saved.has_value() || !*task->saved);
		UAM_ASSERT(chat.git_base_branch_override == std::optional<std::string>{""});
		UAM_ASSERT_EQ(chat.workspace_isolation_kind, uam::paths::kPendingGitWorktreeIsolationKind);
		UAM_ASSERT(chat.workspace_worktree_directory.empty() && chat.workspace_base_ref.empty() && chat.workspace_branch_name.empty());
		UAM_ASSERT_EQ(chat.title, "newer choice title");
		UAM_ASSERT_EQ(chat.messages.back().content, "newer choice body");
		UAM_ASSERT(!fixture.app.worktree_launch_preparations.contains(chat.id));
		UAM_ASSERT(!fixture.app.worktree_operation_chat_ids.contains(chat.id));
		// Stale ownership preserves already-created artifacts and their receipt; cleanup must revalidate separately.
		if (task->prepared && task->prepared->operation.ok)
		{
			UAM_ASSERT(fs::exists(task->receipt_file));
			const fs::path old_worktree = uam::paths::PathFromUtf8(task->prepared->metadata.workspace_worktree_directory);
			UAM_ASSERT_EQ(ReadFile(old_worktree / "source.txt"), "earlier\n");
			UAM_ASSERT_EQ(task->prepared->metadata.workspace_base_ref, fixture.earlier);
		}
		uam::acp_detail::ScheduleChatSave(fixture.app, chat, 0.0);
		const std::shared_ptr<std::optional<bool>> newer_saved = std::make_shared<std::optional<bool>>();
		UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, chat.id, [newer_saved](bool saved) { *newer_saved = saved; }));
		while (!newer_saved->has_value() && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(fixture.app);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(newer_saved->has_value() && **newer_saved);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
		UAM_ASSERT(saved && saved->git_base_branch_override == std::optional<std::string>{""});
		UAM_ASSERT(saved->workspace_worktree_directory.empty() && saved->workspace_base_ref.empty());
		UAM_ASSERT_EQ(saved->title, "newer choice title");
		UAM_ASSERT_EQ(saved->messages.back().content, "newer choice body");
		UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), "head survivor\n");
		UAM_ASSERT_EQ(uam::ReadWorkspaceVcsContext(fixture.repository, true, false).current_commit, fixture.head);
		UAM_ASSERT_EQ(ReadFile(foreign / "keep.txt"), "foreign survivor\n");
		UAM_ASSERT(RunGitForTest(fixture.repository, "show-ref --verify refs/heads/foreign-survivor"));
		UAM_ASSERT(fixture.app.worktree_retention_gate.TryAcquireCleanup());
	}
}
