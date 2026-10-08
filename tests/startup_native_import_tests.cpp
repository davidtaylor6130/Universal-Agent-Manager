#include "test_harness.h"
#include "app/startup_native_import_service.h"

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
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (service.Snapshot().phase == "scanning" || service.ImportPending())
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			service.Poll(app);
			std::this_thread::yield();
		}
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
