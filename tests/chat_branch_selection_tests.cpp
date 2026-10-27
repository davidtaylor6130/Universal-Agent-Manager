#include "test_harness.h"

using namespace uam_test;

namespace
{
	struct ChatBranchFixture
	{
		TempDir temporary{"uam-chat-branch-selection"};
		uam::AppState app;
		fs::path repository = temporary.root / "repo";
		ChatBranchFixture(bool canonical_paths = false)
		{
			if (canonical_paths) { temporary.root = fs::canonical(temporary.root); repository = temporary.root / "repo"; }
			fs::create_directories(repository);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
			UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
			UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
			UAM_ASSERT(uam::io::WriteTextFile(repository / "source.txt", "survivor\n"));
			UAM_ASSERT(RunGitForTest(repository, "add source.txt"));
			UAM_ASSERT(RunGitForTest(repository, "commit -m base"));
			UAM_ASSERT(RunGitForTest(repository, "branch existing"));
			app.data_root = temporary.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.folders.push_back({.id = "folder", .directory = repository.string(), .execution_host_id = "local"});
			ChatSession chat = ChatDomainService().CreateNewChat("folder", "codex-cli");
			chat.workspace_directory = repository.string();
			chat.execution_host_id = "local";
			ChatDomainService().AddMessage(chat, MessageRole::User, "Keep this conversation");
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(uam::GitWorktreeService().CreateForChat(app, app.chats.front()).ok);
			app.chats.front().native_session_id = "01a0ff71-5253-7621-ab10-cd0928b17e5a";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		}
		std::string Git(const fs::path& directory, const std::string& args) const
		{
			const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
			    "git -C " + ShellQuoteForTest(directory.string()) + " " + args, 10000);
			UAM_ASSERT(result.ok && result.exit_code == 0);
			return uam::strings::Trim(result.output);
		}
	};
}

UAM_TEST(ChatBranchSelectionPreservesCwdHistoryAndSource)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::string cwd = chat.workspace_worktree_directory;
	const std::string source_head = fixture.Git(fixture.repository, "rev-parse HEAD");
	const std::string original_branch = chat.workspace_branch_name;
	std::vector<uam::GitChatBranch> branches;
	bool svn = false;
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().ListChatBranches(fixture.app, chat, branches, svn, error));
	UAM_ASSERT(!svn);
	UAM_ASSERT(std::ranges::any_of(branches, [&](const uam::GitChatBranch& item) { return item.name == original_branch && item.current; }));
	UAM_ASSERT(uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, cwd);
	UAM_ASSERT_EQ(chat.native_session_id, "01a0ff71-5253-7621-ab10-cd0928b17e5a");
	UAM_ASSERT_EQ(chat.messages.front().content, "Keep this conversation");
	UAM_ASSERT_EQ(fixture.Git(fixture.repository, "rev-parse HEAD"), source_head);
	UAM_ASSERT_EQ(fixture.Git(cwd, "symbolic-ref --short HEAD"), "existing");
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id, true);
	UAM_ASSERT(saved && saved->workspace_branch_name == "existing" && saved->messages.front().content == "Keep this conversation");
	UAM_ASSERT(uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "created/branch", true, error));
	UAM_ASSERT_EQ(fixture.Git(cwd, "symbolic-ref --short HEAD"), "created/branch");
}

UAM_TEST(ChatBranchListLeadsWithNewestCommit)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	UAM_ASSERT(RunGitForTest(fixture.repository, "checkout -b zzz-newer"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.repository / "source.txt", "newer\n"));
	{
		ScopedEnvVar date("GIT_COMMITTER_DATE", "2090-01-01T00:00:00+0000");
		UAM_ASSERT(RunGitForTest(fixture.repository, "commit -am newer"));
	}
	UAM_ASSERT(RunGitForTest(fixture.repository, "checkout -"));
	std::vector<uam::GitChatBranch> branches;
	bool svn = false;
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().ListChatBranches(fixture.app, fixture.app.chats.front(), branches, svn, error));
	UAM_ASSERT(!branches.empty());
	UAM_ASSERT_EQ(branches.front().name, "zzz-newer");
}

UAM_TEST(ChatBranchSelectionRejectsUnsafeBoundaries)
{
	if (!GitAvailableForTests()) return;
	for (int boundary = 0; boundary < 7; ++boundary)
	{
		ChatBranchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const std::string original = chat.workspace_branch_name;
		const std::string cwd = chat.workspace_worktree_directory;
		std::string error;
		std::string branch = "existing";
		if (boundary == 0) UAM_ASSERT(uam::io::WriteTextFile(fs::path(cwd) / "source.txt", "dirty survivor"));
		if (boundary == 1)
		{
			ChatSession other = chat; other.id = "persisted-other";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, other));
		}
		if (boundary == 2) chat.execution_host_id = "remote";
		if (boundary == 3) chat.imported_read_only = true;
		if (boundary == 4) branch = "--detach";
		if (boundary == 5) branch = "@{-1}";
		if (boundary == 6)
		{
			std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat.id; session->running = true;
			fixture.app.acp_sessions.push_back(std::move(session));
		}
		UAM_ASSERT(!uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, branch, false, error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(fixture.Git(cwd, "symbolic-ref --short HEAD"), original);
	}
}

UAM_TEST(ChatBranchSelectionRejectsForeignAndSymlinkWorktrees)
{
	if (!GitAvailableForTests()) return;
	for (int boundary = 0; boundary < 2; ++boundary)
	{
		ChatBranchFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		const fs::path cwd = chat.workspace_worktree_directory;
		const std::string original = chat.workspace_branch_name;
		if (boundary == 0)
		{
			const fs::path foreign = fixture.temporary.root / "foreign";
			fs::create_directories(foreign);
			UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(foreign.string())));
			chat.workspace_source_directory = foreign.string();
			fixture.app.folders.front().directory = foreign.string();
		}
		else
		{
			const fs::path alias = cwd.parent_path() / "alias";
			std::error_code ec; fs::create_directory_symlink(cwd, alias, ec);
			if (ec) continue;
			chat.workspace_worktree_directory = alias.string();
		}
		std::string error;
		UAM_ASSERT(!uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
		UAM_ASSERT(!uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, chat, &error));
		UAM_ASSERT_EQ(fixture.Git(cwd, "symbolic-ref --short HEAD"), original);
		UAM_ASSERT(fs::exists(cwd / "source.txt"));
	}
}

UAM_TEST(ChatBranchSelectedWorktreeCleanupPreservesSelectedBranch)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
	const std::string cwd = chat.workspace_worktree_directory;
	UAM_ASSERT(uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, chat, &error));
	UAM_ASSERT(!fs::exists(cwd));
	UAM_ASSERT_EQ(fixture.Git(fixture.repository, "rev-parse refs/heads/existing"), fixture.Git(fixture.repository, "rev-parse HEAD"));
}

UAM_TEST(ChatBranchSaveFailureRestoresCheckoutAndPreservesNewBranch)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::string original = chat.workspace_branch_name;
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, chat.id);
	const std::string bytes = ReadFile(primary);
	const fs::path summaries = AppPaths::UamChatSummaryFilePath(fixture.app.data_root, chat.id).parent_path();
	const fs::path saved_summaries = fixture.app.data_root / "saved-summary-survivors";
	fs::rename(summaries, saved_summaries);
	UAM_ASSERT(uam::io::WriteTextFile(summaries, "summary staging blocker survivor"));
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "retained-on-save-failure", true, error));
	UAM_ASSERT_EQ(fixture.Git(chat.workspace_worktree_directory, "symbolic-ref --short HEAD"), original);
	UAM_ASSERT_EQ(chat.workspace_branch_name, original);
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(summaries), "summary staging blocker survivor");
	UAM_ASSERT(!fixture.Git(fixture.repository, "rev-parse refs/heads/retained-on-save-failure").empty());
	UAM_ASSERT(fs::remove(summaries));
	fs::rename(saved_summaries, summaries);
}

UAM_TEST(ChatBranchSelectedSharedCleanupPreservesSurvivors)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	std::string error;
	UAM_ASSERT(uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
	ChatSession other = chat; other.id = "persisted-other";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, other));
	UAM_ASSERT(!uam::GitWorktreeService().RemoveUnusedBranchWorktree(fixture.app, chat, &error));
	UAM_ASSERT(fs::exists(fs::path(chat.workspace_worktree_directory) / "source.txt"));
	UAM_ASSERT_EQ(fixture.Git(chat.workspace_worktree_directory, "symbolic-ref --short HEAD"), "existing");
	UAM_ASSERT(!fixture.Git(fixture.repository, "rev-parse refs/heads/existing").empty());
}

UAM_TEST(ChatBranchSelectedDiscardPreservesExistingBranch)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	std::string error;
	// Build the selected-branch state directly so this regression isolates the cleanup caller.
	UAM_ASSERT(RunGitForTest(chat.workspace_worktree_directory, "switch --no-guess existing"));
	chat.workspace_branch_name = "existing";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::string selected_sha = fixture.Git(fixture.repository, "rev-parse refs/heads/existing");
	UAM_ASSERT(uam::io::WriteTextFile(fs::path(chat.workspace_worktree_directory) / "source.txt", "discard only this modification"));
	UAM_ASSERT(uam::GitWorktreeService().DiscardChatChanges(fixture.app, chat).ok);
	UAM_ASSERT_EQ(fixture.Git(fixture.repository, "rev-parse refs/heads/existing"), selected_sha);
	UAM_ASSERT_EQ(ReadFile(fixture.repository / "source.txt"), "survivor\n");
}

UAM_TEST(ChatBranchCheckoutPreservesIgnoredSurvivor)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture(true);
	ChatSession& chat = fixture.app.chats.front();
	const fs::path worktree = chat.workspace_worktree_directory;
	const std::string original = chat.workspace_branch_name;
	const std::string source_branch = fixture.Git(fixture.repository, "symbolic-ref --short HEAD");
	UAM_ASSERT(uam::io::WriteTextFile(worktree / ".gitignore", "secret.txt\n"));
	UAM_ASSERT(RunGitForTest(worktree, "add .gitignore"));
	UAM_ASSERT(RunGitForTest(worktree, "-c user.name=UAM -c user.email=uam@example.test commit -m ignore"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "secret.txt", "ignored survivor"));
	UAM_ASSERT(RunGitForTest(fixture.repository, "switch --no-guess existing"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.repository / "secret.txt", "tracked branch fixture"));
	UAM_ASSERT(RunGitForTest(fixture.repository, "add secret.txt"));
	UAM_ASSERT(RunGitForTest(fixture.repository, "commit -m tracked"));
	UAM_ASSERT(RunGitForTest(fixture.repository, "switch --no-guess " + ShellQuoteForTest(source_branch)));
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
	UAM_ASSERT_EQ(fixture.Git(worktree, "symbolic-ref --short HEAD"), original);
	UAM_ASSERT_EQ(ReadFile(worktree / "secret.txt"), "ignored survivor");
}


UAM_TEST(ChatBranchSelectionRefusesUnconfirmedOwnedStop)
{
	if (!GitAvailableForTests()) return;
	ChatBranchFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	const std::string original = chat.workspace_branch_name;
	uam::AsyncAcpProcessStopTask task;
	task.chat_id = chat.id;
	task.observe_exit = true;
	task.finished = std::make_shared<std::atomic<bool>>(true);
	task.result = std::make_shared<uam::platform::ObservedProcessStopResult>();
	fixture.app.acp_process_stop_tasks.push_back(std::move(task));
	std::string error;
	UAM_ASSERT(!uam::GitWorktreeService().SwitchChatBranch(fixture.app, chat, "existing", false, error));
	UAM_ASSERT_EQ(fixture.Git(chat.workspace_worktree_directory, "symbolic-ref --short HEAD"), original);
}
