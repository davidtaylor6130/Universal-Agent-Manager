#include "test_harness.h"

using namespace uam_test;

UAM_TEST(DeletionOwnershipCacheKeepsHostBoundariesAndRefreshesEachPreparation)
{
	TempDir temp("uam-deletion-owner-cache");
	ScopedEnvVar gemini("GEMINI_CLI_HOME", (temp.root / "provider-home").string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	ExecutionHost linux_host;
	linux_host.id = "linux-owner";
	linux_host.platform = "linux";
	ExecutionHost windows_host;
	windows_host.id = "windows-owner";
	windows_host.platform = "windows";
	app.settings.execution_hosts.push_back(linux_host);
	app.settings.execution_hosts.push_back(windows_host);
	for (const std::string id : {"victim", "local-one", "local-two", "linux-owner", "windows-owner"})
	{
		ChatSession chat;
		chat.id = id;
		chat.branch_root_chat_id = id;
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = id == "linux-owner" || id == "windows-owner" ? id : "local";
		chat.workspace_directory = chat.execution_host_id == "local" ? workspace.string() : "/Team/Shared";
		app.chats.push_back(chat);
	}
	ChatFolder local_folder;
	local_folder.id = "local-folder";
	local_folder.directory = workspace.string();
	ChatFolder remote_folder;
	remote_folder.id = "remote-folder";
	remote_folder.execution_host_id = "linux-owner";
	remote_folder.directory = "/Team/Folder";
	app.folders = {local_folder, remote_folder};
	for (const std::string platform : {"linux", "windows", "linux"})
	{
		for (ExecutionHost& host : app.settings.execution_hosts)
		{
			if (host.id == "linux-owner") host.platform = platform;
		}
		uam::WorkspaceDeletionTask task;
		UAM_ASSERT(uam::PrepareChatDeletion(app, {"victim"}, task));
		const std::unordered_set<std::string> expected = {
			uam::paths::WorkspaceOwnershipKey(app, "local", workspace.string()),
			uam::paths::WorkspaceOwnershipKey(app, "linux-owner", "/Team/Shared"),
			uam::paths::WorkspaceOwnershipKey(app, "windows-owner", "/Team/Shared"),
			uam::paths::WorkspaceOwnershipKey(app, "linux-owner", "/Team/Folder")};
		UAM_ASSERT_EQ(task.retained_workspace_keys, expected);
		UAM_ASSERT_EQ(task.snapshot.chats.size(), std::size_t(1));
		UAM_ASSERT_EQ(app.chats.size(), std::size_t(5));
	}
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT(!fs::exists(temp.root / "provider-home"));
}
