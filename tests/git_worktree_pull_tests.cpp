#include "test_harness.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/config/settings_frontend_json.h"

using namespace uam_test;


namespace
{
	struct PullFixture
	{
		TempDir temporary{"uam-pull-refusals"};
		uam::AppState app;
		fs::path upstream, source, updater;
		PullFixture()
		{
			upstream = temporary.root / "upstream.git";
			source = temporary.root / "source";
			updater = temporary.root / "updater";
			UAM_ASSERT(RunTestCommand("git init --bare " + ShellQuoteForTest(upstream.string())));
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(source.string())));
			UAM_ASSERT(RunGitForTest(source, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(source, "config user.name UAM"));
			UAM_ASSERT(RunGitForTest(source, "commit --allow-empty -m base"));
			UAM_ASSERT(RunGitForTest(source, "remote add origin " + ShellQuoteForTest(upstream.string())));
			UAM_ASSERT(RunGitForTest(source, "push -u origin HEAD"));
			UAM_ASSERT(RunTestCommand("git clone " + ShellQuoteForTest(upstream.string()) + " " + ShellQuoteForTest(updater.string())));
			UAM_ASSERT(RunGitForTest(updater, "config user.email uam@example.test"));
			UAM_ASSERT(RunGitForTest(updater, "config user.name UAM"));
			UAM_ASSERT(uam::io::WriteTextFile(updater / "upstream.txt", "new upstream content"));
			UAM_ASSERT(RunGitForTest(updater, "add upstream.txt"));
			UAM_ASSERT(RunGitForTest(updater, "commit -m upstream"));
			UAM_ASSERT(RunGitForTest(updater, "push"));
			app.data_root = temporary.root / "data";
			const fs::path settings = temporary.root / "settings.txt";
			UAM_ASSERT(uam::io::WriteTextFile(settings, "git_enabled=true\ngit_pull_before_chat=true\n"));
			UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
			ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
			chat.workspace_directory = source.string();
			app.chats.push_back(std::move(chat));
		}
		std::shared_ptr<uam::WorktreeLaunchPreparationTask> StartAndWait()
		{
			UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
			const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = app.worktree_launch_preparations.at(app.chats.front().id);
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			return task;
		}
	};
}

UAM_TEST(GitWorktreePullBeforeChatUpdatesCheckedOutBase)
{
	if (!GitAvailableForTests()) return;
	PullFixture fixture;
	uam::AppState& app = fixture.app;
	const fs::path source = fixture.source;
	UAM_ASSERT(uam::io::WriteTextFile(source / "untracked.txt", "survive"));
	const fs::path hook = source / ".git" / "hooks" / "post-merge";
	UAM_ASSERT(uam::io::WriteTextFile(hook, "#!/bin/sh\necho hook > hook-ran\n"));
#if !defined(_WIN32)
	fs::permissions(hook, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
#endif
	UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = app.worktree_launch_preparations.at(app.chats.front().id);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
		UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
	(void)uam::PollWorktreeLaunchPreparations(app);
	UAM_ASSERT(task->merged);
	UAM_ASSERT(fs::exists(uam::paths::PathFromUtf8(app.chats.front().workspace_worktree_directory) / "upstream.txt"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "upstream.txt"), "new upstream content");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(source / "untracked.txt"), "survive");
	UAM_ASSERT(!fs::exists(source / "hook-ran"));
}


UAM_TEST(GitWorktreePullPolicyRoundTripDefaultsOff)
{
	TempDir temporary{"uam-pull-settings"};
	AppSettings settings;
	UAM_ASSERT(!settings.git_pull_before_chat);
	settings.git_pull_before_chat = true;
	UAM_ASSERT(SettingsStore::Save(temporary.root / "settings.txt", settings));
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(temporary.root / "settings.txt", restored).loaded);
	UAM_ASSERT(restored.git_pull_before_chat);
	UAM_ASSERT(uam::settings_frontend_json::SerializeCommonSettingsFields(restored).at("gitPullBeforeChat").get<bool>());
	UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "legacy.txt", "git_enabled=true\n"));
	AppSettings legacy;
	UAM_ASSERT(SettingsStore::Load(temporary.root / "legacy.txt", legacy).loaded);
	UAM_ASSERT(!legacy.git_pull_before_chat);
}

UAM_TEST(GitWorktreePullRefusesUnsafeSourceBeforeArtifacts)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view mode : {"dirty", "detached", "different", "upstream", "ongoing", "diverged"})
	{
		PullFixture fixture;
		if (mode == "dirty")
		{
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "dirty.txt", "dirty"));
			UAM_ASSERT(RunGitForTest(fixture.source, "add dirty.txt"));
		}
		if (mode == "detached") UAM_ASSERT(RunGitForTest(fixture.source, "checkout --detach"));
		if (mode == "different")
		{
			UAM_ASSERT(RunGitForTest(fixture.source, "branch different"));
			fixture.app.settings.git_default_base_branch = "different";
		}
		if (mode == "upstream") UAM_ASSERT(RunGitForTest(fixture.source, "branch --unset-upstream"));
		if (mode == "ongoing") UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".git" / "MERGE_HEAD", "invalid"));
		if (mode == "diverged")
		{
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "local.txt", "local"));
			UAM_ASSERT(RunGitForTest(fixture.source, "add local.txt"));
			UAM_ASSERT(RunGitForTest(fixture.source, "commit -m local"));
		}
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->prepared && !task->prepared->operation.ok && !task->merged);
		UAM_ASSERT(!fs::exists(fixture.app.data_root / "worktrees"));
		UAM_ASSERT(!fs::exists(fixture.source / "upstream.txt"));
	}
}

UAM_TEST(GitWorktreePullCancellationKeepsAcceptedFastForward)
{
	if (!GitAvailableForTests()) return;
	for (bool policy_change : {false, true})
	{
		PullFixture fixture;
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
		if (policy_change) fixture.app.settings.git_pull_before_chat = false;
		else task->cancellation->request_stop();
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(fs::exists(fixture.source / "upstream.txt"));
		UAM_ASSERT(!task->merged);
		UAM_ASSERT(fixture.app.chats.front().workspace_worktree_directory.empty());
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(GitWorktreePullDisabledAndManualCreationStayOffline)
{
	if (!GitAvailableForTests()) return;
	for (bool manual : {false, true})
	{
		PullFixture fixture;
		if (manual)
		{
			UAM_ASSERT(uam::GitWorktreeService().CreateForChat(fixture.app, fixture.app.chats.front()).ok);
		}
		else
		{
			fixture.app.settings.git_pull_before_chat = false;
			const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			UAM_ASSERT(task->merged);
		}
		UAM_ASSERT(!fs::exists(fixture.source / "upstream.txt"));
		UAM_ASSERT(!fs::exists(uam::paths::PathFromUtf8(fixture.app.chats.front().workspace_worktree_directory) / "upstream.txt"));
	}
}

UAM_TEST(GitWorktreePullSkipsManagedBaseline)
{
	if (!GitAvailableForTests()) return;
	PullFixture fixture;
	const fs::path folder = fixture.temporary.root / "ordinary-folder";
	fs::create_directories(folder);
	UAM_ASSERT(uam::io::WriteTextFile(folder / "keep.txt", "original"));
	fixture.app.chats.front().workspace_directory = folder.string();
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	UAM_ASSERT(task->prepared->operation.status.managed_repository);
	UAM_ASSERT_EQ(ReadFile(folder / "keep.txt"), std::string("original"));
	UAM_ASSERT(!fs::exists(folder / ".git"));
	UAM_ASSERT_EQ(ReadFile(fs::path(fixture.app.chats.front().workspace_worktree_directory) / "keep.txt"), std::string("original"));
	UAM_ASSERT(!fs::exists(fixture.source / "upstream.txt"));
}

UAM_TEST(GitWorktreePullSkipsSelectedManagedRepository)
{
	if (!GitAvailableForTests()) return;
	PullFixture fixture;
	const fs::path folder = fixture.temporary.root / "ordinary-folder";
	fs::create_directories(folder);
	UAM_ASSERT(uam::io::WriteTextFile(folder / "keep.txt", "original"));
	fixture.app.chats.front().workspace_directory = folder.string();
	UAM_ASSERT(uam::GitWorktreeService().CreateForChat(fixture.app, fixture.app.chats.front()).ok);
	const std::string selected = fixture.app.chats.front().workspace_worktree_directory;
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.workspace_directory = selected;
	fixture.app.chats.front() = std::move(chat);
	const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
	(void)uam::PollWorktreeLaunchPreparations(fixture.app);
	UAM_ASSERT(task->merged);
	UAM_ASSERT_EQ(ReadFile(fs::path(fixture.app.chats.front().workspace_worktree_directory) / "keep.txt"), std::string("original"));
	UAM_ASSERT_EQ(ReadFile(fs::path(selected) / "keep.txt"), std::string("original"));
	UAM_ASSERT_EQ(ReadFile(folder / "keep.txt"), std::string("original"));
	UAM_ASSERT(!fs::exists(folder / ".git"));
}
