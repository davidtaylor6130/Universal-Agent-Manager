#include "test_harness.h"

using namespace uam_test;

UAM_TEST(StateSerializerWorkspaceSnapshotsKeepScopeIdentitiesSeparate)
{
	TempDir temp("uam-workspace-snapshot-scopes");
	temp.root = fs::canonical(temp.root);
	uam::AppState app;
	app.data_root = temp.root;
	ChatFolder folder;
	folder.id = "shared-folder";
	folder.directory = uam::paths::Utf8PathString(temp.root / "source");
	folder.execution_host_id = "local";
	app.folders.push_back(folder);
	ChatSession local;
	local.id = "local";
	local.provider_id = "opencode-cli";
	local.folder_id = folder.id;
	local.execution_host_id = "local";
	ChatSession duplicate = local;
	duplicate.id = "duplicate";
	ChatSession isolated = local;
	isolated.id = "isolated";
	isolated.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	isolated.workspace_worktree_directory = uam::paths::Utf8PathString(temp.root / "worktree");
	ChatSession imported = local;
	imported.id = "imported";
	imported.imported_read_only = true;
	ChatSession peer = local;
	peer.id = "peer";
	peer.execution_host_id = "peer";
	ChatSession explicit_peer = peer;
	explicit_peer.id = "explicit-peer";
	explicit_peer.workspace_directory = "/remote/workspace";
	app.chats = {local, duplicate, isolated, imported, peer, explicit_peer};
	app.selected_chat_index = 0;
	for (bool summary_only : {false, true})
	{
		const nlohmann::json serialized = uam::StateSerializer::Serialize(app, summary_only);
		const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
		for (const nlohmann::json* result : {&serialized, &fingerprint})
		{
			const nlohmann::json& chats = (*result)["chats"];
			UAM_ASSERT_EQ(chats[0]["workspaceDirectory"], folder.directory);
			UAM_ASSERT_EQ(chats[1]["workspaceDirectory"], folder.directory);
			UAM_ASSERT_EQ(chats[2]["workspaceDirectory"], isolated.workspace_worktree_directory);
			UAM_ASSERT_EQ(chats[3]["workspaceDirectory"], std::string{});
			UAM_ASSERT_EQ(chats[4]["workspaceDirectory"], std::string{});
			UAM_ASSERT_EQ(chats[5]["workspaceDirectory"], explicit_peer.workspace_directory);
		}
	}
}

UAM_TEST(StateSerializerWorkspaceSnapshotsRefreshFolderAndCatalogTogether)
{
	TempDir temp("uam-workspace-snapshot-refresh");
	temp.root = fs::canonical(temp.root);
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	const std::string provider = "opencode-cli";
	const fs::path first_root = temp.root / "first";
	const fs::path second_root = temp.root / "second";
	const nlohmann::json first_models = nlohmann::json::array({{{"id", "first/model"}}});
	const nlohmann::json second_models = nlohmann::json::array({{{"id", "second/model"}}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, first_models, first_root.generic_string()));
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, second_models, second_root.generic_string()));
	ChatFolder folder;
	folder.id = "moving-folder";
	folder.directory = uam::paths::Utf8PathString(first_root);
	app.folders.push_back(folder);
	ChatSession chat;
	chat.id = "moving-chat";
	chat.provider_id = provider;
	chat.folder_id = folder.id;
	app.chats.push_back(chat);
	const nlohmann::json first = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(first["chats"][0]["workspaceDirectory"], folder.directory);
	UAM_ASSERT_EQ(first["chats"][0]["acpSession"]["availableModels"], app.provider_model_catalog->GetCatalogSnapshot(provider, first_root.generic_string())["availableModels"]);
	app.folders.front().directory = uam::paths::Utf8PathString(second_root);
	for (const nlohmann::json result : {uam::StateSerializer::SerializeFingerprint(app), uam::StateSerializer::Serialize(app, false)})
	{
		UAM_ASSERT_EQ(result["chats"][0]["workspaceDirectory"], app.folders.front().directory);
		UAM_ASSERT_EQ(result["chats"][0]["acpSession"]["availableModels"], app.provider_model_catalog->GetCatalogSnapshot(provider, second_root.generic_string())["availableModels"]);
	}
}

UAM_TEST(StateSerializerWorkspaceSnapshotsDoNotRetainPriorCurrentDirectory)
{
	TempDir temp("uam-workspace-snapshot-current-directory");
	temp.root = fs::canonical(temp.root);
	struct CurrentDirectoryGuard
	{
		fs::path original = fs::current_path();
		~CurrentDirectoryGuard() { std::error_code error; fs::current_path(original, error); }
	} guard;
	const fs::path first = temp.root / "first";
	const fs::path second = temp.root / "second";
	fs::create_directories(first);
	fs::create_directories(second);
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "default-workspace";
	chat.provider_id = "opencode-cli";
	app.chats.push_back(chat);
	fs::current_path(first);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["workspaceDirectory"], uam::paths::Utf8PathString(first));
	fs::current_path(second);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["workspaceDirectory"], uam::paths::Utf8PathString(second));
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app, false)["chats"][0]["workspaceDirectory"], uam::paths::Utf8PathString(second));
}
