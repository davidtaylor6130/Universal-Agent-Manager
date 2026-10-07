#include "test_harness.h"

using namespace uam_test;

UAM_TEST(TurnCheckpointPreservesUserStagingIntroducedAfterCleanPreflight)
{
	TempDir temp("uam-checkpoint-midturn-staging");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(RunGitForTest(repo, "config core.autocrlf false"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "user.txt", "before\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "agent.txt", "before\n"));
	UAM_ASSERT(RunGitForTest(repo, "add -A"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = repo.string();
	app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.id = "midturn-staging";
	chat.workspace_directory = repo.string();
	chat.messages.push_back(Message{MessageRole::User, "Change agent.txt"});
	chat.messages.push_back(Message{MessageRole::Assistant, "Done"});
	app.chats.push_back(std::move(chat));
	uam::GitWorktreeService service;
	UAM_ASSERT(service.CreateForChat(app, app.chats.front()).ok);
	ChatSession& isolated = app.chats.front();
	const fs::path worktree = isolated.workspace_worktree_directory;
	UAM_ASSERT(service.CanCheckpointTurn(app, isolated));
	const auto git_output = [&worktree](const std::string& args)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(worktree.string()) + " " + args, 120000);
		UAM_ASSERT(result.ok);
		return result.output;
	};
	const std::string original_head = git_output("rev-parse HEAD");
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "user.txt", "User staging during provider turn\n"));
	UAM_ASSERT(RunGitForTest(worktree, "add user.txt"));
	const std::string original_staging = git_output("diff --cached --binary");
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "agent.txt", "Assistant edit\n"));
	const uam::GitTurnCheckpointResult checkpoint = service.CreateTurnCheckpoint(app, isolated, 1);
	UAM_ASSERT(!checkpoint.changed);
	UAM_ASSERT(checkpoint.message.find("staged") != std::string::npos);
	UAM_ASSERT_EQ(git_output("rev-parse HEAD"), original_head);
	UAM_ASSERT_EQ(git_output("diff --cached --binary"), original_staging);
	UAM_ASSERT_EQ(ReadFile(worktree / "user.txt"), std::string("User staging during provider turn\n"));
	UAM_ASSERT_EQ(ReadFile(worktree / "agent.txt"), std::string("Assistant edit\n"));
	UAM_ASSERT(isolated.messages[1].checkpoint_sha.empty());
	UAM_ASSERT_EQ(ReadFile(repo / "user.txt"), std::string("before\n"));
}
