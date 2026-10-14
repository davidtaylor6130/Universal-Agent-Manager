#include "test_harness.h"
#include "app/chat_lifecycle_service.h"

using namespace uam_test;

UAM_TEST(DeletionNativeCleanupFailurePreservesRecoveryEvidence)
{
	TempDir temp("uam-deletion-native-failure");
	ScopedEnvVar provider_home("GEMINI_CLI_HOME", (temp.root / "provider-home").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "victim";
	chat.branch_root_chat_id = chat.id;
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.native_session_id = "native-victim";
	chat.workspace_directory = (temp.root / "workspace").string();
	fs::create_directories(chat.workspace_directory);
	app.chats = {chat};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const fs::path provider_root = temp.root / "provider-home";
	fs::create_directories(provider_root / "tmp" / "private-project");
	UAM_ASSERT(uam::io::WriteTextFile(provider_root / "projects.json",
	    nlohmann::json{{"projects", {{chat.workspace_directory, "private-project"}}}}.dump()));
	const fs::path native_directory = ChatHistorySyncService().ResolveNativeHistoryChatsDirForChat(app, chat);
	UAM_ASSERT(!native_directory.empty());
	const fs::path collision = native_directory / "native-victim.json";
	fs::create_directories(collision);
	UAM_ASSERT(uam::io::WriteTextFile(collision / "keep.txt", "private failure fixture"));
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT(task.metadata_saved && !task.commit_rejected);
	UAM_ASSERT(!uam::CleanupWorkspaceDeletion(task));
	UAM_ASSERT(fs::exists(app.data_root / "deletion-transaction.json"));
	UAM_ASSERT(fs::exists(app.data_root / ".deletion-transaction"));
	UAM_ASSERT_EQ(ReadFile(collision / "keep.txt"), "private failure fixture");
}

UAM_TEST(DeletionProgressCountsOwnedChatsAndWorkspacesAfterCleanup)
{
	TempDir temp("uam-deletion-progress");
	ScopedEnvVar provider_home("GEMINI_CLI_HOME", (temp.root / "provider-home").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const std::string workspace = (temp.root / "workspace").string();
	fs::create_directories(workspace);
	app.folders = {{"delete", "Delete", workspace, false}, {"keep", "Keep", workspace, false}};
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	for (const std::string id : {"first", "second", "survivor"})
	{
		ChatSession chat;
		chat.id = id;
		chat.title = id;
		chat.branch_root_chat_id = id;
		chat.folder_id = id == "survivor" ? "keep" : "delete";
		chat.provider_id = uam::provider_ids::kCodexCli;
		chat.workspace_directory = workspace;
		app.chats.push_back(chat);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	}
	const fs::path survivor_path = AppPaths::UamChatFilePath(app.data_root, "survivor");
	const std::string survivor_bytes = ReadFile(survivor_path);
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete", "delete"}, task));
	std::size_t last_completed = 0;
	task.progress = [&](std::size_t completed, std::size_t total, const std::string& label)
	{
		UAM_ASSERT_EQ(total, std::size_t(3));
		UAM_ASSERT_EQ(completed, last_completed + 1);
		UAM_ASSERT(!label.empty());
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "first")));
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "second")));
		UAM_ASSERT_EQ(ReadFile(survivor_path), survivor_bytes);
		last_completed = completed;
	};
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	UAM_ASSERT_EQ(last_completed, std::size_t(0));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT_EQ(last_completed, std::size_t(0));
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
	UAM_ASSERT_EQ(last_completed, std::size_t(3));
	UAM_ASSERT(fs::exists(workspace));
	UAM_ASSERT(!fs::exists(app.data_root / "deletion-transaction.json"));
}

UAM_TEST(DeletionProgressDoesNotCountFailedStorageCleanup)
{
	TempDir temp("uam-deletion-progress-storage-failure");
	ScopedEnvVar provider_home("GEMINI_CLI_HOME", (temp.root / "provider-home").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "victim";
	chat.branch_root_chat_id = chat.id;
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.workspace_directory = (temp.root / "workspace").string();
	fs::create_directories(chat.workspace_directory);
	app.chats = {chat};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT(task.metadata_saved);
	const fs::path collision = AppPaths::UamChatSummaryFilePath(app.data_root, chat.id);
	fs::remove(collision);
	fs::create_directories(collision);
	UAM_ASSERT(uam::io::WriteTextFile(collision / "keep.txt", "private storage failure"));
	std::size_t updates = 0;
	task.progress = [&](std::size_t, std::size_t, const std::string&) { ++updates; };
	UAM_ASSERT(!uam::CleanupWorkspaceDeletion(task));
	UAM_ASSERT_EQ(updates, std::size_t(0));
	UAM_ASSERT(fs::exists(app.data_root / "deletion-transaction.json"));
	UAM_ASSERT_EQ(ReadFile(collision / "keep.txt"), "private storage failure");
}
