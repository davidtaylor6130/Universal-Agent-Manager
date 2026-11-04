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


namespace
{
	void AssertAutomaticWorktreeContinuesWithoutUpstream(bool remove_remote)
	{
		PullFixture fixture;
		UAM_ASSERT(fixture.app.settings.git_enabled && fixture.app.settings.git_auto_worktree && fixture.app.settings.git_pull_before_chat);
		UAM_ASSERT(RunGitForTest(fixture.source, "branch --unset-upstream"));
		if (remove_remote) UAM_ASSERT(RunGitForTest(fixture.source, "remote remove origin"));
		const fs::path tracked = fixture.source / "local.txt";
		const fs::path untracked = fixture.source / "untracked.txt";
		UAM_ASSERT(uam::io::WriteTextFile(tracked, "local committed content\n"));
		UAM_ASSERT(RunGitForTest(fixture.source, "add local.txt"));
		UAM_ASSERT(RunGitForTest(fixture.source, "commit -m local-only"));
		UAM_ASSERT(uam::io::WriteTextFile(untracked, "local untracked content\n"));
		const auto git_output = [&](const std::string& arguments)
		{
			const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    "git -C " + ShellQuoteForTest(fixture.source.string()) + " " + arguments, 10000);
			UAM_ASSERT(result.ok && result.exit_code == 0 && !result.timed_out && !result.canceled && !result.output_truncated);
			return uam::strings::Trim(result.output);
		};
		UAM_ASSERT_EQ(git_output("remote"), remove_remote ? std::string{} : std::string("origin"));
		const std::string head_before = git_output("rev-parse HEAD");
		const std::string symbolic_head_before = ReadFile(fixture.source / ".git" / "HEAD");
		const std::string index_before = ReadFile(fixture.source / ".git" / "index");
		const std::string tracked_before = ReadFile(tracked), untracked_before = ReadFile(untracked);
		const fs::path fetch_head = fixture.source / ".git" / "FETCH_HEAD";
		const bool fetch_head_existed = fs::exists(fetch_head);
		const std::string fetch_head_before = fetch_head_existed ? ReadFile(fetch_head) : std::string{};
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = fixture.StartAndWait();
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->prepared && task->prepared->operation.ok && task->merged);
		const ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
		UAM_ASSERT(!task->prepared->operation.status.managed_repository);
		UAM_ASSERT_EQ(chat.workspace_base_ref, head_before);
		const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
		UAM_ASSERT(fs::is_directory(worktree) && worktree != fixture.source);
		UAM_ASSERT_EQ(ReadFile(worktree / "local.txt"), tracked_before);
		UAM_ASSERT(!fs::exists(worktree / "upstream.txt"));
		UAM_ASSERT_EQ(git_output("rev-parse HEAD"), head_before);
		UAM_ASSERT_EQ(ReadFile(fixture.source / ".git" / "HEAD"), symbolic_head_before);
		UAM_ASSERT_EQ(ReadFile(fixture.source / ".git" / "index"), index_before);
		UAM_ASSERT_EQ(ReadFile(tracked), tracked_before);
		UAM_ASSERT_EQ(ReadFile(untracked), untracked_before);
		UAM_ASSERT(!fs::exists(fixture.source / "upstream.txt"));
		UAM_ASSERT_EQ(fs::exists(fetch_head), fetch_head_existed);
		if (fetch_head_existed) UAM_ASSERT_EQ(ReadFile(fetch_head), fetch_head_before);
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
	}
}

UAM_TEST(GitWorktreePullWithoutTrackingContinuesOnLocalBranch)
{
	if (!GitAvailableForTests()) return;
	AssertAutomaticWorktreeContinuesWithoutUpstream(false);
}

UAM_TEST(GitWorktreePullWithoutRemotesContinuesOnLocalBranch)
{
	if (!GitAvailableForTests()) return;
	AssertAutomaticWorktreeContinuesWithoutUpstream(true);
}


UAM_TEST(GitWorktreePullRefusesUnsafeSourceBeforeArtifacts)
{
	if (!GitAvailableForTests()) return;
	for (const std::string_view mode : {"detached", "different", "diverged"})
	{
		PullFixture fixture;
		if (mode == "detached") UAM_ASSERT(RunGitForTest(fixture.source, "checkout --detach"));
		if (mode == "different")
		{
			UAM_ASSERT(RunGitForTest(fixture.source, "branch different"));
			fixture.app.settings.git_default_base_branch = "different";
		}
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

UAM_TEST(GitWorktreeAutomaticPullExplainsUnbornHeadWithoutChangingRepository)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const bool with_files : {false, true})
	{
		TempDir temporary("uam-unborn-automatic-pull");
		const fs::path repository = temporary.root / "repository";
		fs::create_directories(repository);
		UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(repository))));
		UAM_ASSERT(RunGitForTest(repository, "symbolic-ref HEAD refs/heads/new-work"));
		if (with_files)
		{
			UAM_ASSERT(uam::io::WriteTextFile(repository / "staged.txt", "staged\n"));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "untracked.txt", "untracked\n"));
			UAM_ASSERT(RunGitForTest(repository, "add staged.txt"));
		}
		const fs::path git = repository / ".git";
		const std::string original_head = ReadFile(git / "HEAD");
		const std::string original_config = ReadFile(git / "config");
		const bool index_existed = fs::exists(git / "index");
		const std::string original_index = index_existed ? ReadFile(git / "index") : std::string{};
		UAM_ASSERT(!fs::exists(git / "refs" / "heads" / "new-work"));
		UAM_ASSERT(!fs::exists(git / "FETCH_HEAD"));
		uam::AppState app;
		app.data_root = temporary.root / "data";
		app.settings.git_enabled = true;
		app.settings.git_auto_worktree = true;
		app.settings.git_pull_before_chat = true;
		app.settings.git_snapshot_plain_folders = true;
		ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli", true);
		chat.workspace_directory = uam::paths::Utf8PathString(repository);
		app.chats.push_back(std::move(chat));
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
		const std::shared_ptr<uam::WorktreeLaunchPreparationTask> task = app.worktree_launch_preparations.at(app.chats.front().id);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (task->worker.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
		(void)uam::PollWorktreeLaunchPreparations(app);
		UAM_ASSERT(task->prepared && task->prepared->operation.ok && task->merged);
		UAM_ASSERT(task->prepared->operation.status.managed_repository);
		// Success requires an owned isolated baseline, never an ordinary source launch.
		UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(app.chats.front()));
		const fs::path owned = uam::paths::PathFromUtf8(app.chats.front().workspace_worktree_directory);
		UAM_ASSERT(fs::is_directory(owned) && owned != repository);
		UAM_ASSERT(uam::paths::IsSameOrInsideRoot(app.data_root / "worktrees", owned));
		UAM_ASSERT(!app.chats.front().workspace_branch_name.empty() && !app.chats.front().workspace_base_ref.empty());
		std::string validation_error;
		UAM_ASSERT(uam::GitWorktreeService().ValidatePreparedWorkspace(app.data_root, task->receipt_file, app.chats.front(), validation_error));
		UAM_ASSERT(validation_error.empty());
		if (with_files)
		{
			UAM_ASSERT_EQ(ReadFile(owned / "staged.txt"), std::string("staged\n"));
			UAM_ASSERT_EQ(ReadFile(owned / "untracked.txt"), std::string("untracked\n"));
		}
		while (!task->saved.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			(void)uam::PollWorktreeLaunchPreparations(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(task->saved.has_value() && *task->saved);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(app, app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT_EQ(ReadFile(git / "HEAD"), original_head);
		UAM_ASSERT_EQ(ReadFile(git / "config"), original_config);
		UAM_ASSERT_EQ(fs::exists(git / "index"), index_existed);
		if (index_existed) UAM_ASSERT_EQ(ReadFile(git / "index"), original_index);
		UAM_ASSERT(!fs::exists(git / "refs" / "heads" / "new-work"));
		UAM_ASSERT(!fs::exists(git / "worktrees"));
		UAM_ASSERT(!fs::exists(git / "FETCH_HEAD"));
		if (with_files)
		{
			UAM_ASSERT_EQ(ReadFile(repository / "staged.txt"), std::string("staged\n"));
			UAM_ASSERT_EQ(ReadFile(repository / "untracked.txt"), std::string("untracked\n"));
		}
		else
		{
			UAM_ASSERT(!fs::exists(repository / "staged.txt"));
			UAM_ASSERT(!fs::exists(repository / "untracked.txt"));
		}
		UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty());
	}
}


#include <sstream>

namespace
{
	std::string PullWarningGitOutput(const fs::path& repository, const std::string& arguments)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(repository.string()) + " " + arguments, 10000);
		UAM_ASSERT(result.ok && result.exit_code == 0 && !result.timed_out && !result.canceled && !result.output_truncated);
		return uam::strings::Trim(result.output);
	}

	struct PullWarningSourceWitness
	{
		struct File { fs::path path; bool existed; std::string bytes; };
		std::vector<File> files;
		std::vector<std::string> refs;
		std::string head;
		explicit PullWarningSourceWitness(const fs::path& source)
		{
			head = PullWarningGitOutput(source, "rev-parse --verify HEAD");
			std::istringstream lines(PullWarningGitOutput(source, "for-each-ref " + ShellQuoteForTest("--format=%(refname) %(objectname)")));
			for (std::string line; std::getline(lines, line);) refs.push_back(line);
			for (const std::string_view relative : {".git/HEAD", ".git/config", ".git/index", ".git/FETCH_HEAD", ".git/ORIG_HEAD", ".git/MERGE_HEAD", ".git/MERGE_MSG", ".git/MERGE_MODE", "upstream.txt", "untracked-survivor.txt"})
			{
				const fs::path path = source / relative;
				const bool exists = fs::exists(path);
				files.push_back({path, exists, exists ? ReadFile(path) : std::string{}});
			}
		}
		void AssertPreservedWithOnlyOwnedBranch(const fs::path& source, const ChatSession& chat) const
		{
			UAM_ASSERT_EQ(PullWarningGitOutput(source, "rev-parse --verify HEAD"), head);
			for (const File& file : files)
			{
				UAM_ASSERT_EQ(fs::exists(file.path), file.existed);
				if (file.existed) UAM_ASSERT_EQ(ReadFile(file.path), file.bytes);
			}
			std::vector<std::string> remaining;
			std::istringstream lines(PullWarningGitOutput(source, "for-each-ref " + ShellQuoteForTest("--format=%(refname) %(objectname)")));
			const std::string owned = "refs/heads/" + chat.workspace_branch_name + " " + head;
			int owned_count = 0;
			for (std::string line; std::getline(lines, line);)
			{
				if (line == owned) ++owned_count;
				else remaining.push_back(line);
			}
			UAM_ASSERT_EQ(owned_count, 1);
			UAM_ASSERT(remaining == refs);
		}
	};

	void AssertPullWarningAfterDurableReady(PullFixture& fixture, const PullWarningSourceWitness& witness)
	{
		fixture.app.status_line = "before-durable-worktree-save";
		const auto task = fixture.StartAndWait();
		UAM_ASSERT_EQ(fixture.app.status_line, std::string("before-durable-worktree-save"));
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT(task->prepared && task->prepared->operation.ok && task->merged);
		UAM_ASSERT(!task->saved.has_value());
		UAM_ASSERT(task->error.empty());
		const std::string warning = task->prepared->operation.status.warning;
		UAM_ASSERT(!warning.empty());
		UAM_ASSERT_EQ(fixture.app.status_line, std::string("before-durable-worktree-save"));
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Pending);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!task->saved.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(fixture.app);
			(void)uam::PollWorktreeLaunchPreparations(fixture.app);
			if (!task->saved.has_value()) UAM_ASSERT_EQ(fixture.app.status_line, std::string("before-durable-worktree-save"));
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(task->saved.has_value() && *task->saved);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		UAM_ASSERT(fixture.app.status_line.find(warning) != std::string::npos);
		const ChatSession& chat = fixture.app.chats.front();
		UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
		UAM_ASSERT_EQ(chat.workspace_base_ref, witness.head);
		UAM_ASSERT(!task->prepared->operation.status.managed_repository);
		const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
		UAM_ASSERT(fs::is_directory(worktree) && worktree != fixture.source);
		UAM_ASSERT_EQ(PullWarningGitOutput(worktree, "rev-parse --verify HEAD"), witness.head);
		UAM_ASSERT(!fs::exists(worktree / "pending-remote.txt"));
		UAM_ASSERT(!fs::exists(fixture.source / "pending-remote.txt"));
		UAM_ASSERT(!fs::exists(worktree / "untracked-survivor.txt"));
		UAM_ASSERT(fixture.app.acp_sessions.empty() && fixture.app.cli_terminals.empty());
		witness.AssertPreservedWithOnlyOwnedBranch(fixture.source, chat);
		fixture.app.status_line = "later-unrelated-status";
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT_EQ(uam::PrepareWorktreeForLaunch(fixture.app, fixture.app.chats.front()).state, uam::WorktreeLaunchPreparationState::Ready);
		(void)uam::PollWorktreeLaunchPreparations(fixture.app);
		UAM_ASSERT_EQ(fixture.app.status_line, std::string("later-unrelated-status"));
		witness.AssertPreservedWithOnlyOwnedBranch(fixture.source, chat);
	}
}

UAM_TEST(GitWorktreeDirtyTrackedAndIndexSkipOptionalPullWithDurableWarning)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const bool staged : {false, true})
	{
		PullFixture fixture;
		// Establish a clean tracked baseline, then leave a distinct incoming commit pending.
		UAM_ASSERT(RunGitForTest(fixture.source, "fetch origin"));
		UAM_ASSERT(RunGitForTest(fixture.source, "merge --ff-only " + ShellQuoteForTest("@{upstream}")));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.updater / "pending-remote.txt", "must not be fetched or copied\n"));
		UAM_ASSERT(RunGitForTest(fixture.updater, "add pending-remote.txt"));
		UAM_ASSERT(RunGitForTest(fixture.updater, "commit -m pending-remote"));
		UAM_ASSERT(RunGitForTest(fixture.updater, "push"));
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "upstream.txt", "source-only dirty tracked bytes\n"));
		if (staged)
		{
			UAM_ASSERT(RunGitForTest(fixture.source, "add upstream.txt"));
			UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "upstream.txt", "source-only staged plus unstaged bytes\n"));
		}
		UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "untracked-survivor.txt", "never copied into committed worktree\n"));
		const PullWarningSourceWitness witness(fixture.source);
		AssertPullWarningAfterDurableReady(fixture, witness);
		UAM_ASSERT_EQ(ReadFile(uam::paths::PathFromUtf8(fixture.app.chats.front().workspace_worktree_directory) / "upstream.txt"), std::string("new upstream content"));
	}
}

UAM_TEST(GitWorktreeInProgressMergeSkipsOptionalPullWithDurableWarning)
{
	UAM_ASSERT(GitAvailableForTests());
	PullFixture fixture;
	// Fetch is fixture setup only: MERGE_HEAD names a real available incoming commit.
	UAM_ASSERT(RunGitForTest(fixture.source, "fetch origin"));
	const std::string incoming = PullWarningGitOutput(fixture.source, "rev-parse --verify " + ShellQuoteForTest("@{upstream}"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".git" / "MERGE_HEAD", incoming + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".git" / "MERGE_MSG", "unfinished merge must remain untouched\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / ".git" / "MERGE_MODE", "no-ff"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "untracked-survivor.txt", "merge survivor\n"));
	const PullWarningSourceWitness witness(fixture.source);
	AssertPullWarningAfterDurableReady(fixture, witness);
	UAM_ASSERT(!fs::exists(uam::paths::PathFromUtf8(fixture.app.chats.front().workspace_worktree_directory) / "upstream.txt"));
}


UAM_TEST(GitWorktreeDirtyWithoutUpstreamPublishesSourceWarningAfterDurableReady)
{
	UAM_ASSERT(GitAvailableForTests());
	PullFixture fixture;
	UAM_ASSERT(RunGitForTest(fixture.source, "fetch origin"));
	UAM_ASSERT(RunGitForTest(fixture.source, "merge --ff-only " + ShellQuoteForTest("@{upstream}")));
	UAM_ASSERT(RunGitForTest(fixture.source, "branch --unset-upstream"));
	UAM_ASSERT_EQ(PullWarningGitOutput(fixture.source, "for-each-ref " + ShellQuoteForTest("--format=%(upstream)") + " " + ShellQuoteForTest("refs/heads/" + PullWarningGitOutput(fixture.source, "symbolic-ref --short HEAD"))), std::string{});
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "upstream.txt", "dirty without tracking remains source-only\n"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source / "untracked-survivor.txt", "untracked without tracking survivor\n"));
	const PullWarningSourceWitness witness(fixture.source);
	AssertPullWarningAfterDurableReady(fixture, witness);
	UAM_ASSERT_EQ(ReadFile(uam::paths::PathFromUtf8(fixture.app.chats.front().workspace_worktree_directory) / "upstream.txt"), std::string("new upstream content"));
}
