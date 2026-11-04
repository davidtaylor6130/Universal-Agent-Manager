#include "test_harness.h"
#include "app/repository_route_match_service.h"

using namespace uam_test;

UAM_TEST(RepositoryRoutesCompareFullIdentityAndProtectSecrets)
{
	const std::optional<std::string> ssh = uam::NormalizeRepositoryRouteIdentity("git@GitHub.com:Owner/Repo.git", true);
	UAM_ASSERT(ssh.has_value());
	UAM_ASSERT_EQ(*ssh, std::string("github.com/Owner/Repo"));
	UAM_ASSERT_EQ(ssh, uam::NormalizeRepositoryRouteIdentity("https://github.com/Owner/Repo/", true));
	UAM_ASSERT(ssh != uam::NormalizeRepositoryRouteIdentity("https://github.com/Other/Repo", true));
	UAM_ASSERT(ssh != uam::NormalizeRepositoryRouteIdentity("ssh://git@github.com:2222/Owner/Repo", true));
	UAM_ASSERT(!uam::NormalizeRepositoryRouteIdentity("https://token@github.com/Owner/Repo", true));
	UAM_ASSERT(!uam::NormalizeRepositoryRouteIdentity("https://user:secret@github.com/Owner/Repo", true));
	UAM_ASSERT(!uam::NormalizeRepositoryRouteIdentity("https://github.com/Owner/Repo\nwarning", true));
	UAM_ASSERT(!uam::NormalizeRepositoryRouteIdentity("file:///tmp/Repo", true));
	UAM_ASSERT(!uam::NormalizeRepositoryRouteIdentity("https://github.com/Owner/../Repo", true));
}

UAM_TEST(RepositoryRoutesRejectAmbiguityAndPreserveManualOverrides)
{
	const uam::RepositoryRouteIdentity identity{"git", "github.com/Owner/Repo", "src"};
	const uam::RepositoryRouteWorkspace source{"local-folder", "local", "/local/repo/src", "local-identity", identity};
	std::vector<uam::RepositoryRouteWorkspace> inventory{source, {"remote-folder", "ssh", "/remote/repo/src", "host-identity", identity}};
	ExecutionRoutingSettings settings;
	ExecutionRoutingHostPolicy policy;
	policy.host_id = "ssh";
	settings.hosts.push_back(policy);
	std::vector<uam::RepositoryRouteMatch> matches = uam::MatchRepositoryRoutes(source, inventory, settings);
	UAM_ASSERT_EQ(matches.size(), std::size_t(1));
	UAM_ASSERT_EQ(matches.front().destination_folder_id, std::string("remote-folder"));
	inventory.push_back({"duplicate", "ssh", "/remote/second/src", "host-identity", identity});
	UAM_ASSERT(uam::MatchRepositoryRoutes(source, inventory, settings).empty());
	settings.hosts.front().workspace_mappings.push_back({source.folder_id, source.directory, "duplicate"});
	matches = uam::MatchRepositoryRoutes(source, inventory, settings);
	UAM_ASSERT_EQ(matches.size(), std::size_t(1));
	UAM_ASSERT(matches.front().manual);
	UAM_ASSERT_EQ(matches.front().destination_folder_id, std::string("duplicate"));
	settings.hosts.front().workspace_mappings.clear();
	inventory.pop_back();
	inventory.back().repository->relative_workspace = "other";
	UAM_ASSERT(uam::MatchRepositoryRoutes(source, inventory, settings).empty());
}

UAM_TEST(RepositoryRoutesRejectStaleHostAndFolderSnapshots)
{
	const std::vector<uam::RepositoryRouteWorkspace> captured{{"a", "local", "/a", "local", {}}, {"b", "ssh", "/b", "host-1", {}}};
	std::vector<uam::RepositoryRouteWorkspace> current = captured;
	UAM_ASSERT(uam::RepositoryRouteSnapshotMatches(captured, current));
	current.back().connection_identity = "host-2";
	UAM_ASSERT(!uam::RepositoryRouteSnapshotMatches(captured, current));
	current = captured;
	current.front().directory = "/other";
	UAM_ASSERT(!uam::RepositoryRouteSnapshotMatches(captured, current));
}

UAM_TEST(RepositoryRoutesKeepSvnBranchesDistinctAndReadOnly)
{
	std::vector<std::vector<std::string>> commands;
	const uam::RepositoryRouteReader trunk = [&](const std::vector<std::string>& argv, std::string& output)
	{
		commands.push_back(argv);
		if (argv.front() == "git") return false;
		output = argv[4] == "repos-root-url" ? "https://svn.example.com/project" : "https://svn.example.com/project/trunk";
		return true;
	};
	const std::optional<uam::RepositoryRouteIdentity> identity = uam::ReadRepositoryRouteIdentity(trunk);
	UAM_ASSERT(identity.has_value());
	UAM_ASSERT_EQ(identity->identity, std::string("svn.example.com/project"));
	UAM_ASSERT_EQ(identity->relative_workspace, std::string("trunk"));
	UAM_ASSERT_EQ(commands.size(), std::size_t(3));
	const uam::RepositoryRouteWorkspace source{"local", "local", "/trunk", "local", identity};
	const std::vector<uam::RepositoryRouteWorkspace> inventory{source, {"branch", "ssh", "/branch", "host", uam::RepositoryRouteIdentity{"svn", identity->identity, "branches/feature"}}};
	ExecutionRoutingSettings settings;
	ExecutionRoutingHostPolicy policy;
	policy.host_id = "ssh";
	settings.hosts.push_back(policy);
	UAM_ASSERT(uam::MatchRepositoryRoutes(source, inventory, settings).empty());
}

UAM_TEST(RepositoryRoutesReadPrivateUnbornGitCheckoutAndSubdirectory)
{
	TempDir fixture("uam-repository-routing");
	const fs::path repo = fixture.root / "checkout";
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunTestCommand("git -C " + ShellQuoteForTest(repo.string()) + " config remote.origin.url git@github.com:Owner/Repo.git"));
	fs::create_directory(repo / "src");
	const uam::RepositoryRouteReader reader = [&](const std::vector<std::string>& argv, std::string& output)
	{
		if (argv.front() != "git") return false;
		std::vector<std::string> local = argv;
		local.insert(local.begin() + 2, {"-C", (repo / "src").string()});
		std::string command;
		for (const std::string& argument : local) command += ShellQuoteForTest(argument) + " ";
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 2000);
		if (!result.ok || result.exit_code != 0 || result.output_truncated || result.timed_out) return false;
		output = result.output;
		return true;
	};
	const std::optional<uam::RepositoryRouteIdentity> identity = uam::ReadRepositoryRouteIdentity(reader);
	UAM_ASSERT(identity.has_value());
	UAM_ASSERT_EQ(identity->identity, std::string("github.com/Owner/Repo"));
	UAM_ASSERT_EQ(identity->relative_workspace, std::string("src"));
}

UAM_TEST(RepositoryRoutesRefreshWithoutOpeningSettingsAndExpireSafely)
{
	TempDir fixture("uam-repository-routing-refresh");
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(fixture.root.string())));
	UAM_ASSERT(RunTestCommand("git -C " + ShellQuoteForTest(fixture.root.string()) + " config remote.origin.url https://github.com/Owner/Repo.git"));
	uam::AppState app;
	app.data_root = fixture.root / "private-data";
	app.settings.execution_hosts.push_back(uam::execution_hosts::LocalHost());
	ChatFolder folder;
	folder.id = "local-fixture";
	folder.directory = fixture.root.string();
	app.folders.push_back(folder);
	UAM_ASSERT(!uam::RefreshRepositoryRoutes(app));
	UAM_ASSERT(app.repository_route_refresh != nullptr);
	const std::shared_ptr<uam::RepositoryRouteTask> original = app.repository_route_refresh;
	(void)uam::RefreshRepositoryRoutes(app);
	UAM_ASSERT_EQ(app.repository_route_tasks.size(), std::size_t(1));
	for (int attempts = 0; attempts < 200 && !original->finished.load(std::memory_order_acquire); ++attempts)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(original->finished.load(std::memory_order_acquire));
	UAM_ASSERT(uam::RefreshRepositoryRoutes(app));
	UAM_ASSERT_EQ(app.repository_route_workspaces.size(), std::size_t(1));
	UAM_ASSERT(app.repository_route_workspaces.front().repository.has_value());
	UAM_ASSERT_EQ(app.repository_route_workspaces.front().repository->identity, std::string("github.com/Owner/Repo"));
	app.repository_route_observed_at_s -= 31.0;
	UAM_ASSERT(!uam::RefreshRepositoryRoutes(app));
	UAM_ASSERT(app.repository_route_refresh != nullptr);
	UAM_ASSERT(app.repository_route_refresh != original);
}

UAM_TEST(RepositoryRoutesLaunchGuardFailsClosedAndAllowsExactManualOverride)
{
	uam::AppState app;
	app.data_root = fs::temp_directory_path() / "uam-synthetic-route-guard";
	app.settings.execution_hosts.push_back(uam::execution_hosts::LocalHost());
	ExecutionHost remote;
	remote.id = "fixture-host";
	remote.transport = "ssh";
	remote.ssh_alias = "never-contact-this-fixture";
	remote.platform = "linux";
	app.settings.execution_hosts.push_back(remote);
	ChatFolder source;
	source.id = "source";
	source.directory = "/fixture/source";
	ChatFolder destination;
	destination.id = "destination";
	destination.directory = "/fixture/destination";
	destination.execution_host_id = remote.id;
	app.folders = {source, destination};
	ChatSession chat;
	chat.id = "fixture-chat";
	chat.routing_mode = "automatic";
	chat.routing_source_folder_id = source.id;
	chat.routing_source_directory = source.directory;
	chat.folder_id = destination.id;
	chat.workspace_directory = destination.directory;
	chat.execution_host_id = remote.id;
	app.chats.push_back(chat);
	app.repository_route_workspaces = uam::CaptureRepositoryRouteWorkspaces(app.folders, app.settings.execution_hosts);
	for (uam::RepositoryRouteWorkspace& workspace : app.repository_route_workspaces)
		workspace.repository = uam::RepositoryRouteIdentity{"git", "github.com/Owner/Repo", ""};
	app.repository_route_data_root = app.data_root;
	app.repository_route_observed_at_s = uam::GetAppTimeSeconds();
	uam::RememberRepositoryRouteAssignment(app, chat);
	const std::shared_ptr<uam::RepositoryRouteTask> rejected = std::make_shared<uam::RepositoryRouteTask>();
	rejected->data_root = app.data_root;
	rejected->captured = app.repository_route_workspaces;
	rejected->chat_id = chat.id;
	rejected->chat_incarnation = chat.metadata_incarnation;
	rejected->chat_identity = nlohmann::json::array({chat.id, chat.routing_source_folder_id, chat.routing_source_directory,
	    chat.folder_id, chat.workspace_directory, chat.execution_host_id, chat.routing_assigned_connection_identity}).dump();
	rejected->started_at_s = uam::GetAppTimeSeconds();
	rejected->completed_at_s = rejected->started_at_s;
	rejected->matches = false;
	rejected->finished.store(true, std::memory_order_release);
	app.repository_route_tasks.push_back(rejected);
	std::string error;
	UAM_ASSERT_EQ(uam::PrepareRepositoryRouteLaunch(app, chat, error), uam::RepositoryRouteLaunchState::Failed);
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(app.repository_route_tasks.size(), std::size_t(1));
	UAM_ASSERT(rejected->worker == nullptr);
	// A changed source folder must fail without scheduling a remote observation.
	app.folders.front().directory = "/fixture/changed-source";
	app.repository_route_workspaces = uam::CaptureRepositoryRouteWorkspaces(app.folders, app.settings.execution_hosts);
	for (uam::RepositoryRouteWorkspace& workspace : app.repository_route_workspaces)
		workspace.repository = uam::RepositoryRouteIdentity{"git", "github.com/Owner/Repo", ""};
	error.clear();
	UAM_ASSERT_EQ(uam::PrepareRepositoryRouteLaunch(app, chat, error), uam::RepositoryRouteLaunchState::Failed);
	UAM_ASSERT_EQ(app.repository_route_tasks.size(), std::size_t(1));
	app.folders.front().directory = source.directory;
	ExecutionRoutingHostPolicy policy;
	policy.host_id = remote.id;
	policy.workspace_mappings.push_back({chat.routing_source_folder_id, chat.routing_source_directory, chat.folder_id});
	app.settings.execution_routing.hosts.push_back(policy);
	error.clear();
	UAM_ASSERT_EQ(uam::PrepareRepositoryRouteLaunch(app, chat, error), uam::RepositoryRouteLaunchState::Ready);
	UAM_ASSERT_EQ(app.repository_route_tasks.size(), std::size_t(1));
}
