#include "test_harness.h"
#include "core/chat_import_utils.h"
#include <memory>

using namespace uam_test;

namespace
{
	ChatSession ImportFixture(const fs::path& workspace, const std::string& id)
	{
		ChatSession chat;
		chat.id = id;
		chat.native_session_id = id;
		chat.provider_id = "codex-cli";
		chat.workspace_directory = uam::paths::Utf8PathString(workspace);
		chat.messages.push_back(Message{MessageRole::User, "fixture history"});
		chat.messages_loaded = true;
		return chat;
	}

	ChatHistorySyncService::ImportResult DrainFixture(uam::AppState& app, ChatHistorySyncService::LocalHistoryDiscovery& discovery,
	    ChatHistorySyncService::ImportPolicy policy)
	{
		ChatHistorySyncService::ImportResult total;
		while (discovery.Pending())
		{
			if (discovery.preparation.valid()) discovery.preparation.wait();
			if (discovery.publication_sync.valid()) discovery.publication_sync.wait();
			total.Merge(ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, policy));
		}
		return total;
	}
}

UAM_TEST(ImportedLinkedWorktreeUsesMainFolderAndKeepsNativeWorkspace)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-import-worktree-owner");
	const fs::path main = temp.root / "main";
	const fs::path worktree = temp.root / "linked";
	fs::create_directories(main);
	UAM_ASSERT(RunGitForTest(main, "init -q"));
	UAM_ASSERT(RunGitForTest(main, "-c user.name=Fixture -c user.email=fixture@example.invalid commit --allow-empty -qm fixture"));
	UAM_ASSERT(RunGitForTest(main, "worktree add --detach " + ShellQuoteForTest(worktree.string())));
	fs::create_directories(main / "child");
	fs::create_directories(worktree / "child");

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"main-owner", "Main", main.string(), false, "local"});
	app.folders.push_back({"worktree-owner", "Linked", worktree.string(), false, "local"});
	const fs::path ordinary = temp.root / "ordinary";
	fs::create_directories(ordinary / "nested");
	app.folders.push_back({"ordinary-owner", "Ordinary", ordinary.string(), false, "local"});
	app.folders.push_back({"nested-owner", "Nested", (ordinary / "nested").string(), false, "local"});
	app.folders.push_back({"remote-owner", "Remote", worktree.string(), false, "ssh-host"});

	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(app.folders, worktree / "child")->id, std::string("main-owner"));
	UAM_ASSERT(uam::FindImportedWorkspaceFolder({app.folders.back()}, worktree) == nullptr);
	UAM_ASSERT_EQ(uam::FindImportedWorkspaceFolder(app.folders, ordinary / "nested")->id, std::string("nested-owner"));

	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(worktree / "child", "native-worktree-session"));
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("main-owner"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("native-worktree-session"));
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, (worktree / "child").string());
}

UAM_TEST(ImportedBranchOnlyWorktreeChildUsesMainFolderAndKeepsNativeWorkspace)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-import-branch-only-worktree-child");
	const fs::path main = temp.root / "main";
	const fs::path worktree = temp.root / "linked";
	const fs::path child = worktree / "branch-only";
	fs::create_directories(main);
	UAM_ASSERT(RunGitForTest(main, "init -q"));
	UAM_ASSERT(RunGitForTest(main, "-c user.name=Fixture -c user.email=fixture@example.invalid commit --allow-empty -qm fixture"));
	UAM_ASSERT(RunGitForTest(main, "worktree add --detach " + ShellQuoteForTest(worktree.string())));
	fs::create_directories(child);
	UAM_ASSERT(!fs::exists(main / "branch-only"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"main-owner", "Main", main.string(), false, "local"});
	app.folders.push_back({"linked-owner", "Linked", worktree.string(), false, "local"});
	const ChatFolder* owner = uam::FindImportedWorkspaceFolder(app.folders, child);
	UAM_ASSERT(owner != nullptr);
	UAM_ASSERT_EQ(owner->id, std::string("main-owner"));
	UAM_ASSERT(uam::ImportedWorkspaceMatchesFolder(child, main));

	const fs::path nested = child / "nested-repository";
	fs::create_directories(nested / ".git");
	fs::create_directories(nested / "deeper");
	UAM_ASSERT(uam::FindImportedWorkspaceFolder(app.folders, nested / "deeper") == nullptr);
	UAM_ASSERT(!uam::ImportedWorkspaceMatchesFolder(nested / "deeper", main));
	app.folders.push_back({"nested-owner", "Nested", nested.string(), false, "local"});
	owner = uam::FindImportedWorkspaceFolder(app.folders, nested / "deeper");
	UAM_ASSERT(owner != nullptr);
	UAM_ASSERT_EQ(owner->id, std::string("nested-owner"));

	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(child, "native-branch-only-session"));
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("main-owner"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("native-branch-only-session"));
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, child.string());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{3});
}

UAM_TEST(AutomaticHistoryImportOnlyPublishesRegisteredWorkspaceOwners)
{
	TempDir temp("uam-import-registered-only");
	const fs::path known = temp.root / "known";
	const fs::path unknown = temp.root / "unknown";
	fs::create_directories(known / "child");
	fs::create_directories(unknown);
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"known", "Known", known.string(), false, "local"});
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(known / "child", "known-chat"));
	discovery.chats.push_back(ImportFixture(unknown, "unknown-chat"));
	ChatSession remote = ImportFixture(known, "remote-chat");
	remote.execution_host_id = "ssh-host";
	discovery.chats.push_back(remote);
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("known"));
	UAM_ASSERT_EQ(app.chats.front().workspace_directory, (known / "child").string());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "unknown-chat")));
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).size(), std::size_t{1});
}

UAM_TEST(AutomaticHistoryImportDoesNotRestoreDeletedWorkspaceForUnseenSessions)
{
	TempDir temp("uam-import-deleted-workspace");
	ScopedEnvVar gemini_home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	fs::create_directories(temp.root / "workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"deleted", "Deleted", (temp.root / "workspace").string(), false, "local"});
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	UAM_ASSERT(DeleteFolderById(app, "deleted"));
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(temp.root / "workspace", "previously-unseen"));
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(ChatFolderStore::Load(app.data_root).empty());
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(AutomaticHistoryImportRechecksWorkspaceAfterPreparation)
{
	TempDir temp("uam-import-owner-recheck");
	fs::create_directories(temp.root / "workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"owner", "Owner", (temp.root / "workspace").string(), false, "local"});
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(temp.root / "workspace", "pending-chat"));
	UAM_ASSERT_EQ(ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly).imported_count, 0);
	UAM_ASSERT(discovery.preparation.valid());
	discovery.preparation.wait();
	app.folders.clear();
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "pending-chat")));
}

UAM_TEST(ExplicitHistoryImportAllocatesDistinctFolderIdsAfterDeletion)
{
	TempDir temp("uam-import-folder-identities");
	fs::create_directories(temp.root / "left" / "proj");
	fs::create_directories(temp.root / "right" / "proj");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"folder_1_proj", "Proj", (temp.root / "left" / "proj").string(), false, "local"});
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(temp.root / "right" / "proj", "explicit-chat"));
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::ExplicitImport);
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{2});
	UAM_ASSERT(app.folders[0].id != app.folders[1].id);
	UAM_ASSERT_EQ(app.chats.front().folder_id, app.folders[1].id);
	const auto persisted = ChatFolderStore::Load(app.data_root);
	UAM_ASSERT_EQ(persisted.size(), std::size_t{2});
	UAM_ASSERT(persisted[0].id != persisted[1].id);
}

UAM_TEST(FailedHistoryPublicationDoesNotLeaveGeneratedEmptyWorkspace)
{
	TempDir temp("uam-import-failed-publication");
	fs::create_directories(temp.root / "workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(temp.root / "workspace", "failed-chat"));
	UAM_ASSERT_EQ(ChatHistorySyncService().ImportDiscoveredProviderChatsBatch(app, {}, discovery).imported_count, 0);
	UAM_ASSERT(discovery.preparation.valid());
	discovery.preparation.wait();
	const auto prepared = discovery.preparation.get();
	UAM_ASSERT_EQ(prepared->chats.size(), std::size_t{1});
	UAM_ASSERT(prepared->chats.front().files != nullptr);
	// Remove only this private staged chat, leaving staged folder metadata writable.
	fs::remove(prepared->chats.front().files->primary_temp);
	const auto result = DrainFixture(app, discovery, ChatHistorySyncService::ImportPolicy::ExplicitImport);
	UAM_ASSERT(!result.success);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(ChatFolderStore::Load(app.data_root).empty());
}

UAM_TEST(FailedSynchronousHistoryImportRetainsExistingWorkspaceOwner)
{
	TempDir temp("uam-sync-import-existing-owner");
	fs::create_directories(temp.root / "workspace");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.folders.push_back({"existing-owner", "Owner", (temp.root / "workspace").string(), false, "local"});
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	ChatSession original = ImportFixture(temp.root / "workspace", "existing-native");
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, original));
	ChatSession updated = original;
	updated.messages.push_back(Message{MessageRole::Assistant, "new native tail"});
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(updated);
#if defined(_WIN32)
	// Permit history reads while denying writes and atomic replacement of the existing chat.
	const HANDLE chat_file = CreateFileW(AppPaths::UamChatFilePath(app.data_root, original.id).c_str(),
	    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	UAM_ASSERT(chat_file != INVALID_HANDLE_VALUE);
	const std::unique_ptr<void, decltype(&CloseHandle)> block_chat_write(chat_file, &CloseHandle);
#else
	ScopedDirectoryNoWrite block_chat_write(AppPaths::UamChatsRootPath(app.data_root));
#endif
	const auto result = ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(app, {}, std::move(discovery), true);
	UAM_ASSERT(!result.success);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.folders.front().id, std::string("existing-owner"));
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).front().id, std::string("existing-owner"));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(app.data_root, original.id)->messages.size(), std::size_t{1});
}

UAM_TEST(FailedSynchronousHistoryImportKeepsSuccessfulSiblingsAndPrunesOnlyNewEmptyFolder)
{
	TempDir temp("uam-sync-import-sibling-rollback");
	fs::create_directories(temp.root / "good");
	fs::create_directories(temp.root / "bad");
	uam::AppState app;
	app.data_root = temp.root / "data";
	fs::create_directories(AppPaths::UamChatFilePath(app.data_root, "blocked-native"));
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(ImportFixture(temp.root / "good", "successful-native"));
	discovery.chats.push_back(ImportFixture(temp.root / "bad", "blocked-native"));
	const auto result = ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(app, {}, std::move(discovery), true);
	UAM_ASSERT(!result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.folders.front().directory, (temp.root / "good").string());
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().folder_id, app.folders.front().id);
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).size(), std::size_t{1});
	UAM_ASSERT(ChatRepository::LoadLocalChat(app.data_root, "successful-native").has_value());
}
