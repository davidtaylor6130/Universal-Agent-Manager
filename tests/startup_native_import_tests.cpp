#include "test_harness.h"
#include "app/startup_native_import_service.h"
#include "app/agent_run_ledger.h"
#include "app/agent_run_scheduler.h"
#include "common/utils/diagnostic_log.h"
#include "common/companion/protocol.h"

#include <atomic>
#include <fstream>
#include <future>
#include <thread>

using namespace uam_test;

namespace
{
	ChatSession StartupHistory(const fs::path& directory, const std::string& id)
	{
		ChatSession chat;
		chat.id = id;
		chat.native_session_id = id;
		chat.provider_id = "gemini-cli";
		chat.execution_host_id = "local";
		chat.workspace_directory = directory.string();
		chat.title = "Fixture " + id;
		chat.messages.push_back(Message{MessageRole::User, "private history body"});
		return chat;
	}

	void StartupWorkspace(uam::AppState& app, const fs::path& root)
	{
		fs::create_directories(root / "workspace");
		app.data_root = root / "data";
		app.folders.push_back({"known", "Known", (root / "workspace").string(), false, "local"});
	}

	uam::StartupNativeImportService::Scanner FixtureScanner(std::vector<ChatSession> chats)
	{
		return [chats = std::move(chats)](const uam::StartupNativeImportScanRequest&, std::stop_token)
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			discovery.chats = chats;
			return discovery;
		};
	}

	void WaitForStartup(uam::StartupNativeImportService& service, uam::AppState& app)
	{
		// Hang guard, not a speed budget: importing 206 chats takes ~7 s locally and longer on Windows runners.
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (service.Snapshot().phase == "scanning" || service.ImportPending())
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			service.Poll(app);
			std::this_thread::yield();
		}
	}
}

UAM_TEST(SettingsNativeDiscoveryIsLocalAndRejectsOverlappingScans)
{
	TempDir temp("uam-settings-native-local-discovery");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.folders.push_back({"offline", "Offline SSH", "/work/remote", false, "offline"});
	app.open_code_history_scan_offset = 7;
	std::size_t calls = 0;
	uam::StartupNativeImportService service([&](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		++calls;
		UAM_ASSERT(request.remote_scopes.empty());
		UAM_ASSERT(!request.remote_only && !request.open_code_only);
		UAM_ASSERT_EQ(request.open_code_offset, std::size_t{7});
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(StartupHistory(temp.root / "workspace", "eligible"));
		return discovery;
	});
	std::string error;
	UAM_ASSERT(service.DiscoverLocalHistory(app, &error));
	const std::string generation = service.Snapshot().generation;
	UAM_ASSERT(!service.DiscoverLocalHistory(app, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(service.Snapshot().generation, generation);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(calls, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT(service.Snapshot().errors.empty());
	UAM_ASSERT(app.chats.empty());
	service.Initialize(app);
	UAM_ASSERT_EQ(service.Snapshot().generation, generation);
	UAM_ASSERT(service.DiscoverLocalHistory(app, &error));
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot next = service.Snapshot();
	UAM_ASSERT(next.generation != generation);
	UAM_ASSERT_EQ(calls, std::size_t{2});
	UAM_ASSERT(!service.Decide(app, generation, {next.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(SettingsNativeRescanPreservesSavedOwnershipAndTombstones)
{
	TempDir temp("uam-settings-native-preservation");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const fs::path directory = temp.root / "workspace";
	ChatSession owner = StartupHistory(directory, "owned-native");
	owner.id = "uam-owner";
	owner.title = "UAM title must survive";
	owner.pinned = true;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
	ChatHistorySyncService().LoadSidebarChats(app);
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {StartupHistory(directory, "deleted")}, added));
	const std::string tombstones = uam::io::ReadTextFile(app.data_root / "native-import-tombstones.json");
	const std::string saved_owner = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, owner.id));
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(directory, "owned-native"),
	    StartupHistory(directory, "deleted"), StartupHistory(directory, "fresh"), StartupHistory(directory, "fresh"),
	    StartupHistory(temp.root / "unregistered", "unknown")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
	UAM_ASSERT_EQ(ready.candidates.front().native_session_id, std::string("fresh"));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, owner.id)), saved_owner);
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT(service.ImportPending());
	UAM_ASSERT(!service.DiscoverLocalHistory(app, &error));
	UAM_ASSERT_EQ(service.Snapshot().generation, ready.generation);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
	UAM_ASSERT(service.DiscoverLocalHistory(app, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("ready"));
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().candidates.front().native_session_id, std::string("unknown"));
	UAM_ASSERT(service.Snapshot().candidates.front().folder_id.empty());
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 0);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(app.data_root / "native-import-tombstones.json"), tombstones);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, owner.id)), saved_owner);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "owned-native")));
}

UAM_TEST(SettingsNativeUnregisteredWorkspaceAssignmentIsReadOnlyUntilSelectedImport)
{
	TempDir temp("uam-settings-native-workspace-assignment");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const fs::path source = temp.root / "unregistered";
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(source, "one"), StartupHistory(source, "two")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
	UAM_ASSERT(ready.candidates.front().folder_id.empty());
	const std::string id = ready.candidates.front().candidate_id;
	std::string error;
	UAM_ASSERT(!service.Decide(app, ready.generation, {id}, {}, &error));
	UAM_ASSERT(!service.AssignWorkspace(app, "stale", id, "Workspace", &error));
	UAM_ASSERT(!service.AssignWorkspace(app, ready.generation, id, "two\nlines", &error));
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, id, "Reviewed workspace", &error));
	const auto assigned = service.Snapshot();
	const std::string folder_id = assigned.candidates.front().folder_id;
	UAM_ASSERT(!folder_id.empty());
	UAM_ASSERT_EQ(assigned.candidates.back().folder_id, folder_id);
	UAM_ASSERT(assigned.candidates.front().new_workspace);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(app.data_root / "folders.txt"));
	UAM_ASSERT(!fs::exists(source));
	UAM_ASSERT(service.Decide(app, ready.generation, {id}, {}, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{2});
	const ChatFolder* saved = ChatDomainService().FindFolderById(app, folder_id);
	UAM_ASSERT(saved != nullptr);
	UAM_ASSERT_EQ(saved->directory, source.string());
	UAM_ASSERT_EQ(saved->title, std::string("Reviewed workspace"));
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).size(), std::size_t{2});
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT(!service.Snapshot().candidates.front().new_workspace);
	UAM_ASSERT(!fs::exists(source));
	const auto imported = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(imported.has_value());
	UAM_ASSERT_EQ(imported->workspace_directory, source.string());
	UAM_ASSERT_EQ(imported->native_session_id, std::string("one"));
	UAM_ASSERT_EQ(imported->messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(imported->messages.front().content, std::string("private history body"));
}

UAM_TEST(SettingsNativeUnregisteredAssignmentRetainsHiddenOwnershipAndLateDeletion)
{
	TempDir temp("uam-settings-native-unregistered-guards");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const fs::path source = temp.root / "unregistered";
	ChatSession hidden = StartupHistory(source, "hidden-native");
	hidden.id = "uam-hidden-owner";
	hidden.agent_run_id = "hidden-run";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, hidden));
	const std::string original = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, hidden.id));
	ChatHistorySyncService().LoadSidebarChats(app);
	UAM_ASSERT(app.chats.empty());
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(source, "hidden-native"), StartupHistory(source, "fresh"), StartupHistory(source, "fresh")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Review"));
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {StartupHistory(source, "fresh")}, added));
	std::string error;
	UAM_ASSERT(!service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, hidden.id)), original);
	UAM_ASSERT(!fs::exists(app.data_root / "folders.txt"));
}

UAM_TEST(SettingsNativeWorkspacePlanRejectsConcurrentCatalogueChanges)
{
	TempDir temp("uam-settings-native-workspace-race");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(temp.root / "unregistered", "fresh")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Review"));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}));
	service.Poll(app); // Starts preparation only; publication requires a later poll.
	app.folders.push_back({"concurrent", "User addition", (temp.root / "other").string(), false, "local"});
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 0);
	UAM_ASSERT(!service.Snapshot().errors.empty());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.folders.back().id, std::string("concurrent"));
	UAM_ASSERT(!fs::exists(app.data_root / "folders.txt"));
}

UAM_TEST(SettingsNativeWorkspaceSaveFailureDoesNotPublishChats)
{
	TempDir temp("uam-settings-native-workspace-save-failure");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	fs::create_directories(app.data_root / "folders.txt");
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(temp.root / "unregistered", "fresh")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Review"));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 0);
	UAM_ASSERT(!service.Snapshot().errors.empty());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "fresh")));
	UAM_ASSERT(fs::remove(app.data_root / "folders.txt"));
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Recovered workspace"));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.folders.back().title, std::string("Recovered workspace"));
}

UAM_TEST(SettingsNativeAssignmentCanPublishIntoAnEmptyWorkspaceCatalogue)
{
	TempDir temp("uam-settings-native-no-workspaces");
	uam::AppState app;
	app.data_root = temp.root / "data";
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(temp.root / "native-workspace", "fresh")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "First workspace"));
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).front().directory, (temp.root / "native-workspace").string());
}

UAM_TEST(SettingsNativeAssignmentRejectsLateHiddenOwnerBeforePublication)
{
	TempDir temp("uam-settings-native-late-hidden-owner");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const auto source = temp.root / "native-workspace";
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(source, "native-shared")}));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Review"));
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}));
	service.Poll(app);
	ChatSession hidden = StartupHistory(source, "native-shared");
	hidden.id = "hidden-uam-owner";
	hidden.agent_run_id = "hidden-run";
	hidden.messages.front().content = "Canonical owned input must survive";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, hidden));
	ChatHistorySyncService().LoadSidebarChats(app); // Publish the newly acquired hidden owner to the app catalogue.
	const std::string original = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, hidden.id));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 0);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, hidden.id)), original);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "native-shared")));
	UAM_ASSERT(!fs::exists(app.data_root / "folders.txt"));
}

UAM_TEST(SettingsNativeAssignmentsResolvePathAliasesAndRefuseLinkedWorktreeConflicts)
{
	TempDir temp("uam-native-assignment-overlap");
	const fs::path main = temp.root / "main", worktree = temp.root / "worktree";
	const fs::path git = main / ".git" / "worktrees" / "fixture";
	fs::create_directories(git);
	fs::create_directories(worktree);
	UAM_ASSERT(uam::io::WriteTextFile(worktree / ".git", "gitdir: " + git.string() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(git / "gitdir", (worktree / ".git").string() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(git / "commondir", "../..\n"));
	UAM_ASSERT(FolderDirectoryMatches(uam::ResolveImportedWorkspaceFolderDirectory(worktree), main));
	for (bool main_first : {true, false})
	{
		uam::AppState app;
		app.data_root = temp.root / (main_first ? "main-first" : "worktree-first");
		uam::StartupNativeImportService service(FixtureScanner({StartupHistory(main, "main-native"), StartupHistory(worktree, "worktree-native")}));
		UAM_ASSERT(service.DiscoverLocalHistory(app));
		WaitForStartup(service, app);
		const auto ready = service.Snapshot();
		const auto main_id = ready.candidates[0].candidate_id, worktree_id = ready.candidates[1].candidate_id;
		std::string error;
		UAM_ASSERT(service.AssignWorkspace(app, ready.generation, main_first ? main_id : worktree_id, "First"));
		if (main_first)
		{
			UAM_ASSERT(service.AssignWorkspace(app, ready.generation, worktree_id, "Second"));
			const auto assigned = service.Snapshot();
			UAM_ASSERT_EQ(assigned.candidates[0].folder_id, assigned.candidates[1].folder_id);
			UAM_ASSERT(service.Decide(app, ready.generation, {main_id, worktree_id}, {}));
			WaitForStartup(service, app);
			UAM_ASSERT_EQ(service.Snapshot().imported_count, 2);
			UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
			UAM_ASSERT_EQ(app.chats[0].workspace_directory, worktree.string());
		}
		else
		{
			UAM_ASSERT(!service.AssignWorkspace(app, ready.generation, main_id, "Conflicting main", &error));
			UAM_ASSERT(error.find("conflicts") != std::string::npos);
			UAM_ASSERT(app.folders.empty());
			UAM_ASSERT(app.chats.empty());
		}
	}
	uam::AppState app;
	app.data_root = temp.root / "aliases";
	uam::StartupNativeImportService aliases(FixtureScanner({StartupHistory(main, "alias-one"), StartupHistory(fs::path(main.string() + "/"), "alias-two"), StartupHistory(main / "missing" / "..", "alias-three")}));
	UAM_ASSERT(aliases.DiscoverLocalHistory(app));
	WaitForStartup(aliases, app);
	const auto ready = aliases.Snapshot();
	for (const auto& candidate : ready.candidates) UAM_ASSERT(aliases.AssignWorkspace(app, ready.generation, candidate.candidate_id, "Same workspace"));
	const auto assigned = aliases.Snapshot();
	std::vector<std::string> ids;
	for (const auto& candidate : assigned.candidates) { UAM_ASSERT_EQ(candidate.folder_id, assigned.candidates[0].folder_id); ids.push_back(candidate.candidate_id); }
	UAM_ASSERT(aliases.Decide(app, ready.generation, ids, {}));
	WaitForStartup(aliases, app);
	UAM_ASSERT_EQ(aliases.Snapshot().imported_count, 3);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
}

UAM_TEST(SettingsNativeMultiBatchPublicationOnlyCreatesAdmittedWorkspaceOwners)
{
	TempDir temp("uam-native-multibatch-owners");
	uam::AppState app;
	app.data_root = temp.root / "data";
	std::vector<ChatSession> chats;
	for (unsigned i = 0; i < 17; ++i) chats.push_back(StartupHistory(temp.root / ("source-" + std::to_string(i / 8)), "batch-native-" + std::to_string(i)));
	uam::StartupNativeImportService service(FixtureScanner(chats));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	for (const auto& candidate : ready.candidates) UAM_ASSERT(service.AssignWorkspace(app, ready.generation, candidate.candidate_id, "Reviewed"));
	std::vector<std::string> ids;
	for (const auto& candidate : service.Snapshot().candidates) ids.push_back(candidate.candidate_id);
	UAM_ASSERT(service.Decide(app, ready.generation, ids, {}));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (service.ImportPending())
	{
		UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
		service.Poll(app);
		for (const ChatFolder& folder : app.folders)
			UAM_ASSERT(std::ranges::any_of(app.chats, [&](const ChatSession& chat) { return chat.folder_id == folder.id; }));
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 17);
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{3});
	UAM_ASSERT(service.Snapshot().errors.empty());
}

UAM_TEST(SettingsNativeExplicitRescanPreservesSettledRemoteCandidatesWithoutRemoteWork)
{
	TempDir temp("uam-native-rescan-remote-staging");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	ExecutionHost host;
	host.id = "synthetic-host"; host.transport = "ssh"; host.platform = "linux"; host.runner_status = "ready";
	app.settings.execution_hosts.push_back(host);
	app.folders.push_back({"remote", "Remote", "/synthetic/remote", false, host.id});
	unsigned remote_calls = 0;
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(temp.root / "workspace", "local-native")}), [&](const uam::StartupNativeImportRemoteScope&, const ProviderProfile*, std::stop_token)
	{
		++remote_calls;
		ChatHistorySyncService::LocalHistoryDiscovery result;
		auto chat = StartupHistory("/synthetic/remote", "remote-native");
		chat.execution_host_id = host.id; chat.folder_id = "remote";
		result.chats.push_back(std::move(chat));
		return result;
	});
	service.Initialize(app); WaitForStartup(service, app);
	UAM_ASSERT_EQ(remote_calls, 1U);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{2});
	UAM_ASSERT(service.DiscoverLocalHistory(app)); WaitForStartup(service, app);
	UAM_ASSERT_EQ(remote_calls, 1U);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{2});
	UAM_ASSERT_EQ(service.Snapshot(0, 100, "remote").candidates[0].native_session_id, std::string("remote-native"));
}

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(SettingsNativeSharedTestHomeNeverReadsAmbientAntigravityHistory)
{
	TempDir ambient("uam-native-ambient-synthetic-home");
	const auto file = ambient.root / ".gemini" / "antigravity-cli" / "conversations" / "3dc412af-b91a-4d9d-82f0-6cd3f991ce2c.db";
	fs::create_directories(file.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(file, "refusable synthetic ambient history"));
	ScopedEnvVar ambient_home("HOME", ambient.root.string()), ambient_profile("USERPROFILE", ambient.root.string());
	{
		ScopedNativeHistoryHome ordinary_runner_home;
		const auto result = ChatHistorySyncService().DiscoverProviderChatsForFolder({}, nullptr, {}, 0, false, {}, {}, "antigravity-cli");
		UAM_ASSERT(result.chats.empty());
		UAM_ASSERT(result.result.errors.empty());
	}
	UAM_ASSERT_EQ(ReadFile(file), std::string("refusable synthetic ambient history"));
	UAM_ASSERT(!fs::exists(file.string() + "-wal"));
	UAM_ASSERT(!fs::exists(file.string() + "-shm"));
	UAM_ASSERT(!uam::companion::FindAction("discoverSystemNativeImports"));
	UAM_ASSERT(!uam::companion::FindAction("assignSystemNativeImportWorkspace"));
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(SettingsNativeSixProviderAssignmentAndDeclineRetainExactSourceScope)
{
	TempDir temp("uam-settings-native-six-provider-scope");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const auto source = temp.root / "unregistered-source";
	std::vector<ChatSession> histories;
	for (const char* provider : {"gemini-cli", "codex-cli", "claude-cli", "opencode-cli", "copilot-cli", "antigravity-cli"})
	{
		ChatSession chat = StartupHistory(source, std::string(provider) == "opencode-cli" ? "ses_syntheticNativeScope" : "11111111-1111-4111-8111-111111111111");
		chat.provider_id = provider;
		histories.push_back(std::move(chat));
	}
	uam::StartupNativeImportService service(FixtureScanner(histories));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	const auto ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{6});
	UAM_ASSERT(service.AssignWorkspace(app, ready.generation, ready.candidates.front().candidate_id, "Original source"));
	const auto assigned = service.Snapshot();
	std::vector<std::string> declined;
	std::unordered_set<std::string> providers;
	for (const auto& candidate : assigned.candidates)
	{
		UAM_ASSERT_EQ(candidate.workspace_directory, source.string());
		UAM_ASSERT_EQ(candidate.folder_id, assigned.candidates.front().folder_id);
		UAM_ASSERT(candidate.new_workspace);
		providers.insert(candidate.provider_id);
		declined.push_back(candidate.candidate_id);
	}
	UAM_ASSERT_EQ(providers.size(), std::size_t{6});
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT(service.Decide(app, ready.generation, {}, declined));
	UAM_ASSERT(service.DiscoverLocalHistory(app));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(source));
	UAM_ASSERT(!fs::exists(app.data_root / "folders.txt"));
}

#endif

#if !defined(UAM_NATIVE_IMPORT_APPLICATION_FOCUSED)
UAM_TEST(StartupNativeCandidatesRetainHiddenAgentOwnershipAcrossProviders)
{
	TempDir temp("uam-startup-hidden-agent-ownership");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const fs::path directory = temp.root / "workspace";
	std::vector<ChatSession> histories;
	std::vector<ChatSession> owners;
	for (const std::string& provider : {std::string("claude-cli"), std::string("codex-cli")})
	{
		const std::string native_id = provider == "codex-cli" ? "11111111-1111-4111-8111-111111111111" : "native-" + provider;
		ChatSession native = StartupHistory(directory, native_id);
		native.provider_id = provider;
		histories.push_back(native);
		ChatSession owner = native;
		owner.id = "chat-agent-" + provider;
		owner.agent_run_id = "run-" + provider;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
		owners.push_back(std::move(owner));
	}
	histories.push_back(StartupHistory(directory, "ordinary-unowned"));
	ChatHistorySyncService().LoadSidebarChats(app);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.hidden_agent_chat_ownership.chats.size(), std::size_t{2});
	uam::StartupNativeImportService service(FixtureScanner(histories));
	service.Initialize(app);
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
	UAM_ASSERT_EQ(ready.candidates.front().native_session_id, std::string("ordinary-unowned"));
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {}, {}, &error, "known", true));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	for (const ChatSession& owner : owners)
	{
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, owner.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->native_session_id, owner.native_session_id);
		UAM_ASSERT_EQ(saved->agent_run_id, owner.agent_run_id);
		UAM_ASSERT_EQ(saved->messages, owner.messages);
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, owner.native_session_id)));
	}
}

UAM_TEST(StartupHiddenAgentOwnershipRefreshesOnSidebarMergeAndRootReload)
{
	TempDir temp("uam-startup-hidden-agent-refresh");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	ChatSession hidden = StartupHistory(temp.root / "workspace", "native-before");
	hidden.provider_id = "claude-cli";
	hidden.id = "chat-hidden";
	hidden.agent_run_id = "run-hidden";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, hidden));
	ChatSession ordinary = StartupHistory(temp.root / "workspace", "ordinary-local");
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, ordinary));
	const ChatHistorySyncService history;
	history.LoadSidebarChats(app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	app.chats.front().title = "Unsaved local title";
	hidden.native_session_id = "native-after";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, hidden));
	history.MergeSidebarChatsPreservingCurrent(app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().title, std::string("Unsaved local title"));
	ChatSession before = StartupHistory(temp.root / "workspace", "native-before");
	before.provider_id = "claude-cli";
	ChatSession after = before;
	after.id = "native-after";
	after.native_session_id = after.id;
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats = {before, after};
	UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(discovery.chats.front().native_session_id, before.native_session_id);
	app.data_root = temp.root / "second-data-root";
	history.LoadSidebarChats(app);
	UAM_ASSERT(app.chats.empty());
	discovery.chats = {before, after};
	UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{2});
}

UAM_TEST(StartupNativeDiagnosticsLogErrorsBeyondBoundedSnapshot)
{
	TempDir temp("uam-startup-import-diagnostics");
	const fs::path log = temp.root / "diagnostics.log";
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	struct StopLog
	{
		~StopLog() { uam::diagnostics::StopFileLog(); }
	} stop;
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		for (int index = 0; index < 21; ++index) discovery.result.Fail("private-history-error-" + std::to_string(index));
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	// Twenty kept errors plus one omitted-issues summary.
	const std::vector<std::string> errors = service.Snapshot().errors;
	UAM_ASSERT_EQ(errors.size(), std::size_t{21});
	UAM_ASSERT(errors.back().starts_with("1 additional"));
	uam::diagnostics::FlushFileLog();
	UAM_ASSERT(uam::io::ReadTextFile(log).find("private-history-error-20") != std::string::npos);
}

UAM_TEST(StartupNativeOwnershipSurvivesAgentCancellationAndResolvedIdRemoval)
{
	for (const bool resolved_only : {false, true})
	{
		TempDir temp("uam-startup-cancelled-agent-ownership");
		uam::AppState app;
		StartupWorkspace(app, temp.root);
		const ChatHistorySyncService history;
		history.LoadSidebarChats(app);
		AgentRun run;
		run.id = uam::AgentRunLedger::NewRunId();
		run.root_chat_id = "root-chat";
		run.transcript_chat_id = "chat-running-agent";
		run.agent_id = "private-agent";
		run.provider_id = "claude-cli";
		run.definition_snapshot = "private fixture definition";
		run.task = "Private fixture task";
		run.effective_workspace_access = "read";
		run.status = "queued";
		run.depth = 1;
		run.created_at = uam::time::TimestampNow();
		run.updated_at = run.created_at;
		UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
		ChatSession transcript = StartupHistory(temp.root / "workspace", "real-native-agent");
		transcript.id = run.transcript_chat_id;
		transcript.agent_run_id = run.id;
		transcript.provider_id = run.provider_id;
		if (resolved_only) transcript.native_session_id = transcript.id;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, transcript));
		app.chats.push_back(transcript);
		app.agent_runs.push_back(run);
		app.queued_agent_run_ids.push_back(run.id);
		if (resolved_only) app.resolved_native_sessions_by_chat_id[transcript.id] = "real-native-agent";
		std::string error;
		UAM_ASSERT(uam::AgentRunScheduler::CancelTree(app, run.id, &error));
		UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("cancelled"));
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(app.acp_sessions.empty());
		UAM_ASSERT(app.queued_agent_run_ids.empty());
		UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(transcript.id));
		ChatSession native = StartupHistory(temp.root / "workspace", "real-native-agent");
		native.provider_id = run.provider_id;
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(native);
		UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
		UAM_ASSERT(discovery.chats.empty());
		UAM_ASSERT_EQ(app.hidden_agent_chat_ownership.chats.size(), std::size_t{1});
		UAM_ASSERT(app.hidden_agent_chat_ownership.chats.front().messages.empty());
		history.MergeSidebarChatsPreservingCurrent(app);
		UAM_ASSERT(app.chats.empty());
		discovery.chats = {native};
		UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
		UAM_ASSERT(discovery.chats.empty());
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, transcript.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->native_session_id, transcript.native_session_id);
		UAM_ASSERT_EQ(saved->messages, transcript.messages);
		if (resolved_only)
		{
			transcript.native_session_id = "newer-native-agent";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, transcript));
			history.MergeSidebarChatsPreservingCurrent(app);
			ChatSession newer = native;
			newer.id = transcript.native_session_id;
			newer.native_session_id = newer.id;
			discovery.chats = {native, newer};
			UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
			UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
			UAM_ASSERT_EQ(discovery.chats.front().native_session_id, native.native_session_id);
		}
		UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, transcript.id).Failed());
		history.MergeSidebarChatsPreservingCurrent(app);
		UAM_ASSERT(app.hidden_agent_chat_ownership.chats.empty());
		ChatSession deleted_source = native;
		deleted_source.id = transcript.native_session_id;
		deleted_source.native_session_id = deleted_source.id;
		discovery.chats = {deleted_source};
		UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
		UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
		app.data_root = temp.root / "second-data-root";
		history.LoadSidebarChats(app);
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(app.hidden_agent_chat_ownership.chats.empty());
		discovery.chats = {native};
		UAM_ASSERT(history.FilterStartupImportCandidates(app, discovery));
		UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
	}
}

UAM_TEST(StartupNativeHistoryImportAllIncludesEveryPageAndPreservesWorkspaceScope)
{
	for (bool scoped : {true, false})
	{
		TempDir temp(scoped ? "uam-startup-import-all-scoped" : "uam-startup-import-all-global");
		uam::AppState app;
		StartupWorkspace(app, temp.root);
		fs::create_directories(temp.root / "other-workspace");
		app.folders.push_back({"other", "Other", (temp.root / "other-workspace").string(), false, "local"});
		std::vector<ChatSession> chats;
		for (int index = 0; index < 205; ++index) chats.push_back(StartupHistory(temp.root / "workspace", "history-" + std::to_string(index)));
		chats.push_back(StartupHistory(temp.root / "other-workspace", "outside"));
		uam::StartupNativeImportService service(FixtureScanner(chats));
		service.Initialize(app);
		WaitForStartup(service, app);
		const uam::StartupNativeImportSnapshot ready = service.Snapshot();
		UAM_ASSERT_EQ(ready.candidates.size(), std::size_t{100});
		UAM_ASSERT_EQ(ready.total_count, std::size_t{206});
		std::string error;
		const std::string folder = scoped ? "known" : "";
		UAM_ASSERT(!service.Decide(app, "stale", {}, {}, &error, folder, true));
		UAM_ASSERT(!service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error, folder, true));
		UAM_ASSERT(!service.Decide(app, ready.generation, {}, {}, &error, "missing", true));
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(!fs::exists(app.data_root));
		UAM_ASSERT(service.Decide(app, ready.generation, {}, {}, &error, folder, true));
		WaitForStartup(service, app);
		UAM_ASSERT_EQ(app.chats.size(), scoped ? std::size_t{205} : std::size_t{206});
		UAM_ASSERT_EQ(service.Snapshot().total_count, scoped ? std::size_t{1} : std::size_t{0});
		if (scoped) UAM_ASSERT_EQ(service.Snapshot().candidates.front().native_session_id, std::string("outside"));
	}
}

UAM_TEST(StartupNativeHistoryScansOnceOffThreadAndPublishesOnlySelectedChats)
{
	TempDir temp("uam-startup-history-selected");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::atomic<int> scans{0};
	std::atomic<bool> off_thread{false};
	const std::thread::id ui_thread = std::this_thread::get_id();
	const fs::path directory = temp.root / "workspace";
	uam::StartupNativeImportService service([&scans, &off_thread, ui_thread, directory](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		++scans;
		off_thread = std::this_thread::get_id() != ui_thread;
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats = {StartupHistory(directory, "first"), StartupHistory(directory, "second")};
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	for (int repeat = 0; repeat < 20; ++repeat) { service.Initialize(app); service.Poll(app); }
	UAM_ASSERT_EQ(scans.load(), 1);
	UAM_ASSERT(off_thread.load());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
	UAM_ASSERT(!fs::exists(app.data_root));
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.candidates.size(), std::size_t{2});
	app.composer_text = "unsent next prompt";
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
	UAM_ASSERT_EQ(app.composer_text, std::string("unsent next prompt"));
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "second")));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummaries(app.data_root).size(), std::size_t{1});
}

UAM_TEST(StartupNativeCandidatesExcludeOwnedDeletedDuplicateAndUnregisteredHistories)
{
	TempDir temp("uam-startup-history-filter");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const fs::path directory = temp.root / "workspace";
	ChatSession owned = StartupHistory(directory, "owned");
	owned.id = "chat-owned";
	app.chats.push_back(owned);
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {StartupHistory(directory, "deleted")}, added));
	ChatSession remote = StartupHistory(directory, "remote");
	remote.execution_host_id = "ssh-host";
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(directory, "fresh"), StartupHistory(directory, "fresh"),
	    StartupHistory(directory, "owned"), StartupHistory(directory, "deleted"), StartupHistory(temp.root / "unknown", "unknown"), remote}));
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().candidates.front().native_session_id, std::string("fresh"));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.folders.size(), std::size_t{1});
}

UAM_TEST(StartupNativeDeclineFailureKeepsAllDecisionsPendingAndSuccessSurvivesRestart)
{
	TempDir temp("uam-startup-history-decline");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::ofstream(app.data_root) << "blocked data root";
	const std::vector<ChatSession> histories{StartupHistory(temp.root / "workspace", "declined"), StartupHistory(temp.root / "workspace", "selected")};
	uam::StartupNativeImportService service(FixtureScanner(histories));
	service.Initialize(app);
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
	std::string error;
	UAM_ASSERT(!service.Decide(app, ready.generation, {ready.candidates[1].candidate_id}, {ready.candidates[0].candidate_id}, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{2});
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!service.ImportPending());
	fs::remove(app.data_root);
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates[1].candidate_id}, {ready.candidates[0].candidate_id}, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("selected"));
	uam::AppState restarted;
	restarted.data_root = app.data_root;
	restarted.folders = app.folders;
	restarted.chats = ChatRepository::LoadLocalChatSummaries(app.data_root);
	uam::StartupNativeImportService next(FixtureScanner(histories));
	next.Initialize(restarted);
	WaitForStartup(next, restarted);
	UAM_ASSERT_EQ(next.Snapshot().total_count, std::size_t{0});
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "declined")));
}

UAM_TEST(StartupNativeDecisionsRejectStaleOwnershipWithoutDecliningAnOwnedChat)
{
	for (const bool change_folder : {false, true})
	{
		TempDir temp("uam-startup-history-stale");
		uam::AppState app;
		StartupWorkspace(app, temp.root);
		ChatSession history = StartupHistory(temp.root / "workspace", "native");
		uam::StartupNativeImportService service(FixtureScanner({history}));
		service.Initialize(app);
		WaitForStartup(service, app);
		const uam::StartupNativeImportSnapshot ready = service.Snapshot();
		if (change_folder) app.folders.front().directory = (temp.root / "other").string();
		else { history.id = "chat-attached"; app.chats.push_back(history); }
		std::string error;
		UAM_ASSERT(!service.Decide(app, ready.generation, {}, {ready.candidates.front().candidate_id}, &error));
		UAM_ASSERT(!fs::exists(app.data_root / "native-import-tombstones.json"));
		UAM_ASSERT(!service.ImportPending());
		UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
	}
}

UAM_TEST(StartupNativeSelectedPublicationRechecksOwnershipAfterBackgroundPreparation)
{
	TempDir temp("uam-startup-history-publication");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	ChatSession history = StartupHistory(temp.root / "workspace", "native");
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(history);
	const ChatHistorySyncService sync;
	const ChatHistorySyncService::ImportResult preparing = sync.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
	UAM_ASSERT_EQ(preparing.imported_count, 0);
	UAM_ASSERT(discovery.preparation.valid());
	discovery.preparation.wait();
	history.id = "chat-attached";
	app.chats.push_back(history);
	const ChatHistorySyncService::ImportResult published = sync.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
	UAM_ASSERT_EQ(published.imported_count, 0);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().id, std::string("chat-attached"));
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "native")));
}

UAM_TEST(StartupNativeChooserRejectsInvalidDecisionsAndBoundsMetadataPages)
{
	TempDir temp("uam-startup-history-pages");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::vector<ChatSession> chats;
	for (int index = 0; index < 205; ++index)
	{
		ChatSession chat = StartupHistory(temp.root / "workspace", "history-" + std::to_string(index));
		chat.title = std::string(700, 'x');
		chats.push_back(std::move(chat));
	}
	uam::StartupNativeImportService service(FixtureScanner(std::move(chats)));
	service.Initialize(app);
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot page = service.Snapshot(0, 999);
	UAM_ASSERT_EQ(page.total_count, std::size_t{205});
	UAM_ASSERT_EQ(page.candidates.size(), std::size_t{200});
	UAM_ASSERT_EQ(page.candidates.front().title.size(), std::size_t{512});
	UAM_ASSERT_EQ(service.Snapshot(page.next_offset).candidates.size(), std::size_t{5});
	std::string error;
	const std::string id = page.candidates.front().candidate_id;
	UAM_ASSERT(!service.Decide(app, "stale", {id}, {}, &error));
	UAM_ASSERT(!service.Decide(app, page.generation, {id}, {id}, &error));
	UAM_ASSERT(!service.Decide(app, page.generation, {"unknown"}, {}, &error));
	fs::create_directories(app.data_root);
	std::ofstream(app.data_root / "native-import-tombstones.json") << "malformed";
	UAM_ASSERT(!service.Decide(app, page.generation, {id}, {}, &error));
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{205});
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(StartupNativeChooserPaginatesWithinValidatedWorkspaceOwnership)
{
	TempDir temp("uam-startup-history-workspace-pages");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	fs::create_directories(temp.root / "other-workspace");
	app.folders.push_back({"other", "Other", (temp.root / "other-workspace").string(), false, "local"});
	std::vector<ChatSession> chats;
	for (int index = 0; index < 125; ++index)
		chats.push_back(StartupHistory(temp.root / "other-workspace", "other-" + std::to_string(index)));
	for (int index = 0; index < 3; ++index)
		chats.push_back(StartupHistory(temp.root / "workspace", "target-" + std::to_string(index)));
	uam::StartupNativeImportService service(FixtureScanner(std::move(chats)));
	service.Initialize(app);
	WaitForStartup(service, app);

	const uam::StartupNativeImportSnapshot target_page = service.Snapshot(0, 2, "known");
	UAM_ASSERT_EQ(target_page.total_count, std::size_t{3});
	UAM_ASSERT_EQ(target_page.candidates.size(), std::size_t{2});
	UAM_ASSERT_EQ(target_page.next_offset, std::size_t{2});
	UAM_ASSERT_EQ(target_page.candidates.front().folder_id, std::string("known"));
	const uam::StartupNativeImportSnapshot target_last = service.Snapshot(target_page.next_offset, 2, "known");
	UAM_ASSERT_EQ(target_last.total_count, std::size_t{3});
	UAM_ASSERT_EQ(target_last.candidates.size(), std::size_t{1});
	UAM_ASSERT_EQ(target_last.next_offset, std::size_t{3});
	UAM_ASSERT_EQ(target_last.candidates.front().folder_id, std::string("known"));
	UAM_ASSERT_EQ(service.Snapshot(0, 200).total_count, std::size_t{128});
	UAM_ASSERT_EQ(service.Snapshot(0, 2, "missing-folder").total_count, std::size_t{0});
	UAM_ASSERT_EQ(service.Snapshot(0, 2, "missing-folder").candidates.size(), std::size_t{0});
	std::string error;
	UAM_ASSERT(service.Decide(app, target_page.generation, {target_page.candidates.front().candidate_id}, {}, &error));
	const uam::StartupNativeImportSnapshot resolved_target_page = service.Snapshot(0, 100, "known");
	UAM_ASSERT_EQ(resolved_target_page.total_count, std::size_t{2});
	for (const uam::StartupNativeImportCandidate& candidate : resolved_target_page.candidates)
		UAM_ASSERT_EQ(candidate.folder_id, std::string("known"));
}

UAM_TEST(StartupNativeChooserRejectsCrossWorkspaceSelectionsAndDeclinesWithoutMutation)
{
	for (const bool decline : {false, true})
	{
		TempDir temp(decline ? "uam-startup-cross-workspace-decline" : "uam-startup-cross-workspace-select");
		uam::AppState app;
		StartupWorkspace(app, temp.root);
		fs::create_directories(temp.root / "other-workspace");
		app.folders.push_back({"other", "Other", (temp.root / "other-workspace").string(), false, "local"});
		uam::StartupNativeImportService service(FixtureScanner({
		    StartupHistory(temp.root / "workspace", "target"),
		    StartupHistory(temp.root / "other-workspace", "outside")}));
		service.Initialize(app);
		WaitForStartup(service, app);
		const uam::StartupNativeImportSnapshot target = service.Snapshot(0, 100, "known");
		const uam::StartupNativeImportSnapshot outside = service.Snapshot(0, 100, "other");
		const std::vector<std::string> ids{target.candidates.front().candidate_id, outside.candidates.front().candidate_id};
		std::string error;
		UAM_ASSERT(!service.Decide(app, target.generation, decline ? std::vector<std::string>{} : ids,
		    decline ? ids : std::vector<std::string>{}, &error, "known"));
		UAM_ASSERT_EQ(error, std::string("A history candidate belongs to another workspace. Review the current list."));
		UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{2});
		UAM_ASSERT_EQ(service.Snapshot().phase, std::string("ready"));
		UAM_ASSERT(!service.ImportPending());
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(!fs::exists(app.data_root));
		UAM_ASSERT(service.Decide(app, target.generation, {target.candidates.front().candidate_id}, {}, &error, "known"));
		WaitForStartup(service, app);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
		UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("known"));
		UAM_ASSERT_EQ(service.Snapshot(0, 100, "other").total_count, std::size_t{1});
	}
}

UAM_TEST(StartupNativeCancellationNeverRestartsDiscoveryOrWritesHistory)
{
	TempDir temp("uam-startup-history-cancel");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const std::shared_ptr<std::promise<void>> release = std::make_shared<std::promise<void>>();
	const std::shared_future<void> wait = release->get_future().share();
	const std::shared_ptr<std::promise<void>> entered = std::make_shared<std::promise<void>>();
	std::future<void> running = entered->get_future();
	std::atomic<int> scans{0};
	uam::StartupNativeImportService service([wait, entered, &scans](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		++scans;
		entered->set_value();
		wait.wait();
		return ChatHistorySyncService::LocalHistoryDiscovery{};
	});
	service.Initialize(app);
	UAM_ASSERT(running.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	service.Cancel();
	service.Initialize(app);
	service.Poll(app);
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("cancelled"));
	UAM_ASSERT_EQ(scans.load(), 1);
	UAM_ASSERT(!fs::exists(app.data_root));
	release->set_value();
}

namespace
{
	ExecutionHost StartupRemoteHost(const std::string& id)
	{
		ExecutionHost host;
		host.id = id;
		host.transport = "ssh";
		host.ssh_alias = id;
		host.platform = "windows";
		host.runner_status = "ready";
		host.runner_version = "fixture-version";
		return host;
	}

	ChatSession StartupRemoteHistory(const std::string& host, const std::string& folder, const std::string& path, const std::string& id)
	{
		ChatSession chat;
		chat.id = id;
		chat.native_session_id = id;
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = host;
		chat.folder_id = folder;
		chat.workspace_directory = path;
		chat.title = "Remote fixture " + id;
		chat.messages.push_back(Message{MessageRole::User, "Verified fixture transcript"});
		return chat;
	}
}

UAM_TEST(StartupNativeRemoteCandidatesUseCapturedReadyHostAndWindowsWorkspaceOwnership)
{
	TempDir temp("uam-startup-remote-scope");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.settings.execution_hosts.push_back(StartupRemoteHost("runner-a"));
	app.settings.execution_hosts.push_back(StartupRemoteHost("runner-b"));
	ExecutionHost offline = StartupRemoteHost("offline");
	offline.runner_status = "offline";
	app.settings.execution_hosts.push_back(offline);
	app.folders.push_back({"remote-a", "Remote A", "C:\\work\\repo", false, "runner-a"});
	app.folders.push_back({"remote-b", "Remote B", "C:\\work\\repo", false, "runner-b"});
	app.folders.push_back({"unavailable", "Offline", "C:\\work\\repo", false, "offline"});
	std::atomic<int> scope_count{0};
	std::atomic<bool> copied_scope{false};
	uam::StartupNativeImportService service([&scope_count, &copied_scope](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		scope_count = static_cast<int>(request.remote_scopes.size());
		copied_scope = request.remote_scopes.size() == 2 && request.remote_scopes.front().folder.id == "remote-a" &&
		    request.remote_scopes.front().host.ssh_alias == "runner-a";
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(StartupRemoteHistory("runner-a", "remote-a", "c:/work/repo", "ses_same"));
		discovery.chats.push_back(StartupRemoteHistory("runner-b", "remote-b", "C:\\work\\repo", "ses_same"));
		discovery.chats.push_back(StartupRemoteHistory("runner-a", "remote-a", "C:\\work\\other", "ses_wrong_path"));
		discovery.chats.push_back(StartupRemoteHistory("missing", "remote-a", "C:\\work\\repo", "ses_missing_host"));
		discovery.chats.push_back(StartupRemoteHistory("offline", "unavailable", "C:\\work\\repo", "ses_offline"));
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(scope_count.load(), 2);
	UAM_ASSERT(copied_scope.load());
	const uam::StartupNativeImportSnapshot snapshot = service.Snapshot();
	UAM_ASSERT_EQ(snapshot.total_count, std::size_t{2});
	UAM_ASSERT(!snapshot.errors.empty());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	std::string error;
	UAM_ASSERT(service.Decide(app, snapshot.generation, {snapshot.candidates.front().candidate_id}, {}, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().execution_host_id, std::string("runner-a"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("ses_same"));
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("remote-a"));
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().candidates.front().execution_host_id, std::string("runner-b"));
}

UAM_TEST(StartupNativeRemoteDecisionsAndPreparedPublicationRejectChangedConnectionOrFolder)
{
	for (const bool change_host : {false, true})
	{
		TempDir temp("uam-startup-remote-stale");
		uam::AppState app;
		StartupWorkspace(app, temp.root);
		app.settings.execution_hosts.push_back(StartupRemoteHost("runner"));
		app.folders.push_back({"remote", "Remote", "C:\\work\\repo", false, "runner"});
		ChatSession history = StartupRemoteHistory("runner", "remote", "C:\\work\\repo", "ses_stale");
		uam::StartupNativeImportService service(FixtureScanner({history}));
		service.Initialize(app);
		WaitForStartup(service, app);
		const uam::StartupNativeImportSnapshot snapshot = service.Snapshot();
		UAM_ASSERT_EQ(snapshot.total_count, std::size_t{1});
		if (change_host) app.settings.execution_hosts.back().ssh_alias = "replacement";
		else app.folders.back().directory = "C:\\work\\moved";
		std::string error;
		UAM_ASSERT(!service.Decide(app, snapshot.generation, {}, {snapshot.candidates.front().candidate_id}, &error));
		UAM_ASSERT(!fs::exists(app.data_root / "native-import-tombstones.json"));
		UAM_ASSERT(app.chats.empty());
	}
	{
		TempDir replaced("uam-startup-remote-replaced-before-preparation");
		uam::AppState app;
		StartupWorkspace(app, replaced.root);
		app.settings.execution_hosts.push_back(StartupRemoteHost("runner"));
		app.folders.push_back({"remote", "Remote", "C:\\work\\repo", false, "runner"});
		uam::StartupNativeImportService service(FixtureScanner({StartupRemoteHistory("runner", "remote", "C:\\work\\repo", "ses_selected")}));
		service.Initialize(app);
		WaitForStartup(service, app);
		const uam::StartupNativeImportSnapshot snapshot = service.Snapshot();
		UAM_ASSERT_EQ(snapshot.total_count, std::size_t{1});
		std::string error;
		UAM_ASSERT(service.Decide(app, snapshot.generation, {snapshot.candidates.front().candidate_id}, {}, &error));
		app.folders.back().id = "replacement-folder";
		WaitForStartup(service, app);
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "ses_selected")));
	}
	{
		TempDir moved("uam-startup-local-parent-after-preparation");
		uam::AppState app;
		StartupWorkspace(app, moved.root);
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		ChatSession selected = StartupHistory(moved.root / "workspace", "local-selected");
		selected.folder_id = "known";
		discovery.chats.push_back(selected);
		discovery.selected_folder_snapshots.emplace("known", app.folders.front());
		const ChatHistorySyncService history;
		history.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
		UAM_ASSERT(discovery.preparation.valid());
		discovery.preparation.wait();
		// The old cwd still belongs to the same id under nested ownership, but the approved owner changed.
		app.folders.front().directory = moved.root.string();
		const ChatHistorySyncService::ImportResult published = history.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
		UAM_ASSERT_EQ(published.imported_count, 0);
		UAM_ASSERT(app.chats.empty());
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "local-selected")));
	}
	TempDir temp("uam-startup-remote-publication");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.settings.execution_hosts.push_back(StartupRemoteHost("runner"));
	app.folders.push_back({"remote", "Remote", "C:\\work\\repo", false, "runner"});
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats.push_back(StartupRemoteHistory("runner", "remote", "C:\\work\\repo", "ses_prepared"));
	discovery.expected_hosts.emplace("runner", app.settings.execution_hosts.back());
	const ChatHistorySyncService history;
	history.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
	UAM_ASSERT(discovery.preparation.valid());
	discovery.preparation.wait();
	app.settings.execution_hosts.back().ssh_alias = "replacement";
	const ChatHistorySyncService::ImportResult published = history.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
	UAM_ASSERT_EQ(published.imported_count, 0);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, "ses_prepared")));
}

UAM_TEST(StartupSelectedRemoteAliasesPreserveSingleNativeOwnerAcrossPreparationAndPublication)
{
	struct ImportCase
	{
		std::string directory;
		std::string host;
		std::string native_id;
		bool duplicate;
		bool tombstoned;
		std::string platform = "linux";
		std::string owner_directory = "/srv/Project";
	};
	const std::vector<ImportCase> cases = {
	    {"/srv/Project/.", "runner", "ses_owned", true, false},
	    {"/srv//Project", "runner", "ses_owned", true, false},
	    {"/srv/Project/./", "runner", "ses_owned", true, false},
	    {"/srv/Project/.", "runner", "ses_different", false, false},
	    {"/srv/Project/.", "other-runner", "ses_owned", false, false},
	    {"/srv/project", "runner", "ses_owned", false, false},
	    {"/srv/Project\\literal", "runner", "ses_owned", false, false},
	    {"/srv/Project/nested", "runner", "ses_owned", false, false},
	    {"/srv/Project/link/..", "runner", "ses_owned", false, false},
	    {"//srv/Project", "runner", "ses_owned", false, false},
	    {"/srv/Project/.", "runner", "ses_declined", false, true},
	    {"c:\\PROJECT\\.\\", "runner", "ses_owned", true, false, "windows", "C:/Project"},
	    {"C:\\.", "runner", "ses_owned", true, false, "windows", "C:/"},
	    {"\\\\SERVER\\share\\.", "runner", "ses_owned", true, false, "windows", "\\\\server\\share\\"},
	    {"D:/", "runner", "ses_owned", false, false, "windows", "C:/"},
	    {"\\\\other\\share", "runner", "ses_owned", false, false, "windows", "\\\\server\\share\\"}};
	for (const int owner_timing : {0, 1, 2})
	{
		const bool publication_race = owner_timing == 1;
		const bool disk_only = owner_timing == 2;
		for (const ImportCase& item : cases)
		{
			TempDir temp("uam-selected-remote-alias");
			uam::AppState app;
			app.data_root = temp.root / "data";
			for (const std::string host_id : {"runner", "other-runner"})
			{
				ExecutionHost host = StartupRemoteHost(host_id);
				host.platform = item.platform;
				app.settings.execution_hosts.push_back(host);
			}
			app.folders = {{"canonical", "Canonical", item.owner_directory, false, "runner"},
			               {"legacy", "Legacy title", item.directory, false, item.host}};
			UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
			const std::string folder_bytes = ChatFolderStore::Serialize(app.folders);
			ChatSession owner = StartupRemoteHistory("runner", "canonical", item.owner_directory, "ses_owned");
			owner.id = "chat-intentional-branch";
			owner.parent_chat_id = "chat-parent";
			owner.branch_root_chat_id = "chat-parent";
			owner.branch_from_message_index = 3;
			owner.created_at = "2026-01-01T00:00:00Z";
			owner.updated_at = owner.created_at;
			owner.messages.push_back({MessageRole::User, "Preserve existing branch"});
			const std::string owner_bytes = uam::StateSerializer::SerializeSession(owner).dump();
			const auto add_owner = [&]()
			{
				if (!disk_only) app.chats.push_back(owner);
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
			};
			if (!publication_race) add_owner();
			ChatSession candidate = StartupRemoteHistory(item.host, "legacy", item.directory, item.native_id);
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			discovery.chats.push_back(candidate);
			discovery.expected_hosts.emplace(item.host, *uam::execution_hosts::Find(app.settings.execution_hosts, item.host));
			discovery.selected_folder_snapshots.emplace("legacy", app.folders.back());
			const ChatHistorySyncService sync;
			std::string tombstone_bytes;
			if (item.tombstoned)
			{
				std::vector<std::string> added;
				UAM_ASSERT(sync.AddNativeImportTombstones(app.data_root, {candidate}, added));
				UAM_ASSERT_EQ(added.size(), std::size_t{1});
				tombstone_bytes = uam::io::ReadTextFile(app.data_root / "native-import-tombstones.json");
			}
			ChatHistorySyncService::ImportResult result = sync.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
			if (publication_race)
			{
				if (discovery.preparation.valid()) discovery.preparation.wait();
				add_owner();
			}
			while (discovery.preparation.valid() || discovery.publication_sync.valid() || !discovery.chats.empty())
			{
				if (discovery.preparation.valid()) discovery.preparation.wait();
				if (discovery.publication_sync.valid()) discovery.publication_sync.wait();
				result.Merge(sync.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly));
			}
			UAM_ASSERT(result.success);
			const int expected_imports = item.duplicate || item.tombstoned ? 0 : 1;
			if (result.imported_count != expected_imports)
				throw std::runtime_error("Remote ownership case " + item.directory + " timing " + std::to_string(owner_timing) + ": expected imports " + std::to_string(expected_imports) + ", got " + std::to_string(result.imported_count));
			UAM_ASSERT_EQ(app.chats.size(), (disk_only ? std::size_t{0} : std::size_t{1}) + (item.duplicate || item.tombstoned ? std::size_t{0} : std::size_t{1}));
			const ChatSession* retained = ChatDomainService().FindChatById(app, owner.id);
			if (!disk_only)
			{
				UAM_ASSERT(retained != nullptr);
				UAM_ASSERT_EQ(uam::StateSerializer::SerializeSession(*retained).dump(), owner_bytes);
			}
			else UAM_ASSERT(retained == nullptr);
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, owner.id);
			UAM_ASSERT(saved.has_value());
			UAM_ASSERT_EQ(uam::StateSerializer::SerializeSession(*saved).dump(), owner_bytes);
			UAM_ASSERT_EQ(ChatFolderStore::Serialize(app.folders), folder_bytes);
			if (item.tombstoned)
				UAM_ASSERT_EQ(uam::io::ReadTextFile(app.data_root / "native-import-tombstones.json"), tombstone_bytes);
		}
	}
	for (const bool registered_alias : {false, true})
	{
		TempDir temp("uam-selected-remote-batch-alias");
		uam::AppState app;
		app.data_root = temp.root / "data";
		ExecutionHost host = StartupRemoteHost("runner");
		host.platform = "linux";
		app.settings.execution_hosts.push_back(host);
		app.folders = {{"canonical", "Canonical", "/srv/Project", false, "runner"}};
		if (registered_alias)
		{
			app.folders.push_back({"dot", "Legacy dot", "/srv/Project/.", false, "runner"});
			app.folders.push_back({"separator", "Legacy separator", "/srv//Project", false, "runner"});
		}
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(StartupRemoteHistory("runner", "dot", "/srv/Project/.", "ses_new"));
		ChatSession second = StartupRemoteHistory("runner", "separator", "/srv//Project", "ses_new");
		second.id = "different-import-id";
		discovery.chats.push_back(second);
		discovery.expected_hosts.emplace(host.id, host);
		const ChatHistorySyncService sync;
		ChatHistorySyncService::LocalHistoryDiscovery chooser;
		chooser.chats = discovery.chats;
		UAM_ASSERT(sync.FilterStartupImportCandidates(app, chooser));
		UAM_ASSERT_EQ(chooser.chats.size(), registered_alias ? std::size_t{1} : std::size_t{0});
		ChatHistorySyncService::ImportResult result;
		while (!discovery.chats.empty() || discovery.preparation.valid() || discovery.publication_sync.valid())
		{
			if (discovery.preparation.valid()) discovery.preparation.wait();
			if (discovery.publication_sync.valid()) discovery.publication_sync.wait();
			result.Merge(sync.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly));
		}
		// Unregistered aliases have no reviewed workspace, so selection refuses rather than guessing one.
		UAM_ASSERT_EQ(result.success, registered_alias);
		if (!registered_alias)
			UAM_ASSERT(std::ranges::find(result.errors, std::string("A selected history has no reviewed workspace; assign it again.")) != result.errors.end());
		UAM_ASSERT_EQ(result.imported_count, registered_alias ? 1 : 0);
		UAM_ASSERT_EQ(app.chats.size(), registered_alias ? std::size_t{1} : std::size_t{0});
		UAM_ASSERT_EQ(app.folders.size(), registered_alias ? std::size_t{3} : std::size_t{1});
	}
}

UAM_TEST(DeletionWaitsForStartupImportPublicationBeforeCapturingTargets)
{
	TempDir temp("uam-delete-startup-import");
	ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	ChatSession existing = StartupHistory(temp.root / "workspace", "existing");
	existing.folder_id = "known";
	app.chats.push_back(existing);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, existing));
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>(
	    FixtureScanner({StartupHistory(temp.root / "workspace", "new-import")}));
	uam::StartupNativeImportService& service = *app.startup_native_import_service;
	service.Initialize(app);
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.candidates.size(), std::size_t{1});
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {ready.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT(service.ImportPending());
	uam::WorkspaceDeletionTask folder_task;
	uam::WorkspaceDeletionTask chat_task;
	UAM_ASSERT(!uam::PrepareWorkspaceDeletion(app, {"known"}, folder_task));
	UAM_ASSERT(!uam::PrepareChatDeletion(app, {existing.id}, chat_task));
	UAM_ASSERT(!fs::exists(app.data_root / "deletion-transaction.json"));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
	uam::WorkspaceDeletionTask settled;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"known"}, settled));
	UAM_ASSERT_EQ(settled.deleted_ids.size(), std::size_t{2});
	UAM_ASSERT(uam::StageWorkspaceDeletion(settled));
	uam::CommitWorkspaceDeletion(app, settled);
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(settled));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChats(app.data_root).empty());
}


UAM_TEST(StartupNativePartialPublicationKeepsFailedCandidatesRetryable)
{
	TempDir temp("uam-startup-partial-retry");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
	uam::StartupNativeImportService service(FixtureScanner({StartupHistory(temp.root / "workspace", "good"),
	    StartupHistory(temp.root / "workspace", "failed")}));
	service.Initialize(app);
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	const fs::path blocked = AppPaths::UamChatFilePath(app.data_root, "failed");
	fs::create_directories(blocked);
	std::ofstream(blocked / "keep") << "private obstruction";
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {}, {}, &error, "known", true));
	WaitForStartup(service, app);
	const uam::StartupNativeImportSnapshot partial = service.Snapshot();
	UAM_ASSERT_EQ(partial.imported_count, 1);
	UAM_ASSERT_EQ(partial.total_count, std::size_t{1});
	UAM_ASSERT_EQ(partial.candidates.front().native_session_id, std::string("failed"));
	UAM_ASSERT(!partial.errors.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChat(app.data_root, "good").has_value());
	fs::remove_all(blocked);
	UAM_ASSERT(service.Decide(app, partial.generation, {partial.candidates.front().candidate_id}, {}, &error));
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(service.Snapshot().imported_count, 2);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
	UAM_ASSERT(ChatRepository::LoadLocalChat(app.data_root, "failed").has_value());
}

UAM_TEST(CodexStreamingImportPreservesLargeSourceAndAllLocalMessages)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-streaming-import");
	ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
	ScopedEnvVar gemini_home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	ScopedEnvVar copilot_home("COPILOT_HOME", (temp.root / "copilot").string());
	ScopedEnvVar claude_home("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	const fs::path rollout = temp.root / "codex" / "sessions" / "rollout-private.jsonl";
	fs::create_directories(rollout.parent_path());
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const std::string id = "11111111-1111-4111-8111-111111111111";
	{
		std::ofstream records(rollout, std::ios::binary);
		records << nlohmann::json{{"type", "session_meta"}, {"payload", {{"id", id}, {"cwd", app.folders.front().directory}}}}.dump() << '\n';
		const std::string ignored = nlohmann::json{{"type", "event_msg"}, {"payload", {{"type", "private_fixture"}, {"text", std::string(65536, 'x')}}}}.dump() + "\n";
		for (int index = 0; index < 1025; ++index) records << ignored;
		for (int index = 0; index < 10002; ++index)
		{
			records << nlohmann::json{{"type", "response_item"}, {"payload", {{"type", "message"},
			    {"role", index == 0 ? "user" : "assistant"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", "private message " + std::to_string(index)}}})}}}}.dump() << '\n';
		}
	}
	UAM_ASSERT(fs::file_size(rollout) > 64U * 1024U * 1024U);
	const ChatHistorySyncService::LocalHistoryDiscovery discovery = ChatHistorySyncService().DiscoverProviderChatsForFolder(app.folders.front());
	UAM_ASSERT(discovery.result.success);
	UAM_ASSERT_EQ(discovery.codex_rollouts.size(), std::size_t{1});
	UAM_ASSERT(discovery.codex_rollouts.front().first.messages.empty());
	UAM_ASSERT(!discovery.codex_rollouts.front().first.messages_loaded);
	for (const int mode : {0, 1, 2})
	{
		app.data_root = temp.root / ("data-" + std::to_string(mode));
		app.chats.clear();
		if (mode == 2)
		{
			uam::StartupNativeImportService service([discovery](const uam::StartupNativeImportScanRequest&, std::stop_token) { return discovery; });
			service.Initialize(app);
			WaitForStartup(service, app);
			const uam::StartupNativeImportSnapshot ready = service.Snapshot();
			UAM_ASSERT_EQ(ready.total_count, std::size_t{1});
			std::string error;
			UAM_ASSERT(service.Decide(app, ready.generation, {}, {}, &error, "known", true));
			WaitForStartup(service, app);
			UAM_ASSERT_EQ(service.Snapshot().imported_count, 1);
			UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
		}
		else
		{
			const ChatHistorySyncService::ImportResult imported = mode == 0
			    ? ChatHistorySyncService().ImportCodexRolloutChatsForFolder(app, "known")
			    : ChatHistorySyncService().ImportDiscoveredProviderChatsForFolder(app, app.folders.front(), discovery, true);
			UAM_ASSERT(imported.success);
			UAM_ASSERT_EQ(imported.imported_count, 1);
		}
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages.size(), std::size_t{10002});
		UAM_ASSERT_EQ(saved->messages.front().content, std::string("private message 0"));
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("private message 10001"));
		const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, id);
		UAM_ASSERT(summary.has_value());
		UAM_ASSERT_EQ(summary->persisted_message_count, std::size_t{10002});
		UAM_ASSERT_EQ(summary->persisted_messages_digest, saved->persisted_messages_digest);
	}

#endif
}

UAM_TEST(StreamedImportedStorageRetainsCompleteContentBeyondOrdinaryJsonBudget)
{
	TempDir temp("uam-streamed-storage-budget");
	ChatSession metadata;
	metadata.id = "private-streamed-import";
	metadata.provider_id = "codex-cli";
	metadata.title = "Complete private import";
	Message message{MessageRole::User, std::string(2 * 1024 * 1024, 'q')};
	const std::size_t count = 130;
	const std::shared_ptr<PreparedChatSave> staged = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
	    [&](const std::function<bool(const Message&)>& emit, ChatSession& final_metadata)
	{
		for (std::size_t index = 0; index < count; ++index)
			if (!emit(message)) return false;
		final_metadata.updated_at = "2026-10-12T20:00:00Z";
		return true;
	});
	UAM_ASSERT(staged != nullptr);
	UAM_ASSERT(metadata.messages.empty());
	UAM_ASSERT(!metadata.messages_loaded);
	UAM_ASSERT_EQ(metadata.persisted_message_count, count);
	UAM_ASSERT(fs::file_size(staged->primary_temp) > 256U * 1024U * 1024U);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, metadata.id)));
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(staged, true, &metadata));
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, metadata.id);
	UAM_ASSERT(summary.has_value());
	UAM_ASSERT_EQ(summary->persisted_message_count, count);
	UAM_ASSERT_EQ(summary->persisted_messages_digest, std::string("2026-10-12T20:00:00Z:130"));
	std::ifstream saved(AppPaths::UamChatFilePath(temp.root, metadata.id), std::ios::binary);
	std::array<char, 65536> buffer;
	std::size_t complete_contents = 0;
	std::size_t run = 0;
	while (saved.read(buffer.data(), buffer.size()) || saved.gcount() > 0)
	{
		for (std::streamsize index = 0; index < saved.gcount(); ++index)
		{
			if (buffer[static_cast<std::size_t>(index)] == 'q') ++run;
			else if (run > 0)
			{
				if (run == message.content.size()) ++complete_contents;
				else UAM_ASSERT(run < 64);
				run = 0;
			}
		}
	}
	UAM_ASSERT_EQ(complete_contents, count);
}

UAM_TEST(StreamedImportedStorageAbortAndRevisionGuardsPreserveExistingFiles)
{
	for (const bool change_primary : {false, true})
	{
		TempDir temp("uam-streamed-storage-guards");
		ChatSession metadata;
		metadata.id = "private-import";
		metadata.title = "Approved metadata";
		const std::shared_ptr<PreparedChatSave> staged = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
		    [&](const std::function<bool(const Message&)>& emit, ChatSession&)
		{
			if (!emit(Message{MessageRole::User, "complete imported message"})) return false;
			if (change_primary)
			{
				ChatSession owner = metadata;
				owner.title = "Newer owned chat";
				owner.messages.push_back({MessageRole::User, "newer private content"});
				UAM_ASSERT(ChatRepository::SaveChat(temp.root, owner));
				return true;
			}
			return false;
		});
		UAM_ASSERT(staged == nullptr);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, metadata.id);
		if (change_primary)
		{
			UAM_ASSERT(saved.has_value());
			UAM_ASSERT_EQ(saved->title, std::string("Newer owned chat"));
			UAM_ASSERT_EQ(saved->messages.front().content, std::string("newer private content"));
		}
		else UAM_ASSERT(!saved.has_value());
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(temp.root))
			UAM_ASSERT(entry.path().filename().string().find(".tmp.") == std::string::npos);
	}
}

UAM_TEST(CodexStreamedCallAssociationsMatchCompleteNativeHydration)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-streamed-call-parity");
	ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	const std::string id = "11111111-1111-4111-8111-111111111111";
	const fs::path rollout = temp.root / "codex" / "sessions" / ("rollout-" + id + ".jsonl");
	fs::create_directories(rollout.parent_path());
	const auto output = [](const std::string& content)
	{
		return nlohmann::json{{"type", "response_item"}, {"timestamp", "2026-10-12T20:00:05Z"},
		    {"payload", {{"type", "function_call_output"}, {"call_id", "call-1"}, {"output", content}}}};
	};
	const std::vector<nlohmann::json> records = {
	    {{"type", "session_meta"}, {"payload", {{"id", id}, {"cwd", app.folders.front().directory}, {"timestamp", "2026-10-12T20:00:00Z"}}}},
	    output("ignored output before its call"),
	    {{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "user"},
	        {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", "Inspect complete UTF8: café\nsecond line"}}})}}}},
	    {{"type", "response_item"}, {"payload", {{"type", "function_call"}, {"name", "spawn_agent"}, {"call_id", "call-1"}, {"arguments", "{}"}}}},
	    output(nlohmann::json{{"agent_id", "22222222-2222-4222-8222-222222222222"}, {"nickname", "private child"}}.dump()),
	    {{"type", "response_item"}, {"payload", {{"type", "function_call"}, {"name", "duplicate ignored"}, {"call_id", "call-1"}, {"arguments", "{}"}}}},
	    {{"type", "response_item"}, {"payload", {{"type", "reasoning"}, {"summary", nlohmann::json::array({{{"type", "summary_text"}, {"text", "Retained thought"}}})}}}},
	    output("last complete output without identity"),
	    {{"type", "response_item"}, {"payload", {{"type", "message"}, {"role", "assistant"},
	        {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", "Complete final response"}}})}}}}
	};
	{
		std::ofstream source(rollout, std::ios::binary);
		for (const nlohmann::json& record : records) source << record.dump() << '\n';
	}
	ChatSession identity;
	identity.native_session_id = id;
	identity.workspace_directory = app.folders.front().directory;
	const std::optional<ChatSession> complete = ChatHistorySyncService().LoadLocalCodexChildChat(identity);
	UAM_ASSERT(complete.has_value());
	const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportCodexRolloutChatsForFolder(app, "known");
	UAM_ASSERT(result.success);
	UAM_ASSERT_EQ(result.imported_count, 1);
	const std::optional<ChatSession> imported = ChatRepository::LoadLocalChat(app.data_root, id);
	UAM_ASSERT(imported.has_value());
	UAM_ASSERT_EQ(imported->messages, complete->messages);
	UAM_ASSERT_EQ(imported->updated_at, complete->updated_at);
	UAM_ASSERT_EQ(imported->title, complete->title);
	UAM_ASSERT_EQ(imported->messages[1].tool_calls.front().sub_agent_title, std::string("private child"));
#endif
}

UAM_TEST(StreamedAtomicWriterDoesNotCloseReusedHandlesAfterSyncHookThrows)
{
	TempDir temp("uam-streamed-atomic-hook");
	const fs::path staged = temp.root / "private.tmp";
	std::ofstream replacement;
	bool rejected = false;
	uam::io::AtomicWriteResult result;
	const uam::io::AtomicTempProducer produce = [](const std::function<bool(std::string_view)>& append)
	{
		return append("complete private content");
	};
	try
	{
		uam::io::WriteAndSyncAtomicTemp(staged, produce, [&](uam::io::AtomicWriteStage stage)
		{
			if (stage == uam::io::AtomicWriteStage::AfterFileSync)
			{
				replacement.open(temp.root / "replacement", std::ios::binary);
				throw std::runtime_error("private hook failure");
			}
			return true;
		}, result);
	}
	catch (const std::runtime_error&) { rejected = true; }
	UAM_ASSERT(rejected);
	replacement << "replacement remains open";
	replacement.flush();
	UAM_ASSERT(replacement.good());
}

UAM_TEST(StartupNativePartialOpenCodeScanRetainsContinuationOffset)
{
	TempDir temp("uam-startup-opencode-offset");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.open_code_history_scan_offset = 3;
	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		UAM_ASSERT_EQ(request.open_code_offset, std::size_t{3});
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.open_code_next_offset = 17;
		discovery.result.Fail("OpenCode history scan stopped before all sessions were read; retry to continue.");
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(app.open_code_history_scan_offset, std::size_t{17});
	UAM_ASSERT(app.chats.empty());
	std::size_t resumed = 0;
	uam::StartupNativeImportService next([&resumed](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		resumed = request.open_code_offset;
		return ChatHistorySyncService::LocalHistoryDiscovery{};
	});
	next.Initialize(app);
	WaitForStartup(next, app);
	UAM_ASSERT_EQ(resumed, std::size_t{17});
}

UAM_TEST(StartupNativeOpenCodeAutomaticallyContinuesEmptyPartialPage)
{
	TempDir temp("uam-startup-opencode-pages");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::size_t calls = 0;
	uam::StartupNativeImportService service([&](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		++calls;
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.open_code_session_count = 3;
		discovery.open_code_list_fingerprint = "stable-fixture-list";
		if (calls == 1)
		{
			UAM_ASSERT(!request.open_code_only);
			discovery.open_code_scanned_count = 1;
			discovery.open_code_next_offset = 1;
		}
		else
		{
			UAM_ASSERT_EQ(calls, std::size_t{2});
			UAM_ASSERT(request.open_code_only && request.remote_scopes.empty());
			UAM_ASSERT_EQ(request.open_code_offset, std::size_t{1});
			UAM_ASSERT(request.open_code_remaining && *request.open_code_remaining == 2);
			UAM_ASSERT_EQ(request.open_code_list_fingerprint, discovery.open_code_list_fingerprint);
			discovery.open_code_scanned_count = 2;
			discovery.open_code_next_offset = 0;
			discovery.chats.push_back(StartupHistory(temp.root / "workspace", "later-page"));
		}
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(calls, std::size_t{2});
	UAM_ASSERT_EQ(service.Snapshot().candidates.size(), std::size_t{1});
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(StartupNativeOpenCodeContinuationStopsAfterFinalWrapAndDeduplicates)
{
	TempDir temp("uam-startup-opencode-wrap");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	app.open_code_history_scan_offset = 3;
	std::size_t calls = 0;
	uam::StartupNativeImportService service([&](const uam::StartupNativeImportScanRequest& request, std::stop_token)
	{
		++calls;
		UAM_ASSERT(calls <= 2);
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.open_code_session_count = 4;
		discovery.open_code_scanned_count = 2;
		discovery.open_code_list_fingerprint = "stable-fixture-list";
		discovery.open_code_next_offset = calls == 1 ? 1 : 3;
		if (calls == 2) UAM_ASSERT(request.open_code_remaining && *request.open_code_remaining == 2);
		discovery.chats.push_back(StartupHistory(temp.root / "workspace", "repeated-candidate"));
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(calls, std::size_t{2});
	UAM_ASSERT_EQ(service.Snapshot().candidates.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.open_code_history_scan_offset, std::size_t{3});
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(StartupNativeOpenCodeContinuationStopsOnChangedList)
{
	TempDir temp("uam-startup-opencode-change");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::size_t calls = 0;
	uam::StartupNativeImportService service([&](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		++calls;
		UAM_ASSERT(calls <= 2);
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.open_code_session_count = 3;
		discovery.open_code_scanned_count = 1;
		discovery.open_code_next_offset = calls;
		discovery.open_code_list_fingerprint = calls == 1 ? "first-list" : "changed-list";
		return discovery;
	});
	service.Initialize(app);
	WaitForStartup(service, app);
	UAM_ASSERT_EQ(calls, std::size_t{2});
	UAM_ASSERT(!service.Snapshot().errors.empty());
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(StartupNativeOpenCodeCancellationDoesNotScheduleContinuation)
{
	TempDir temp("uam-startup-opencode-cancel");
	uam::AppState app;
	StartupWorkspace(app, temp.root);
	std::atomic<std::size_t> calls{0};
	std::promise<void> entered;
	std::promise<void> completed;
	uam::StartupNativeImportService service([&](const uam::StartupNativeImportScanRequest&, std::stop_token stop)
	{
		++calls;
		entered.set_value();
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.open_code_session_count = 3;
		discovery.open_code_scanned_count = 1;
		discovery.open_code_next_offset = 1;
		discovery.open_code_list_fingerprint = "stable-fixture-list";
		completed.set_value();
		return discovery;
	});
	service.Initialize(app);
	UAM_ASSERT(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	service.Cancel();
	UAM_ASSERT(completed.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	service.Poll(app);
	UAM_ASSERT_EQ(calls.load(), std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("cancelled"));
	UAM_ASSERT(app.chats.empty());
}
#endif
