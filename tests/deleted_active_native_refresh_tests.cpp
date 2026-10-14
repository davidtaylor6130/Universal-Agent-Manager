// Draft only. Not registered or executed during the native compiler gate.
#include "test_harness.h"

using namespace uam_test;

UAM_TEST(LoadedNativeRefreshCannotRestoreTombstonedChat)
{
	TempDir temp("uam-stale-deleted-native-refresh");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.selected_chat_index = -1;
	ChatSession stale;
	stale.id = "deleted-fixture-chat";
	stale.provider_id = "gemini-cli";
	stale.native_session_id = "deleted-fixture-native";
	stale.workspace_directory = (temp.root / "workspace").string();
	stale.messages.push_back(Message{MessageRole::User, "Private stale history"});
	ChatSession survivor;
	survivor.id = "survivor-fixture";
	survivor.provider_id = "codex-cli";
	survivor.messages.push_back(Message{MessageRole::User, "Survivor bytes"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, survivor));
	app.chats.push_back(survivor);
	app.selected_chat_index = 0;
	const fs::path survivor_path = AppPaths::UamChatFilePath(app.data_root, survivor.id);
	const std::string survivor_bytes = uam::io::ReadTextFile(survivor_path);
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {stale}, added));
	// Models a completed native scan captured before deletion, without deleting provider source files.
	(void)uam::SyncChatsFromLoadedNative(app, {stale}, stale.native_session_id, true);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().id, survivor.id);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(app.data_root).size(), std::size_t{1});
	UAM_ASSERT_EQ(uam::io::ReadTextFile(survivor_path), survivor_bytes);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), survivor.id);
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app).value("selectedChatId", ""), survivor.id);
}

UAM_TEST(LoadedNativeRefreshFailsClosedWhenDeletionEvidenceIsUnreadable)
{
	TempDir temp("uam-unreadable-deleted-native-refresh");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.selected_chat_index = -1;
	ChatSession stale;
	stale.id = "unknown-deletion-fixture";
	stale.provider_id = "gemini-cli";
	stale.native_session_id = "unknown-deletion-native";
	stale.messages.push_back(Message{MessageRole::User, "Private stale history"});
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "native-import-tombstones.json", "{"));
	UAM_ASSERT(!uam::SyncChatsFromLoadedNative(app, {stale}, stale.native_session_id, true));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChats(app.data_root).empty());
	UAM_ASSERT(ChatDomainService().SelectedChatId(app).empty());
}

UAM_TEST(NativeOverlayCannotMergeTombstonedChatIntoMemory)
{
	TempDir temp("uam-deleted-native-overlay");
	uam::AppState app;
	app.data_root = temp.root;
	app.selected_chat_index = -1;
	ChatSession stale;
	stale.id = "deleted-overlay-fixture";
	stale.provider_id = "gemini-cli";
	stale.native_session_id = "deleted-overlay-native";
	stale.workspace_directory = (temp.root / "workspace").string();
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {stale}, added));
	std::vector<ChatSession> native{stale};
	ChatHistorySyncService().ApplyLocalOverrides(app, native, false);
	UAM_ASSERT(native.empty());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatDomainService().SelectedChatId(app).empty());
}

UAM_TEST(NativeRefreshDeletionGuardPreservesSameNativeIdInAnotherWorkspace)
{
	TempDir temp("uam-native-refresh-workspace-scope");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession deleted;
	deleted.id = "deleted-scope-fixture";
	deleted.provider_id = "gemini-cli";
	deleted.native_session_id = "shared-native-id";
	deleted.workspace_directory = (temp.root / "deleted-workspace").string();
	std::vector<std::string> added;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {deleted}, added));
	ChatSession unrelated = deleted;
	unrelated.id = "other-workspace-fixture";
	unrelated.workspace_directory = (temp.root / "other-workspace").string();
	unrelated.messages.push_back(Message{MessageRole::User, "Other workspace"});
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {unrelated}, unrelated.native_session_id, true));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().id, unrelated.id);
}

UAM_TEST(NativeAsyncRefreshSnapshotsCannotCrossUamDataRoots)
{
	TempDir temp("uam-native-refresh-data-root");
	uam::AppState app;
	app.data_root = temp.root / "first-data";
	const ProviderProfile provider = ProviderProfileStore::DefaultGeminiProfile();
	const fs::path directory = temp.root / "source" / "chats";
	uam::platform::AsyncNativeChatLoadTask& task = uam::AsyncNativeChatLoadTaskFor(app, provider.id, directory);
	task.running = true;
	task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
	task.state->chats = {ChatSession{}};
	task.state->completed.store(true);
	app.data_root = temp.root / "second-data";
	std::vector<ChatSession> chats;
	std::string digest;
	std::string error;
	UAM_ASSERT(!uam::TryConsumeAsyncNativeChatLoadForTerminal(app, provider, directory, chats, digest, error));
	UAM_ASSERT(chats.empty());
	uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks);
}

UAM_TEST(NativeGeminiRefreshCarriesSourceWorkspaceBeforeDeletionCheck)
{
	TempDir temp("uam-native-refresh-source-scope");
	uam::AppState app;
	app.data_root = temp.root / "data";
	const ProviderProfile provider = ProviderProfileStore::DefaultGeminiProfile();
	const fs::path directory = temp.root / "native-source" / "chats";
	const fs::path workspace = temp.root / "workspace";
	UAM_ASSERT(uam::io::WriteTextFile(directory.parent_path() / ".project_root", workspace.string()));
	const fs::path source = directory / "fixture.json";
	const std::string bytes = R"({"sessionId":"scoped-native-fixture","messages":[{"type":"user","content":"Private source"}]})";
	UAM_ASSERT(uam::io::WriteTextFile(source, bytes));
	std::vector<ChatSession> loaded = ChatHistorySyncService().LoadNativeSessionChats(directory, provider);
	UAM_ASSERT_EQ(loaded.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded.front().workspace_directory, workspace.string());
	std::vector<std::string> added;
	ChatSession deleted = loaded.front();
	deleted.workspace_directory = workspace.string();
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {deleted}, added));
	(void)uam::SyncChatsFromLoadedNative(app, loaded, deleted.native_session_id, true);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(source), bytes);
}
