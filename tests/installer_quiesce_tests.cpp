#include "test_harness.h"
#include "app/startup_native_import_service.h"
#include "cef/remote_host_install_guard.h"
#include "common/chat/chat_repository.h"

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

UAM_TEST(InstallerQuiescePauseCancelsRunningHostScanAndRescansLater)
{
	TempDir temp("uam-installer-active-scope");
	uam::AppState app;
	InstallerWorkspace(app, temp.root);
	app.folders.push_back({"second-folder", "Second", "/fixture/second-workspace", false, InstallerHost().id});
	const auto active_entered = std::make_shared<std::promise<void>>();
	const std::future<void> active_started = active_entered->get_future();
	const auto remote_calls = std::make_shared<std::atomic<int>>(0);
	app.startup_native_import_service = std::make_shared<uam::StartupNativeImportService>(
	    [](const uam::StartupNativeImportScanRequest&, std::stop_token) { return ChatHistorySyncService::LocalHistoryDiscovery{}; },
	    [active_entered, remote_calls](const uam::StartupNativeImportRemoteScope& scope, const ProviderProfile*, std::stop_token stop)
	    {
		    ChatHistorySyncService::LocalHistoryDiscovery result;
		    result.chats.push_back(InstallerScopedHistory(scope));
		    if (++*remote_calls != 1) return result;
		    // A long export pass that only ends when its bounded export observes cancellation.
		    active_entered->set_value();
		    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
		    // The interrupted pass reports a distinct partial session that must never be published.
		    result.chats.front().id = result.chats.front().native_session_id = "ses_cancelled_partial";
		    return result;
	    });
	uam::StartupNativeImportService& service = *app.startup_native_import_service;
	service.Initialize(app);
	UAM_ASSERT(active_started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	UAM_ASSERT(service.BackgroundScanPendingOnHost(InstallerHost().id));
	std::shared_ptr<void> pause = service.PauseBackgroundScanOnHost(InstallerHost().id);
	app.settings.execution_hosts[1].runner_status = "installing";
	UAM_ASSERT(!service.BackgroundScanPendingOnHost("another-host"));
	WaitInstallerCondition([&]() { return !service.BackgroundScanPendingOnHost(InstallerHost().id); });
	UAM_ASSERT(!uam::remote_install::HostHasActiveRuntimeWork(app, InstallerHost().id));
	UAM_ASSERT(!service.Poll(app));
	app.settings.execution_hosts[1].runner_status = "ready";
	for (int poll = 0; poll < 20; ++poll) (void)service.Poll(app);
	// The cancelled partial pass is not published or marked complete while the install lease is held.
	UAM_ASSERT_EQ(service.Snapshot().phase, std::string("scanning"));
	UAM_ASSERT_EQ(service.Snapshot().total_count, std::size_t{0});
	UAM_ASSERT_EQ(remote_calls->load(), 1);
	pause.reset();
	WaitInstallerScan(service, app);
	UAM_ASSERT_EQ(remote_calls->load(), 3);
	const uam::StartupNativeImportSnapshot ready = service.Snapshot();
	UAM_ASSERT_EQ(ready.total_count, std::size_t{2});
	UAM_ASSERT(std::ranges::find(ready.candidates, std::string("ses_cancelled_partial"), &uam::StartupNativeImportCandidate::native_session_id) == ready.candidates.end());
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

namespace
{
	// A Claude chat that finished its last turn days ago but still saves helper process authority (#771).
	ChatSession InstallerIdleRetainedChat()
	{
		ChatSession chat = InstallerHistory();
		chat.id = "installer-idle-retained";
		chat.provider_id = uam::provider_ids::kClaudeCli;
		chat.title = "Homelab notes";
		chat.remote_process_exists = true;
		chat.remote_process_control_token = "fixture-retained-control-token";
		chat.remote_structured_protocol = "claude-code-stream-json";
		return chat;
	}

	uam::AcpSessionState& InstallerRetainedSession(uam::AppState& app, const ChatSession& chat)
	{
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = chat.id;
		session->provider_id = chat.provider_id;
		session->process_execution_host_id = chat.execution_host_id;
		session->remote_runner_protocol_version = 3;
		session->remote_output_delivery_token = chat.remote_process_control_token;
		app.acp_sessions.push_back(std::move(session));
		return *app.acp_sessions.back();
	}

	void AssertInstallWaitReason(const uam::AppState& app, const std::string& host_id, uam::remote_install::HostRuntimeBlockReason expected)
	{
		uam::remote_install::HostRuntimeBlockReason reason = uam::remote_install::HostRuntimeBlockReason::VcsLease;
		const bool blocked = uam::remote_install::HostHasRuntimeWork(app, host_id, uam::remote_install::HostWorkScope::InstallWait, &reason);
		UAM_ASSERT(reason == expected);
		UAM_ASSERT(blocked == (expected != uam::remote_install::HostRuntimeBlockReason::None));
	}
}

UAM_TEST(InstallerIdleSavedProcessWithoutSessionDoesNotBlockUpdate)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	// Neither install check refuses; host removal and other strict callers still see the saved authority.
	AssertInstallerBlockReason(app, host, Reason::None, true);
	AssertInstallWaitReason(app, host, Reason::None);
	AssertInstallerBlockReason(app, host, Reason::ChatProcess);
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::Unreachable);
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), "another-host") == uam::remote_install::RetainedProcess::NotRetained);
	// A continued continuation with no live runtime has nothing to continue in (an in-flight turn keeps its own turn markers);
	// one that can still dispatch, or input being saved, blocks.
	app.chats.front().claude_quota_continuation.state = "continued";
	AssertInstallerBlockReason(app, host, Reason::None, true);
	app.chats.front().claude_quota_continuation.state = "claimed";
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	app.chats.front().claude_quota_continuation.state.clear();
	app.pending_chat_save_at_by_chat_id[app.chats.front().id] = 1.0;
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	app.pending_chat_save_at_by_chat_id.clear();

	// Quiescing changes no saved field: recovery settles it against the committed helper.
	UAM_ASSERT_EQ(uam::remote_install::QuiesceRetainedProcessesOnHost(app, host), std::size_t{0});
	UAM_ASSERT(app.chats.front().remote_process_exists);
	UAM_ASSERT_EQ(app.chats.front().remote_process_control_token, std::string("fixture-retained-control-token"));
	AssertInstallWaitReason(app, host, Reason::None);
}

UAM_TEST(InstallerEndlessReattachWithoutLiveProcessDoesNotBlockUpdate)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	uam::AcpSessionState& session = InstallerRetainedSession(app, app.chats.front());
	session.reconnect_pending = true;
	session.recovering_remote_process = true;
	session.reconnect_attempts = 40;
	session.reconnect_not_before_time_s = 1.0e9;
	AssertInstallerBlockReason(app, host, Reason::None, true);
	// The wait first retries the reattach once at once: a process that only lost its connection could still be alive.
	uam::remote_install::ReattachProbes probes;
	UAM_ASSERT_EQ(uam::remote_install::QuiesceRetainedProcessesOnHost(app, host, &probes), std::size_t{0});
	UAM_ASSERT(probes.contains(app.chats.front().id));
	UAM_ASSERT(session.reconnect_not_before_time_s < 1.0e9);
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host, &probes) == uam::remote_install::RetainedProcess::Reattaching);
	UAM_ASSERT(uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, nullptr, nullptr, &probes));
	{
		Reason waiting = Reason::None;
		std::string waiting_id;
		UAM_ASSERT(uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, &waiting, &waiting_id, &probes));
		UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, waiting, waiting_id).find("UAM is reconnecting to the process for") != std::string::npos);
	}
	// A second pass does not retry it again.
	const double retried_at = session.reconnect_not_before_time_s;
	(void)uam::remote_install::QuiesceRetainedProcessesOnHost(app, host, &probes);
	UAM_ASSERT_EQ(session.reconnect_not_before_time_s, retried_at);
	// A retry a launch gate only deferred is not a failure.
	session.reconnect_not_before_time_s = retried_at + 5.0;
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host, &probes) == uam::remote_install::RetainedProcess::Reattaching);
	// Once that attempt fails, the record no longer blocks.
	++session.reconnect_attempts;
	UAM_ASSERT(!uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, nullptr, nullptr, &probes));
	// Without the saved chat flag the same loop used to refuse as session-reconnect.
	app.chats.front().remote_process_exists = false;
	AssertInstallerBlockReason(app, host, Reason::None, true);
	AssertInstallerBlockReason(app, host, Reason::SessionReconnect);
	app.chats.front().remote_process_exists = true;
	// Quiescing leaves the reattach loop and its saved fields to recovery.
	UAM_ASSERT_EQ(app.acp_sessions.size(), std::size_t{1});
	UAM_ASSERT(app.chats.front().remote_process_exists);
	UAM_ASSERT(!uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, nullptr, nullptr, &probes));
	// A runtime that gave up reattaching is set aside without a retry.
	session.reconnect_pending = false;
	AssertInstallWaitReason(app, host, Reason::None);

	// A reattach loop left behind by a deleted chat does not block either.
	app.acp_sessions.clear();
	ChatSession deleted = InstallerIdleRetainedChat();
	deleted.id = "installer-deleted-chat";
	uam::AcpSessionState& orphan = InstallerRetainedSession(app, deleted);
	orphan.reconnect_pending = true;
	AssertInstallerBlockReason(app, host, Reason::None, true);
	AssertInstallWaitReason(app, host, Reason::None);
	AssertInstallerBlockReason(app, host, Reason::ChatProcess);

	// A reattach loop for a turn still blocks.
	app.acp_sessions.clear();
	uam::AcpSessionState& turn = InstallerRetainedSession(app, app.chats.front());
	turn.reconnect_pending = true;
	turn.recovering_remote_turn = true;
	turn.processing = true;
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	AssertInstallWaitReason(app, host, Reason::ChatProcess);
	turn.recovering_remote_turn = false;
	turn.processing = false;
	// An offline usage-limit continuation could come due during the install, so it blocks and is named.
	app.acp_sessions.clear();
	app.chats.front().claude_quota_continuation.state = "waiting";
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	AssertInstallWaitReason(app, host, Reason::ChatProcess);
	Reason reason = Reason::None;
	std::string chat_id;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("usage limit") != std::string::npos);
}

UAM_TEST(InstallerIdleKeptAliveProcessIsStoppedNotRefused)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	uam::AcpSessionState& session = InstallerRetainedSession(app, app.chats.front());
	session.running = true;
	session.session_ready = true;
	session.lifecycle_state = "ready";
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::StopIdle);
	// A goal continuation waiting to run is queued input, so the process is kept and the refusal says so.
	app.chats.front().goal_pending_continuation_id = "fixture-continuation";
	{
		Reason goal = Reason::None;
		std::string goal_id;
		UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &goal, &goal_id));
		UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, goal, goal_id).find("goal continuation waiting") != std::string::npos);
	}
	app.chats.front().goal_pending_continuation_id.clear();
	AssertInstallerBlockReason(app, host, Reason::None, true);
	// The update waits for the confirmed stop before it installs.
	AssertInstallWaitReason(app, host, Reason::ChatProcess);
	// A launch being prepared on a ready process is work, even though idle shutdown would not fire yet.
	session.worktree_launch_pending = true;
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::NotRetained);
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	session.worktree_launch_pending = false;
	// A waiting usage-limit continuation would be cancelled by the stop, so it is kept and named.
	app.chats.front().claude_quota_continuation.state = "waiting";
	Reason reason = Reason::None;
	std::string chat_id;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("usage limit") != std::string::npos);
	app.chats.front().claude_quota_continuation.state.clear();
	// A reattached process still connecting is admitted; the wait holds the install until it is idle or gone,
	// and a wait that runs out says it is connecting rather than naming a turn.
	session.recovering_remote_process = true;
	session.session_ready = false;
	session.initialize_request_id = 3;
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::Connecting);
	AssertInstallerBlockReason(app, host, Reason::None, true);
	UAM_ASSERT(uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("still connecting") != std::string::npos);
	UAM_ASSERT_EQ(uam::remote_install::QuiesceRetainedProcessesOnHost(app, host), std::size_t{0});
	UAM_ASSERT(session.running);
	session.initialize_request_id = 0;
	session.recovering_remote_process = false;
	session.session_ready = true;
	session.claude_live_background_task_ids.insert("fixture-background-task");
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::NotRetained);
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("background tasks") != std::string::npos);
}

UAM_TEST(InstallerRealTurnStillBlocksWithNamedReason)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	uam::AcpSessionState& session = InstallerRetainedSession(app, app.chats.front());
	session.running = true;
	session.processing = true;
	session.prompt_request_id = 7;
	AssertInstallerBlockReason(app, host, Reason::ChatProcess, true);
	AssertInstallWaitReason(app, host, Reason::ChatProcess);
	Reason reason = Reason::None;
	std::string chat_id;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT_EQ(chat_id, app.chats.front().id);
	const std::string message = uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id);
	UAM_ASSERT(message.find("\xE2\x80\x9CHomelab notes\xE2\x80\x9D has a turn in progress on this host. Stop it or let it finish") != std::string::npos);
	UAM_ASSERT(message.find("SSH") == std::string::npos);
	UAM_ASSERT(message.find("(chat-process)") != std::string::npos);
	UAM_ASSERT_EQ(uam::remote_install::QuiesceRetainedProcessesOnHost(app, host), std::size_t{0});
	UAM_ASSERT(session.running);
	UAM_ASSERT(app.chats.front().remote_process_exists);

	// A saved turn marker without a live runtime still blocks and names the turn.
	app.acp_sessions.clear();
	app.chats.front().remote_turn_reconnect_pending = true;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(reason == Reason::ChatProcess);
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("Homelab notes\xE2\x80\x9D has a turn in progress") != std::string::npos);

	// Queued input waiting to launch is named, not silently dropped.
	app.chats.front().remote_turn_reconnect_pending = false;
	app.chats.front().remote_process_exists = false;
	uam::AcpSessionState& queued = InstallerRetainedSession(app, app.chats.front());
	queued.reconnect_pending = true;
	queued.queued_user_prompts.emplace_back();
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(reason == Reason::SessionReconnect);
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("has queued or unsent input for this host") != std::string::npos);
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, Reason::TerminalRunning, chat_id).find("A terminal for \xE2\x80\x9CHomelab notes\xE2\x80\x9D") != std::string::npos);
	app.chats.front().title.clear();
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).starts_with("An untitled chat has queued or unsent input"));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, Reason::CliUpdate, "").find("provider CLI") != std::string::npos);
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, Reason::None, "").empty());
}

UAM_TEST(InstallerQuiesceStopsIdleKeptAliveProcessAndWaitsForIt)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	TempDir temp("uam-installer-idle-stop");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), InstallerHost()};
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	app.chats.front().messages_loaded = true;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	uam::AcpSessionState& session = InstallerRetainedSession(app, app.chats.front());
	session.running = true;
	session.session_ready = true;
	session.lifecycle_state = "ready";
	AssertInstallerBlockReason(app, host, Reason::None, true);
	// The fixture has no helper pipe, so the stop request cannot be confirmed; either way the stop is in flight
	// and the install waits for it instead of proceeding or naming a turn.
	(void)uam::remote_install::QuiesceRetainedProcessesOnHost(app, host);
	UAM_ASSERT(uam::AcpStopInProgress(app, app.chats.front().id));
	Reason reason = Reason::None;
	std::string chat_id;
	UAM_ASSERT(uam::remote_install::HostHasRuntimeWork(app, host, uam::remote_install::HostWorkScope::InstallWait, &reason, &chat_id));
	// Still stopping while the local cleanup runs, or unconfirmed once it ends; both name the chat and keep the update waiting.
	const std::string stopping = uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id);
	UAM_ASSERT(stopping.find("is still stopping on this host") != std::string::npos || stopping.find("could not confirm that") != std::string::npos);
	UAM_ASSERT(app.chats.front().remote_process_exists);
	// A second pass does not send another stop, and an unconfirmed stop keeps blocking rather than being set aside.
	const std::size_t stop_tasks = app.acp_process_stop_tasks.size();
	const std::size_t remote_stops = app.pending_acp_remote_stops.size();
	UAM_ASSERT_EQ(uam::remote_install::QuiesceRetainedProcessesOnHost(app, host), std::size_t{0});
	UAM_ASSERT_EQ(app.acp_process_stop_tasks.size(), stop_tasks);
	UAM_ASSERT_EQ(app.pending_acp_remote_stops.size(), remote_stops);
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::NotRetained);
	AssertInstallerBlockReason(app, host, reason, true);
}

UAM_TEST(InstallerUnconfirmedStopStillBlocksAndReconnectWaitsForInstall)
{
	using Reason = uam::remote_install::HostRuntimeBlockReason;
	uam::AppState app;
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), InstallerHost()};
	const std::string host = InstallerHost().id;
	app.chats.push_back(InstallerIdleRetainedChat());
	app.chats.front().remote_stop_cleanup_pending = true;
	// A stop the helper never confirmed may have left a turn running under the old helper.
	uam::AcpSessionState& session = InstallerRetainedSession(app, app.chats.front());
	session.remote_stop_unconfirmed = true;
	session.recovering_remote_process = true;
	session.reconnect_pending = true;
	UAM_ASSERT(uam::remote_install::ClassifyRetainedProcess(app, app.chats.front(), host) == uam::remote_install::RetainedProcess::NotRetained);
	Reason reason = Reason::None;
	std::string chat_id;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("is still stopping on this host") != std::string::npos);
	AssertInstallWaitReason(app, host, Reason::ChatProcess);

	// Once no retry is pending, the message says how to retry the stop.
	session.reconnect_pending = false;
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("Restart UAM so it retries the stop") != std::string::npos);
	// A saved stop cleanup with no live runtime is still an unconfirmed stop.
	app.acp_sessions.clear();
	UAM_ASSERT(uam::remote_install::HostHasActiveRuntimeWork(app, host, true, &reason, &chat_id));
	UAM_ASSERT(uam::remote_install::HostRuntimeBlockMessage(app, reason, chat_id).find("could not confirm that \xE2\x80\x9CHomelab notes\xE2\x80\x9D stopped") != std::string::npos);
	AssertInstallWaitReason(app, host, Reason::ChatProcess);

	// Reattach waits only while this process owns the host's install, never for a status an interrupted install saved.
	static bool installing = false;
	static bool waiting = false;
	// The same rule the install handler registers: a busy-work wait holds only plain relaunches.
	uam::SetRemoteHelperInstallProbe([](std::string_view id, bool recovery) { return installing && id == InstallerHost().id && (!recovery || !waiting); });
	app.settings.execution_hosts[1].runner_status = "installing";
	app.settings.execution_hosts[1].runner_install_nonce = "interrupted-install";
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), true));
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), false));
	installing = true;
	UAM_ASSERT(uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), true));
	UAM_ASSERT(uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), false));
	waiting = true;
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), true));
	UAM_ASSERT(uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), false));
	waiting = false;
	ChatSession local = app.chats.front();
	local.execution_host_id = std::string(uam::execution_hosts::kLocalHostId);
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(local, false));
	installing = false;
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), false));
	uam::SetRemoteHelperInstallProbe(nullptr);
	UAM_ASSERT(!uam::RemoteReconnectHeldForHelperInstall(app.chats.front(), false));
}
