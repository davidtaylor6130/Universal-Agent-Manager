#include "app/model_discovery_service.h"
#include "app/persistence_coordinator.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/paths/app_paths.h"
#include "common/config/settings_store.h"
#include "common/config/provider_availability.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/paths/workspace_root.h"
#include "test_harness.h"

using namespace uam_test;

namespace
{
	struct StartupDiscoveryFixture
	{
		TempDir temp{"uam-startup-discovery"};
		ScopedEnvVar codex{"CODEX_HOME", (temp.root / "codex").string()};
		ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
		ScopedEnvVar opencode{"XDG_CONFIG_HOME", (temp.root / "config").string()};
		ScopedEnvVar opencode_config{"OPENCODE_CONFIG", (temp.root / "config" / "opencode.json").string()};
		ScopedEnvVar gemini{"GEMINI_CLI_HOME", (temp.root / "gemini").string()};
		ScopedEnvVar copilot{"COPILOT_HOME", (temp.root / "copilot").string()};
		uam::AppState app;
		std::vector<uam::ModelDiscoveryScope> launched;

		StartupDiscoveryFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile(), ProviderProfileStore::DefaultClaudeProfile(), ProviderProfileStore::DefaultAntigravityProfile()};
			app.settings.default_new_chat_provider_id = "codex";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			for (int index = 0; index < 3; ++index)
			{
				const fs::path workspace = temp.root / ("workspace-" + std::to_string(index));
				fs::create_directories(workspace);
				app.folders.push_back(ChatFolder{"folder-" + std::to_string(index), "Workspace", workspace.string(), false});
			}
			app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
			app.runtime_cli_versions_by_provider_id["codex-cli"].supported = true;
			app.runtime_cli_versions_by_provider_id["claude-cli"].checked = true;
			app.runtime_cli_versions_by_provider_id["claude-cli"].supported = false;
			app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
		}

		uam::StartupModelDiscoveryService::LaunchFunction Launcher()
		{
			return [this](uam::AppState& state, const std::string& provider, const std::string& workspace, const std::string& host, std::string*)
			{
				launched.push_back({provider, workspace, host});
				std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
				session->chat_id = "discovery-" + std::to_string(launched.size());
				session->provider_id = provider;
				session->ephemeral_model_discovery = true;
				session->model_discovery_only = true;
				session->running = true;
				state.acp_sessions.push_back(std::move(session));
				return true;
			};
		}
	};
}

UAM_TEST(StartupModelDiscoveryRefreshesFreshCacheOncePerRestart)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.resize(1);
	const std::string workspace = fixture.app.folders.front().directory;
	const nlohmann::json cached = nlohmann::json::array({{{"id", "cached-before-restart"}}});
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("codex-cli", cached, workspace));
	fixture.app.provider_model_catalog->Initialize(fixture.app.data_root, fixture.app.provider_profiles);
	fixture.app.provider_model_catalog->SetExecutionHostContext(fixture.app.settings.execution_hosts);
	uam::StartupModelDiscoveryService discovery(fixture.Launcher());
	discovery.Initialize(fixture.app);
	UAM_ASSERT(discovery.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.provider_model_catalog->GetCachedProviderModels("codex-cli", workspace), cached);
	UAM_ASSERT(fixture.app.provider_model_catalog->IsDiscoveryPending("codex-cli", workspace));
	UAM_ASSERT(!discovery.Poll(fixture.app));
	fixture.app.provider_model_catalog->RememberRefreshFailure("codex-cli", "isolated refresh failure", workspace);
	fixture.app.acp_sessions.clear();
	discovery.Initialize(fixture.app);
	UAM_ASSERT(!discovery.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.app.provider_model_catalog->GetCachedProviderModels("codex-cli", workspace), cached);
	// A settings-requested retry remains available after the one startup attempt.
	UAM_ASSERT(fixture.app.provider_model_catalog->BeginDiscovery("codex-cli", workspace));
}

UAM_TEST(StartupModelDiscoveryDoesNotDuplicateAnExplicitRefresh)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.resize(1);
	const std::string workspace = fixture.app.folders.front().directory;
	UAM_ASSERT(fixture.app.provider_model_catalog->BeginDiscovery("codex-cli", workspace));
	uam::StartupModelDiscoveryService discovery(fixture.Launcher());
	discovery.Initialize(fixture.app);
	UAM_ASSERT(!discovery.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
	fixture.app.provider_model_catalog->RememberRefreshFailure("codex-cli", "explicit refresh failed", workspace);
	UAM_ASSERT(!discovery.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
}

UAM_TEST(StartupModelDiscoveryDeduplicatesRegisteredScopesWithoutProviderFolderProduct)
{
	StartupDiscoveryFixture fixture;
	ChatSession chat;
	chat.id = "existing";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = fixture.app.folders.front().directory;
	fixture.app.chats.push_back(chat);
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "cached"}}}), chat.workspace_directory));
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "claude"}}}), chat.workspace_directory));
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("antigravity-cli", nlohmann::json::array({{{"id", "unsupported"}}}), chat.workspace_directory));
	const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
	UAM_ASSERT_EQ(scopes.size(), std::size_t{4});
	UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id), 1);
	UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("antigravity-cli"), &uam::ModelDiscoveryScope::provider_id), 0);
}

UAM_TEST(StartupModelDiscoveryPrewarmsOnlyInitialNewChatWorkspaceAcrossProviders)
{
	StartupDiscoveryFixture fixture;
	ExecutionHost remote;
	remote.id = "remote";
	remote.transport = "ssh";
	remote.platform = "linux";
	remote.runner_status = "ready";
	fixture.app.settings.execution_hosts.push_back(remote);
	ChatFolder remote_folder{"remote-folder", "Remote", "/srv/private-fixture", false};
	remote_folder.execution_host_id = remote.id;
	fixture.app.folders.push_back(remote_folder);
	UAM_ASSERT(uam::provider_availability::Set(fixture.app.settings, "claude-cli", remote.id, false));
	ChatSession selected;
	selected.id = "selected-fixture-chat";
	selected.provider_id = "codex-cli";
	selected.folder_id = fixture.app.folders[2].id;
	selected.workspace_directory = fixture.app.folders[2].directory;
	fixture.app.chats.push_back(selected);
	fixture.app.selected_chat_index = 0;
	fixture.app.new_chat_folder_id = fixture.app.folders[1].id;
	UAM_ASSERT(fixture.app.provider_model_catalog->GetCachedProviderModels("claude-cli", fixture.app.folders[1].directory).empty());
	const std::function<void(const std::string&)> expect_preferred = [&](const std::string& workspace)
	{
		const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
		UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("codex-cli"), &uam::ModelDiscoveryScope::provider_id), 4);
		UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id), 1);
		UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("antigravity-cli"), &uam::ModelDiscoveryScope::provider_id), 0);
		UAM_ASSERT_EQ(scopes.size(), std::size_t{5});
		const std::vector<uam::ModelDiscoveryScope>::const_iterator claude = std::ranges::find(scopes, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id);
		UAM_ASSERT_EQ(claude->workspace_directory, fs::weakly_canonical(workspace).generic_string());
		UAM_ASSERT_EQ(claude->execution_host_id, std::string("local"));
	};
	expect_preferred(fixture.app.folders[1].directory);
	fixture.app.new_chat_folder_id = "removed-folder";
	expect_preferred(fixture.app.folders[2].directory);
	fixture.app.selected_chat_index = -1;
	expect_preferred(fixture.app.folders.front().directory);
	fixture.app.new_chat_folder_id = remote_folder.id;
	const std::vector<uam::ModelDiscoveryScope> disabled_remote = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
	UAM_ASSERT_EQ(disabled_remote.size(), std::size_t{4});
	UAM_ASSERT_EQ(std::ranges::count(disabled_remote, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id), 0);
	UAM_ASSERT(fixture.launched.empty());
	UAM_ASSERT(fixture.app.acp_sessions.empty());
}

UAM_TEST(StartupModelDiscoveryBoundsConcurrencyAndLeavesExistingTurnsUntouched)
{
	StartupDiscoveryFixture fixture;
	for (const bool processing : {false, true})
	{
		std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = processing ? "active-chat" : "idle-chat";
		session->running = true;
		session->processing = processing;
		session->session_id = "keep-existing-session";
		fixture.app.acp_sessions.push_back(std::move(session));
	}
	uam::StartupModelDiscoveryService service(fixture.Launcher());
	service.Initialize(fixture.app);
	service.Initialize(fixture.app);
	UAM_ASSERT(service.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{2});
	UAM_ASSERT(!service.Poll(fixture.app));
	fixture.app.acp_sessions[2]->running = false;
	UAM_ASSERT(service.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{3});
	UAM_ASSERT(!service.Poll(fixture.app));
	for (int index = 0; index < 2; ++index)
	{
		UAM_ASSERT(fixture.app.acp_sessions[index]->running);
		UAM_ASSERT_EQ(fixture.app.acp_sessions[index]->processing, index == 1);
		UAM_ASSERT_EQ(fixture.app.acp_sessions[index]->session_id, std::string("keep-existing-session"));
	}
}

UAM_TEST(StartupModelDiscoveryDefersOfflineAndVersionChecksWithoutBlockingReadyScopes)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.resize(1);
	ExecutionHost remote;
	remote.id = "remote";
	remote.transport = "ssh";
	remote.platform = "linux";
	remote.runner_status = "offline";
	fixture.app.settings.execution_hosts.push_back(remote);
	fixture.app.provider_model_catalog->SetExecutionHostContext(fixture.app.settings.execution_hosts);
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", "remote")].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", "remote")].supported = true;
	ChatFolder remote_folder{"remote-folder", "Remote", "/srv/project", false};
	remote_folder.execution_host_id = "remote";
	fixture.app.folders.push_back(remote_folder);
	UAM_ASSERT(fixture.app.provider_model_catalog->BeginDiscoveryIfStale("codex-cli", "/srv/project", "remote"));
	fixture.app.provider_model_catalog->RememberRefreshFailure("codex-cli", "", "/srv/project", "remote");
	uam::StartupModelDiscoveryService service(fixture.Launcher());
	service.Initialize(fixture.app);
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].checked = false;
	UAM_ASSERT(!service.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
	UAM_ASSERT(fixture.app.provider_model_catalog->GetProviderRefreshError("codex-cli", fixture.app.folders.front().directory).empty());
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	UAM_ASSERT(service.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.launched.front().execution_host_id, std::string("local"));
	fixture.app.settings.execution_hosts.back().runner_status = "ready";
	UAM_ASSERT(service.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.back().execution_host_id, std::string("remote"));
	UAM_ASSERT(!service.Poll(fixture.app));
}

UAM_TEST(StartupModelDiscoveryKeepsFreshCacheAndAttemptsFailedLaunchOnlyOnce)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.resize(1);
	const std::string workspace = fixture.app.folders.front().directory;
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "cached"}}}), workspace));
	uam::StartupModelDiscoveryService fresh(fixture.Launcher());
	fresh.Initialize(fixture.app);
	UAM_ASSERT(fresh.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{1});
	UAM_ASSERT(!fresh.Poll(fixture.app));
	fixture.app.acp_sessions.clear();
	fixture.app.provider_model_catalog->RememberRefreshFailure("codex-cli", "Fixture launch failure.", workspace);
	fixture.app.folders.front().directory = fixture.temp.root.string();
	int attempts = 0;
	uam::StartupModelDiscoveryService failed([&](uam::AppState&, const std::string&, const std::string&, const std::string&, std::string* error) {
		++attempts;
		*error = "Fixture launch failure.";
		return false;
	});
	failed.Initialize(fixture.app);
	UAM_ASSERT(failed.Poll(fixture.app));
	UAM_ASSERT(!failed.Poll(fixture.app));
	UAM_ASSERT_EQ(attempts, 1);
	UAM_ASSERT_EQ(fixture.app.provider_model_catalog->GetCachedProviderModels("codex-cli", workspace)[0].value("id", ""), std::string("cached"));
	UAM_ASSERT(!fixture.app.provider_model_catalog->IsDiscoveryPending("codex-cli", fixture.temp.root.string()));
}

UAM_TEST(StartupModelDiscoveryDiscardsRemovedOrChangedHostOwnership)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.resize(1);
	uam::StartupModelDiscoveryService service(fixture.Launcher());
	service.Initialize(fixture.app);
	fixture.app.settings.execution_hosts.front().runner_directory = "changed-connection";
	UAM_ASSERT(!service.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
	fixture.app.settings.execution_hosts.clear();
	UAM_ASSERT(!service.Poll(fixture.app));
}

UAM_TEST(ModelDiscoveryCompatibilityRetriesNeverTakeOverExistingActiveOrIdleChats)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_COPILOT_CLI
	for (const bool processing : {false, true})
	{
		StartupDiscoveryFixture fixture;
		const fs::path fake = fixture.temp.root / "copilot-cli-fixture";
		fs::create_directories(fake);
		const fs::path executable = fake / "copilot";
		UAM_ASSERT(uam::io::WriteTextFile(executable, "#!/bin/sh\ncat >/dev/null\n"));
		fs::permissions(executable, fs::perms::owner_exec, fs::perm_options::add);
		ScopedEnvVar search_path("PATH", fake.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
		fixture.app.provider_profiles.push_back(ProviderProfileStore::DefaultCopilotProfile());
		ChatSession chat;
		chat.id = processing ? "active" : "idle";
		chat.provider_id = "copilot-cli";
		chat.workspace_directory = fixture.app.folders.front().directory;
		fixture.app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> current = std::make_unique<uam::AcpSessionState>();
		current->chat_id = chat.id;
		current->provider_id = chat.provider_id;
		current->running = true;
		current->session_ready = true;
		current->processing = processing;
		current->session_id = "existing-session";
		uam::AcpSessionState* existing = current.get();
		fixture.app.acp_sessions.push_back(std::move(current));
		fixture.app.runtime_cli_versions_by_provider_id[chat.provider_id] = {};
		UAM_ASSERT(fixture.app.provider_model_catalog->BeginDiscovery(chat.provider_id, chat.workspace_directory));
		UAM_ASSERT(uam::QueueAcpModelDiscoveryCompatibilityRetry(fixture.app, chat.id, chat.provider_id, chat.workspace_directory, "local", "Checking compatibility."));
		UAM_ASSERT(fixture.app.pending_model_discovery_retries.back().chat_id.empty());
		UAM_ASSERT(fixture.app.provider_model_catalog->GetProviderRefreshError(chat.provider_id, chat.workspace_directory).empty());
		// Previously queued requests can still contain the caller's conversation ID.
		fixture.app.pending_model_discovery_retries.back().chat_id = chat.id;
		fixture.app.runtime_cli_versions_by_provider_id[chat.provider_id].checked = true;
		fixture.app.runtime_cli_versions_by_provider_id[chat.provider_id].supported = true;
		struct DiscoveryCleanup
		{
			uam::AppState& app;
			~DiscoveryCleanup()
			{
				for (const std::unique_ptr<uam::AcpSessionState>& session : app.acp_sessions)
					if (session->ephemeral_model_discovery)
					{
						PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
						PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
						session->running = false;
					}
			}
		} cleanup{fixture.app};
		UAM_ASSERT(uam::RetryCompatibilityBlockedAcpModelDiscoveries(fixture.app));
		UAM_ASSERT(existing->running);
		UAM_ASSERT_EQ(existing->processing, processing);
		UAM_ASSERT_EQ(existing->session_id, std::string("existing-session"));
		UAM_ASSERT(fixture.app.acp_sessions.back()->ephemeral_model_discovery);
		UAM_ASSERT(fixture.app.acp_sessions.back()->chat_id != chat.id);
		UAM_ASSERT(fixture.app.pending_model_discovery_retries.empty());
	}
#endif
}

UAM_TEST(StartupModelDiscoveryUsesWorktreeRootAndRejectsChangedWorkspaceOwnership)
{
	for (int change = 0; change < 4; ++change)
	{
		StartupDiscoveryFixture fixture;
		const fs::path base = fixture.app.folders.front().directory;
		const fs::path worktree = fixture.temp.root / "isolated-worktree";
		fs::create_directories(worktree);
		fixture.app.folders.clear();
		ChatSession chat;
		chat.id = "worktree-chat";
		chat.provider_id = "codex-cli";
		chat.workspace_directory = base.string();
		chat.workspace_worktree_directory = worktree.string();
		chat.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		fixture.app.chats.push_back(chat);
		const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
		UAM_ASSERT_EQ(scopes.size(), std::size_t{1});
		UAM_ASSERT_EQ(scopes.front().workspace_directory, fs::weakly_canonical(worktree).generic_string());
		uam::StartupModelDiscoveryService service(fixture.Launcher());
		service.Initialize(fixture.app);
		if (change == 1) fixture.app.chats.front().workspace_worktree_directory = base.string();
		if (change == 2) fixture.app.chats.clear();
		if (change == 3)
		{
			fixture.app.chats.front().workspace_isolation_kind.clear();
			fixture.app.chats.front().workspace_directory = fixture.temp.root.string();
		}
		UAM_ASSERT_EQ(service.Poll(fixture.app), change == 0);
		UAM_ASSERT_EQ(fixture.launched.size(), change == 0 ? std::size_t{1} : std::size_t{0});
	}
}

UAM_TEST(StartupModelDiscoveryDeduplicatesWindowsRemoteCaseWithoutFoldingLinuxPaths)
{
	for (const std::string& platform : {"windows", "linux"})
	{
		StartupDiscoveryFixture fixture;
		fixture.app.folders.clear();
		ExecutionHost remote;
		remote.id = "remote";
		remote.transport = "ssh";
		remote.platform = platform;
		fixture.app.settings.execution_hosts.push_back(remote);
		const std::vector<std::string> directories = platform == "windows" ? std::vector<std::string>{"C:/Project", "c:/project"} : std::vector<std::string>{"/srv/Project", "/srv/project"};
		for (const std::string& directory : directories)
		{
			ChatFolder folder{directory, "Remote", directory, false};
			folder.execution_host_id = "remote";
			fixture.app.folders.push_back(folder);
		}
		const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
		UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("codex-cli"), &uam::ModelDiscoveryScope::provider_id), platform == "windows" ? 1 : 2);
		UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id), 1);
		UAM_ASSERT_EQ(scopes.size(), platform == "windows" ? std::size_t{2} : std::size_t{3});
	}
}

UAM_TEST(StartupModelDiscoverySkipsUnavailableCliAndNeverPreemptsVersionCheck)
{
	StartupDiscoveryFixture fixture;
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].supported = false;
	uam::StartupModelDiscoveryService unavailable(fixture.Launcher());
	unavailable.Initialize(fixture.app);
	UAM_ASSERT(!unavailable.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
	UAM_ASSERT(fixture.app.provider_model_catalog->GetProviderRefreshError("codex-cli", fixture.app.folders.front().directory).empty());
	fixture.app.runtime_cli_versions_by_provider_id.clear();
	fixture.app.runtime_cli_version_check_task.running = true;
	uam::StartupModelDiscoveryService unknown(fixture.Launcher());
	unknown.Initialize(fixture.app);
	UAM_ASSERT(!unknown.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
	UAM_ASSERT(fixture.app.runtime_cli_version_check_task.running);
	UAM_ASSERT(fixture.app.runtime_cli_versions_by_provider_id.empty());
	fixture.app.runtime_cli_version_check_task.running = false;
}

UAM_TEST(StartupModelDiscoveryPreservesRemoteLinuxLiteralBackslashesInScopeAndLaunch)
{
	StartupDiscoveryFixture fixture;
	fixture.app.folders.clear();
	ExecutionHost host;
	host.id = "literal-path-host";
	host.transport = "ssh";
	host.platform = "linux";
	host.runner_status = "ready";
	fixture.app.settings.execution_hosts.push_back(host);
	fixture.app.provider_model_catalog->SetExecutionHostContext(fixture.app.settings.execution_hosts);
	const std::vector<std::string> paths = {"/srv/Project\\literal", "/srv/Project/literal"};
	for (std::size_t index = 0; index < paths.size(); ++index)
	{
		ChatFolder folder{"literal-" + std::to_string(index), "Remote", paths[index], false};
		folder.execution_host_id = host.id;
		fixture.app.folders.push_back(folder);
	}
	const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
	UAM_ASSERT_EQ(scopes.size(), std::size_t{3});
	UAM_ASSERT_EQ(std::ranges::count(scopes, std::string("claude-cli"), &uam::ModelDiscoveryScope::provider_id), 1);
	UAM_ASSERT_EQ(scopes[0].workspace_directory, paths[0]);
	UAM_ASSERT_EQ(scopes[1].workspace_directory, paths[1]);
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)].supported = true;
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("claude-cli", host.id)].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("claude-cli", host.id)].supported = false;
	uam::StartupModelDiscoveryService service(fixture.Launcher());
	service.Initialize(fixture.app);
	UAM_ASSERT(service.Poll(fixture.app));
	UAM_ASSERT_EQ(fixture.launched.size(), std::size_t{2});
	UAM_ASSERT_EQ(fixture.launched[0].workspace_directory, paths[0]);
	UAM_ASSERT_EQ(fixture.launched[1].workspace_directory, paths[1]);
}

UAM_TEST(ModelCatalogSeparatesRemotePosixLiteralBackslashesAndPreservesWindowsAliases)
{
	StartupDiscoveryFixture fixture;
	uam::ProviderModelCatalogService& catalog = *fixture.app.provider_model_catalog;
	ExecutionHost remote;
	remote.id = "remote-a";
	remote.transport = "ssh";
	remote.ssh_alias = "fixture-remote";
	ExecutionHost windows = remote;
	windows.id = "windows-host";
	windows.platform = "windows";
	const std::vector<ExecutionHost> hosts{remote, windows};
	catalog.SetExecutionHostContext(hosts);
	const std::string literal = "/srv/Project\\literal";
	const std::string nested = "/srv/Project/literal";
	UAM_ASSERT(catalog.BeginDiscovery("codex-cli", literal, "remote-a"));
	UAM_ASSERT(!catalog.IsDiscoveryPending("codex-cli", nested, "remote-a"));
	UAM_ASSERT(catalog.BeginDiscovery("codex-cli", nested, "remote-a"));
	UAM_ASSERT(catalog.RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "literal-model"}}}), literal, nlohmann::json::array(), "remote-a"));
	UAM_ASSERT(catalog.IsDiscoveryPending("codex-cli", nested, "remote-a"));
	UAM_ASSERT(catalog.RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "nested-model"}}}), nested, nlohmann::json::array(), "remote-a"));
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels("codex-cli", literal, "remote-a")[0].value("id", ""), std::string("literal-model"));
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels("codex-cli", nested, "remote-a")[0].value("id", ""), std::string("nested-model"));
	UAM_ASSERT(catalog.GetCachedProviderModels("codex-cli", literal, "remote-b").empty());
	for (const std::pair<std::string, std::string>& paths : {
	    std::pair<std::string, std::string>{"C:\\Project\\", "C:/Project"},
	    std::pair<std::string, std::string>{"\\\\server\\share\\Project", "//server/share/Project"}})
	{
		UAM_ASSERT(catalog.RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "windows-model"}}}), paths.first, nlohmann::json::array(), "windows-host"));
		UAM_ASSERT_EQ(catalog.GetCachedProviderModels("codex-cli", paths.second, "windows-host")[0].value("id", ""), std::string("windows-model"));
	}
	// Reload persisted scope keys rather than relying only on the in-memory cache.
	uam::ProviderModelCatalogService reloaded;
	reloaded.Initialize(fixture.app.data_root, fixture.app.provider_profiles);
	reloaded.SetExecutionHostContext(hosts);
	UAM_ASSERT_EQ(reloaded.GetCachedProviderModels("codex-cli", literal, "remote-a")[0].value("id", ""), std::string("literal-model"));
	UAM_ASSERT_EQ(reloaded.GetCachedProviderModels("codex-cli", nested, "remote-a")[0].value("id", ""), std::string("nested-model"));
}

namespace
{
	ExecutionHost CatalogRemoteHost(const std::string& alias)
	{
		ExecutionHost host;
		host.id = "ssh-lab-a";
		host.transport = "ssh";
		host.ssh_alias = alias;
		host.platform = "linux";
		host.runner_status = "ready";
		return host;
	}
}

UAM_TEST(ModelCatalogSettingsSaveRebindsReusedRemoteHostIdentity)
{
	StartupDiscoveryFixture fixture;
	PersistenceCoordinator persistence;
	fixture.app.settings.execution_hosts.push_back(CatalogRemoteHost("lab.a"));
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	uam::ProviderModelCatalogService& catalog = *fixture.app.provider_model_catalog;
	const nlohmann::json models = nlohmann::json::array({{{"id", "old-server"}}});
	const nlohmann::json options = nlohmann::json::array({{{"id", "old-config"}}});
	UAM_ASSERT(catalog.RememberSuccessfulModels("codex-cli", models, "/project", options, "ssh-lab-a"));
	fixture.app.settings.execution_hosts.back().label = "Renamed label";
	fixture.app.settings.execution_hosts.back().runner_status = "offline";
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	UAM_ASSERT_EQ(catalog.GetCachedProviderModels("codex-cli", "/project", "ssh-lab-a"), models);
	fixture.app.settings.execution_hosts.pop_back();
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	fixture.app.settings.execution_hosts.push_back(CatalogRemoteHost("lab-a"));
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	UAM_ASSERT(catalog.GetCachedProviderModels("codex-cli", "/project", "ssh-lab-a").empty());
	UAM_ASSERT(catalog.GetCachedProviderConfigOptions("codex-cli", "/project", "ssh-lab-a").empty());
	UAM_ASSERT(catalog.BeginDiscoveryIfStale("codex-cli", "/project", "ssh-lab-a"));
	const nlohmann::json replacement_models = nlohmann::json::array({{{"id", "new-server"}}});
	UAM_ASSERT(catalog.RememberSuccessfulModels("codex-cli", replacement_models, "/project", options, "ssh-lab-a"));
	uam::ProviderModelCatalogService reloaded;
	reloaded.Initialize(fixture.app.data_root, fixture.app.provider_profiles);
	reloaded.SetExecutionHostContext(fixture.app.settings.execution_hosts);
	UAM_ASSERT_EQ(reloaded.GetCachedProviderModels("codex-cli", "/project", "ssh-lab-a"), replacement_models);
	UAM_ASSERT_EQ(reloaded.GetCachedProviderConfigOptions("codex-cli", "/project", "ssh-lab-a"), options);
	UAM_ASSERT(!reloaded.BeginDiscoveryIfStale("codex-cli", "/project", "ssh-lab-a"));
	UAM_ASSERT(reloaded.CaptureScopeKey("codex-cli", "/project", "unknown-host").empty());
	UAM_ASSERT(!reloaded.BeginDiscovery("codex-cli", "/project", "unknown-host"));
	UAM_ASSERT(!reloaded.RememberSuccessfulModels("codex-cli", models, "/project", options, "unknown-host"));

}

UAM_TEST(ModelCatalogSettingsLoadRejectsPersistedDifferentRemoteConnection)
{
	StartupDiscoveryFixture fixture;
	PersistenceCoordinator persistence;
	fixture.app.settings.execution_hosts.push_back(CatalogRemoteHost("lab.a"));
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("codex-cli", nlohmann::json::array({{{"id", "old-server"}}}), "/project", nlohmann::json::array({{{"id", "old-config"}}}), "ssh-lab-a"));
	// Simulate the pre-fingerprint on-disk format, retaining valid model metadata.
	const fs::path cache_file = fixture.app.data_root / "provider_model_catalog_cache.json";
	nlohmann::json legacy = nlohmann::json::parse(uam::io::ReadTextFile(cache_file));
	const std::string owned_key = fixture.app.provider_model_catalog->CaptureScopeKey("codex-cli", "/project", "ssh-lab-a");
	const std::size_t connection_suffix = owned_key.find("-connection-");
	if (connection_suffix != std::string::npos)
	{
		legacy["catalogs"][owned_key.substr(0, connection_suffix)] = legacy["catalogs"][owned_key];
		legacy["catalogs"].erase(owned_key);
		legacy["catalogs"][""] = legacy["catalogs"][owned_key.substr(0, connection_suffix)];
		const fs::path legacy_root = fixture.temp.root / "legacy-data";
		fs::create_directories(legacy_root);
		UAM_ASSERT(uam::io::WriteTextFile(legacy_root / "provider_model_catalog_cache.json", legacy.dump()));
		uam::ProviderModelCatalogService legacy_catalog;
		legacy_catalog.Initialize(legacy_root, fixture.app.provider_profiles);
		legacy_catalog.SetExecutionHostContext(fixture.app.settings.execution_hosts);
		UAM_ASSERT(legacy_catalog.GetCachedProviderModels("codex-cli", "/project", "unknown-host").empty());
		UAM_ASSERT(legacy_catalog.GetCachedProviderConfigOptions("codex-cli", "/project", "unknown-host").empty());
		UAM_ASSERT(legacy_catalog.GetCatalogSnapshot("codex-cli", "/project", "unknown-host")["availableModels"].empty());
		UAM_ASSERT(legacy_catalog.GetCachedProviderModels("codex-cli", "/project", "ssh-lab-a").empty());
		UAM_ASSERT(legacy_catalog.GetCachedProviderConfigOptions("codex-cli", "/project", "ssh-lab-a").empty());
	}
	AppSettings replacement = fixture.app.settings;
	replacement.execution_hosts.back() = CatalogRemoteHost("lab-a");
	UAM_ASSERT(SettingsStore::Save(AppPaths::SettingsFilePath(fixture.app.data_root), replacement));
	fixture.app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	fixture.app.provider_model_catalog->Initialize(fixture.app.data_root, fixture.app.provider_profiles);
	UAM_ASSERT(persistence.LoadSettings(fixture.app));
	UAM_ASSERT_EQ(fixture.app.settings.execution_hosts.back().ssh_alias, std::string("lab-a"));
	// A catalog without connection metadata belongs to neither configured server.
	UAM_ASSERT(fixture.app.provider_model_catalog->CaptureScopeKey("codex-cli", "/project", "ssh-lab-a") != std::string{});

	UAM_ASSERT(fixture.app.provider_model_catalog->GetCachedProviderModels("codex-cli", "/project", "ssh-lab-a").empty());
	UAM_ASSERT(fixture.app.provider_model_catalog->GetCachedProviderConfigOptions("codex-cli", "/project", "ssh-lab-a").empty());
}

UAM_TEST(ModelCatalogLateClaudeResponseCannotRepublishRemovedRemoteHost)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	StartupDiscoveryFixture fixture;
	PersistenceCoordinator persistence;
	fixture.app.settings.execution_hosts.push_back(CatalogRemoteHost("lab.a"));
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	ChatSession chat;
	chat.id = "discovery-old-host";
	chat.provider_id = "claude-cli";
	chat.workspace_directory = "/project";
	chat.execution_host_id = "ssh-lab-a";
	fixture.app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.protocol_kind = "claude-code-stream-json";
	// Simulate launch metadata; responses still pass through the real Claude projection.
	session.model_catalog_scope_key = fixture.app.provider_model_catalog->CaptureScopeKey(chat.provider_id, chat.workspace_directory, chat.execution_host_id);
	session.initialize_request_id = 7;
	session.pending_request_methods[7] = "initialize";
	fixture.app.settings.execution_hosts.pop_back();
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, session, fixture.app.chats.front(), R"({"type":"control_response","response":{"subtype":"success","request_id":"7","response":{"models":[{"value":"obsolete-server-model","displayName":"Old server"}]}}})"));
	UAM_ASSERT_EQ(session.available_models.size(), std::size_t{1});
	UAM_ASSERT(fixture.app.provider_model_catalog->GetCachedProviderModels("claude-cli", "/project", "ssh-lab-a").empty());
	fixture.app.settings.execution_hosts.push_back(CatalogRemoteHost("lab-a"));
	UAM_ASSERT(persistence.SaveSettings(fixture.app));
	uam::ProviderModelCatalogService& catalog = *fixture.app.provider_model_catalog;
	UAM_ASSERT(catalog.BeginDiscovery("claude-cli", "/project", "ssh-lab-a"));
	session.initialized = false;
	session.initialize_request_id = 8;
	session.pending_request_methods[8] = "initialize";
	UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, session, fixture.app.chats.front(), R"({"type":"control_response","response":{"subtype":"success","request_id":"8","response":{"models":[{"value":"obsolete-server-model","displayName":"Old server"}]}}})"));
	UAM_ASSERT(catalog.GetCachedProviderModels("claude-cli", "/project", "ssh-lab-a").empty());
	UAM_ASSERT(catalog.IsDiscoveryPending("claude-cli", "/project", "ssh-lab-a"));
	session.model_discovery_only = true;
	session.initialize_request_id = 9;
	session.pending_request_methods[9] = "initialize";
	UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, session, fixture.app.chats.front(), R"({"type":"control_response","response":{"subtype":"error","request_id":"9","error":"Old connection failed"}})"));
	UAM_ASSERT(catalog.IsDiscoveryPending("claude-cli", "/project", "ssh-lab-a"));
	UAM_ASSERT(catalog.GetProviderRefreshError("claude-cli", "/project", "ssh-lab-a").empty());
	UAM_ASSERT(!catalog.RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "missing-owner"}}}), "/project", {}, "ssh-lab-a", std::string_view{}));
	std::unique_ptr<uam::AcpSessionState> running = std::make_unique<uam::AcpSessionState>();
	running->chat_id = chat.id;
	running->provider_id = chat.provider_id;
	running->running = true;
	running->session_ready = true;
	running->model_catalog_scope_key = session.model_catalog_scope_key;
	running->available_models = session.available_models;
	fixture.app.acp_sessions.push_back(std::move(running));
	std::string error;
	UAM_ASSERT(!uam::StartAcpModelDiscovery(fixture.app, chat.id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fixture.app.acp_sessions.front()->running);
	UAM_ASSERT(catalog.IsDiscoveryPending("claude-cli", "/project", "ssh-lab-a"));
	// A current producer may settle that pending request; a later local/profile
	// reinitialization must still reject results owned by its previous scope.
	const std::string current = catalog.CaptureScopeKey("claude-cli", "/project", "ssh-lab-a");
	UAM_ASSERT(catalog.RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "new-server"}}}), "/project", {}, "ssh-lab-a", current));
	UAM_ASSERT(!catalog.IsDiscoveryPending("claude-cli", "/project", "ssh-lab-a"));
	const std::string local_scope = catalog.CaptureScopeKey("claude-cli", fixture.app.folders.front().directory);
	catalog.Initialize(fixture.app.data_root, fixture.app.provider_profiles, "changed-provider-flags");
	UAM_ASSERT(catalog.BeginDiscovery("claude-cli", fixture.app.folders.front().directory));
	UAM_ASSERT(!catalog.RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "old-local"}}}), fixture.app.folders.front().directory, {}, "local", local_scope));
	catalog.RememberRefreshFailure("claude-cli", "old-local-error", fixture.app.folders.front().directory, "local", local_scope);
	UAM_ASSERT(catalog.IsDiscoveryPending("claude-cli", fixture.app.folders.front().directory));
	UAM_ASSERT(catalog.GetProviderRefreshError("claude-cli", fixture.app.folders.front().directory).empty());

#endif
}

UAM_TEST(OpenCodeExplicitConfigurationRefreshRetainsCacheOnFailureAndRetries)
{
	TempDir temp("uam-config-refresh-retry");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar config_override("OPENCODE_CONFIG", "");
	ScopedEnvVar disable_zen("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	const fs::path config = temp.root / "opencode" / "opencode.json";
	UAM_ASSERT(uam::io::WriteTextFile(config, R"({"model":"cached/original"})"));
	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data");
	const auto consume = [&catalog]()
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (catalog.Poll()) return true;
			std::this_thread::yield();
		}
		return false;
	};
	UAM_ASSERT(uam::io::WriteTextFile(config, "malformed configuration"));
	UAM_ASSERT(catalog.StartConfiguredOpenCodeRefresh());
	UAM_ASSERT(consume());
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("cached/original"));
	UAM_ASSERT(fs::remove(config));
	UAM_ASSERT(fs::create_directory(config));
	UAM_ASSERT(catalog.StartConfiguredOpenCodeRefresh());
	UAM_ASSERT(consume());
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("cached/original"));
	UAM_ASSERT(fs::remove(config));
	UAM_ASSERT(uam::io::WriteTextFile(config, R"({"model":"explicit/reloaded"})"));
	UAM_ASSERT(catalog.StartConfiguredOpenCodeRefresh());
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("cached/original"));
	UAM_ASSERT(consume());
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("explicit/reloaded"));
	UAM_ASSERT(catalog.StartConfiguredOpenCodeRefresh());
	const fs::path second = temp.root / "second.json";
	UAM_ASSERT(uam::io::WriteTextFile(second, R"({"model":"restart/current"})"));
	ScopedEnvVar replacement("OPENCODE_CONFIG", second.string());
	catalog.Initialize(temp.root / "second-data");
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("restart/current"));
	UAM_ASSERT(!catalog.Poll());
	UAM_ASSERT(uam::io::WriteTextFile(second, "malformed new owner"));
	catalog.Initialize(temp.root / "third-data");
	UAM_ASSERT(catalog.GetConfiguredOpenCodeDefaultModel().empty());
}
