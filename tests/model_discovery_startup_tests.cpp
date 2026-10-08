#include "app/model_discovery_service.h"
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
	UAM_ASSERT(!fresh.Poll(fixture.app));
	UAM_ASSERT(fixture.launched.empty());
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
		UAM_ASSERT_EQ(scopes.size(), platform == "windows" ? std::size_t{1} : std::size_t{2});
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
	const std::vector<std::string> paths = {"/srv/Project\\literal", "/srv/Project/literal"};
	for (std::size_t index = 0; index < paths.size(); ++index)
	{
		ChatFolder folder{"literal-" + std::to_string(index), "Remote", paths[index], false};
		folder.execution_host_id = host.id;
		fixture.app.folders.push_back(folder);
	}
	const std::vector<uam::ModelDiscoveryScope> scopes = uam::StartupModelDiscoveryService::CollectScopes(fixture.app);
	UAM_ASSERT_EQ(scopes.size(), std::size_t{2});
	UAM_ASSERT_EQ(scopes[0].workspace_directory, paths[0]);
	UAM_ASSERT_EQ(scopes[1].workspace_directory, paths[1]);
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)].supported = true;
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
	UAM_ASSERT_EQ(reloaded.GetCachedProviderModels("codex-cli", literal, "remote-a")[0].value("id", ""), std::string("literal-model"));
	UAM_ASSERT_EQ(reloaded.GetCachedProviderModels("codex-cli", nested, "remote-a")[0].value("id", ""), std::string("nested-model"));
}
