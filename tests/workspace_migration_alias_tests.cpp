#include "test_harness.h"
#include "core/chat_import_utils.h"

using namespace uam_test;

UAM_TEST(RegisteredWorkspaceNestedOwnershipStopsAtDistinctRepository)
{
	TempDir temp("uam-nested-folder-ownership");
	const fs::path base = temp.root / "base";
	fs::create_directories(base / "ordinary" / "deep");
	fs::create_directories(base / "nested-repository" / ".git");
	fs::create_directories(base / "nested-repository" / "child");
	fs::create_directories(base / "svn-repository" / ".svn");
	fs::create_directories(base / "svn-repository" / "child");
	const std::vector<ChatFolder> folders = {{"base", "Base", base.string(), false, "local"},
	    {"ordinary", "Ordinary", (base / "ordinary").string(), false, "local"},
	    {"remote", "Remote", (base / "nested-repository").string(), false, "ssh-host"}};
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(folders, base / "ordinary" / "deep") != nullptr);
	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(folders, base / "ordinary" / "deep")->id, std::string("ordinary"));
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(folders, base / "nested-repository" / "child") == nullptr);
	UAM_ASSERT(uam::ImportedWorkspaceMatchesFolder(base / "ordinary" / "deep", base));
	UAM_ASSERT(!uam::ImportedWorkspaceMatchesFolder(base / "nested-repository" / "child", base));
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(folders, base / "svn-repository" / "child") == nullptr);
	UAM_ASSERT(!uam::ImportedWorkspaceMatchesFolder(base / "svn-repository" / "child", base));
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(folders, temp.root / "base-other") == nullptr);
}

UAM_TEST(WorkspaceMigrationRetainsCanonicalWorktreeAndNestedOwners)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-migrate-worktree-owner");
	const fs::path main = temp.root / "main";
	const fs::path worktree = temp.root / "worktree";
	fs::create_directories(main);
	UAM_ASSERT(RunGitForTest(main, "init -q"));
	UAM_ASSERT(RunGitForTest(main, "-c user.name=Fixture -c user.email=fixture@example.invalid commit --allow-empty -qm fixture"));
	UAM_ASSERT(RunGitForTest(main, "worktree add -qb fixture-worktree " + ShellQuoteForTest(worktree.string())));
	fs::create_directories(main / "child");
	fs::create_directories(worktree / "child");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"main-owner", "Main", main.string(), false, "local"});
	for (const fs::path& cwd : {main / "child", worktree / "child"})
	{
		ChatSession chat;
		chat.id = cwd == main / "child" ? "main-chat" : "worktree-chat";
		chat.folder_id = "main-owner";
		chat.workspace_directory = cwd.string();
		chat.messages.push_back(Message{MessageRole::User, "preserve authored history"});
		chat.messages_loaded = true;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(chat);
	}
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(app.folders, worktree / "child") != nullptr);
	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(app.folders, worktree / "child")->id, std::string("main-owner"));
	UAM_ASSERT(uam::MigrateWorkspaceFolderOwnership(app));
	UAM_ASSERT(uam::MigrateWorkspaceFolderOwnership(app));
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	for (const ChatSession& chat : ChatRepository::LoadLocalChats(app.data_root))
	{
		UAM_ASSERT_EQ(chat.folder_id, std::string("main-owner"));
		UAM_ASSERT_EQ(chat.messages.front().content, std::string("preserve authored history"));
	}
	UAM_ASSERT_EQ(app.chats[1].workspace_directory, (worktree / "child").string());
	app.folders.push_back({"explicit-worktree", "Worktree", worktree.string(), false, "local"});
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(app.folders, worktree / "child") != nullptr);
	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(app.folders, worktree / "child")->id, std::string("main-owner"));
	const std::vector<ChatFolder> worktree_only = {app.folders.back()};
	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(worktree_only, worktree / "child")->id, std::string("explicit-worktree"));
}
