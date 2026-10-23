#include "test_harness.h"
#include "app/startup_native_import_service.h"
#include "cef/remote_host_install_guard.h"

#include <future>
#include <thread>

using namespace uam_test;

namespace
{
	ExecutionHost InstallerHost()
	{
		ExecutionHost host;
		host.id = "installer-fixture";
		host.transport = "ssh";
		host.ssh_alias = "never-connect-installer-fixture";
		host.platform = "linux";
		host.runner_status = "ready";
		host.runner_version = "fixture-version";
		host.runner_protocol_version = 3;
		host.runner_directory = "/fixture/old-runner";
		return host;
	}

	void InstallerWorkspace(uam::AppState& app, const fs::path& root)
	{
		app.data_root = root / "data";
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), InstallerHost()};
		app.folders.push_back({"installer-folder", "Fixture", "/fixture/workspace", false, InstallerHost().id});
	}

	ChatSession InstallerHistory()
	{
		ChatSession chat;
		chat.id = "ses_installer_candidate";
		chat.native_session_id = chat.id;
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = InstallerHost().id;
		chat.folder_id = "installer-folder";
		chat.workspace_directory = "/fixture/workspace";
		chat.title = "Fixture history";
		chat.messages.push_back(Message{MessageRole::User, "Private fixture history"});
		return chat;
	}

	void WaitInstallerScan(uam::StartupNativeImportService& service, uam::AppState& app)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (service.Snapshot().phase == "scanning")
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			service.Poll(app);
			std::this_thread::yield();
		}
	}

	uam::AcpSessionState& InstallerDiscovery(uam::AppState& app)
	{
		ChatSession chat;
		chat.id = "model-discovery-installer-fixture";
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = InstallerHost().id;
		chat.remote_process_exists = true;
		chat.remote_process_control_token = "fixture-owned-control-token";
		app.model_discovery_chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = chat.provider_id;
		session->process_execution_host_id = chat.execution_host_id;
		session->ephemeral_model_discovery = true;
		session->model_discovery_only = true;
		session->running = true;
		session->remote_runner_protocol_version = 3;
		session->remote_output_delivery_token = chat.remote_process_control_token;
		app.acp_sessions.push_back(std::move(session));
		return *app.acp_sessions.back();
	}
	template <typename Predicate>
	void WaitInstallerCondition(Predicate condition)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!condition())
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			std::this_thread::yield();
		}
	}

	struct InstallerBarrierRelease
	{
		std::shared_ptr<std::promise<void>> signal = std::make_shared<std::promise<void>>();
		std::shared_future<void> wait = signal->get_future().share();
		~InstallerBarrierRelease() { Release(); }
		void Release() { try { signal->set_value(); } catch (const std::future_error&) {} }
	};

	ChatSession InstallerScopedHistory(const uam::StartupNativeImportRemoteScope& scope)
	{
		ChatSession chat = InstallerHistory();
		chat.id = chat.native_session_id = "ses_" + scope.folder.id;
		chat.execution_host_id = scope.host.id;
		chat.folder_id = scope.folder.id;
		chat.workspace_directory = scope.folder.directory;
		return chat;
	}

}

namespace
{
	void AssertInstallerBlockReason(const uam::AppState& app, const std::string& host_id,
	    uam::remote_install::HostRuntimeBlockReason expected, bool allow_quiesce = false)
	{
		using Reason = uam::remote_install::HostRuntimeBlockReason;
		Reason reason = Reason::VcsLease;
		const bool blocked = uam::remote_install::HostHasActiveRuntimeWork(app, host_id, allow_quiesce, &reason);
		UAM_ASSERT(blocked == uam::remote_install::HostHasActiveRuntimeWork(app, host_id, allow_quiesce));
		UAM_ASSERT(reason == expected);
		UAM_ASSERT(blocked == (reason != Reason::None));
	}
}

UAM_TEST(InstallerQuiesceAdmitsOnlyOwnedHiddenDiscovery)
{
	uam::AppState app;
	InstallerDiscovery(app);
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
}

UAM_TEST(InstallerQuiesceNeverStopsVisibleOrGoalWork)
{
	for (const bool visible : {false, true})
	{
		uam::AppState app;
		uam::AcpSessionState& session = InstallerDiscovery(app);
		if (visible) app.chats.push_back(app.model_discovery_chats.front());
		else session.goal_internal_session = true;
		UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
		UAM_ASSERT(session.running);
		UAM_ASSERT(!app.model_discovery_chats.front().remote_process_control_token.empty());
	}
}

UAM_TEST(InstallerQuiesceRetainsUnconfirmedHiddenAuthority)
{
	uam::AppState app;
	uam::AcpSessionState& session = InstallerDiscovery(app);
	session.running = false;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	session.remote_stop_unconfirmed = true;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
}

UAM_TEST(InstallerQuiesceBlocksHostCliMaintenance)
{
	for (int kind = 0; kind < 4; ++kind)
	{
		uam::AppState app;
		const ExecutionHost host = InstallerHost();
		if (kind == 0)
		{
			app.runtime_cli_version_check_task.running = true;
			app.runtime_cli_version_check_task.execution_host = host;
		}
		else if (kind == 1)
		{
			app.runtime_cli_install_tasks["fixture"].running = true;
			app.runtime_cli_install_tasks["fixture"].execution_host = host;
		}
		else if (kind == 2) app.pending_cli_updates["fixture"].execution_host = host;
		else
		{
			app.runtime_cli_versions_by_provider_id["fixture"].execution_host = host;
			app.runtime_cli_versions_by_provider_id["fixture"].last_install_status = "awaiting-verification";
		}
		UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host.id, true));
		UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
		using Reason = uam::remote_install::HostRuntimeBlockReason;
		const Reason reasons[] = {Reason::CliVersionCheck, Reason::CliInstall, Reason::CliUpdate, Reason::CliVerification};
		AssertInstallerBlockReason(app, host.id, reasons[kind], true);
		AssertInstallerBlockReason(app, "another-host", Reason::None, true);
	}
}

UAM_TEST(InstallerQuiesceWaitsForBackgroundHistoryScan)
{
	TempDir temp("uam-installer-history-scan");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	std::shared_ptr<std::promise<void>> release = std::make_shared<std::promise<void>>();
	const std::shared_future<void> wait = release->get_future().share();
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>([wait](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		wait.wait();
		return ChatHistorySyncService::LocalHistoryDiscovery{};
	});
	app.startup_native_import_service->Initialize(app);
	const bool blocks = uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id);
	const bool can_wait = !uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true);
	uam::remote_install::HostRuntimeBlockReason scan_reason, quiesce_reason;
	const bool diagnosed_blocks = uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, false, &scan_reason);
	const bool diagnosed_wait = !uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true, &quiesce_reason);
	release->set_value();
	WaitInstallerScan(*app.startup_native_import_service, app);
	UAM_ASSERT(blocks);
	UAM_ASSERT(can_wait);
	UAM_ASSERT(diagnosed_blocks == blocks);
	UAM_ASSERT(diagnosed_wait == can_wait);
	UAM_ASSERT(scan_reason == uam::remote_install::HostRuntimeBlockReason::BackgroundScan);
	UAM_ASSERT(quiesce_reason == uam::remote_install::HostRuntimeBlockReason::None);
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
}

UAM_TEST(InstallerQuiesceBlocksExplicitSelectedImports)
{
	TempDir temp("uam-installer-selected-import");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>([](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(InstallerHistory());
		return discovery;
	});
	app.startup_native_import_service->Initialize(app);
	WaitInstallerScan(*app.startup_native_import_service, app);
	const uam::StartupNativeImportSnapshot snapshot = app.startup_native_import_service->Snapshot();
	UAM_ASSERT_EQ(snapshot.total_count, std::size_t{1});
	std::string error;
	UAM_ASSERT(app.startup_native_import_service->Decide(app, snapshot.generation, {snapshot.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
	UAM_ASSERT(app.startup_native_import_service->ImportPending());
	AssertInstallerBlockReason(app, InstallerHost().id, uam::remote_install::HostRuntimeBlockReason::SelectedImport, true);
	AssertInstallerBlockReason(app, "another-host", uam::remote_install::HostRuntimeBlockReason::None, true);
}

UAM_TEST(InstallerQuiesceRejectsNewSelectionWithoutLosingCandidates)
{
	TempDir temp("uam-installer-preserve-candidates");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(InstallerHistory());
		return discovery;
	});
	service.Initialize(app);
	WaitInstallerScan(service, app);
	const uam::StartupNativeImportSnapshot before = service.Snapshot();
	UAM_ASSERT_EQ(before.total_count, std::size_t{1});
	app.settings.execution_hosts.back().runner_status = "installing";
	std::string error;
	UAM_ASSERT(!service.Decide(app, before.generation, {before.candidates.front().candidate_id}, {}, &error));
	UAM_ASSERT_EQ(service.Snapshot().total_count, before.total_count);
	UAM_ASSERT(!service.ImportPending());
	app.settings.execution_hosts.back().runner_status = "ready";
	UAM_ASSERT(service.Decide(app, before.generation, {before.candidates.front().candidate_id}, {}, &error));
}

UAM_TEST(InstallerQuiesceRequiresExactHiddenProcessOwnership)
{
	for (int kind = 0; kind < 4; ++kind)
	{
		uam::AppState app;
		uam::AcpSessionState& session = InstallerDiscovery(app);
		if (kind == 0) session.remote_output_delivery_token = "another-owner";
		else if (kind == 1) session.managed_agent_run_id = "agent-run";
		else if (kind == 2) app.model_discovery_chats.front().messages.push_back(Message{MessageRole::User, "Preserve user work"});
		else session.queued_prompt = "Preserve queued input";
		UAM_ASSERT(!uam::remote_install::IsQuiesceableDiscovery(app, session, InstallerHost().id));
		UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	}
}

UAM_TEST(InstallerQuiesceWaitsForObservedProcessStop)
{
	uam::AppState app;
	uam::AsyncAcpProcessStopTask stop;
	stop.execution_host_id = InstallerHost().id;
	stop.finished = std::make_shared<std::atomic<bool>>(false);
	stop.result = std::make_shared<uam::platform::ObservedProcessStopResult>();
	app.acp_process_stop_tasks.push_back(std::move(stop));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	app.acp_process_stop_tasks.front().finished->store(true);
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	app.acp_process_stop_tasks.front().result->exit_confirmed = true;
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
}

UAM_TEST(InstallerQuiesceRollbackPreservesUnrelatedSettings)
{
	AppSettings settings;
	const ExecutionHost previous = InstallerHost();
	ExecutionHost reserved = previous;
	reserved.runner_status = "installing";
	ExecutionHost other = previous;
	other.id = "other-host";
	other.label = "New unrelated setting";
	settings.execution_hosts = {reserved, other};
	settings.default_new_chat_provider_id = "claude-cli";
	UAM_ASSERT_EQ(settings.execution_hosts.front().runner_directory, previous.runner_directory);
	UAM_ASSERT(uam::remote_install::RestoreReservedHost(settings.execution_hosts, reserved, previous));
	UAM_ASSERT(settings.execution_hosts.front() == previous);
	UAM_ASSERT(settings.execution_hosts.back() == other);
	UAM_ASSERT_EQ(settings.default_new_chat_provider_id, std::string("claude-cli"));
	UAM_ASSERT(!uam::remote_install::RestoreReservedHost(settings.execution_hosts, reserved, previous));
}

UAM_TEST(InstallerQuiesceRollbackRefusesNewerHostAndRemovesOnlyNewReservation)
{
	const ExecutionHost previous = InstallerHost();
	ExecutionHost reserved = previous;
	reserved.runner_status = "installing";
	ExecutionHost changed = reserved;
	changed.runner_directory = "/fixture/newer-runner";
	std::vector<ExecutionHost> hosts{changed};
	UAM_ASSERT(!uam::remote_install::RestoreReservedHost(hosts, reserved, previous));
	UAM_ASSERT(hosts.front() == changed);
	ExecutionHost new_host = reserved;
	new_host.id = "new-reservation";
	hosts.push_back(new_host);
	UAM_ASSERT(uam::remote_install::RestoreReservedHost(hosts, new_host, std::nullopt));
	UAM_ASSERT_EQ(hosts.size(), std::size_t{1});
	UAM_ASSERT(hosts.front() == changed);
}

UAM_TEST(InstallerQuiescePreservesInstallingCandidatesDuringOtherHostSelection)
{
	TempDir temp("uam-installer-other-host-selection");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	ExecutionHost other = InstallerHost();
	other.id = "other-host";
	other.ssh_alias = "never-connect-other-fixture";
	app.settings.execution_hosts.push_back(other);
	app.folders.push_back({"other-folder", "Other", "/fixture/other-workspace", false, other.id});
	uam::StartupNativeImportService service([](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(InstallerHistory());
		ChatSession other_chat = InstallerHistory();
		other_chat.id = other_chat.native_session_id = "ses_other_installer_candidate";
		other_chat.execution_host_id = "other-host";
		other_chat.folder_id = "other-folder";
		other_chat.workspace_directory = "/fixture/other-workspace";
		discovery.chats.push_back(std::move(other_chat));
		return discovery;
	});
	service.Initialize(app);
	WaitInstallerScan(service, app);
	const uam::StartupNativeImportSnapshot snapshot = service.Snapshot();
	UAM_ASSERT_EQ(snapshot.total_count, std::size_t{2});
	app.settings.execution_hosts[1].runner_status = "installing";
	std::string error;
	const auto selected = std::ranges::find(snapshot.candidates, other.id, &uam::StartupNativeImportCandidate::execution_host_id);
	UAM_ASSERT(selected != snapshot.candidates.end());
	UAM_ASSERT(service.Decide(app, snapshot.generation, {selected->candidate_id}, {}, &error));
	const uam::StartupNativeImportSnapshot pending = service.Snapshot();
	UAM_ASSERT_EQ(pending.phase, std::string("importing"));
	UAM_ASSERT_EQ(pending.total_count, std::size_t{1});
	UAM_ASSERT_EQ(pending.candidates.front().execution_host_id, InstallerHost().id);
	UAM_ASSERT_EQ(pending.candidates.front().native_session_id, InstallerHistory().native_session_id);
	UAM_ASSERT(service.SelectedImportPendingOnHost(other.id));
	UAM_ASSERT(!service.SelectedImportPendingOnHost(InstallerHost().id));
}

UAM_TEST(InstallerQuiesceRejectsDisconnectedDiscoveryImmediately)
{
	uam::AppState app;
	uam::AcpSessionState& session = InstallerDiscovery(app);
	session.running = false;
	UAM_ASSERT(!uam::remote_install::IsQuiesceableDiscovery(app, session, InstallerHost().id));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
}

UAM_TEST(InstallerQuiesceScopesPendingStopsWithoutChatMetadata)
{
	uam::AppState app;
	uam::AcpSessionState& session = InstallerDiscovery(app);
	session.running = false;
	app.model_discovery_chats.clear();
	std::unique_ptr<uam::PendingAcpRemoteStop> pending = std::make_unique<uam::PendingAcpRemoteStop>();
	pending->chat_id = session.chat_id;
	app.pending_acp_remote_stops.push_back(std::move(pending));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
	app.acp_sessions.clear();
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, "another-host", true));
	app.pending_acp_remote_stops.clear();
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
}

UAM_TEST(InstallerQuiesceRetainsCompletedScanUntilHostReady)
{
	TempDir temp("uam-installer-completed-scan");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	std::shared_ptr<std::promise<void>> release = std::make_shared<std::promise<void>>();
	const std::shared_future<void> wait = release->get_future().share();
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>([wait](const uam::StartupNativeImportScanRequest&, std::stop_token)
	{
		wait.wait();
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(InstallerHistory());
		return discovery;
	});
	uam::StartupNativeImportService& service = *app.startup_native_import_service;
	service.Initialize(app);
	app.settings.execution_hosts[1].runner_status = "installing";
	release->set_value();
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (service.BackgroundScanPendingOnHost(InstallerHost().id))
	{
		UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
		std::this_thread::yield();
	}
	UAM_ASSERT(!service.Poll(app));
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("scanning"));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	app.settings.execution_hosts[1].runner_status = "ready";
	WaitInstallerScan(service, app);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
	UAM_ASSERT_EQ(service.Snapshot().candidates.front().native_session_id, InstallerHistory().native_session_id);
}

UAM_TEST(InstallerQuiesceLocalScanCannotBlockOrStartPausedHost)
{
	TempDir temp("uam-installer-scope-admission");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	ExecutionHost other = InstallerHost();
	other.id = "other-host";
	other.ssh_alias = "never-connect-other-fixture";
	app.settings.execution_hosts.push_back(other);
	app.folders.push_back({"other-folder", "Other", "/fixture/other-workspace", false, other.id});
	InstallerBarrierRelease local_release;
	InstallerBarrierRelease other_release;
	const auto local_entered = std::make_shared<std::promise<void>>();
	const std::future<void> local_started = local_entered->get_future();
	const auto other_entered = std::make_shared<std::promise<void>>();
	const std::future<void> other_started = other_entered->get_future();
	const auto local_calls = std::make_shared<std::atomic<int>>(0);
	const auto target_calls = std::make_shared<std::atomic<int>>(0);
	const auto refreshed_version = std::make_shared<std::atomic<bool>>(false);
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>(
	    [wait = local_release.wait, local_entered, local_calls](const uam::StartupNativeImportScanRequest&, std::stop_token)
	    {
		    ++*local_calls;
		    local_entered->set_value();
		    wait.wait();
		    return ChatHistorySyncService::LocalHistoryDiscovery{};
	    },
	    [wait = other_release.wait, other_entered, target_calls, refreshed_version](const uam::StartupNativeImportRemoteScope& scope, const ProviderProfile*, std::stop_token)
	    {
		    if (scope.host.id == InstallerHost().id)
		    {
			    ++*target_calls;
			    refreshed_version->store(scope.host.runner_version == "fixture-version-102");
		    }
		    else
		    {
			    other_entered->set_value();
			    wait.wait();
		    }
		    ChatHistorySyncService::LocalHistoryDiscovery result;
		    result.chats.push_back(InstallerScopedHistory(scope));
		    return result;
	    });
	uam::StartupNativeImportService& service = *app.startup_native_import_service;
	service.Initialize(app);
	UAM_ASSERT(local_started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	std::shared_ptr<void> outer_pause = service.PauseBackgroundScanOnHost(InstallerHost().id);
	UAM_ASSERT(outer_pause != nullptr);
	app.settings.execution_hosts[1].runner_status = "installing";
	UAM_ASSERT(!service.BackgroundScanPendingOnHost(InstallerHost().id));
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	local_release.Release();
	UAM_ASSERT(other_started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	UAM_ASSERT_EQ(target_calls->load(), 0);
	UAM_ASSERT(!service.BackgroundScanPendingOnHost(InstallerHost().id));
	UAM_ASSERT(service.BackgroundScanPendingOnHost(other.id));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, other.id));
	std::shared_ptr<void> inner_pause = service.PauseBackgroundScanOnHost(InstallerHost().id);
	outer_pause.reset();
	app.settings.execution_hosts[1].runner_status = "ready";
	app.settings.execution_hosts[1].runner_version = "fixture-version-102";
	other_release.Release();
	WaitInstallerCondition([&]()
	{
		service.Poll(app);
		return service.Snapshot().total_count == 1;
	});
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("scanning"));
	UAM_ASSERT_EQ(target_calls->load(), 0);
	UAM_ASSERT_EQ(local_calls->load(), 1);
	inner_pause.reset();
	WaitInstallerScan(service, app);
	UAM_ASSERT_EQ(target_calls->load(), 1);
	UAM_ASSERT(refreshed_version->load());
	UAM_ASSERT_EQ(local_calls->load(), 1);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
	const auto selected = std::ranges::find(ready.candidates, InstallerHost().id, &uam::StartupNativeImportCandidate::execution_host_id);
	UAM_ASSERT(selected != ready.candidates.end());
	std::string error;
	UAM_ASSERT(service.Decide(app, ready.generation, {selected->candidate_id}, {}, &error));
	UAM_ASSERT(service.SelectedImportPendingOnHost(InstallerHost().id));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id, true));
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
}

UAM_TEST(InstallerQuiesceActiveHostFinishesBeforeQueuedScopeRetry)
{
	TempDir temp("uam-installer-active-scope");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	app.folders.push_back({"second-folder", "Second", "/fixture/second-workspace", false, InstallerHost().id});
	InstallerBarrierRelease active_release;
	const auto active_entered = std::make_shared<std::promise<void>>();
	const std::future<void> active_started = active_entered->get_future();
	const auto remote_calls = std::make_shared<std::atomic<int>>(0);
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>(
	    [](const uam::StartupNativeImportScanRequest&, std::stop_token) { return ChatHistorySyncService::LocalHistoryDiscovery{}; },
	    [wait = active_release.wait, active_entered, remote_calls](const uam::StartupNativeImportRemoteScope& scope, const ProviderProfile*, std::stop_token)
	    {
		    if (++*remote_calls == 1)
		    {
			    active_entered->set_value();
			    wait.wait();
		    }
		    ChatHistorySyncService::LocalHistoryDiscovery result;
		    result.chats.push_back(InstallerScopedHistory(scope));
		    return result;
	    });
	uam::StartupNativeImportService& service = *app.startup_native_import_service;
	service.Initialize(app);
	UAM_ASSERT(active_started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	std::shared_ptr<void> pause = service.PauseBackgroundScanOnHost(InstallerHost().id);
	app.settings.execution_hosts[1].runner_status = "installing";
	UAM_ASSERT(service.BackgroundScanPendingOnHost(InstallerHost().id));
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	UAM_ASSERT(!service.BackgroundScanPendingOnHost("another-host"));
	active_release.Release();
	WaitInstallerCondition([&]() { return !service.BackgroundScanPendingOnHost(InstallerHost().id); });
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	UAM_ASSERT(!service.Poll(app));
	app.settings.execution_hosts[1].runner_status = "ready";
	WaitInstallerCondition([&]()
	{
		service.Poll(app);
		return service.Snapshot().total_count == 1;
	});
	UAM_ASSERT_EQ(remote_calls->load(), 1);
	pause.reset();
	WaitInstallerScan(service, app);
	UAM_ASSERT_EQ(remote_calls->load(), 2);
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{2});
}

UAM_TEST(InstallerQuiesceDeferredScanRejectsChangedConnection)
{
	for (int changed = 0; changed < 5; ++changed)
	{
		TempDir temp("uam-installer-deferred-owner");
		uam::AppState app;
		InstallerWorkspace(app, temp.root);
		ExecutionHost other = InstallerHost();
		other.id = "other-host";
		other.ssh_alias = "never-connect-other-fixture";
		app.settings.execution_hosts.push_back(other);
		app.folders.push_back({"other-folder", "Other", "/fixture/other-workspace", false, other.id});
		InstallerBarrierRelease local_release;
		const auto local_entered = std::make_shared<std::promise<void>>();
		const std::future<void> local_started = local_entered->get_future();
		const auto target_calls = std::make_shared<std::atomic<int>>(0);
		const auto other_calls = std::make_shared<std::atomic<int>>(0);
		uam::StartupNativeImportService service(
		    [wait = local_release.wait, local_entered](const uam::StartupNativeImportScanRequest&, std::stop_token)
		    {
			    local_entered->set_value();
			    wait.wait();
			    return ChatHistorySyncService::LocalHistoryDiscovery{};
		    },
		    [target_calls, other_calls](const uam::StartupNativeImportRemoteScope& scope, const ProviderProfile*, std::stop_token)
		    {
			    if (scope.host.id == InstallerHost().id) ++*target_calls;
			    else ++*other_calls;
			    ChatHistorySyncService::LocalHistoryDiscovery result;
			    result.chats.push_back(InstallerScopedHistory(scope));
			    return result;
		    });
		service.Initialize(app);
		UAM_ASSERT(local_started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		std::shared_ptr<void> pause = service.PauseBackgroundScanOnHost(InstallerHost().id);
		if (changed == 0) app.settings.execution_hosts[1].ssh_alias = "another-connection";
		else if (changed == 1) app.settings.execution_hosts[1].transport = "local";
		else if (changed == 2) app.settings.execution_hosts[1].platform = "windows";
		else if (changed == 3) app.settings.execution_hosts[1].runner_directory = "/another-runner";
		else app.folders.front().directory = "/another-workspace";
		pause.reset();
		local_release.Release();
		WaitInstallerScan(service, app);
		UAM_ASSERT_EQ(target_calls->load(), 0);
		UAM_ASSERT_EQ(other_calls->load(), 1);
		UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{1});
		UAM_ASSERT_EQ(service.Snapshot().candidates.front().execution_host_id, other.id);
	}
}

UAM_TEST(InstallerBlockReasonPreservesFirstMatchAndHostScope)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	ChatSession chat = InstallerHistory();
	chat.remote_stop_cleanup_pending = true;
	app.chats.push_back(chat);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->process_execution_host_id = host;
	session->remote_stop_unconfirmed = true;
	app.acp_sessions.push_back(std::move(session));
	std::shared_ptr<void> lease = std::make_shared<int>(1);
	app.remote_vcs_operation_leases_by_host_id[host] = lease;
	AssertInstallerBlockReason(app, host, Reason::VcsLease, true);
	AssertInstallerBlockReason(app, "another-host", Reason::None, true);
	lease.reset();
	AssertInstallerBlockReason(app, host, Reason::ChatStopCleanup, true);
	app.chats.front().remote_process_exists = true;
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	app.chats.front().remote_process_exists = false;
	app.chats.front().remote_stop_cleanup_pending = false;
	AssertInstallerBlockReason(app, host, Reason::SessionUnconfirmedStop, true);
	app.acp_sessions.front()->remote_stop_pending = true;
	AssertInstallerBlockReason(app, host, Reason::SessionRemoteStop, true);
	app.acp_sessions.front()->running = true;
	AssertInstallerBlockReason(app, host, Reason::SessionRunning, true);
	app.acp_sessions.front()->running = false;
	app.acp_sessions.front()->remote_stop_pending = false;
	app.acp_sessions.front()->remote_stop_unconfirmed = false;
	app.acp_sessions.front()->reconnect_pending = true;
	AssertInstallerBlockReason(app, host, Reason::SessionReconnect, true);
	app.acp_sessions.front()->reconnect_pending = false;
	AssertInstallerBlockReason(app, host, Reason::None, true);
}

UAM_TEST(InstallerBlockReasonRetainsQuiesceAndUnknownStopAuthority)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	uam::AcpSessionState& session = InstallerDiscovery(app);
	const std::string host = InstallerHost().id;
	AssertInstallerBlockReason(app, host, Reason::None, true);
	AssertInstallerBlockReason(app, host, Reason::ChatProcess);
	// Stopped is insufficient: the retained hidden process authority cannot quiesce.
	session.running = false;
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	app.model_discovery_chats.clear();
	auto stop = std::make_unique<uam::PendingAcpRemoteStop>();
	stop->chat_id = session.chat_id;
	app.pending_acp_remote_stops.push_back(std::move(stop));
	AssertInstallerBlockReason(app, host, Reason::PendingRemoteStop, true);
	AssertInstallerBlockReason(app, "another-host", Reason::None, true);
	app.acp_sessions.clear();
	AssertInstallerBlockReason(app, host, Reason::UnknownStopOwner, true);
	AssertInstallerBlockReason(app, "another-host", Reason::UnknownStopOwner, true);
	app.pending_acp_remote_stops.clear();
	AssertInstallerBlockReason(app, host, Reason::None, true);
}

UAM_TEST(InstallerBlockReasonDistinguishesPhysicalCleanupAndTerminal)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerHistory());
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = app.chats.front().id;
	terminal->should_launch = true;
	app.cli_terminals.push_back(std::move(terminal));
	uam::AsyncAcpProcessStopTask stop;
	stop.execution_host_id = host;
	stop.finished = std::make_shared<std::atomic<bool>>(false);
	stop.result = std::make_shared<uam::platform::ObservedProcessStopResult>();
	app.acp_process_stop_tasks.push_back(std::move(stop));
	AssertInstallerBlockReason(app, host, Reason::PhysicalStop, true);
	AssertInstallerBlockReason(app, "another-host", Reason::None, true);
	app.acp_process_stop_tasks.front().finished->store(true);
	AssertInstallerBlockReason(app, host, Reason::PhysicalStop, true);
	app.acp_process_stop_tasks.front().result->exit_confirmed = true;
	AssertInstallerBlockReason(app, host, Reason::TerminalLaunch, true);
	app.cli_terminals.front()->running = true;
	AssertInstallerBlockReason(app, host, Reason::TerminalRunning, true);
	app.cli_terminals.front()->running = false;
	app.cli_terminals.front()->should_launch = false;
	AssertInstallerBlockReason(app, host, Reason::None, true);
}
