#include "test_harness.h"
#include "core/chat_import_utils.h"
#include <memory>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace uam_test;

namespace
{
#if !defined(_WIN32)
	void AssertNonregularGitMetadataRejected(const std::string& metadata)
	{
		TempDir temp("uam-import-nonregular-git-" + metadata);
		const fs::path main = temp.root / "main";
		const fs::path linked = temp.root / "linked";
		const fs::path nested = linked / "nested";
		const fs::path workspace = nested / "child";
		const fs::path registration = main / ".git" / "worktrees" / "nested";
		const fs::path outer_registration = main / ".git" / "worktrees" / "outer";
		fs::create_directories(workspace);
		fs::create_directories(registration);
		fs::create_directories(outer_registration);
		UAM_ASSERT(uam::io::WriteTextFile(linked / ".git", "gitdir: " + outer_registration.string()));
		UAM_ASSERT(uam::io::WriteTextFile(outer_registration / "commondir", "../.."));
		UAM_ASSERT(uam::io::WriteTextFile(outer_registration / "gitdir", (linked / ".git").string()));
		UAM_ASSERT(uam::io::WriteTextFile(nested / ".git", "gitdir: " + registration.string()));
		UAM_ASSERT(uam::io::WriteTextFile(registration / "commondir", "../.."));
		UAM_ASSERT(uam::io::WriteTextFile(registration / "gitdir", (nested / ".git").string()));
		UAM_ASSERT(FolderDirectoryMatches(uam::ResolveImportedWorkspaceFolderDirectory(workspace), main / "child"));
		const fs::path marker = metadata == ".git" ? nested / metadata : registration / metadata;
		const fs::path regular_target = temp.root / "regular-metadata";
		fs::rename(marker, regular_target);
		fs::create_symlink(regular_target, marker);
		UAM_ASSERT(FolderDirectoryMatches(uam::ResolveImportedWorkspaceFolderDirectory(workspace), main / "child"));
		fs::remove(marker);
		UAM_ASSERT_EQ(::mkfifo(marker.c_str(), 0600), 0);

		// A private child bounds a regression that otherwise blocks forever opening a FIFO.
		const pid_t child = ::fork();
		UAM_ASSERT(child >= 0);
		if (child == 0)
		{
			const std::vector<ChatFolder> folders{{"outer", "Outer", linked.string(), false, "local"}};
			const bool rejected = FolderDirectoryMatches(uam::ResolveImportedWorkspaceFolderDirectory(workspace), workspace) &&
			    !uam::ImportedWorkspaceMatchesFolder(workspace, main) &&
			    uam::FindImportedWorkspaceFolder(folders, workspace) == nullptr;
			::_exit(rejected ? 0 : 1);
		}
		int status = 0;
		bool finished = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (::waitpid(child, &status, WNOHANG) == child) { finished = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (!finished)
		{
			::kill(child, SIGKILL);
			while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
		}
		UAM_ASSERT(finished);
		UAM_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		fs::remove(marker);
		fs::create_directory(marker);
		UAM_ASSERT(FolderDirectoryMatches(uam::ResolveImportedWorkspaceFolderDirectory(workspace), workspace));
		UAM_ASSERT(!uam::ImportedWorkspaceMatchesFolder(workspace, main));
	}
#endif

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

#if !defined(_WIN32)
UAM_TEST(ImportedWorkspaceRejectsNonregularGitMarker)
{
	AssertNonregularGitMetadataRejected(".git");
}

UAM_TEST(ImportedWorkspaceRejectsNonregularGitCommondir)
{
	AssertNonregularGitMetadataRejected("commondir");
}

UAM_TEST(ImportedWorkspaceRejectsNonregularGitBacklink)
{
	AssertNonregularGitMetadataRejected("gitdir");
}
#endif

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

UAM_TEST(RemoteIsolatedWorkspaceResolutionNeverUsesSourceDirectory)
{
	uam::AppState app;
	ChatSession chat;
	chat.execution_host_id = "ssh-host";
	chat.workspace_directory = "/remote/source";
	chat.workspace_source_directory = "/remote/source";
	chat.workspace_isolation_kind = "gitWorktree";
	for (const std::string worktree : {std::string("/remote/isolated"), std::string(R"(C:\Users\david\isolated)")})
	{
		chat.workspace_worktree_directory = worktree;
		UAM_ASSERT_EQ(uam::paths::RemoteWorkspaceDirectory(chat), worktree);
		UAM_ASSERT_EQ(uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), worktree);
		UAM_ASSERT(uam::paths::ResolveControllerWorkspaceRootPath(app, chat).empty());
	}
	chat.workspace_worktree_directory.clear();
	UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(app, chat).empty());
	ExecutionHost host;
	host.id = chat.execution_host_id; host.platform = "linux"; host.transport = "ssh";
	app.settings.execution_hosts.push_back(host);
	const uam::VcsCommitStatus unavailable = uam::VcsCommitService().Status(app, chat);
	UAM_ASSERT(!unavailable.available);
	UAM_ASSERT_EQ(unavailable.error, std::string("The remote workspace directory is unavailable."));
	chat.workspace_isolation_kind.clear();
	UAM_ASSERT_EQ(uam::paths::RemoteWorkspaceDirectory(chat), chat.workspace_directory);
	chat.imported_read_only = true;
	UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(app, chat).empty());
}
