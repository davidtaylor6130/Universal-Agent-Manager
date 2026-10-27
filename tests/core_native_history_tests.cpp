#include "common/companion/protocol.h"
#include "core/chat_import_utils.h"
#include "cef/uam_bridge_request.h"
#include "common/platform/platform_speech_output.h"
#include "test_harness.h"
#include "common/platform/file_explorer_application.h"
#include "common/platform/powershell_selection.h"
#include "remote/remote_workspace_terminal.h"
#include "common/config/custom_icon.h"
#include "common/config/companion_network.h"
#include "app/chat_lifecycle_service.h"
#include "app/chat_search_service.h"
#include "app/local_chat_bundle_service.h"
#include "app/persistence_coordinator.h"
#include "app/transcript_retention_service.h"
#include "app/startup_native_import_service.h"
#include "app/runtime_orchestration_internal.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_client.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "common/config/mcp_server_config.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"

#include <cstdio>
#include <fstream>
#include <future>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <versionhelpers.h>
#endif

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

UAM_TEST(OpenCodeOpenNativeSessionChatImportsLocalHistorySubAgent)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.folder_id = "folder-1";
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);

	ChatSession local_sub_agent;
	local_sub_agent.id = "chat-sub-agent";
	local_sub_agent.provider_id = uam::provider_ids::kOpenCodeCli;
	local_sub_agent.title = "Planner";
	local_sub_agent.folder_id = "folder-1";
	local_sub_agent.workspace_directory = workspace_root.string();
	local_sub_agent.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local_sub_agent));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, " agent-session-1 ");

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(imported->title, std::string("Planner"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[imported->id], std::string("agent-session-1"));
	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1") != nullptr);
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatRollbackClearsInsertedResolvedMapping)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-rollback");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.folder_id = "folder-1";
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);
	ChatDomainService().SelectChatById(app, source_chat.id);

	ChatSession imported_chat;
	imported_chat.id = "chat-sub-agent";
	imported_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	imported_chat.folder_id = "folder-1";
	imported_chat.workspace_directory = workspace_root.string();
	imported_chat.native_session_id = "agent-session-1";
	imported_chat.messages = {Message{MessageRole::Assistant, "Previously saved child result"}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, imported_chat));

	const ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(
	    app, app.chats.front(), provider, imported_chat.native_session_id, false);
	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->id, imported_chat.id);
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[imported_chat.id], imported_chat.native_session_id);
	ChatDomainService().SelectChatById(app, imported_chat.id);

	const fs::path imported_chat_file = AppPaths::UamChatFilePath(app.data_root, imported_chat.id);
	UAM_ASSERT(uam::paths::PathExistsNoThrow(imported_chat_file));

	const std::string persisted_before = uam::io::ReadTextFile(imported_chat_file);
	const fs::path settings_path = AppPaths::SettingsFilePath(app.data_root);
	fs::create_directories(settings_path);
	UAM_ASSERT(uam::io::WriteTextFile(settings_path / "blocked", "not a settings file"));
	UAM_ASSERT(!PersistenceCoordinator().SaveSettings(app));
	ChatHistorySyncService().RollbackOpenNativeSessionChatImport(app, imported_chat.id, source_chat.id);

	UAM_ASSERT(ChatDomainService().FindChatById(app, imported_chat.id) == nullptr);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.find(imported_chat.id) == app.resolved_native_sessions_by_chat_id.end());
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), source_chat.id);
	UAM_ASSERT(uam::paths::PathExistsNoThrow(imported_chat_file));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(imported_chat_file), persisted_before);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, imported_chat.id, true);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved->messages.front().content, "Previously saved child result");
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionResolvedMappingRestoreUsesPreviousStateOrClearsNewMapping)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.resolved_native_sessions_by_chat_id["chat-opencode"] = "old-session";

	ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(app, "chat-opencode", true, "restored-session");
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id["chat-opencode"], std::string("restored-session"));

	ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(app, "chat-opencode", false, "ignored");
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.find("chat-opencode") == app.resolved_native_sessions_by_chat_id.end());
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatFailureRestoresSourceAndTargetResolvedMappings)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.resolved_native_sessions_by_chat_id["chat-source"] = "source-session";
	app.resolved_native_sessions_by_chat_id["chat-target"] = "old-target-session";

	const bool had_previous_source_resolved_native_session = true;
	const std::string previous_source_resolved_native_session_id = "source-session";
	const bool had_previous_target_resolved_native_session = true;
	const std::string previous_target_resolved_native_session_id = "old-target-session";

	app.resolved_native_sessions_by_chat_id["chat-target"] = "new-target-session";
	ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(app, "chat-target", had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
	ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(app, "chat-source", had_previous_source_resolved_native_session, previous_source_resolved_native_session_id);

	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id["chat-target"], std::string("old-target-session"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id["chat-source"], std::string("source-session"));
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatSeedsResolvedMappingWhenReusingExistingChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-reuse");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);

	ChatSession local_sub_agent;
	local_sub_agent.id = "chat-sub-agent";
	local_sub_agent.provider_id = uam::provider_ids::kOpenCodeCli;
	local_sub_agent.title = "Planner";
	local_sub_agent.workspace_directory = workspace_root.string();
	local_sub_agent.native_session_id = "agent-session-1";
	ChatDomainService().AddMessage(local_sub_agent, MessageRole::Assistant, "Fresh sub-agent result");
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local_sub_agent));
	local_sub_agent.messages.clear();
	app.chats.push_back(local_sub_agent);

	ChatSession* existing = ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1");
	UAM_ASSERT(existing != nullptr);
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.empty());

	ChatDomainService().SelectChatById(app, source_chat.id);
	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* reused = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(reused != nullptr);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), source_chat.id);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(reused->provider_id, opencode_provider.id);
	UAM_ASSERT_EQ(reused->workspace_directory, workspace_root.string());
	UAM_ASSERT_EQ(reused->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(reused->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(reused->messages.front().content, std::string("Fresh sub-agent result"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[reused->id], std::string("agent-session-1"));
	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1") == reused);
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatRepairsStaleLoadedChatNativeSessionIdWhenReusingExistingChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-reuse-stale-raw");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);

	ChatSession loaded_chat;
	loaded_chat.id = "chat-live";
	loaded_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	loaded_chat.title = "Planner";
	loaded_chat.workspace_directory = workspace_root.string();
	loaded_chat.native_session_id = "stale-session";
	app.chats.push_back(loaded_chat);
	app.resolved_native_sessions_by_chat_id[loaded_chat.id] = "agent-session-1";

	ChatSession disk_copy = loaded_chat;
	disk_copy.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, disk_copy));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* reused = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(reused != nullptr);
	UAM_ASSERT_EQ(reused->id, std::string("chat-live"));
	UAM_ASSERT_EQ(reused->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[reused->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatRepairsResolvedOnlyLoadedChatNativeSessionIdWithoutDiskCandidate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-resolved-only-live-reuse");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);

	ChatSession loaded_chat;
	loaded_chat.id = "chat-live";
	loaded_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	loaded_chat.title = "Planner";
	loaded_chat.workspace_directory = workspace_root.string();
	loaded_chat.native_session_id = "stale-session";
	app.chats.push_back(loaded_chat);
	app.resolved_native_sessions_by_chat_id[loaded_chat.id] = "agent-session-1";

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* reused = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(reused != nullptr);
	UAM_ASSERT_EQ(reused->id, std::string("chat-live"));
	UAM_ASSERT_EQ(reused->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[reused->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatReusesResolvedOnlyLoadedChatBeforeImportingDiskCandidate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-resolved-only-reuse");
	const fs::path workspace_root = temp.root / "workspace";
	fs::create_directories(workspace_root);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = workspace_root.string();
	app.chats.push_back(source_chat);

	ChatSession resolved_only_live;
	resolved_only_live.id = "chat-live";
	resolved_only_live.provider_id = uam::provider_ids::kOpenCodeCli;
	resolved_only_live.title = "Planner";
	resolved_only_live.workspace_directory = workspace_root.string();
	resolved_only_live.native_session_id = "stale-session";
	app.chats.push_back(resolved_only_live);
	app.resolved_native_sessions_by_chat_id[resolved_only_live.id] = "agent-session-1";

	ChatSession disk_copy;
	disk_copy.id = "chat-live";
	disk_copy.provider_id = uam::provider_ids::kOpenCodeCli;
	disk_copy.title = "Planner";
	disk_copy.workspace_directory = workspace_root.string();
	disk_copy.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, disk_copy));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* reused = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(reused != nullptr);
	UAM_ASSERT_EQ(reused->id, std::string("chat-live"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[reused->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatRespectsWorkspaceFiltering)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-workspace");
	const fs::path source_workspace = temp.root / "workspace-a";
	const fs::path other_workspace = temp.root / "workspace-b";
	fs::create_directories(source_workspace);
	fs::create_directories(other_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession local_sub_agent;
	local_sub_agent.id = "chat-sub-agent";
	local_sub_agent.provider_id = uam::provider_ids::kOpenCodeCli;
	local_sub_agent.title = "Planner";
	local_sub_agent.workspace_directory = other_workspace.string();
	local_sub_agent.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local_sub_agent));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(imported == nullptr);
	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1") == nullptr);
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatPrefersWorkspaceMatchingChatOverStaleExistingChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-stale-existing");
	const fs::path source_workspace = temp.root / "workspace-a";
	const fs::path other_workspace = temp.root / "workspace-b";
	fs::create_directories(source_workspace);
	fs::create_directories(other_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession stale_existing;
	stale_existing.id = "chat-stale";
	stale_existing.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_existing.title = "Wrong workspace";
	stale_existing.workspace_directory = other_workspace.string();
	stale_existing.native_session_id = "agent-session-1";
	app.chats.push_back(stale_existing);

	ChatSession local_sub_agent;
	local_sub_agent.id = "chat-sub-agent";
	local_sub_agent.provider_id = uam::provider_ids::kOpenCodeCli;
	local_sub_agent.title = "Planner";
	local_sub_agent.workspace_directory = source_workspace.string();
	local_sub_agent.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local_sub_agent));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->title, std::string("Planner"));
	UAM_ASSERT_EQ(imported->workspace_directory, source_workspace.string());
	UAM_ASSERT(imported->id != stale_existing.id);
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatIgnoresCrossProviderSameSessionCollision)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-opencode-open-subagent-cross-provider");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession codex_collision;
	codex_collision.id = "chat-codex";
	codex_collision.provider_id = uam::provider_ids::kCodexCli;
	codex_collision.title = "Codex collision";
	codex_collision.workspace_directory = source_workspace.string();
	codex_collision.native_session_id = "agent-session-1";
	app.chats.push_back(codex_collision);

	ChatSession opencode_sub_agent;
	opencode_sub_agent.id = "chat-open-code";
	opencode_sub_agent.provider_id = uam::provider_ids::kOpenCodeCli;
	opencode_sub_agent.title = "Planner";
	opencode_sub_agent.workspace_directory = source_workspace.string();
	opencode_sub_agent.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, opencode_sub_agent));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(imported->title, std::string("Planner"));
	UAM_ASSERT(imported->id != codex_collision.id);
#endif
}

UAM_TEST(OpenCodeOpenNativeSessionChatRejectsWrongProviderDiskCollision)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-opencode-open-subagent-disk-cross-provider");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession codex_collision;
	codex_collision.id = "chat-codex";
	codex_collision.provider_id = uam::provider_ids::kCodexCli;
	codex_collision.title = "Codex collision";
	codex_collision.workspace_directory = source_workspace.string();
	codex_collision.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, codex_collision));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(imported == nullptr);
	UAM_ASSERT(app.chats.size() == static_cast<std::size_t>(1));
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.empty());
#endif
}

UAM_TEST(OpenCodeFindInMemoryNativeSessionChatForOpenPrefersMatchingLoadedChatOverStaleResolvedCollision)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-memory-fallback");
	const fs::path source_workspace = temp.root / "workspace-a";
	const fs::path other_workspace = temp.root / "workspace-b";
	fs::create_directories(source_workspace);
	fs::create_directories(other_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession stale_existing;
	stale_existing.id = "chat-stale";
	stale_existing.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_existing.title = "Wrong workspace";
	stale_existing.workspace_directory = other_workspace.string();
	stale_existing.native_session_id = "agent-session-1";
	app.chats.push_back(stale_existing);
	app.resolved_native_sessions_by_chat_id[stale_existing.id] = "agent-session-1";

	ChatSession live_loaded;
	live_loaded.id = "chat-live";
	live_loaded.provider_id.clear();
	live_loaded.title = "Planner";
	live_loaded.workspace_directory = source_workspace.string();
	live_loaded.native_session_id = "agent-session-1";
	app.chats.push_back(live_loaded);

	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1") != nullptr);
	UAM_ASSERT_EQ(ChatDomainService().FindChatByNativeSessionId(app, "agent-session-1")->id, std::string("chat-stale"));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, std::string("chat-live"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[matched->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeFindInMemoryNativeSessionChatForOpenPrefersRawLoadedChatOverStaleResolvedCollisionInSameWorkspace)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-memory-fallback-same-workspace");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession stale_existing;
	stale_existing.id = "chat-stale";
	stale_existing.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_existing.title = "Wrong resolved mapping";
	stale_existing.workspace_directory = source_workspace.string();
	stale_existing.native_session_id = "stale-session";
	app.chats.push_back(stale_existing);
	app.resolved_native_sessions_by_chat_id[stale_existing.id] = "agent-session-1";

	ChatSession live_loaded;
	live_loaded.id = "chat-live";
	live_loaded.provider_id.clear();
	live_loaded.title = "Planner";
	live_loaded.workspace_directory = source_workspace.string();
	live_loaded.native_session_id = "agent-session-1";
	app.chats.push_back(live_loaded);

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, std::string("chat-live"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[matched->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeFindInMemoryNativeSessionChatForOpenPrefersNewerEqualPriorityCandidate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-memory-newer-priority");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession older_loaded;
	older_loaded.id = "chat-older";
	older_loaded.provider_id = uam::provider_ids::kOpenCodeCli;
	older_loaded.title = "Older";
	older_loaded.workspace_directory = source_workspace.string();
	older_loaded.native_session_id = "agent-session-1";
	older_loaded.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(older_loaded);

	ChatSession newer_loaded = older_loaded;
	newer_loaded.id = "chat-newer";
	newer_loaded.title = "Newer";
	newer_loaded.updated_at = "2026-01-01T00:00:02.000Z";
	app.chats.push_back(newer_loaded);

	ChatSession remote_collision = newer_loaded;
	remote_collision.id = "agent-session-1";
	remote_collision.execution_host_id = "remote-host";
	app.chats.push_back(remote_collision);

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, std::string("chat-newer"));
#endif
}

UAM_TEST(OpenCodeFindInMemoryNativeSessionChatForOpenCanSkipResolvedMappingMutation)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-memory-readonly");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession live_loaded;
	live_loaded.id = "chat-live";
	live_loaded.provider_id.clear();
	live_loaded.title = "Planner";
	live_loaded.workspace_directory = source_workspace.string();
	live_loaded.native_session_id = "agent-session-1";
	app.chats.push_back(live_loaded);

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1", false);

	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, std::string("chat-live"));
	UAM_ASSERT(matched->provider_id.empty());
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.find(matched->id) == app.resolved_native_sessions_by_chat_id.end());
#endif
}

UAM_TEST(OpenCodeFindOrImportNativeSessionChatForOpenCanSkipProviderNormalizationForImportedLegacyChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-import-readonly");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession legacy_disk_chat;
	legacy_disk_chat.id = "chat-legacy";
	legacy_disk_chat.provider_id.clear();
	legacy_disk_chat.title = "Legacy OpenCode chat";
	legacy_disk_chat.workspace_directory = source_workspace.string();
	legacy_disk_chat.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, legacy_disk_chat));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1", false);

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->id, std::string("chat-legacy"));
	UAM_ASSERT(imported->provider_id.empty());
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[imported->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeFindOrImportNativeSessionChatForOpenCanReuseResolvedOnlyLoadedChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-resolved-only-reuse");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession resolved_legacy_chat;
	resolved_legacy_chat.id = "chat-live";
	resolved_legacy_chat.provider_id.clear();
	resolved_legacy_chat.title = "Resolved Legacy OpenCode chat";
	resolved_legacy_chat.workspace_directory = source_workspace.string();
	resolved_legacy_chat.native_session_id.clear();
	app.chats.push_back(resolved_legacy_chat);
	app.resolved_native_sessions_by_chat_id[resolved_legacy_chat.id] = "agent-session-1";

	ChatSession legacy_disk_chat;
	legacy_disk_chat.id = "chat-legacy";
	legacy_disk_chat.provider_id.clear();
	legacy_disk_chat.title = "Legacy OpenCode chat";
	legacy_disk_chat.workspace_directory = source_workspace.string();
	legacy_disk_chat.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, legacy_disk_chat));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1", false);
	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, resolved_legacy_chat.id);
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1", false);

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->id, std::string("chat-live"));
	UAM_ASSERT(imported->provider_id.empty());
	UAM_ASSERT_EQ(imported->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[imported->id], std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
#endif
}

UAM_TEST(OpenCodeFindInMemoryNativeSessionChatForOpenAcceptsBlankProviderLegacyChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-blank-provider");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession legacy_loaded;
	legacy_loaded.id = "chat-legacy";
	legacy_loaded.provider_id.clear();
	legacy_loaded.title = "Legacy OpenCode chat";
	legacy_loaded.workspace_directory = source_workspace.string();
	legacy_loaded.native_session_id = "agent-session-1";
	app.chats.push_back(legacy_loaded);

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* matched = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(matched != nullptr);
	UAM_ASSERT_EQ(matched->id, std::string("chat-legacy"));
	UAM_ASSERT_EQ(matched->provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[matched->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeFindOrImportNativeSessionChatForOpenPrefersRawLoadedChatOverStaleResolvedCollisionInSameWorkspace)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-import-fallback-same-workspace");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession stale_existing;
	stale_existing.id = "chat-stale";
	stale_existing.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_existing.title = "Wrong resolved mapping";
	stale_existing.workspace_directory = source_workspace.string();
	stale_existing.native_session_id = "stale-session";
	app.chats.push_back(stale_existing);
	app.resolved_native_sessions_by_chat_id[stale_existing.id] = "agent-session-1";

	ChatSession live_loaded;
	live_loaded.id = "chat-live";
	live_loaded.provider_id = uam::provider_ids::kOpenCodeCli;
	live_loaded.title = "Planner";
	live_loaded.workspace_directory = source_workspace.string();
	live_loaded.native_session_id = "agent-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, live_loaded));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* hint = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1", false);
	UAM_ASSERT(hint != nullptr);
	UAM_ASSERT_EQ(hint->id, stale_existing.id);
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[stale_existing.id], std::string("agent-session-1"));
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(imported != nullptr);
	UAM_ASSERT_EQ(imported->id, std::string("chat-live"));
	UAM_ASSERT_EQ(imported->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[imported->id], std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(3));
	const std::string imported_id = imported->id;
	ChatHistorySyncService().RollbackOpenNativeSessionChatImport(app, imported_id, source_chat.id);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.back().id, stale_existing.id);
	UAM_ASSERT_EQ(app.chats.back().native_session_id, stale_existing.native_session_id);
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[stale_existing.id], std::string("agent-session-1"));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, imported_id, true);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, std::string("agent-session-1"));
#endif
}

UAM_TEST(OpenCodeFindOrImportNativeSessionChatForOpenPrefersNewerLoadedRawDuplicate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-open-subagent-import-raw-duplicate");
	const fs::path source_workspace = temp.root / "workspace";
	fs::create_directories(source_workspace);

	uam::AppState app;
	app.data_root = temp.root;

	ChatSession source_chat;
	source_chat.id = "chat-source";
	source_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	source_chat.workspace_directory = source_workspace.string();
	app.chats.push_back(source_chat);

	ChatSession older_loaded;
	older_loaded.id = "chat-older";
	older_loaded.provider_id = uam::provider_ids::kOpenCodeCli;
	older_loaded.title = "Older";
	older_loaded.workspace_directory = source_workspace.string();
	older_loaded.native_session_id = "agent-session-1";
	older_loaded.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(older_loaded);

	ChatSession newer_loaded = older_loaded;
	newer_loaded.id = "chat-newer";
	newer_loaded.title = "Newer";
	newer_loaded.updated_at = "2026-01-01T00:00:02.000Z";
	app.chats.push_back(newer_loaded);

	ChatSession disk_copy = older_loaded;
	disk_copy.id = "chat-disk";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, disk_copy));

	const ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession* reused = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), opencode_provider, "agent-session-1");

	UAM_ASSERT(reused != nullptr);
	UAM_ASSERT_EQ(reused->id, std::string("chat-newer"));
	UAM_ASSERT_EQ(reused->native_session_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[reused->id], std::string("agent-session-1"));
#endif
}

UAM_TEST(RemoteChatsCannotLinkOrOpenControllerNativeHistory)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	ChatSession remote;
	remote.id = "chat-1700000000000-remote";
	remote.provider_id = uam::provider_ids::kOpenCodeCli;
	remote.execution_host_id = "homelab";
	remote.workspace_directory = "/srv/project";
	app.chats.push_back(remote);

	ChatSession local_native;
	local_native.id = "local-native";
	local_native.provider_id = uam::provider_ids::kOpenCodeCli;
	local_native.native_session_id = "agent-session-1";
	local_native.workspace_directory = remote.workspace_directory;
	app.chats.push_back(local_native);

	const ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	UAM_ASSERT(ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(
	               app, app.chats.front(), provider, local_native.native_session_id) == nullptr);
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(
	               app, app.chats.front(), provider, local_native.native_session_id) == nullptr);
	UAM_ASSERT(!NativeSessionLinkService().MatchNativeSessionIdForLocalDraft(
	                app.chats.front(), {local_native}).has_value());
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.empty());
#endif
}

UAM_TEST(ControllerNativeHistoryCannotOverlayOrDropRemoteChats)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-remote-native-overlay-isolation");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession remote;
	remote.id = "chat-remote";
	remote.title = "Remote";
	remote.provider_id = uam::provider_ids::kOpenCodeCli;
	remote.native_session_id = "agent-session-1";
	remote.folder_id = "remote-folder";
	remote.execution_host_id = "homelab";
	remote.workspace_directory = "/srv/project";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, remote));

	ChatSession controller_native;
	controller_native.id = "controller-native";
	controller_native.title = "Controller";
	controller_native.provider_id = uam::provider_ids::kOpenCodeCli;
	controller_native.native_session_id = remote.native_session_id;
	controller_native.folder_id = "local-folder";
	controller_native.execution_host_id = "local";
	controller_native.workspace_directory = (temp.root / "project").string();
	std::vector<ChatSession> native_chats{controller_native};

	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats);
	const ChatSession* preserved_remote = ChatDomainService().FindChatById(app, remote.id);
	const ChatSession* preserved_local = ChatDomainService().FindChatById(app, controller_native.id);
	UAM_ASSERT(preserved_remote != nullptr);
	UAM_ASSERT(preserved_local != nullptr);
	UAM_ASSERT_EQ(preserved_remote->execution_host_id, std::string("homelab"));
	UAM_ASSERT_EQ(preserved_remote->workspace_directory, std::string("/srv/project"));
	UAM_ASSERT_EQ(preserved_local->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(preserved_local->folder_id, std::string("local-folder"));
#endif
}

UAM_TEST(DeletingRemoteChatsTombstonesOnlyTheirMachineScopedIdentity)
{
	TempDir temp("uam-remote-native-tombstone-isolation");
	ChatSession remote;
	remote.provider_id = uam::provider_ids::kOpenCodeCli;
	remote.native_session_id = "agent-session-1";
	remote.execution_host_id = "homelab";
	remote.workspace_directory = "/srv/project";

	std::vector<std::string> added_keys;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(
	    temp.root, {remote}, added_keys));
	UAM_ASSERT_EQ(added_keys.size(), static_cast<std::size_t>(1));
	ChatSession controller = remote;
	controller.execution_host_id = "local";
	UAM_ASSERT(added_keys.front() !=
	           uam::chat_identity::NativeIdentityKeyForHistoryImport(controller));
	UAM_ASSERT(uam::paths::PathExistsNoThrow(
	    temp.root / "native-import-tombstones.json"));
}

UAM_TEST(RemoteOpenCodeScansSkipOwnedPersistedAndLiveAttachments)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	for (int link_mode : {0, 1, 2, 3})
	{
		TempDir temp("uam-owned-remote-opencode");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "remote-host";
		host.platform = "linux";
		app.settings.execution_hosts.push_back(host);
		ChatFolder folder;
		folder.id = "remote-folder";
		folder.directory = "/srv/workspace";
		folder.execution_host_id = host.id;
		app.folders.push_back(folder);
		ChatSession owner;
		owner.id = "chat-123-owner";
		owner.title = "Keep my OpenCode title";
		owner.provider_id = "opencode-cli";
		owner.native_session_id = "ses_owned";
		owner.workspace_directory = folder.directory;
		owner.execution_host_id = host.id;
		if (link_mode == 0) UAM_ASSERT(ChatRepository::SaveChat(app.data_root, owner));
		else
		{
			if (link_mode >= 2) owner.native_session_id.clear();
			if (link_mode == 2) app.resolved_native_sessions_by_chat_id[owner.id] = "ses_owned";
			if (link_mode == 3)
			{
				std::unique_ptr<uam::AcpSessionState> pending = std::make_unique<uam::AcpSessionState>();
				pending->chat_id = owner.id;
				pending->running = true;
				app.acp_sessions.push_back(std::move(pending));
			}
			app.chats.push_back(owner);
		}
		const ChatHistorySyncService::ImportResult result = ChatHistorySyncService().ImportRemoteOpenCodeChatsForFolder(
		    app, folder.id, {{"ses_owned", "Provider title", folder.directory, 1, 2}});
		UAM_ASSERT(result.success);
		UAM_ASSERT_EQ(result.total_count, 1);
		UAM_ASSERT_EQ(result.imported_count, 0);
		UAM_ASSERT(!ChatRepository::LoadLocalChat(app.data_root, "ses_owned").has_value());
	}
#endif
}

UAM_TEST(SaveNativeTranscriptPreservesUniqueSteeringAcrossMessageRegrouping)
{
	TempDir temp("uam-steering-history-regroup");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatHistorySyncService service;
	for (int scenario = 0; scenario < 5; ++scenario)
	{
		ChatSession current;
		current.id = "steering-history-" + std::to_string(scenario);
		Message response{MessageRole::Assistant, "Response"};
		Message steer{MessageRole::User, "Also check recovery", "2026-09-06T15:00:00Z"};
		steer.continues_turn = true;
		if (scenario == 4) steer.created_at.clear();
		current.messages = {response, steer};
		if (scenario == 1) current.messages.push_back(steer);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
		Message native_steer = steer;
		native_steer.continues_turn = false;
		if (scenario == 3) native_steer.created_at = "2026-09-06T15:01:00Z";
		std::vector<Message> native{response, response, native_steer};
		if (scenario == 2) native.push_back(native_steer);
		UAM_ASSERT(service.SaveNativeTranscript(app, current, native));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages[2].continues_turn, scenario == 0);
		if (scenario == 2) UAM_ASSERT(!saved->messages.back().continues_turn);
	}
}

UAM_TEST(SaveNativeTranscriptPreservesUniqueManagedOriginAcrossMessageRegrouping)
{
	TempDir temp("uam-managed-origin-history-regroup");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatHistorySyncService service;
	const std::string origin = "5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
	for (int scenario = 0; scenario < 5; ++scenario)
	{
		ChatSession current;
		current.id = "managed-origin-history-" + std::to_string(scenario);
		Message response{MessageRole::Assistant, "Response"};
		Message authored{MessageRole::User, "Also check recovery", "2026-09-06T15:00:00Z"};
		authored.managed_origin_run_id = origin;
		if (scenario == 4) authored.created_at.clear();
		current.messages = {response, authored};
		if (scenario == 1) current.messages.push_back(authored); // Ambiguous saved identity.
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
		Message native_authored = authored;
		native_authored.managed_origin_run_id.reset(); // Native input supplies no UAM annotation.
		if (scenario == 3) native_authored.created_at = "2026-09-06T15:01:00Z";
		// All native user rows move beyond every saved row: no positional merge can rescue a negative arm.
		std::vector<Message> native{response, response, response, native_authored};
		if (scenario == 2) native.push_back(native_authored); // Ambiguous native identity.
		const std::size_t native_count = native.size();
		UAM_ASSERT(native_count > current.messages.size());
		UAM_ASSERT(service.SaveNativeTranscript(app, current, native));
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages.size(), native_count); // The incoming regrouped transcript won.
		UAM_ASSERT_EQ(saved->messages[1].role, MessageRole::Assistant);
		UAM_ASSERT_EQ(saved->messages[2].role, MessageRole::Assistant);
		UAM_ASSERT_EQ(saved->messages[3].role, MessageRole::User);
		UAM_ASSERT_EQ(saved->messages[3].content, authored.content);
		UAM_ASSERT(saved->messages[3].managed_origin_run_id == (scenario == 0 ? std::optional<std::string>{origin} : std::nullopt));
		if (scenario == 2) UAM_ASSERT(!saved->messages.back().managed_origin_run_id);
		UAM_ASSERT_EQ(current.messages, saved->messages);
	}
	// Preserve the same-index match while independently making the saved or incoming identity ambiguous.
	std::string ambiguous_origins;
	for (const bool duplicate_saved : {true, false})
	{
		ChatSession current;
		current.id = duplicate_saved ? "managed-origin-same-index-saved-duplicate" : "managed-origin-same-index-native-duplicate";
		Message response{MessageRole::Assistant, "Response"};
		Message authored{MessageRole::User, "Also check recovery", "2026-09-06T15:00:00Z"};
		authored.managed_origin_run_id = origin;
		current.messages = {response, authored};
		if (duplicate_saved) current.messages.push_back(authored);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
		Message native_authored = authored;
		native_authored.managed_origin_run_id.reset();
		const std::vector<Message> native = {response, native_authored, response, duplicate_saved ? response : native_authored};
		UAM_ASSERT(native.size() > current.messages.size()); // The actual incoming history must win.
		UAM_ASSERT(service.SaveNativeTranscript(app, current, native));
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
		UAM_ASSERT(saved && saved->messages.size() == native.size());
		UAM_ASSERT_EQ(saved->messages[1].role, MessageRole::User);
		UAM_ASSERT_EQ(saved->messages[1].content, authored.content);
		UAM_ASSERT_EQ(saved->messages[1].created_at, authored.created_at);
		UAM_ASSERT_EQ(saved->messages[2].role, MessageRole::Assistant);
		UAM_ASSERT_EQ(saved->messages[3].role, duplicate_saved ? MessageRole::Assistant : MessageRole::User);
		if (saved->messages[1].managed_origin_run_id)
			ambiguous_origins += duplicate_saved ? " saved-duplicate" : " native-duplicate";
		if (!duplicate_saved) UAM_ASSERT(!saved->messages[3].managed_origin_run_id);
		UAM_ASSERT_EQ(current.messages, saved->messages);
	}
	// Both public save/reload arms complete before reporting either ambiguous binding.
	UAM_ASSERT_EQ(ambiguous_origins, std::string{});
}

UAM_TEST(SaveNativeTranscriptPreservesChildIdentityAcrossMessageRegrouping)
{
	TempDir temp("uam-child-history-regroup");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession current;
	current.id = "child-history-regroup";
	Message local{MessageRole::Assistant, "Combined streamed response"};
	ToolCall child;
	child.id = "child-call";
	child.is_sub_agent = true;
	child.sub_agent_id = "child-session";
	child.sub_agent_title = "Reviewer";
	local.tool_calls = {child};
	current.messages = {local};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
	Message response{MessageRole::Assistant, "Separated native response"};
	Message native_tool{MessageRole::Assistant, ""};
	ToolCall tool;
	tool.id = child.id;
	tool.status = "completed";
	tool.result_text = "Fresh provider result";
	native_tool.tool_calls = {tool};
	const ChatHistorySyncService service;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {response, native_tool}));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
	UAM_ASSERT(saved.has_value());
	const ToolCall& restored = saved->messages.back().tool_calls.front();
	UAM_ASSERT(restored.is_sub_agent);
	UAM_ASSERT_EQ(restored.sub_agent_id, child.sub_agent_id);
	UAM_ASSERT_EQ(restored.sub_agent_title, child.sub_agent_title);
	UAM_ASSERT_EQ(restored.result_text, tool.result_text);
	UAM_ASSERT_EQ(restored.status, tool.status);
	// Provider identities win; a local title must not label a different child.
	native_tool.tool_calls.front().sub_agent_id = "different-child";
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {response, native_tool}));
	UAM_ASSERT_EQ(current.messages.back().tool_calls.front().sub_agent_id, std::string("different-child"));
	UAM_ASSERT(current.messages.back().tool_calls.front().sub_agent_title.empty());
	for (const std::string& ambiguous_id : {std::string("child-call"), std::string()})
	{
		local.tool_calls.front().id = ambiguous_id;
		current.messages = {local, local};
		native_tool.tool_calls.front() = tool;
		native_tool.tool_calls.front().id = ambiguous_id;
		UAM_ASSERT(service.SaveNativeTranscript(app, current, {response, native_tool}));
		UAM_ASSERT(current.messages.back().tool_calls.front().sub_agent_id.empty());
	}
}

UAM_TEST(NativeChildToolMetadataIgnoresUnrelatedAndMalformedRecords)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const IProviderRuntime& codex = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli);
	for (const std::string& name : {"spawn_agent", "multi_agent_v1.spawn_agent", "functions.spawn_agent"})
	{
		ToolCall child;
		child.name = name;
		child.result_text = R"({"agent_id":"44444444-4444-4444-8444-444444444444"})";
		codex.ApplyNativeToolMetadata(child, nlohmann::json::object());
		UAM_ASSERT(child.is_sub_agent);
		UAM_ASSERT_EQ(child.sub_agent_id, std::string("44444444-4444-4444-8444-444444444444"));
	}
	ToolCall spawn;
	spawn.name = "multi_agent_v1.spawn_agent";
	spawn.result_text = R"({"agent_id":"44444444-4444-4444-8444-444444444444","nickname":"Reviewer"})";
	codex.ApplyNativeToolMetadata(spawn, nlohmann::json::object());
	UAM_ASSERT_EQ(spawn.sub_agent_id, std::string("44444444-4444-4444-8444-444444444444"));
	for (const std::string& malformed : {std::string("{incomplete"), std::string(R"({"agent_id":42})"), std::string(R"({"agent_id":"../other"})")})
	{
		spawn.result_text = malformed;
		codex.ApplyNativeToolMetadata(spawn, nlohmann::json::object());
		UAM_ASSERT_EQ(spawn.sub_agent_id, std::string("44444444-4444-4444-8444-444444444444"));
		UAM_ASSERT_EQ(spawn.sub_agent_title, std::string("Reviewer"));
	}
	ToolCall unrelated;
	unrelated.name = "read_file";
	unrelated.result_text = R"({"agent_id":"44444444-4444-4444-8444-444444444444"})";
	codex.ApplyNativeToolMetadata(unrelated, nlohmann::json::object());
	UAM_ASSERT(!unrelated.is_sub_agent);
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const IProviderRuntime& opencode = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kOpenCodeCli);
	ToolCall task;
	task.name = "task";
	for (const nlohmann::json& malformed : {nlohmann::json(), nlohmann::json::array(), nlohmann::json{{"state", {{"metadata", {{"sessionId", 42}}}}}}})
	{
		opencode.ApplyNativeToolMetadata(task, malformed);
		UAM_ASSERT(task.sub_agent_id.empty());
	}
#endif
}

UAM_TEST(ChatRepositoryAdoptsOnlyUnchangedHydratedMessages)
{
	ChatSession current;
	current.id = "hydration-guard";
	current.title = "Renamed while loading";
	current.folder_id = "live-folder";
	current.messages_loaded = false;
	current.persisted_message_count = 1;
	current.persisted_messages_digest = "digest-1";

	ChatSession loaded = current;
	loaded.title = "Persisted title";
	loaded.messages_loaded = true;
	loaded.messages = {Message{MessageRole::Assistant, "loaded"}};
	UAM_ASSERT(ChatRepository::AdoptHydratedMessagesIfUnchanged(current, std::move(loaded), 1, "digest-1"));
	UAM_ASSERT(current.messages_loaded);
	UAM_ASSERT_EQ(current.messages.front().content, std::string("loaded"));
	UAM_ASSERT_EQ(current.title, std::string("Renamed while loading"));
	UAM_ASSERT_EQ(current.folder_id, std::string("live-folder"));

	current.messages_loaded = false;
	current.messages.clear();
	current.persisted_messages_digest = "digest-2";
	ChatSession stale_current = current;
	stale_current.messages_loaded = true;
	stale_current.messages = {Message{MessageRole::Assistant, "stale"}};
	UAM_ASSERT(!ChatRepository::AdoptHydratedMessagesIfUnchanged(current, std::move(stale_current), 1, "digest-1"));
	UAM_ASSERT(current.messages.empty());

	ChatSession mismatched_loaded = current;
	mismatched_loaded.messages_loaded = true;
	mismatched_loaded.persisted_messages_digest = "digest-other";
	mismatched_loaded.messages = {Message{MessageRole::Assistant, "wrong"}};
	UAM_ASSERT(!ChatRepository::AdoptHydratedMessagesIfUnchanged(current, std::move(mismatched_loaded), 1, "digest-2"));
	UAM_ASSERT(current.messages.empty());
}

UAM_TEST(ColdToolCallContentLookupIsDetachedIdentityBoundAndAmbiguitySafe)
{
	TempDir temp("uam-cold-tool-content");
	ChatSession chat;
	chat.id = "cold-tool-content";
	chat.provider_id = "codex";
	chat.native_session_id = "native-session-before";
	chat.workspace_directory = "/workspace/project";
	chat.updated_at = "2026-10-01T00:00:00.000Z";
	Message first{MessageRole::User, "prompt"};
	Message second{MessageRole::Assistant, "result"};
	ToolCall tool;
	tool.id = "tool-call-1";
	tool.name = "read_file";
	tool.args_json = R"({"path":"safe.txt"})";
	tool.result_text = "persisted output";
	second.tool_calls.push_back(tool);
	chat.messages = {first, second};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::optional<ChatSession> cold = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(cold.has_value() && !cold->messages_loaded && cold->messages.empty());
	const uam::ToolCallContentIdentity identity =
	    uam::CaptureToolCallContentIdentity(*cold, static_cast<std::size_t>(1));
	const std::optional<uam::ToolCallContentSnapshot> resolved =
	    uam::LoadToolCallContentSnapshot(temp.root, identity, tool.id);
	UAM_ASSERT(resolved.has_value());
	UAM_ASSERT_EQ(resolved->message_index, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(resolved->tool_call, tool);
	UAM_ASSERT(uam::MatchesToolCallContentSnapshot(*cold, *resolved));
	UAM_ASSERT(cold->messages.empty() && !cold->messages_loaded);
	UAM_ASSERT(!uam::LoadToolCallContentSnapshot(temp.root, identity, "missing-tool").has_value());
	UAM_ASSERT(uam::LoadToolCallContentSnapshot(temp.root,
	    uam::CaptureToolCallContentIdentity(*cold, std::nullopt), tool.id).has_value());

	uam::ToolCallContentIdentity stale = identity;
	stale.native_session_id = "native-session-after";
	UAM_ASSERT(!uam::LoadToolCallContentSnapshot(temp.root, stale, tool.id).has_value());
	stale = identity;
	stale.messages_digest += "stale";
	UAM_ASSERT(!uam::LoadToolCallContentSnapshot(temp.root, stale, tool.id).has_value());

	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(uam::MatchesToolCallContentSnapshot(*loaded, *resolved));
	loaded->messages[1].tool_calls[0].result_text = "changed during lookup";
	UAM_ASSERT(!uam::MatchesToolCallContentSnapshot(*loaded, *resolved));

	ChatSession duplicate = chat;
	duplicate.messages[0].tool_calls.push_back(tool);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, duplicate));
	const std::optional<ChatSession> duplicate_cold = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(duplicate_cold.has_value());
	const uam::ToolCallContentIdentity duplicate_identity =
	    uam::CaptureToolCallContentIdentity(*duplicate_cold, std::nullopt);
	UAM_ASSERT(!uam::LoadToolCallContentSnapshot(temp.root, duplicate_identity, tool.id).has_value());
	UAM_ASSERT(uam::LoadToolCallContentSnapshot(temp.root,
	    uam::CaptureToolCallContentIdentity(*duplicate_cold, static_cast<std::size_t>(1)), tool.id).has_value());
}

UAM_TEST(ChatRepositoryHydratesMissingDatesWithoutChangingPersistedHistory)
{
	TempDir temp("uam-stable-missing-chat-dates");
	for (const bool has_updated_at : {false, true})
	{
		ChatSession chat;
		chat.id = has_updated_at ? "known-updated-date" : "missing-chat-dates";
		chat.updated_at = has_updated_at ? "2026-01-02T03:04:05.000Z" : "";
		chat.messages = {Message{MessageRole::Assistant, "persisted history"}};
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		const std::string original = ReadFile(primary);
		const fs::file_time_type original_time = fs::last_write_time(primary);
		std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
		UAM_ASSERT(summary.has_value());
		const std::size_t count = summary->persisted_message_count;
		const std::string digest = summary->persisted_messages_digest;
		// Cross the parser's second-resolution clock boundary without modifying the file.
		if (!has_updated_at) std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		std::optional<ChatSession> full = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
		UAM_ASSERT(full.has_value());
		UAM_ASSERT_EQ(full->persisted_messages_digest, digest);
		UAM_ASSERT(ChatRepository::AdoptHydratedMessagesIfUnchanged(*summary, std::move(*full), count, digest));
		UAM_ASSERT_EQ(summary->messages.front().content, std::string("persisted history"));
		if (has_updated_at)
		{
			UAM_ASSERT_EQ(summary->created_at, chat.updated_at);
			UAM_ASSERT_EQ(summary->updated_at, chat.updated_at);
		}
		UAM_ASSERT(summary->interaction_at.empty());
		UAM_ASSERT_EQ(ReadFile(primary), original);
		UAM_ASSERT_EQ(fs::last_write_time(primary), original_time);
	}
}

UAM_TEST(ChatRepositoryRefreshesSummaryDigestAfterLoadedTranscriptChanges)
{
	TempDir temp("uam-chat-summary-digest");
	ChatSession chat;
	chat.id = "summary-digest";
	chat.title = "Remote chat";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = chat.created_at;
	chat.messages.push_back(Message{MessageRole::User, "first"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> initial = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(initial.has_value());
	chat.persisted_messages_digest = initial->persisted_messages_digest;
	chat.messages.push_back(Message{MessageRole::Assistant, "second"});
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	const std::optional<ChatSession> full = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(summary.has_value() && full.has_value());
	UAM_ASSERT_EQ(summary->persisted_message_count, static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(summary->persisted_messages_digest, full->persisted_messages_digest);
	UAM_ASSERT_EQ(summary->persisted_messages_digest, std::string("2026-01-01T00:00:01.000Z:2"));
	const fs::path cache_path = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	auto corrupt_cache = [&]()
	{
		nlohmann::json cached = nlohmann::json::parse(ReadFile(cache_path));
		cached["persisted_messages_digest"] = initial->persisted_messages_digest;
		UAM_ASSERT(uam::io::WriteTextFile(cache_path, cached.dump(2)));
	};
	corrupt_cache();
	const std::vector<ChatSession> sidebar = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(sidebar.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(sidebar.front().persisted_messages_digest, full->persisted_messages_digest);
	corrupt_cache();
	const std::optional<ChatSession> reopened = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(reopened.has_value());
	UAM_ASSERT_EQ(reopened->persisted_messages_digest, full->persisted_messages_digest);
}

UAM_TEST(SaveNativeTranscriptPreservesDetailsAndCommitsBeforeReplacingLiveChat)
{
	TempDir temp("uam-native-transcript-save");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession current;
	current.id = "native-transcript";
	current.provider_id = uam::provider_ids::kOpenCodeCli;
	current.native_session_id = "ses_transcript";
	current.title = "My title";
	current.folder_id = "my-folder";
	current.pinned = true;
	current.model_id = "my-model";
	current.updated_at = "2026-09-01T00:00:00Z";
	Message first{MessageRole::User, "Original prompt"};
	first.created_at = current.updated_at;
	first.markdown_store_files = {"docs/context.uam"};
	first.markdown_store_prompt_blocks = {"Original context"};
	first.attachments.push_back(MessageAttachment{.id = "attachment", .name = "notes.txt", .path = "notes.txt"});
	current.messages = {first};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
	current.messages.clear();
	current.messages_loaded = false;
	current.persisted_message_count = 1;
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	app.cli_terminals.back()->frontend_chat_id = current.id;
	app.cli_terminals.back()->running = true;
	app.cli_terminals.back()->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	Message native_first{MessageRole::User, first.content};
	native_first.created_at = first.created_at;
	Message second{MessageRole::Assistant, "Native result"};
	second.created_at = "2026-09-02T00:00:00Z";
	const ChatHistorySyncService service;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT(current.messages_loaded);
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(current.messages.front().attachments.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(current.messages.front().markdown_store_files, first.markdown_store_files);
	UAM_ASSERT_EQ(current.messages.front().markdown_store_prompt_blocks, first.markdown_store_prompt_blocks);
	UAM_ASSERT_EQ(current.title, "My title");
	UAM_ASSERT_EQ(current.folder_id, "my-folder");
	UAM_ASSERT(current.pinned);
	UAM_ASSERT_EQ(current.model_id, "my-model");
	UAM_ASSERT_EQ(current.native_session_id, "ses_transcript");
	UAM_ASSERT_EQ(current.updated_at, second.created_at);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(saved->messages.front().markdown_store_files, first.markdown_store_files);
	UAM_ASSERT_EQ(saved->messages.front().attachments.size(), static_cast<std::size_t>(1));
	const fs::path primary = AppPaths::UamChatFilePath(app.data_root, current.id);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(app.data_root, current.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string backup_before = uam::io::ReadTextFile(backup);
	const fs::file_time_type old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(primary, old_time);
	fs::last_write_time(summary, old_time);
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT_EQ(fs::last_write_time(primary), old_time);
	UAM_ASSERT_EQ(fs::last_write_time(summary), old_time);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(backup), backup_before);
	fs::remove(summary);
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT(fs::is_regular_file(summary));
	UAM_ASSERT_EQ(fs::last_write_time(primary), old_time);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(backup), backup_before);
	fs::last_write_time(summary, old_time - std::chrono::hours(1));
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT(fs::last_write_time(summary) >= fs::last_write_time(primary));
	fs::remove(primary);
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT(ChatRepository::LoadLocalChat(app.data_root, current.id, true).has_value());
#if !defined(_WIN32)
	const fs::path real_primary = temp.root / "real-primary.json";
	fs::rename(primary, real_primary);
	fs::create_symlink(real_primary, primary);
	UAM_ASSERT(!service.SaveNativeTranscript(app, current, {native_first, second}));
	fs::remove(primary);
	fs::rename(real_primary, primary);
#endif
	second.tool_calls.push_back(ToolCall{.id = "result", .result_text = "first"});
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	second.tool_calls.front().result_text = "other";
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(app.data_root, current.id, true)->messages.back().tool_calls.front().result_text, "other");
	second.thoughts = "Thought-only update";
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first, second}));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(app.data_root, current.id, true)->messages.back().thoughts, second.thoughts);
	UAM_ASSERT(service.SaveNativeTranscript(app, current, {native_first}));
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	Message third{MessageRole::Assistant, "Later result"};
	third.created_at = "2026-09-03T00:00:00Z";
	const std::vector<Message> longer = {native_first, second, third};
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	app.acp_sessions.back()->chat_id = current.id;
	app.acp_sessions.back()->running = true;
	app.acp_sessions.back()->processing = true;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, longer));
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	app.acp_sessions.back()->processing = false;
	app.acp_sessions.back()->turn_checkpoint_commit_pending = true;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, longer));
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	app.acp_sessions.clear();
	app.pending_chat_save_at_by_chat_id[current.id] = 1.0;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, longer));
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	app.pending_chat_save_at_by_chat_id.clear();
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "not a directory"));
	UAM_ASSERT(!service.SaveNativeTranscript(app, current, longer));
	UAM_ASSERT_EQ(current.messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(current.messages.back().content, second.content);
	UAM_ASSERT_EQ(current.updated_at, second.created_at);
	UAM_ASSERT_EQ(current.messages.front().markdown_store_files, first.markdown_store_files);
	app.cli_terminals.back()->running = false;
}

UAM_TEST(SaveNativeTranscriptKeepsWorktreeOwnedDirtyHistoryAndAllowsUnrelatedChat)
{
	uam_test::TempDir temp("uam-worktree-owned-history");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession current;
	current.id = "checkpoint-owned";
	current.provider_id = uam::provider_ids::kOpenCodeCli;
	current.messages = {Message{MessageRole::Assistant, "saved answer"}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, current));
	const fs::path primary = AppPaths::UamChatFilePath(app.data_root, current.id);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(app.data_root, current.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string primary_before = uam::io::ReadTextFile(primary);
	const std::string summary_before = uam::io::ReadTextFile(summary);
	const bool backup_existed = fs::exists(backup);
	const std::string backup_before = backup_existed ? uam::io::ReadTextFile(backup) : std::string();
	const fs::file_time_type old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(primary, old_time);
	fs::last_write_time(summary, old_time);
	const fs::file_time_type primary_before_time = fs::last_write_time(primary);
	const fs::file_time_type summary_before_time = fs::last_write_time(summary);
	const fs::file_time_type backup_before_time = backup_existed ? fs::last_write_time(backup) : fs::file_time_type();
	current.messages.back().content = "unsaved worktree-owned answer";
	const std::vector<Message> dirty_messages = current.messages;
	app.worktree_operation_chat_ids.insert(current.id);
	const std::vector<Message> imported = {
	    Message{MessageRole::Assistant, "native replacement"},
	    Message{MessageRole::Assistant, "native additional answer"}};
	const ChatHistorySyncService service;
	UAM_ASSERT(service.SaveNativeTranscript(app, current, imported));
	UAM_ASSERT_EQ(current.messages, dirty_messages);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), primary_before);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(summary), summary_before);
	UAM_ASSERT_EQ(fs::last_write_time(primary), primary_before_time);
	UAM_ASSERT_EQ(fs::last_write_time(summary), summary_before_time);
	UAM_ASSERT_EQ(fs::exists(backup), backup_existed);
	if (backup_existed)
	{
		UAM_ASSERT_EQ(uam::io::ReadTextFile(backup), backup_before);
		UAM_ASSERT_EQ(fs::last_write_time(backup), backup_before_time);
	}
	ChatSession unrelated;
	unrelated.id = "unrelated-chat";
	unrelated.provider_id = current.provider_id;
	unrelated.messages = {Message{MessageRole::Assistant, "other saved answer"}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated));
	UAM_ASSERT(service.SaveNativeTranscript(app, unrelated, imported));
	UAM_ASSERT_EQ(unrelated.messages, imported);
	const std::optional<ChatSession> unrelated_disk = ChatRepository::LoadLocalChat(app.data_root, unrelated.id, true);
	UAM_ASSERT(unrelated_disk.has_value());
	UAM_ASSERT_EQ(unrelated_disk->messages, imported);
	app.worktree_operation_chat_ids.erase(current.id);
	UAM_ASSERT(service.SaveNativeTranscript(app, current, imported));
	UAM_ASSERT_EQ(current.messages, imported);
	const std::optional<ChatSession> released_disk = ChatRepository::LoadLocalChat(app.data_root, current.id, true);
	UAM_ASSERT(released_disk.has_value());
	UAM_ASSERT_EQ(released_disk->messages, imported);
}

UAM_TEST(OpenCodeExportSnapshotImportsRefreshesAndPreservesActiveOrDeletedChats)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-export-import");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "parent";
	parent.provider_id = uam::provider_ids::kOpenCodeCli;
	parent.workspace_directory = workspace.string();
	app.chats.push_back(parent);
	ChatDomainService().SelectChatById(app, parent.id);
	const ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	ChatSession snapshot;
	snapshot.id = "ses_child";
	snapshot.native_session_id = snapshot.id;
	snapshot.provider_id = provider.id;
	snapshot.workspace_directory = parent.workspace_directory;
	snapshot.messages.push_back(Message{MessageRole::Assistant, "First result"});
	snapshot.created_at = "2026-09-01T00:00:00Z";
	snapshot.updated_at = snapshot.created_at;
	snapshot.messages.front().created_at = snapshot.created_at;
	ChatSession remote_collision = snapshot;
	remote_collision.execution_host_id = "remote-host";
	remote_collision.messages = {Message{MessageRole::Assistant, "Remote result"}};
	app.chats.push_back(remote_collision);

	const auto import = [&]() { return ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), provider, "ses_child", false, &snapshot); };
	ChatSession* child = import();
	UAM_ASSERT(child != nullptr);
	UAM_ASSERT_EQ(child->messages.back().content, std::string("First result"));
	UAM_ASSERT(uam::paths::IsControllerLocalWorkspace(*child));
	UAM_ASSERT(child->id != remote_collision.id);
	UAM_ASSERT_EQ(app.chats[1].messages.front().content, std::string("Remote result"));

	const std::string child_id = child->id;
	child->title = "My child name";
	child->messages.front().markdown_store_files = {"docs/child-context.uam"};
	child->messages.front().markdown_store_prompt_blocks = {"Original child context"};
	child->messages.front().attachments.push_back(MessageAttachment{.id = "child-attachment", .name = "notes.txt", .path = "notes.txt"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *child));
	const std::optional<ChatSession> cold_child = ChatRepository::LoadLocalChat(app.data_root, child_id, false);
	UAM_ASSERT(cold_child.has_value());
	UAM_ASSERT(!cold_child->messages_loaded);
	*child = *cold_child;
	snapshot.messages.push_back(Message{MessageRole::Assistant, "Final result"});
	snapshot.updated_at = "2026-09-02T00:00:00Z";
	child = import();
	UAM_ASSERT(child != nullptr);
	UAM_ASSERT_EQ(child->id, child_id);
	UAM_ASSERT_EQ(child->title, std::string("My child name"));
	UAM_ASSERT_EQ(child->messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(child->messages.front().markdown_store_files, std::vector<std::string>{"docs/child-context.uam"});
	UAM_ASSERT_EQ(child->messages.front().markdown_store_prompt_blocks, std::vector<std::string>{"Original child context"});
	UAM_ASSERT_EQ(child->messages.front().attachments.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(child->messages.front().attachments.front().id, "child-attachment");
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(app.chats[1].messages.front().content, std::string("Remote result"));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), parent.id);

	std::unique_ptr<uam::AcpSessionState> busy_session = std::make_unique<uam::AcpSessionState>();
	busy_session->chat_id = child_id;
	busy_session->running = true;
	busy_session->processing = true;
	app.acp_sessions.push_back(std::move(busy_session));
	child->messages.push_back(Message{MessageRole::Assistant, "Live unsaved work"});
	child = import();
	UAM_ASSERT(child != nullptr);
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Live unsaved work"));
	app.acp_sessions.back()->running = false;
	app.acp_sessions.back()->reconnect_pending = true;
	UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live unsaved work"));
	app.acp_sessions.back()->processing = false;
	app.acp_sessions.back()->reconnect_pending = false;
	app.acp_sessions.back()->turn_checkpoint_commit_pending = true;
	UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live unsaved work"));
	app.acp_sessions.clear();
	app.pending_chat_save_at_by_chat_id[child_id] = uam::GetAppTimeSeconds() + 60.0;
	UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live unsaved work"));
	app.pending_chat_save_at_by_chat_id.erase(child_id);

	snapshot.workspace_directory = (temp.root / "elsewhere").string();
	UAM_ASSERT(import() == nullptr);
	snapshot.workspace_directory = parent.workspace_directory;
	snapshot.native_session_id = "ses_wrong";
	UAM_ASSERT(import() == nullptr);
	snapshot.native_session_id = "ses_child";
	std::vector<std::string> tombstones;
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {snapshot}, tombstones));
	UAM_ASSERT(import() == nullptr);
#endif
}

UAM_TEST(RemoteNativeChildSnapshotsPreserveHostMetadataActiveWorkAndDeletion)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI
	for (const ProviderProfile& provider : {ProviderProfileStore::DefaultOpenCodeProfile(), ProviderProfileStore::DefaultCodexProfile()})
	for (const std::string platform : {"linux", "windows"})
	{
		TempDir temp("uam-remote-native-child");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "remote-a";
		host.transport = "ssh";
		host.platform = platform;
		app.settings.execution_hosts.push_back(host);
		ChatSession parent;
		parent.id = "parent";
		parent.provider_id = provider.id;
		parent.execution_host_id = host.id;
		parent.workspace_directory = platform == "windows" ? "C:\\Work" : "/work";
		app.chats.push_back(parent);
		ChatSession snapshot = parent;
		snapshot.id = "01234567-89ab-4cde-8f01-23456789abcd";
		snapshot.native_session_id = snapshot.id;
		snapshot.title = "Native child";
		snapshot.messages = {Message{MessageRole::Assistant, "Initial result"}};
		snapshot.created_at = "2026-09-01T00:00:00Z";
		snapshot.updated_at = snapshot.created_at;
		ChatSession local_collision = snapshot;
		local_collision.execution_host_id = "local";
		local_collision.messages.front().content = "Local result";
		app.chats.push_back(local_collision);
		ChatSession remote_collision = snapshot;
		remote_collision.id = "other-host";
		remote_collision.execution_host_id = "remote-b";
		remote_collision.messages.front().content = "Other host result";
		app.chats.push_back(remote_collision);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local_collision));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, remote_collision));
		const auto import = [&]() { return ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, app.chats.front(), provider, snapshot.native_session_id, false, &snapshot); };
		UAM_ASSERT(ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(app, app.chats.front(), provider, snapshot.native_session_id, false) == nullptr);
		ChatSession* child = import();
		UAM_ASSERT(child != nullptr);
		UAM_ASSERT_EQ(child->execution_host_id, host.id);
		UAM_ASSERT(child->id != local_collision.id);
		const std::string child_id = child->id;
		child->title = "Saved custom title";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *child));
		app.chats.pop_back();
		snapshot.messages.push_back(Message{MessageRole::Assistant, "Final result"});
		snapshot.updated_at = "2026-09-02T00:00:00Z";
		child = import();
		UAM_ASSERT(child != nullptr);
		UAM_ASSERT_EQ(child->id, child_id);
		UAM_ASSERT_EQ(child->title, std::string("Saved custom title"));
		UAM_ASSERT_EQ(child->messages.back().content, std::string("Final result"));
		UAM_ASSERT_EQ(app.chats[1].messages.front().content, std::string("Local result"));
		UAM_ASSERT_EQ(app.chats[2].messages.front().content, std::string("Other host result"));
		std::unique_ptr<uam::AcpSessionState> busy_session = std::make_unique<uam::AcpSessionState>();
		busy_session->chat_id = child_id;
		busy_session->running = true;
		busy_session->processing = true;
		app.acp_sessions.push_back(std::move(busy_session));
		child->messages.back().content = "Live work";
		UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live work"));
		app.acp_sessions.back()->running = false;
		app.acp_sessions.back()->reconnect_pending = true;
		UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live work"));
		app.acp_sessions.back()->processing = false;
		app.acp_sessions.back()->reconnect_pending = false;
		app.acp_sessions.back()->turn_checkpoint_commit_pending = true;
		UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live work"));
		app.acp_sessions.clear();
		app.pending_chat_save_at_by_chat_id[child_id] = uam::GetAppTimeSeconds() + 60.0;
		UAM_ASSERT_EQ(import()->messages.back().content, std::string("Live work"));
		app.pending_chat_save_at_by_chat_id.erase(child_id);
		snapshot.execution_host_id = "remote-b";
		UAM_ASSERT(import() == nullptr);
		snapshot.execution_host_id = host.id;
		snapshot.workspace_directory += "/other";
		UAM_ASSERT(import() == nullptr);
		snapshot.workspace_directory = parent.workspace_directory;
		std::vector<std::string> tombstones;
		UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {snapshot}, tombstones));
		UAM_ASSERT(import() == nullptr);
	}
#endif
}

UAM_TEST(LocalOpenCodeExportValidatesSessionWorkspaceAndProcessFailure)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-local-opencode-export");
	const fs::path workspace = temp.root / "workspace with spaces";
	fs::create_directories(workspace);
#if defined(_WIN32)
	const fs::path shim = temp.root / "custom-opencode.cmd";
	const std::string script = "@echo off\r\necho invoked> export-invoked.txt\r\nif not \"%~1\"==\"export\" exit /b 9\r\nif not \"%~2\"==\"ses_child\" exit /b 9\r\nif not \"%~3\"==\"--pure\" exit /b 9\r\nif not \"%~4\"==\"\" exit /b 9\r\ntype export.json\r\n";
	const std::string path_separator = ";";
#else
	const fs::path shim = temp.root / "custom-opencode";
	const std::string script = "#!/bin/sh\nprintf invoked > export-invoked.txt\n[ \"$#\" = 3 ] && [ \"$1\" = export ] && [ \"$2\" = ses_child ] && [ \"$3\" = --pure ] || exit 9\n/bin/cat export.json\n";
	const std::string path_separator = ":";
#endif
	UAM_ASSERT(uam::io::WriteTextFile(shim, script));
	fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
	const char* previous_path = std::getenv("PATH");
	ScopedEnvVar path_env("PATH", temp.root.string() + path_separator + (previous_path == nullptr ? "" : previous_path));
	ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
	profile.interactive_command = uam::shell::JoinEscapedArgs({uam::paths::Utf8PathString(shim)});
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "ses_child";
	chat.workspace_directory = uam::paths::Utf8PathString(workspace);
	nlohmann::json exported = {
	    {"info", {{"id", chat.native_session_id}, {"directory", chat.workspace_directory}}},
	    {"messages", nlohmann::json::array({{
	        {"info", {{"role", "assistant"}}},
	        {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Child result"}}})}
	    }})}
	};
	const auto write_export = [&]() { UAM_ASSERT(uam::io::WriteTextFile(workspace / "export.json", exported.dump())); };
	write_export();
	const auto loaded = ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat, profile);
	UAM_ASSERT(loaded.success);
	UAM_ASSERT_EQ(loaded.messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.messages.front().content, std::string("Child result"));
	const fs::path invocation_marker = workspace / "export-invoked.txt";
	UAM_ASSERT(fs::exists(invocation_marker));
	for (const std::string& field : {std::string("id"), std::string("directory")})
	{
		const nlohmann::json previous = exported["info"][field];
		exported["info"][field] = "different";
		write_export();
		const auto rejected = ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat, profile);
		UAM_ASSERT(!rejected.success);
		UAM_ASSERT(rejected.messages.empty());
		exported["info"][field] = previous;
	}
	write_export();
	std::stop_source canceled;
	canceled.request_stop();
	UAM_ASSERT(!ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat, profile, canceled.get_token()).success);
	chat.native_session_id = "ses_other";
	const auto failed = ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat, profile);
	UAM_ASSERT(!failed.success);
	UAM_ASSERT(failed.messages.empty());
	UAM_ASSERT(fs::remove(invocation_marker));
	for (const std::string invalid_id : {"../../invalid", "--help", "-v"})
	{
		chat.native_session_id = invalid_id;
		UAM_ASSERT(!ChatHistorySyncService().LoadLocalOpenCodeTranscript(chat, profile).success);
		UAM_ASSERT(!fs::exists(invocation_marker));
	}
#endif
}

UAM_TEST(RemoteOpenCodeTranscriptParsesExportAndRejectsInvalidOrOversizedData)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const nlohmann::json exported = {
	    {"info", {{"id", "ses_remote_1"}, {"directory", R"(C:\Users\example\project)"}}},
	    {"messages", nlohmann::json::array({
	         {{"info", {{"role", "user"},
	                     {"time", {{"created", 1787951665685}}},
	                     {"tokens", {{"input", 12}, {"output", 0}}}}},
	          {"parts", nlohmann::json::array({{{"type", "text"}, {"text", "Run the check"}}})}},
	         {{"info", {{"role", "assistant"}, {"providerID", "openai"}, {"modelID", "test-model"},
	                     {"time", {{"created", 1787951666685}, {"completed", 1787951668185}}},
	                     {"tokens", {{"input", 4}, {"output", 8}}},
	                     {"cost", 0.25}}},
	          {"parts", nlohmann::json::array({
	                        {{"type", "reasoning"}, {"text", "Need the clock"}},
	                        {{"type", "tool"},
	                         {"callID", "call-date"},
	                         {"tool", "bash"},
	                         {"state", {{"status", "completed"},
	                                    {"input", {{"command", "date"}}},
	                                    {"output", "Saturday"}}}},
	                        {{"type", "text"}, {"text", "It is Saturday"}},
	                    })}},
	     })},
	};
	const ChatHistorySyncService::RemoteOpenCodeTranscript parsed =
	    ChatHistorySyncService::ParseRemoteOpenCodeTranscript(
	        "Exporting session: ses_remote_1\n" + exported.dump() + "\n");
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.session_id, std::string("ses_remote_1"));
	UAM_ASSERT_EQ(parsed.directory, std::string(R"(C:\Users\example\project)"));
	UAM_ASSERT_EQ(parsed.messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(parsed.messages[0].role, MessageRole::User);
	UAM_ASSERT_EQ(parsed.messages[0].content, std::string("Run the check"));
	UAM_ASSERT_EQ(parsed.messages[0].tokens_input, 12);
	UAM_ASSERT_EQ(parsed.messages[1].role, MessageRole::Assistant);
	UAM_ASSERT_EQ(parsed.messages[1].model_id, std::string("openai/test-model"));
	UAM_ASSERT_EQ(parsed.messages[1].content, std::string("It is Saturday"));
	UAM_ASSERT_EQ(parsed.messages[1].thoughts, std::string("Need the clock"));
	UAM_ASSERT_EQ(parsed.messages[1].processing_time_ms, 1500);
	UAM_ASSERT_EQ(parsed.messages[1].blocks.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[0].type, std::string("thought"));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[1].type, std::string("tool_call"));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[2].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls[0].id, std::string("call-date"));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls[0].name, std::string("bash"));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls[0].result_text, std::string("Saturday"));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls[0].status, std::string("completed"));
	for (const nlohmann::json& malformed_part : {nlohmann::json(nullptr), nlohmann::json("broken"),
	    nlohmann::json{{"type", "text"}}, nlohmann::json{{"type", "text"}, {"text", 42}},
	    nlohmann::json{{"type", "reasoning"}}, nlohmann::json{{"type", "reasoning"}, {"text", nullptr}}})
	{
		nlohmann::json malformed = exported;
		malformed["messages"][1]["parts"][0] = malformed_part;
		const ChatHistorySyncService::RemoteOpenCodeTranscript rejected = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(malformed.dump());
		UAM_ASSERT(!rejected.success && rejected.messages.empty() && !rejected.error.empty());
	}
	nlohmann::json interrupted_export = exported;
	interrupted_export["messages"][1]["info"]["error"] = {{"name", "MessageAbortedError"}, {"data", {{"message", "Aborted"}}}};
	interrupted_export["messages"][1]["parts"] = nlohmann::json::array();
	const ChatHistorySyncService::RemoteOpenCodeTranscript interrupted = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(interrupted_export.dump());
	UAM_ASSERT(interrupted.success && interrupted.messages.size() == 2);
	UAM_ASSERT(interrupted.messages[1].interrupted);
	nlohmann::json failed_export = exported;
	failed_export["messages"][1]["parts"][1]["state"] = {{"status", "error"}, {"error", "Permission denied"}};
	const ChatHistorySyncService::RemoteOpenCodeTranscript failed_tool = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(failed_export.dump());
	UAM_ASSERT(failed_tool.success);
	UAM_ASSERT_EQ(failed_tool.messages[1].tool_calls[0].status, std::string("error"));
	UAM_ASSERT_EQ(failed_tool.messages[1].tool_calls[0].result_text, std::string("Permission denied"));
	nlohmann::json child_export = exported;
	nlohmann::json& child_part = child_export["messages"][1]["parts"][1];
	child_part["tool"] = "task";
	child_part["state"]["metadata"] = {{"sessionId", "ses_child"}};
	child_part["state"]["title"] = "Review child";
	const ChatHistorySyncService::RemoteOpenCodeTranscript child_history = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(child_export.dump());
	UAM_ASSERT(child_history.success);
	UAM_ASSERT(child_history.messages[1].tool_calls[0].is_sub_agent);
	UAM_ASSERT_EQ(child_history.messages[1].tool_calls[0].sub_agent_id, std::string("ses_child"));
	UAM_ASSERT_EQ(child_history.messages[1].tool_calls[0].sub_agent_title, std::string("Review child"));
	child_part["state"]["metadata"]["sessionId"] = "../wrong";
	const ChatHistorySyncService::RemoteOpenCodeTranscript invalid_child = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(child_export.dump());
	UAM_ASSERT(invalid_child.success);
	UAM_ASSERT(invalid_child.messages[1].tool_calls[0].sub_agent_id.empty());

	for (const char* field : {"/info/id", "/info/directory", "/messages/1/info/role", "/messages/1/parts/1/type", "/messages/1/parts/1/callID", "/messages/1/parts/1/state/status"})
	{
		nlohmann::json malformed = exported;
		malformed[nlohmann::json::json_pointer(field)] = nlohmann::json::array();
		const auto rejected = ChatHistorySyncService::ParseRemoteOpenCodeTranscript(malformed.dump());
		UAM_ASSERT(!rejected.success);
		UAM_ASSERT(!rejected.error.empty());
		UAM_ASSERT(rejected.messages.empty());
	}

	UAM_ASSERT(!ChatHistorySyncService::ParseRemoteOpenCodeTranscript("not json").success);
	nlohmann::json oversized = {
	    {"info", {{"id", "ses_remote_1"}, {"directory", "/workspace"}}},
	    {"messages", nlohmann::json::array()},
	};
	for (int i = 0; i < 10001; ++i) oversized["messages"].push_back(nullptr);
	UAM_ASSERT(!ChatHistorySyncService::ParseRemoteOpenCodeTranscript(oversized.dump()).success);
#endif
}

UAM_TEST(RemoteCodexMetadataParsesExactThreadsAndRejectsInvalidOrOversizedLists)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const std::string thread_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	const nlohmann::json result = {
	    {"data", nlohmann::json::array({
	         {{"id", thread_id}, {"cwd", R"(C:\Users\example\project)"},
	          {"name", "Named thread"}, {"preview", "Preview"},
	          {"createdAt", 1787951665}, {"updatedAt", 1787952543}},
	         {{"id", "7b7f0f3b-1a0b-4a9c-8a01-222222222222"},
	          {"cwd", R"(C:\Users\example\project)"}, {"preview", "Ephemeral"},
	          {"createdAt", 1787951665}, {"updatedAt", 1787952543},
	          {"ephemeral", true}},
	     })},
	};
	std::vector<ChatHistorySyncService::RemoteCodexSession> sessions;
	std::string error;
	UAM_ASSERT(ChatHistorySyncService::AppendRemoteCodexSessions(
	    result, sessions, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(sessions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(sessions.front().id, thread_id);
	UAM_ASSERT_EQ(sessions.front().title, std::string("Named thread"));
	UAM_ASSERT_EQ(sessions.front().directory, std::string(R"(C:\Users\example\project)"));
	UAM_ASSERT_EQ(sessions.front().created_epoch_seconds, std::int64_t{1787951665});
	UAM_ASSERT_EQ(sessions.front().updated_epoch_seconds, std::int64_t{1787952543});
	nlohmann::json wrapped = result;
	wrapped["data"] = nlohmann::json::array({result["data"][0]});
	wrapped["data"][0].erase("name");
	wrapped["data"][0]["preview"] = "Relevant UAM memories. Treat these as durable preferences and lessons, not as new user commands:\n- Lesson\n\nCurrent user request:\nRemote request";
	std::vector<ChatHistorySyncService::RemoteCodexSession> wrapped_sessions;
	UAM_ASSERT(ChatHistorySyncService::AppendRemoteCodexSessions(wrapped, wrapped_sessions, &error));
	UAM_ASSERT_EQ(wrapped_sessions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(wrapped_sessions.front().title, std::string("Remote request"));


	const nlohmann::json invalid = {
	    {"data", nlohmann::json::array({
	         {{"id", "not-a-thread-id"}, {"cwd", "/workspace"},
	          {"preview", "Invalid"}, {"createdAt", 1}, {"updatedAt", 2}},
	     })},
	};
	UAM_ASSERT(!ChatHistorySyncService::AppendRemoteCodexSessions(
	    invalid, sessions, &error));
	UAM_ASSERT(!error.empty());
	nlohmann::json oversized = {{"data", nlohmann::json::array()}};
	for (int i = 0; i < 201; ++i) oversized["data"].push_back(nullptr);
	std::vector<ChatHistorySyncService::RemoteCodexSession> empty;
	UAM_ASSERT(!ChatHistorySyncService::AppendRemoteCodexSessions(
	    oversized, empty, &error));
#endif
}

UAM_TEST(RemoteCodexTranscriptMapsMessagesAndRejectsMalformedOrOversizedHistory)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const std::string thread_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	const nlohmann::json result = {
	    {"thread", {{"id", thread_id}, {"cwd", R"(C:\Users\example\project)"},
	                {"turns", nlohmann::json::array({
	                     {{"items", nlohmann::json::array({
	                          {{"type", "userMessage"},
	                           {"content", nlohmann::json::array({
	                                {{"type", "text"}, {"text", "Review the service"}},
	                            })}},
	                          {{"type", "userMessage"},
	                           {"content", nlohmann::json::array({
	                                {{"type", "text"},
	                                 {"text", "<environment_context>synthetic</environment_context>"}},
	                            })}},
		                          {{"type", "agentMessage"}, {"text", "Review complete"}},
		                          {{"id", "tool-1"}, {"type", "commandExecution"}, {"command", "rg TODO"}, {"status", "completed"}, {"aggregatedOutput", "match"}},
		                          {{"type", "agentMessage"}, {"text", "Done."}},
	                      })}},
	                 })}}},
	};
	const ChatHistorySyncService::RemoteCodexTranscript parsed =
	    ChatHistorySyncService::ParseRemoteCodexTranscript(result);
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.session_id, thread_id);
	UAM_ASSERT_EQ(parsed.directory, std::string(R"(C:\Users\example\project)"));
	UAM_ASSERT_EQ(parsed.messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(parsed.messages[0].role, MessageRole::User);
	UAM_ASSERT_EQ(parsed.messages[0].content, std::string("Review the service"));
	UAM_ASSERT_EQ(parsed.messages[0].provider, std::string(uam::provider_ids::kCodexCli));
	UAM_ASSERT_EQ(parsed.messages[1].role, MessageRole::Assistant);
	UAM_ASSERT_EQ(parsed.messages[1].content, std::string("Review complete\nDone."));
	UAM_ASSERT_EQ(parsed.messages[1].blocks.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[1].type, std::string("tool_call"));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[1].tool_call_id, std::string("tool-1"));
	UAM_ASSERT_EQ(parsed.messages[1].blocks[2].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(parsed.messages[1].tool_calls[0].result_text, std::string("match"));

	for (const char* field : {"/thread/turns/1/items/0/type", "/thread/turns/1/items/0/content/0/type", "/thread/turns/1/items/3/command", "/thread/turns/1/items/3/status"})
	{
		nlohmann::json malformed = result;
		malformed["thread"]["turns"].push_back(result["thread"]["turns"][0]);
		malformed[nlohmann::json::json_pointer(field)] = nlohmann::json::array();
		const auto rejected = ChatHistorySyncService::ParseRemoteCodexTranscript(malformed);
		UAM_ASSERT(!rejected.success);
		UAM_ASSERT(!rejected.error.empty());
		UAM_ASSERT(rejected.messages.empty());
	}

	UAM_ASSERT(!ChatHistorySyncService::ParseRemoteCodexTranscript(
	    nlohmann::json{{"thread", nullptr}}).success);
	nlohmann::json oversized = {
	    {"thread", {{"id", thread_id}, {"cwd", "/workspace"},
	                {"turns", nlohmann::json::array()}}},
	};
	for (int i = 0; i < 10001; ++i)
		oversized["thread"]["turns"].push_back(nullptr);
	UAM_ASSERT(!ChatHistorySyncService::ParseRemoteCodexTranscript(oversized).success);
#endif
}

UAM_TEST(CodexCurrentToolItemsRetainArgumentsAndResultsAcrossLiveSaveAndRemoteReload)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-tools");
	const std::string thread_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	const std::string large_result = "  first\n" + std::string(80000, 'x') + "\nC:\\new\\notes";
	const nlohmann::json items = nlohmann::json::array({
	    {{"id", "cmd"}, {"type", "commandExecution"}, {"status", "completed"}, {"command", "cat large.txt"}, {"cwd", "/workspace"}, {"aggregatedOutput", large_result}, {"exitCode", 0}},
	    {{"id", "patch"}, {"type", "fileChange"}, {"status", "completed"}, {"changes", nlohmann::json::array({{{"path", "main.cpp"}, {"diff", "- old\n+ new\n"}, {"kind", {{"type", "update"}}}}})}},
	    {{"id", "mcp"}, {"type", "mcpToolCall"}, {"status", "completed"}, {"tool", "inspect"}, {"server", "local"}, {"arguments", {{"path", R"(C:\new\notes)"}}}, {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", large_result}}})}, {"structuredContent", {{"count", 42}}}}}},
	    {{"id", "dynamic"}, {"type", "dynamicToolCall"}, {"status", "completed"}, {"tool", "custom"}, {"arguments", {{"value", 42}}}, {"contentItems", nlohmann::json::array({{{"type", "inputText"}, {"text", "Detailed result"}}})}, {"success", true}},
	    {{"id", "output"}, {"type", "functionCallOutput"}, {"name", "exec"}, {"output", "full tool output"}},
	    {{"id", "search"}, {"type", "webSearch"}, {"query", "docs"}, {"results", nlohmann::json::array({{{"url", "https://example.com"}, {"title", "Source"}}})}},
	    {{"id", "image"}, {"type", "imageView"}, {"path", "image.png"}},
	});
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "codex-tools";
	chat.provider_id = "codex-cli";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.codex_thread_id = thread_id;
	session.session_id = thread_id;
	session.processing = true;
	session.running = true;
	for (const nlohmann::json& item : items)
	{
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json{{"method", "item/completed"}, {"params", {{"threadId", thread_id}, {"item", item}}}}.dump()));
	}
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", thread_id}, {"turn", {{"status", "completed"}}}}}}.dump()));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls.size(), items.size());
	const ChatHistorySyncService::RemoteCodexTranscript parsed = ChatHistorySyncService::ParseRemoteCodexTranscript({{"thread", {{"id", thread_id}, {"cwd", "/workspace"}, {"turns", nlohmann::json::array({{{"items", items}}})}}}});
	UAM_ASSERT(parsed.success);
	UAM_ASSERT_EQ(parsed.messages[0].tool_calls.size(), items.size());
	for (std::size_t index = 0; index < items.size(); ++index)
	{
		const ToolCall& live = app.chats.front().messages[0].tool_calls[index];
		const ToolCall& restored = parsed.messages[0].tool_calls[index];
		UAM_ASSERT_EQ(live.args_json, restored.args_json);
		UAM_ASSERT_EQ(live.result_text, restored.result_text);
		UAM_ASSERT_EQ(live.status, restored.status);
		UAM_ASSERT_EQ(uam::StateSerializer::ToolCallContentForFrontend(session.tool_calls[index]), uam::StateSerializer::ToolCallContentForFrontend(restored));
	}
	UAM_ASSERT_EQ(parsed.messages[0].tool_calls[0].result_text, large_result);
	UAM_ASSERT_EQ(nlohmann::json::parse(parsed.messages[0].tool_calls[1].result_text)["changes"][0]["diff"], nlohmann::json("- old\n+ new\n"));
	UAM_ASSERT_EQ(nlohmann::json::parse(parsed.messages[0].tool_calls[2].result_text)["result"]["structuredContent"]["count"], nlohmann::json(42));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats.front()));
	const std::vector<ChatSession> reloaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(reloaded.size(), std::size_t{1});
	for (std::size_t index = 0; index < items.size(); ++index)
	{
		UAM_ASSERT_EQ(reloaded[0].messages[0].tool_calls[index].result_text, parsed.messages[0].tool_calls[index].result_text);
		UAM_ASSERT_EQ(reloaded[0].messages[0].tool_calls[index].args_json, parsed.messages[0].tool_calls[index].args_json);
	}
#endif
}

UAM_TEST(ForgetResolvedNativeSessionForChatClearsProviderSwitchResidualMapping)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-1";

	ChatHistorySyncService().ForgetResolvedNativeSessionForChat(app, chat.id);

	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.find(chat.id) == app.resolved_native_sessions_by_chat_id.end());
#endif
}

UAM_TEST(ClearStoppedCliTerminalAttachmentForChatClearsProviderSwitchResidualTerminalIdentity)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "opencode-session-1";
	terminal->terminal_id = "term-chat-opencode";
	terminal->should_launch = true;
	app.cli_terminals.push_back(std::move(terminal));

	uam::ClearStoppedCliTerminalAttachmentForChat(app, chat.id);

	UAM_ASSERT(uam::CliTerminalAttachedSessionId(*app.cli_terminals.front()).empty());
	UAM_ASSERT(uam::CliTerminalAttachedChatId(*app.cli_terminals.front()).empty());
	UAM_ASSERT(app.cli_terminals.front()->terminal_id.empty());
#endif
}

UAM_TEST(CopilotCliBuildsCommandsAndInteractiveArgv)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultCopilotProfile();
	AppSettings settings;
	settings.provider_extra_flags = "--debug";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "copilot-cli";
	chat.native_session_id = " 6a6f0f3b-1a0b-4a9c-8a01-111111111111 ";
	chat.model_id = " gpt-5.1 ";
	chat.approval_mode = " plan ";
	chat.reasoning_effort = " xhigh ";

	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(9));
	UAM_ASSERT_EQ(argv[0], std::string("copilot"));
	UAM_ASSERT_EQ(argv[1], std::string("--session-id"));
	UAM_ASSERT_EQ(argv[2], std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT_EQ(argv[3], std::string("--model"));
	UAM_ASSERT_EQ(argv[4], std::string("gpt-5.1"));
	UAM_ASSERT_EQ(argv[5], std::string("--effort"));
	UAM_ASSERT_EQ(argv[6], std::string("xhigh"));
	UAM_ASSERT_EQ(argv[7], std::string("--plan"));
	UAM_ASSERT_EQ(argv[8], std::string("--debug"));
	chat.reasoning_effort = " MAX ";
	const std::vector<std::string> max_effort_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(max_effort_argv[5], std::string("--effort"));
	UAM_ASSERT_EQ(max_effort_argv[6], std::string("max"));
	chat.reasoning_effort = "invalid";
	const std::vector<std::string> invalid_effort_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT(!uam::ranges::Contains(invalid_effort_argv, "--effort"));
	chat.reasoning_effort = " xhigh ";

	chat.approval_mode = "yolo";
	const std::vector<std::string> yolo_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT(!uam::ranges::Contains(yolo_argv, "--allow-all"));

	// UAM mediates per-chat YOLO in structured mode; it is never translated to terminal bypass flags.
	chat.approval_mode.clear();
	const std::vector<std::string> yolo_mode_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT(!uam::ranges::Contains(yolo_mode_argv, "--allow-all"));

	// PR-4: yolo off → --allow-all absent.
	const std::vector<std::string> no_yolo_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT(!uam::ranges::Contains(no_yolo_argv, "--allow-all"));

	chat.command_safety_tier = "yolo";
	const std::vector<std::string> chat_yolo_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT(!uam::ranges::Contains(chat_yolo_argv, "--allow-all"));

	AppSettings worker_settings;
	worker_settings.provider_extra_flags = "--no-auto-update --agent unsafe --plugin-dir injected";
	const std::vector<std::string> worker_argv = ProviderRuntimeRegistry::Resolve(profile).BuildWorkerArgv(profile, worker_settings, "review this change", " gpt-5.1 ");
	UAM_ASSERT_EQ(worker_argv, (std::vector<std::string>{
	                               "copilot",
	                               "-p",
	                               "review this change",
	                               "--model",
	                               "gpt-5.1",
	                               "--no-auto-update",
	                               "--allow-all-tools",
	                               "--available-tools=__uam_text_only_worker_no_tools_7f4938d1__",
	                               "--disable-builtin-mcps",
	                               "--no-custom-instructions",
	                               "--sync-session=off",
	                               "--disallow-temp-dir",
	                               "--silent",
	                           }));
	uam::AppState worker_app;
	uam::CliProviderVersionState& worker_version = worker_app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	worker_version.checked = true;
	worker_version.supported = true;
	const uam::ProviderWorkerInvocation worker_invocation = uam::BuildProviderWorkerInvocation(worker_app, profile, worker_settings, "review this change", "gpt-5.1", uam::ProviderWorkerPathMode::BasePath);
	const std::string& worker_command = worker_invocation.command_preview;
	UAM_ASSERT(uam::strings::Contains(worker_command, "copilot"));
	UAM_ASSERT(worker_invocation.isolated_working_directory != nullptr);
	UAM_ASSERT(uam::strings::Contains(uam::paths::Utf8PathString(*worker_invocation.isolated_working_directory), "uam-text-worker-"));
	UAM_ASSERT(!uam::strings::Contains(worker_command, "COPILOT_HOME="));
	UAM_ASSERT(!uam::strings::Contains(worker_command, "--agent"));
	UAM_ASSERT(!uam::strings::Contains(worker_command, "--plugin-dir"));

	// Copilot 1.0.103 removed --no-remote and renamed --no-remote-export; older CLIs reject --sync-session.
	const auto worker_flags = [&](std::string_view host_id)
	{
		return uam::BuildProviderWorkerInvocation(worker_app, profile, worker_settings, "review this change", "gpt-5.1", uam::ProviderWorkerPathMode::BasePath, nullptr, host_id).argv;
	};
	const auto uses_legacy_flags = [](const std::vector<std::string>& argv)
	{
		return uam::ranges::Contains(argv, std::string("--no-remote")) && uam::ranges::Contains(argv, std::string("--no-remote-export")) &&
		       !uam::ranges::Contains(argv, std::string("--sync-session=off"));
	};
	const auto uses_current_flags = [](const std::vector<std::string>& argv)
	{
		return uam::ranges::Contains(argv, std::string("--sync-session=off")) && !uam::ranges::Contains(argv, std::string("--no-remote")) &&
		       !uam::ranges::Contains(argv, std::string("--no-remote-export"));
	};
	UAM_ASSERT(uses_current_flags(worker_invocation.argv));
	worker_version.installed_version = "1.0.102";
	UAM_ASSERT(uses_legacy_flags(worker_flags({})));
	worker_version.installed_version = "1.0.103-0";
	UAM_ASSERT(uses_current_flags(worker_flags({})));
	worker_app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(uam::provider_ids::kCopilotCli, "ssh-old")].installed_version = "1.0.98";
	UAM_ASSERT(uses_legacy_flags(worker_flags("ssh-old")));
	UAM_ASSERT(uses_current_flags(worker_flags("ssh-unchecked")));

	const std::string long_prompt(12000, 'x');
	const uam::ProviderWorkerInvocation long_worker = uam::BuildProviderWorkerInvocation(worker_app, profile, worker_settings, long_prompt, "gpt-5.1", uam::ProviderWorkerPathMode::BasePath);
	UAM_ASSERT(long_worker.direct_process);
	UAM_ASSERT(long_worker.shell_command.empty());
	UAM_ASSERT_EQ(long_worker.standard_input, long_prompt);
	UAM_ASSERT(!uam::ranges::Contains(long_worker.argv, long_prompt));
	UAM_ASSERT(!uam::ranges::Contains(long_worker.argv, "-p"));
	UAM_ASSERT(long_worker.command_preview.find(long_prompt) == std::string::npos);

	worker_version.checked = false;
	worker_version.supported = false;
	std::string worker_error;
	UAM_ASSERT(uam::BuildProviderWorkerInvocation(worker_app, profile, worker_settings, "review this change", "gpt-5.1", uam::ProviderWorkerPathMode::BasePath, &worker_error).Empty());
	UAM_ASSERT(worker_error.find("Checking") != std::string::npos);

	worker_version.checked = true;
	worker_version.installed_version = "1.0.59";
	worker_error.clear();
	UAM_ASSERT(uam::BuildProviderWorkerInvocation(worker_app, profile, worker_settings, "review this change", "gpt-5.1", uam::ProviderWorkerPathMode::BasePath, &worker_error).Empty());
	UAM_ASSERT(worker_error.find("1.0.60 or newer") != std::string::npos);
#endif
}

UAM_TEST(CopilotTerminalLaunchIdentityIsPersistedAndReused)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	TempDir temp("uam-copilot-terminal-session");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ProviderProfile profile = ProviderProfileStore::DefaultCopilotProfile();

	ChatSession chat;
	chat.id = "chat-copilot-terminal";
	chat.provider_id = uam::provider_ids::kCopilotCli;

	std::string error;
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(app, chat, profile, uam::ResolveProviderInteractiveResumeId(app, chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(!chat.native_session_id.empty());
	UAM_ASSERT_EQ(ProviderRuntimeRegistry::Resolve(profile).ResolveInteractiveResumeId(app, chat), chat.native_session_id);
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	UAM_ASSERT_EQ(ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).OnAcpValidateResumeId(chat), chat.native_session_id);
#endif
	const std::string session_id = chat.native_session_id;

	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->native_session_id, session_id);

	const std::vector<std::string> argv = uam::BuildProviderInteractiveArgv(app, chat);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(argv[0], std::string("copilot"));
	UAM_ASSERT_EQ(argv[1], std::string("--session-id"));
	UAM_ASSERT_EQ(argv[2], session_id);
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(app, chat, profile, uam::ResolveProviderInteractiveResumeId(app, chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT_EQ(chat.native_session_id, session_id);

	ExecutionHost remote_host;
	remote_host.id = "owned-copilot-remote";
	ChatSession remote_chat;
	remote_chat.id = "chat-copilot-remote-owned";
	remote_chat.provider_id = uam::provider_ids::kCopilotCli;
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(app, remote_chat, profile, "", remote_host, &error));
	UAM_ASSERT(!remote_chat.native_session_id.empty());
	UAM_ASSERT(remote_chat.native_session_id != session_id);
	const std::optional<ChatSession> remote_saved = ChatRepository::LoadLocalChat(temp.root, remote_chat.id);
	UAM_ASSERT(remote_saved.has_value());
	UAM_ASSERT_EQ(remote_saved->native_session_id, remote_chat.native_session_id);
	const std::vector<std::string> remote_argv = ProviderRuntimeRegistry::Resolve(profile).BuildInteractiveArgv(profile, remote_chat, app.settings);
	UAM_ASSERT_EQ(remote_argv[1], std::string("--session-id"));
	UAM_ASSERT_EQ(remote_argv[2], remote_chat.native_session_id);

	ChatSession task_chat;
	task_chat.id = "chat-copilot-task";
	task_chat.provider_id = uam::provider_ids::kCopilotCli;
	task_chat.native_session_id = "task-opaque-id";
	app.resolved_native_sessions_by_chat_id[task_chat.id] = task_chat.native_session_id;
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(app, task_chat, profile, uam::ResolveProviderInteractiveResumeId(app, task_chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT_EQ(task_chat.native_session_id, std::string("task-opaque-id"));
	const std::vector<std::string> task_argv = uam::BuildProviderInteractiveArgv(app, task_chat);
	UAM_ASSERT_EQ(task_argv[1], std::string("--session-id"));
	UAM_ASSERT_EQ(task_argv[2], std::string("task-opaque-id"));

	uam::AppState pending_check_app;
	pending_check_app.data_root = temp.root;
	pending_check_app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli] = {};
	ChatSession pending_check_chat;
	pending_check_chat.id = "chat-copilot-checking";
	pending_check_chat.provider_id = uam::provider_ids::kCopilotCli;
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(pending_check_app, pending_check_chat, profile, uam::ResolveProviderInteractiveResumeId(pending_check_app, pending_check_chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT(error.find("Checking") != std::string::npos);
	UAM_ASSERT(pending_check_chat.native_session_id.empty());

	uam::CliProviderVersionState& outdated_state = pending_check_app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	outdated_state.checked = true;
	outdated_state.installed_version = "1.0.50";
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(pending_check_app, pending_check_chat, profile, uam::ResolveProviderInteractiveResumeId(pending_check_app, pending_check_chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT(error.find("1.0.60 or newer") != std::string::npos);
	UAM_ASSERT(pending_check_chat.native_session_id.empty());

	uam::AppState blocked_app;
	blocked_app.data_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_app.data_root, "blocked"));
	ChatSession blocked_chat;
	blocked_chat.id = "chat-copilot-blocked";
	blocked_chat.provider_id = uam::provider_ids::kCopilotCli;
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(profile).PrepareInteractiveSession(blocked_app, blocked_chat, profile, uam::ResolveProviderInteractiveResumeId(blocked_app, blocked_chat, profile), ExecutionHost{}, &error));
	UAM_ASSERT(blocked_chat.native_session_id.empty());
	UAM_ASSERT(!error.empty());
#endif
}

UAM_TEST(RuntimeHandoffRejectsBusyAcpAndStopsIdleAcpBeforeTerminalLaunch)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-runtime-handoff-acp";
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	session->processing = true;
	session->lifecycle_state = "processing";
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(app.acp_sessions.front()->running);

	app.acp_sessions.front()->processing = false;
	app.acp_sessions.front()->reconnect_pending = true;
	app.acp_sessions.front()->lifecycle_state = "ready";
	bool prepared = uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error);
	const std::chrono::steady_clock::time_point stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!prepared && error.empty() && std::chrono::steady_clock::now() < stop_deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		prepared = uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error);
	}
	UAM_ASSERT(prepared);
	UAM_ASSERT(!uam::AcpStopInProgress(app, chat.id));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT(!app.acp_sessions.front()->reconnect_pending);
}

UAM_TEST(RuntimeHandoffRejectsBusyTerminalAndStopsIdleOrShuttingTerminalBeforeAcp)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-runtime-handoff-terminal";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->running = true;
	terminal->should_launch = true;
	terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(terminal));

	std::string error;
	UAM_ASSERT(!uam::PrepareCliTerminalForAcpLaunch(app, chat.id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(app.cli_terminals.front()->running);

	app.cli_terminals.front()->lifecycle_state = uam::CliTerminalLifecycleState::ShuttingDown;
	UAM_ASSERT(uam::PrepareCliTerminalForAcpLaunch(app, chat.id, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(!app.cli_terminals.front()->running);
	UAM_ASSERT(!app.cli_terminals.front()->should_launch);

	app.cli_terminals.front()->running = true;
	app.cli_terminals.front()->should_launch = true;
	app.cli_terminals.front()->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	app.cli_terminals.front()->turn_state = uam::CliTerminalTurnState::Idle;
	app.cli_terminals.front()->last_error = "stale";
	UAM_ASSERT(uam::PrepareCliTerminalForAcpLaunch(app, chat.id, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(!app.cli_terminals.front()->running);
	UAM_ASSERT(!app.cli_terminals.front()->should_launch);
	UAM_ASSERT(app.cli_terminals.front()->last_error.empty());
}

UAM_TEST(CodexSessionIndexRejectsAmbiguousNewSessionsForMatchingCwd)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-index");
	ScopedEnvVar codex_home_env("CODEX_HOME", temp.root.string());
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile());
	const fs::path cwd = temp.root / "workspace";
	fs::create_directories(cwd);
	fs::create_directories(temp.root / "sessions" / "2026");

	const std::string old_id = "11111111-1111-4111-8111-111111111111";
	const std::string wrong_id = "22222222-2222-4222-8222-222222222222";
	const std::string match_id = "33333333-3333-4333-8333-333333333333";
	const std::string newer_match_id = "44444444-4444-4444-8444-444444444444";

	const auto index_line = [](const std::string& id, const std::string& updated_at)
	{
		const nlohmann::json line = {{"id", id}, {"updated_at", updated_at}};
		return "  " + line.dump() + "  \n";
	};

	const auto rollout_text = [](const std::string& id, const fs::path& rollout_cwd)
	{
		const nlohmann::json payload = {{"id", " " + id + " "}, {"cwd", " " + rollout_cwd.string() + " "}};
		const nlohmann::json line = {{"type", " session_meta "}, {"payload", payload}};
		return line.dump() + "\n";
	};

	const fs::path rollout_dir = temp.root / "sessions" / "2026";

	std::string index_text;
	index_text += "{not-json}\n";
	index_text += index_line(" " + newer_match_id + " ", " 2026-04-18T10:03:00Z ");
	index_text += index_line(old_id, "2026-04-18T10:00:00Z");
	index_text += index_line("not-a-session-id", "2026-04-18T10:05:00Z");
	index_text += index_line(wrong_id, "2026-04-18T10:04:00Z");
	index_text += index_line(match_id, "2026-04-18T10:02:00Z");
	index_text += index_line(" " + match_id + " ", "2026-04-18T10:06:00Z");
	index_text += index_line(old_id, "2026-04-18T10:07:00Z");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "session_index.jsonl", index_text));

	const fs::path wrong_rollout = rollout_dir / ("rollout-" + wrong_id + ".jsonl");
	const fs::path match_rollout = rollout_dir / ("rollout-" + match_id + ".jsonl");
	const fs::path newer_rollout = rollout_dir / ("rollout-" + newer_match_id + ".jsonl");
	UAM_ASSERT(uam::io::WriteTextFile(wrong_rollout, rollout_text(wrong_id, temp.root / "other")));
	UAM_ASSERT(uam::io::WriteTextFile(match_rollout, rollout_text(match_id, cwd)));
	UAM_ASSERT(uam::io::WriteTextFile(newer_rollout, rollout_text(newer_match_id, cwd)));

	const std::vector<std::string> indexed_ids = runtime.SnapshotInteractiveSessionIds();
	UAM_ASSERT_EQ(indexed_ids.size(), static_cast<std::size_t>(4));
	UAM_ASSERT(!uam::ranges::Contains(indexed_ids, "not-a-session-id"));

	const std::vector<std::string> before = {old_id};
	bool ambiguous = false;
	UAM_ASSERT_EQ(runtime.DiscoverInteractiveSessionId(before, cwd, &ambiguous), std::string(""));
	UAM_ASSERT(ambiguous);
	UAM_ASSERT_EQ(runtime.DiscoverInteractiveSessionId({old_id, newer_match_id}, cwd, &ambiguous), match_id);
	UAM_ASSERT(!ambiguous);
	UAM_ASSERT_EQ(runtime.DiscoverInteractiveSessionId({old_id, newer_match_id}, cwd), match_id);
	UAM_ASSERT_EQ(runtime.DiscoverInteractiveSessionId({old_id, match_id, newer_match_id}, cwd), std::string(""));
	const fs::path backup_rollout = match_rollout.string() + ".bak";
	fs::rename(match_rollout, backup_rollout);
	UAM_ASSERT(runtime.DiscoverInteractiveSessionId({old_id, newer_match_id}, cwd).empty());
	fs::rename(backup_rollout, match_rollout);
	UAM_ASSERT_EQ(runtime.DiscoverInteractiveSessionId({old_id, newer_match_id}, cwd), match_id);
#endif
}

UAM_TEST(NativeSessionLinkMatchesDraftByStrictTimestamp)
{
	NativeSessionLinkService linker;
	ChatSession local;
	local.id = "chat-1700000000000-draft";
	local.provider_id = "gemini-cli";

	ChatSession native;
	native.provider_id = "gemini-cli";
	native.native_session_id = "gemini-native-1";
	native.created_at = " 2023-11-14T22:13:20.000Z ";

	const std::optional<std::string> matched = linker.MatchNativeSessionIdForLocalDraft(local, {native});
	UAM_ASSERT(matched.has_value());
	UAM_ASSERT_EQ(matched.value(), std::string("gemini-native-1"));

	local.id = " chat-1700000000000-draft ";
	const std::optional<std::string> padded_match = linker.MatchNativeSessionIdForLocalDraft(local, {native});
	UAM_ASSERT(padded_match.has_value());
	UAM_ASSERT_EQ(padded_match.value(), std::string("gemini-native-1"));
}

UAM_TEST(NativeSessionLinkTrimsDraftChatIdsAtPublicBoundary)
{
	NativeSessionLinkService linker;
	UAM_ASSERT(linker.IsLocalDraftChatId(" chat-1700000000000-draft "));
	UAM_ASSERT(!linker.IsLocalDraftChatId(" native-session-id "));
}

UAM_TEST(NativeSessionLinkNormalizesCodexProviderAliasForThreadIds)
{
	NativeSessionLinkService linker;
	ChatSession chat;
	chat.provider_id = " CoDeX ";
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	UAM_ASSERT(linker.HasRealNativeSessionId(chat));
	UAM_ASSERT_EQ(linker.RealNativeSessionId(chat), std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));

	chat.native_session_id = "not-a-codex-thread-id";
	UAM_ASSERT(!linker.HasRealNativeSessionId(chat));
	UAM_ASSERT_EQ(linker.RealNativeSessionId(chat), std::string(""));
	for (const char* provider : {"gemini-cli", "opencode-cli", "claude-cli", "copilot-cli"})
	{
		chat.provider_id = provider;
		chat.native_session_id = " native-session-1 ";
		UAM_ASSERT_EQ(linker.RealNativeSessionId(chat), std::string("native-session-1"));
		chat.native_session_id = " chat-123-local-draft ";
		UAM_ASSERT(!linker.HasRealNativeSessionId(chat));
	}

}

UAM_TEST(NativeSessionLinkRejectsMalformedTimestampOnlyMatches)
{
	NativeSessionLinkService linker;
	ChatSession native;
	native.provider_id = "gemini-cli";
	native.native_session_id = "gemini-native-1";
	native.created_at = "2023-11-14T22:13:20.000Z";

	ChatSession signed_draft_id;
	signed_draft_id.id = "chat-+1700000000000-draft";
	signed_draft_id.provider_id = "gemini-cli";
	UAM_ASSERT(!linker.MatchNativeSessionIdForLocalDraft(signed_draft_id, {native}).has_value());

	ChatSession suffixed_zulu_time;
	suffixed_zulu_time.id = "chat-draft";
	suffixed_zulu_time.provider_id = "gemini-cli";
	suffixed_zulu_time.created_at = "2023-11-14T22:13:20.000Zjunk";
	UAM_ASSERT(!linker.MatchNativeSessionIdForLocalDraft(suffixed_zulu_time, {native}).has_value());

	ChatSession empty_fractional_zulu_time;
	empty_fractional_zulu_time.id = "chat-draft";
	empty_fractional_zulu_time.provider_id = "gemini-cli";
	empty_fractional_zulu_time.created_at = "2023-11-14T22:13:20.Z";
	UAM_ASSERT(!linker.MatchNativeSessionIdForLocalDraft(empty_fractional_zulu_time, {native}).has_value());
}

UAM_TEST(NativeSessionLinkCollectsNewSessionIdsOnlyOnce)
{
	NativeSessionLinkService linker;

	ChatSession first;
	first.provider_id = "gemini-cli";
	first.native_session_id = " native-new ";

	ChatSession duplicate = first;

	ChatSession existing;
	existing.provider_id = "gemini-cli";
	existing.native_session_id = "native-existing";

	const std::vector<std::string> collected = linker.CollectNewSessionIds({first, duplicate, existing}, {" native-existing "});
	UAM_ASSERT_EQ(collected.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(collected.front(), std::string("native-new"));
	UAM_ASSERT(linker.SessionIdExistsInLoadedChats({first}, "native-new"));
	UAM_ASSERT(linker.SessionIdExistsInLoadedChats({first}, " native-new "));
}

UAM_TEST(NativeSessionLinkPicksFirstNormalizedUnblockedSessionId)
{
	NativeSessionLinkService linker;
	const std::vector<std::string> candidates = {" ", " native-blocked ", " native-open "};

	UAM_ASSERT_EQ(linker.PickFirstUnblockedSessionId(candidates, {"native-blocked"}), std::string("native-open"));
	UAM_ASSERT_EQ(linker.PickFirstUnblockedSessionId({" native-blocked "}, {" native-blocked "}), std::string(""));
}

UAM_TEST(NativeSessionLinkMatchesAndBlocksNormalizedNativeSessionIds)
{
	NativeSessionLinkService linker;
	ChatSession local;
	local.id = "chat-1700000000000-draft";
	local.provider_id = "gemini-cli";

	ChatSession native;
	native.provider_id = "gemini-cli";
	native.native_session_id = " gemini-native-1 ";
	native.created_at = "2023-11-14T22:13:20.000Z";

	const std::optional<std::string> matched = linker.MatchNativeSessionIdForLocalDraft(local, {native});
	UAM_ASSERT(matched.has_value());
	UAM_ASSERT_EQ(matched.value(), std::string("gemini-native-1"));
	UAM_ASSERT(!linker.MatchNativeSessionIdForLocalDraft(local, {native}, {" gemini-native-1 "}).has_value());
}

UAM_TEST(AcpJsonRpcBuildersUseProtocolMethods)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("gemini-cli");
	uam::AcpSessionState session;
	std::string method;
	const nlohmann::json initialize = runtime.OnAcpBuildInitialize(session, 7);
	UAM_ASSERT_EQ(initialize.value("jsonrpc", ""), std::string("2.0"));
	UAM_ASSERT_EQ(initialize.value("id", 0), 7);
	UAM_ASSERT_EQ(initialize.value("method", ""), std::string("initialize"));
	UAM_ASSERT_EQ(initialize["params"].value("protocolVersion", 0), 1);

	ChatSession fresh_chat;
	fresh_chat.provider_id = "gemini-cli";
	const nlohmann::json session_new = runtime.OnAcpBuildSetupRequest(8, fresh_chat, "/tmp/project", false, method);
	UAM_ASSERT_EQ(method, std::string("session/new"));
	UAM_ASSERT_EQ(session_new.value("method", ""), std::string("session/new"));
	UAM_ASSERT_EQ(session_new["params"].value("cwd", ""), std::string("/tmp/project"));
	UAM_ASSERT(session_new["params"]["mcpServers"].is_array());

	ChatSession draft_chat;
	draft_chat.id = "chat-local";
	draft_chat.provider_id = "gemini-cli";
	draft_chat.native_session_id = "chat-local";
	draft_chat.native_session_id = runtime.OnAcpValidateResumeId(draft_chat);
	UAM_ASSERT(draft_chat.native_session_id.empty());
	const nlohmann::json draft_setup = runtime.OnAcpBuildSetupRequest(12, draft_chat, "/tmp/project", true, method);
	UAM_ASSERT_EQ(draft_setup.value("method", ""), std::string("session/new"));

	ChatSession native_chat = draft_chat;
	native_chat.id = "chat-local";
	native_chat.native_session_id = "native-session";
	native_chat.native_session_id = runtime.OnAcpValidateResumeId(native_chat);
	const nlohmann::json native_setup = runtime.OnAcpBuildSetupRequest(13, native_chat, "/tmp/project", true, method);
	UAM_ASSERT_EQ(method, std::string("session/load"));
	UAM_ASSERT_EQ(native_setup.value("method", ""), std::string("session/load"));
	UAM_ASSERT_EQ(native_setup["params"].value("sessionId", ""), std::string("native-session"));
	const nlohmann::json unsupported_load = runtime.OnAcpBuildSetupRequest(15, native_chat, "/tmp/project", false, method);
	UAM_ASSERT_EQ(method, std::string("session/new"));
	UAM_ASSERT_EQ(unsupported_load.value("method", ""), method);

	for (const std::string provider_id : {"gemini-cli", "copilot-cli", "opencode-cli"})
	{
		ChatSession fresh_chat;
		fresh_chat.provider_id = provider_id;
		std::string method;
		const nlohmann::json setup = ProviderRuntimeRegistry::ResolveById(provider_id).OnAcpBuildSetupRequest(14, fresh_chat, "/tmp/project", true, method);
		const std::string expected_method = provider_id == uam::provider_ids::kCopilotCli ? "session.create" : "session/new";
		UAM_ASSERT_EQ(method, expected_method);
		UAM_ASSERT_EQ(setup.value("method", ""), expected_method);
		if (provider_id == uam::provider_ids::kCopilotCli)
		{
			UAM_ASSERT_EQ(setup["params"].value("workingDirectory", ""), std::string("/tmp/project"));
			UAM_ASSERT(!setup["params"].contains("sessionId"));
		}
	}

	session.session_id = "sess-1";
	const nlohmann::json prompt = runtime.OnAcpBuildPrompt(session, 9, "hello", fresh_chat, method);
	UAM_ASSERT_EQ(method, std::string("session/prompt"));
	UAM_ASSERT_EQ(prompt.value("method", ""), std::string("session/prompt"));
	UAM_ASSERT_EQ(prompt["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(prompt["params"]["prompt"][0].value("type", ""), std::string("text"));
	UAM_ASSERT_EQ(prompt["params"]["prompt"][0].value("text", ""), std::string("hello"));

	const nlohmann::json set_mode = nlohmann::json::parse(uam::BuildAcpSetModeRequestForTests(10, "sess-1", "plan"));
	UAM_ASSERT_EQ(set_mode.value("method", ""), std::string("session/set_mode"));
	UAM_ASSERT_EQ(set_mode["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(set_mode["params"].value("modeId", ""), std::string("plan"));

	const nlohmann::json set_model = nlohmann::json::parse(uam::BuildAcpSetModelRequestForTests(11, "sess-1", "auto-gemini-3"));
	UAM_ASSERT_EQ(set_model.value("method", ""), std::string("session/set_model"));
	UAM_ASSERT_EQ(set_model["params"].value("sessionId", ""), std::string("sess-1"));
	UAM_ASSERT_EQ(set_model["params"].value("modelId", ""), std::string("auto-gemini-3"));
}

UAM_TEST(CodexAppServerRequestBuildersUseCodexProtocolMethods)
{
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	chat.model_id = "gpt-5.6";
	chat.reasoning_effort = "ultra";
	chat.service_tier = "fast";
	chat.service_tier_explicit = true;
	chat.approval_mode = "plan";

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	uam::AcpSessionState session;
	session.session_id = chat.native_session_id;
	std::string method;
	const nlohmann::json initialize = runtime.OnAcpBuildInitialize(session, 21);
	UAM_ASSERT_EQ(initialize.value("method", ""), std::string("initialize"));
	UAM_ASSERT(initialize["params"].contains("clientInfo"));
	UAM_ASSERT_EQ(initialize["params"]["clientInfo"].value("version", ""), std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT(initialize["params"]["capabilities"].is_object());
	UAM_ASSERT(initialize["params"]["capabilities"].value("experimentalApi", false));

	ChatSession invalid_resume_chat = chat;
	invalid_resume_chat.native_session_id = "chat-1";
	std::string setup_method;
	const nlohmann::json invalid_setup = runtime.OnAcpBuildSetupRequest(20, invalid_resume_chat, "/tmp/project", true, setup_method);
	UAM_ASSERT_EQ(setup_method, std::string("thread/start"));
	UAM_ASSERT_EQ(invalid_setup.value("method", ""), std::string("thread/start"));

	const nlohmann::json valid_setup = runtime.OnAcpBuildSetupRequest(20, chat, "/tmp/project", true, setup_method);
	UAM_ASSERT_EQ(setup_method, std::string("thread/resume"));
	UAM_ASSERT_EQ(valid_setup.value("method", ""), std::string("thread/resume"));
	UAM_ASSERT_EQ(valid_setup["params"].value("threadId", ""), chat.native_session_id);

	const nlohmann::json thread_start = runtime.OnAcpBuildSetupRequest(23, invalid_resume_chat, "/tmp/project", true, setup_method);
	UAM_ASSERT_EQ(thread_start.value("method", ""), std::string("thread/start"));
	UAM_ASSERT_EQ(thread_start["params"].value("cwd", ""), std::string("/tmp/project"));
	UAM_ASSERT_EQ(thread_start["params"].value("approvalPolicy", ""), std::string("on-request"));
	UAM_ASSERT_EQ(thread_start["params"].value("sandbox", ""), std::string("read-only"));
	UAM_ASSERT_EQ(thread_start["params"].value("approvalsReviewer", ""), std::string("user"));
	UAM_ASSERT_EQ(thread_start["params"].value("model", ""), std::string("gpt-5.6"));
	UAM_ASSERT(thread_start["params"].value("persistExtendedHistory", false));
	UAM_ASSERT_EQ(thread_start["params"].value("serviceName", ""), std::string("universal-agent-manager"));
	UAM_ASSERT(thread_start["params"]["experimentalRawEvents"] == false);
	UAM_ASSERT(!thread_start["params"].contains("threadId"));

	const nlohmann::json thread_resume = runtime.OnAcpBuildSetupRequest(24, chat, "/tmp/project", true, setup_method);
	UAM_ASSERT_EQ(thread_resume.value("method", ""), std::string("thread/resume"));
	UAM_ASSERT_EQ(thread_resume["params"].value("threadId", ""), chat.native_session_id);
	UAM_ASSERT_EQ(thread_resume["params"].value("approvalPolicy", ""), std::string("on-request"));
	UAM_ASSERT_EQ(thread_resume["params"].value("sandbox", ""), std::string("read-only"));
	UAM_ASSERT_EQ(thread_resume["params"].value("approvalsReviewer", ""), std::string("user"));
	UAM_ASSERT_EQ(thread_resume["params"].value("model", ""), std::string("gpt-5.6"));
	UAM_ASSERT(thread_resume["params"].value("persistExtendedHistory", false));
	UAM_ASSERT(!thread_resume["params"].contains("serviceName"));
	UAM_ASSERT(!thread_resume["params"].contains("experimentalRawEvents"));

	ChatSession yolo_chat = chat;
	yolo_chat.command_safety_tier = "yolo";
	yolo_chat.native_session_id.clear();
	const nlohmann::json yolo_thread_start = runtime.OnAcpBuildSetupRequest(241, yolo_chat, "/tmp/project", false, setup_method);
	UAM_ASSERT_EQ(yolo_thread_start["params"].value("approvalPolicy", ""), std::string("on-request"));
	UAM_ASSERT_EQ(yolo_thread_start["params"].value("sandbox", ""), std::string("read-only"));
	yolo_chat.native_session_id = chat.native_session_id;
	const nlohmann::json yolo_thread_resume = runtime.OnAcpBuildSetupRequest(242, yolo_chat, "/tmp/project", false, setup_method);
	UAM_ASSERT_EQ(yolo_thread_resume["params"].value("approvalPolicy", ""), std::string("on-request"));
	UAM_ASSERT_EQ(yolo_thread_resume["params"].value("sandbox", ""), std::string("read-only"));

	ChatSession default_model_chat = chat;
	default_model_chat.model_id.clear();
	default_model_chat.native_session_id.clear();
	const nlohmann::json default_model_thread_start = runtime.OnAcpBuildSetupRequest(240, default_model_chat, "/tmp/project", false, setup_method);
	UAM_ASSERT(!default_model_thread_start["params"].contains("model"));

	const nlohmann::json turn_start = runtime.OnAcpBuildPrompt(session, 25, "hello", chat, method);
	UAM_ASSERT_EQ(method, std::string("turn/start"));
	UAM_ASSERT_EQ(turn_start.value("method", ""), std::string("turn/start"));
	UAM_ASSERT_EQ(turn_start["params"].value("threadId", ""), chat.native_session_id);
	UAM_ASSERT_EQ(turn_start["params"]["input"][0].value("text", ""), std::string("hello"));
	UAM_ASSERT_EQ(turn_start["params"]["input"][0].value("type", ""), std::string("text"));
	UAM_ASSERT_EQ(turn_start["params"]["input"][0]["text_elements"], nlohmann::json::array());
	UAM_ASSERT_EQ(turn_start["params"].value("model", ""), std::string("gpt-5.6"));
	UAM_ASSERT_EQ(turn_start["params"].value("effort", ""), std::string("ultra"));
	UAM_ASSERT_EQ(turn_start["params"].value("serviceTier", ""), std::string("fast"));
	UAM_ASSERT_EQ(turn_start["params"]["collaborationMode"].value("mode", ""), std::string("plan"));
	UAM_ASSERT_EQ(turn_start["params"]["collaborationMode"]["settings"].value("model", ""), std::string("gpt-5.6"));
	UAM_ASSERT_EQ(turn_start["params"]["collaborationMode"]["settings"].value("reasoning_effort", ""), std::string("ultra"));

	ChatSession cleared_speed_chat = chat;
	cleared_speed_chat.service_tier.clear();
	const nlohmann::json cleared_speed_turn_start = runtime.OnAcpBuildPrompt(session, 253, "hello", cleared_speed_chat, method);
	UAM_ASSERT(cleared_speed_turn_start["params"].contains("serviceTier"));
	UAM_ASSERT(cleared_speed_turn_start["params"]["serviceTier"].is_null());
	cleared_speed_chat.service_tier_explicit = false;
	const nlohmann::json inherited_speed_turn_start = runtime.OnAcpBuildPrompt(session, 254, "hello", cleared_speed_chat, method);
	UAM_ASSERT(!inherited_speed_turn_start["params"].contains("serviceTier"));

	ChatSession active_model_chat = chat;
	active_model_chat.model_id.clear();
	session.current_model_id = "gpt-5.4";
	const nlohmann::json active_model_turn_start = runtime.OnAcpBuildPrompt(session, 250, "hello", active_model_chat, method);
	UAM_ASSERT(!active_model_turn_start["params"].contains("model"));
	UAM_ASSERT_EQ(active_model_turn_start["params"]["collaborationMode"].value("mode", ""), std::string("plan"));
	UAM_ASSERT_EQ(active_model_turn_start["params"]["collaborationMode"]["settings"].value("model", ""), std::string("gpt-5.4"));

	active_model_chat.approval_mode = "default";
	const nlohmann::json default_mode_turn_start = runtime.OnAcpBuildPrompt(session, 252, "hello", active_model_chat, method);
	UAM_ASSERT_EQ(default_mode_turn_start["params"]["collaborationMode"].value("mode", ""), std::string("default"));
	UAM_ASSERT_EQ(default_mode_turn_start["params"]["collaborationMode"]["settings"].value("model", ""), std::string("gpt-5.4"));

	session.current_model_id.clear();
	const nlohmann::json missing_model_turn_start = runtime.OnAcpBuildPrompt(session, 251, "hello", active_model_chat, method);
	UAM_ASSERT(!missing_model_turn_start["params"].contains("model"));
	UAM_ASSERT(!missing_model_turn_start["params"].contains("collaborationMode"));
	UAM_ASSERT_EQ(missing_model_turn_start["params"].value("effort", ""), std::string("ultra"));
	UAM_ASSERT_EQ(missing_model_turn_start["params"].value("serviceTier", ""), std::string("fast"));

	session.codex_turn_id = "turn-1";
	const nlohmann::json interrupt = runtime.OnAcpBuildCancel(session, 26, method);
	UAM_ASSERT_EQ(method, std::string("turn/interrupt"));
	UAM_ASSERT_EQ(interrupt["params"].value("threadId", ""), chat.native_session_id);
	UAM_ASSERT_EQ(interrupt.value("method", ""), std::string("turn/interrupt"));
	UAM_ASSERT_EQ(interrupt["params"].value("turnId", ""), std::string("turn-1"));
	session.codex_turn_id.clear();
	UAM_ASSERT(runtime.OnAcpBuildCancel(session, 27, method).is_null());
	UAM_ASSERT(method.empty());
	session.codex_turn_id = "turn-1";
	session.session_id.clear();
	method = "stale";
	UAM_ASSERT(runtime.OnAcpBuildCancel(session, 28, method).is_null());
	UAM_ASSERT(method.empty());
}

UAM_TEST(AcpStaleWaitDetectionFlagsLongInactivePermissionWaits)
{
	uam::AcpSessionState session;
	session.running = true;
	session.waiting_for_permission = true;
	session.pending_permission.request_id_json = "42";
	session.pending_permission.tool_call_id = "tool-1";
	session.wait_started_time_s = 10.0;
	session.last_runtime_activity_time_s = 10.0;

	UAM_ASSERT(!uam::UpdateAcpStaleWaitForTests(session, 60.0));
	UAM_ASSERT(!session.wait_is_stale);

	UAM_ASSERT(uam::UpdateAcpStaleWaitForTests(session, 131.0));
	UAM_ASSERT(session.wait_is_stale);
	UAM_ASSERT(session.wait_stale_reason.find("approval") != std::string::npos);
	UAM_ASSERT_EQ(session.diagnostics.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(session.diagnostics[0].reason, std::string("stale_permission_wait"));

	UAM_ASSERT(!uam::UpdateAcpStaleWaitForTests(session, 150.0));
	UAM_ASSERT_EQ(session.diagnostics.size(), static_cast<std::size_t>(1));
}

UAM_TEST(AcpStaleWaitDetectionRequiresRuntimeInactivity)
{
	uam::AcpSessionState session;
	session.running = true;
	session.waiting_for_user_input = true;
	session.pending_user_input.request_id_json = "input-1";
	session.pending_user_input.item_id = "question-1";
	session.wait_started_time_s = 10.0;
	session.last_runtime_activity_time_s = 100.0;

	UAM_ASSERT(!uam::UpdateAcpStaleWaitForTests(session, 150.0));
	UAM_ASSERT(!session.wait_is_stale);

	UAM_ASSERT(uam::UpdateAcpStaleWaitForTests(session, 221.0));
	UAM_ASSERT(session.wait_is_stale);
	UAM_ASSERT(session.wait_stale_reason.find("user input") != std::string::npos);
	UAM_ASSERT_EQ(session.diagnostics[0].reason, std::string("stale_user_input_wait"));

	session.waiting_for_user_input = false;
	UAM_ASSERT(uam::UpdateAcpStaleWaitForTests(session, 222.0));
	UAM_ASSERT(!session.wait_is_stale);
	UAM_ASSERT_EQ(session.wait_started_time_s, 0.0);
}

UAM_TEST(AcpReconnectBackoffIsBounded)
{
	UAM_ASSERT_EQ(uam::AcpReconnectDelaySecondsForTests(-1), 0.25);
	UAM_ASSERT_EQ(uam::AcpReconnectDelaySecondsForTests(0), 0.25);
	UAM_ASSERT_EQ(uam::AcpReconnectDelaySecondsForTests(1), 0.5);
	UAM_ASSERT_EQ(uam::AcpReconnectDelaySecondsForTests(2), 1.0);
	UAM_ASSERT_EQ(uam::AcpReconnectDelaySecondsForTests(99), 1.0);

	uam::AcpSessionState session;
	uam::ScheduleAcpReconnectForTests(session, 10.0);
	UAM_ASSERT(session.reconnect_pending);
	UAM_ASSERT_EQ(session.reconnect_attempts, 0);
	UAM_ASSERT_EQ(session.reconnect_not_before_time_s, 10.25);
	UAM_ASSERT_EQ(session.diagnostics.back().reason, std::string("scheduled"));

	session.reconnect_attempts = 2;
	uam::ScheduleAcpReconnectForTests(session, 20.0);
	UAM_ASSERT(session.reconnect_pending);
	UAM_ASSERT_EQ(session.reconnect_attempts, 2);
	UAM_ASSERT_EQ(session.reconnect_not_before_time_s, 21.0);

	session.reconnect_attempts = 3;
	uam::ScheduleAcpReconnectForTests(session, 30.0);
	UAM_ASSERT(!session.reconnect_pending);
	UAM_ASSERT_EQ(session.reconnect_not_before_time_s, 0.0);
	UAM_ASSERT_EQ(session.diagnostics.back().reason, std::string("exhausted"));
}

UAM_TEST(AcpStdoutBufferRejectsOversizedLines)
{
	uam::AcpSessionState session;
	UAM_ASSERT(uam::acp_detail::AppendAcpStdoutChunk(session, std::string(5 * 1024 * 1024, 'x')));
	UAM_ASSERT_EQ(session.stdout_buffer.size(), static_cast<std::size_t>(5 * 1024 * 1024));
	session.stdout_buffer.clear();
	UAM_ASSERT(uam::acp_detail::AppendAcpStdoutChunk(session, std::string(uam::acp_detail::kMaxAcpStdoutLineBytes, 'x')));
	UAM_ASSERT(!uam::acp_detail::AppendAcpStdoutChunk(session, "x"));
	UAM_ASSERT(session.stdout_buffer.empty());
}

UAM_TEST(AcpLaunchArgsIncludeSelectedModel)
{
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-1";

	const std::vector<std::string> default_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(default_argv.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(default_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(default_argv[1], std::string("--acp"));
	UAM_ASSERT_EQ(default_argv[2], std::string("--policy"));
	UAM_ASSERT_EQ(std::filesystem::path(default_argv[3]).filename().string(),
	    std::string("gemini-policy.toml"));

	chat.model_id = " flash ";
	const std::vector<std::string> selected_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(selected_argv.size(), static_cast<std::size_t>(6));
	UAM_ASSERT_EQ(selected_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(selected_argv[1], std::string("--acp"));
	UAM_ASSERT_EQ(selected_argv[2], std::string("--model"));
	UAM_ASSERT_EQ(selected_argv[3], std::string("flash"));
	UAM_ASSERT_EQ(selected_argv[4], std::string("--policy"));

	chat.approval_mode = " plan ";
	const std::vector<std::string> plan_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(plan_argv.size(), static_cast<std::size_t>(8));
	UAM_ASSERT_EQ(plan_argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(plan_argv[1], std::string("--acp"));
	UAM_ASSERT_EQ(plan_argv[2], std::string("--approval-mode"));
	UAM_ASSERT_EQ(plan_argv[3], std::string("plan"));
	UAM_ASSERT_EQ(plan_argv[4], std::string("--model"));
	UAM_ASSERT_EQ(plan_argv[5], std::string("flash"));
	UAM_ASSERT_EQ(plan_argv[6], std::string("--policy"));

	chat.approval_mode = " acceptEdits ";
	const std::vector<std::string> accept_edits_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(accept_edits_argv[2], std::string("--approval-mode"));
	UAM_ASSERT_EQ(accept_edits_argv[3], std::string("auto_edit"));

	chat.approval_mode = " plan ";
	const std::string detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", chat);
	UAM_ASSERT(detail.find("cwd=/tmp/project") != std::string::npos);
	UAM_ASSERT(detail.find("argv=gemini --acp --approval-mode plan --model flash --policy") != std::string::npos);
	UAM_ASSERT(detail.find("nativeSessionId=native-1") != std::string::npos);

	ChatSession codex_chat;
	codex_chat.id = "codex-chat";
	codex_chat.provider_id = " CoDeX ";
	codex_chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	codex_chat.model_id = "gpt-5.4";
	const std::vector<std::string> codex_argv = uam::BuildAcpLaunchArgvForTests(codex_chat);
	UAM_ASSERT_EQ(codex_argv.size(), static_cast<std::size_t>(16));
	UAM_ASSERT_EQ(codex_argv[0], std::string("codex"));
	UAM_ASSERT_EQ(codex_argv[1], std::string("--disable"));
	UAM_ASSERT_EQ(codex_argv[2], std::string("computer_use"));
	UAM_ASSERT_EQ(codex_argv[13], std::string("app-server"));
	UAM_ASSERT_EQ(codex_argv[14], std::string("--listen"));
	UAM_ASSERT_EQ(codex_argv[15], std::string("stdio://"));
	const std::string codex_detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", codex_chat);
	UAM_ASSERT(codex_detail.find("argv=codex --disable computer_use") != std::string::npos);
	UAM_ASSERT(codex_detail.find("app-server --listen stdio://") != std::string::npos);
	UAM_ASSERT(codex_detail.find("nativeSessionId=6a6f0f3b-1a0b-4a9c-8a01-111111111111") != std::string::npos);

	ChatSession claude_chat;
	claude_chat.id = "claude-chat";
	claude_chat.provider_id = "claude-cli";
	claude_chat.native_session_id = "claude-session-2";
	claude_chat.model_id = "sonnet";
	claude_chat.approval_mode = "plan";
	const std::vector<std::string> claude_argv = uam::BuildAcpLaunchArgvForTests(claude_chat);
	UAM_ASSERT_EQ(claude_argv.size(), static_cast<std::size_t>(19));
	UAM_ASSERT_EQ(claude_argv[0], std::string("claude"));
	UAM_ASSERT_EQ(claude_argv[1], std::string("-p"));
	UAM_ASSERT_EQ(claude_argv[2], std::string("--output-format"));
	UAM_ASSERT_EQ(claude_argv[3], std::string("stream-json"));
	UAM_ASSERT_EQ(claude_argv[4], std::string("--input-format"));
	UAM_ASSERT_EQ(claude_argv[5], std::string("stream-json"));
	UAM_ASSERT_EQ(claude_argv[6], std::string("--verbose"));
	UAM_ASSERT_EQ(claude_argv[7], std::string("--permission-prompt-tool"));
	UAM_ASSERT_EQ(claude_argv[8], std::string("stdio"));
	UAM_ASSERT_EQ(claude_argv[9], std::string("--permission-mode"));
	UAM_ASSERT_EQ(claude_argv[10], std::string("plan"));
	UAM_ASSERT_EQ(claude_argv[11], std::string("--model"));
	UAM_ASSERT_EQ(claude_argv[12], std::string("sonnet"));
	UAM_ASSERT_EQ(claude_argv[13], std::string("--resume"));
	UAM_ASSERT_EQ(claude_argv[14], std::string("claude-session-2"));
	UAM_ASSERT_EQ(claude_argv[15], std::string("--mcp-config"));
	UAM_ASSERT(nlohmann::json::parse(claude_argv[16])["mcpServers"].contains("uam-computer"));
	UAM_ASSERT_EQ(claude_argv[17], std::string("--allowedTools"));
	UAM_ASSERT_EQ(claude_argv[18], std::string("mcp__uam-computer__computer_observe,mcp__uam-computer__computer_action"));
	const std::string claude_detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", claude_chat);
	UAM_ASSERT(claude_detail.find("argv=claude -p --output-format stream-json --input-format stream-json --verbose --permission-prompt-tool stdio --permission-mode plan --model sonnet --resume claude-session-2") != std::string::npos);

	ChatSession opencode_chat;
	opencode_chat.id = "opencode-chat";
	opencode_chat.provider_id = " open-code ";
	opencode_chat.native_session_id = "opencode-session-1";
	const std::vector<std::string> opencode_argv = uam::BuildAcpLaunchArgvForTests(opencode_chat);
	UAM_ASSERT_EQ(opencode_argv.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(opencode_argv[0], std::string("opencode"));
	UAM_ASSERT_EQ(opencode_argv[1], std::string("acp"));
	const std::string opencode_detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", opencode_chat);
	UAM_ASSERT(opencode_detail.find("argv=opencode acp") != std::string::npos);
	UAM_ASSERT(opencode_detail.find("nativeSessionId=opencode-session-1") != std::string::npos);

	uam::AppState opencode_app;
	ChatSession opencode_resolved_chat = opencode_chat;
	opencode_resolved_chat.native_session_id = "stale-opencode-session";
	opencode_app.resolved_native_sessions_by_chat_id[opencode_resolved_chat.id] = "resolved-opencode-session";
	const std::string resolved_opencode_detail = uam::BuildAcpLaunchDetailForTests(opencode_app, "/tmp/project", opencode_resolved_chat);
	UAM_ASSERT(resolved_opencode_detail.find("argv=opencode acp") != std::string::npos);
	UAM_ASSERT(resolved_opencode_detail.find("nativeSessionId=resolved-opencode-session") != std::string::npos);

	ChatSession copilot_chat;
	copilot_chat.id = "copilot-chat";
	copilot_chat.provider_id = "copilot-cli";
	copilot_chat.native_session_id = "copilot-session-1";
	copilot_chat.reasoning_effort = "max";
	const std::vector<std::string> copilot_argv = uam::BuildAcpLaunchArgvForTests(copilot_chat);
	UAM_ASSERT_EQ(copilot_argv.size(), static_cast<std::size_t>(7));
	UAM_ASSERT_EQ(copilot_argv[0], std::string("copilot"));
	UAM_ASSERT_EQ(copilot_argv[1], std::string("--headless"));
	UAM_ASSERT_EQ(copilot_argv[2], std::string("--stdio"));
	UAM_ASSERT_EQ(copilot_argv[3], std::string("--no-auto-update"));
	UAM_ASSERT_EQ(copilot_argv[4], std::string("--effort"));
	UAM_ASSERT_EQ(copilot_argv[5], std::string("max"));
	UAM_ASSERT_EQ(copilot_argv[6], std::string("--allow-tool=uam-computer(computer_observe),uam-computer(computer_action)"));
	const std::string copilot_detail = uam::BuildAcpLaunchDetailForTests("/tmp/project", copilot_chat);
	UAM_ASSERT(copilot_detail.find("argv=copilot --headless --stdio --no-auto-update --effort max") != std::string::npos);
	UAM_ASSERT(copilot_detail.find("nativeSessionId=copilot-session-1") != std::string::npos);
}

UAM_TEST(ProviderCancelStrategyUsesWireMessageOrStopFallback)
{
	uam::AcpSessionState session;
	session.session_id = "session-1";
	session.codex_turn_id = "turn-1";
	std::string method;

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	const nlohmann::json claude_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kClaudeCli).OnAcpBuildCancel(session, 1, method);
	UAM_ASSERT_EQ(claude_cancel["request"]["subtype"], nlohmann::json("interrupt"));
	UAM_ASSERT_EQ(method, std::string("claude/interrupt"));
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const nlohmann::json gemini_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kGeminiCli).OnAcpBuildCancel(session, 2, method);
	UAM_ASSERT(gemini_cancel.is_object());
	UAM_ASSERT_EQ(gemini_cancel.value("method", ""), std::string("session/cancel"));
	UAM_ASSERT(method.empty());
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const nlohmann::json codex_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).OnAcpBuildCancel(session, 3, method);
	UAM_ASSERT(codex_cancel.is_object());
	UAM_ASSERT_EQ(codex_cancel.value("method", ""), std::string("turn/interrupt"));
	UAM_ASSERT_EQ(codex_cancel.value("id", 0), 3);
	UAM_ASSERT_EQ(method, std::string("turn/interrupt"));
	session.codex_turn_id.clear();
	const nlohmann::json deferred_codex_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCodexCli).OnAcpBuildCancel(session, 31, method);
	UAM_ASSERT(deferred_codex_cancel.is_null());
	UAM_ASSERT(method.empty());
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const nlohmann::json opencode_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kOpenCodeCli).OnAcpBuildCancel(session, 4, method);
	UAM_ASSERT(opencode_cancel.is_object());
	UAM_ASSERT_EQ(opencode_cancel.value("method", ""), std::string("session/cancel"));
	UAM_ASSERT(method.empty());
#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	const nlohmann::json copilot_cancel = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli).OnAcpBuildCancel(session, 5, method);
	UAM_ASSERT(copilot_cancel.is_object());
	UAM_ASSERT_EQ(copilot_cancel.value("method", ""), std::string("session.abort"));
	UAM_ASSERT_EQ(method, std::string("session.abort"));
#endif
}

UAM_TEST(AcpLaunchArgsUseResolvedProviderForBlankOpenCodeChat)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = uam::provider_ids::kOpenCodeCli;

	ChatSession chat;
	chat.id = "chat-opencode-blank";
	chat.provider_id.clear();
	chat.native_session_id = "stale-session";
	app.resolved_native_sessions_by_chat_id[chat.id] = "resolved-opencode-session";

	const std::string detail = uam::BuildAcpLaunchDetailForTests(app, "/tmp/project", chat);
	UAM_ASSERT(detail.find("argv=opencode acp") != std::string::npos);
	UAM_ASSERT(detail.find("nativeSessionId=resolved-opencode-session") != std::string::npos);
#endif
}

UAM_TEST(ClaudeStreamJsonMessagesUpdateChatAndSession)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "claude-cli";
	chat.approval_mode = "plan";
	chat.native_session_id = "claude-session-old";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	uam::AcpSessionState session;
	session.chat_id = "chat-1";
	session.provider_id = "claude-cli";
	session.protocol_kind = "claude-code-stream-json";
	session.running = true;
	session.initialized = true;
	session.session_ready = true;
	session.processing = true;
	session.lifecycle_state = "processing";
	session.queued_prompt = "hello";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"system","subtype":"init","session_id":"claude-session-3","model":"sonnet","permissionMode":"plan"})"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("claude-session-3"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at("chat-1"), std::string("claude-session-3"));
	UAM_ASSERT_EQ(session.current_model_id, std::string("sonnet"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"assistant","session_id":"claude-session-3","message":{"role":"assistant","content":[{"type":"text","text":"Working on it."},{"type":"tool_use","id":"tool-1","name":"Read","input":{"file_path":"README.md"}}]}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("Working on it."));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls[0].id, std::string("tool-1"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"user","session_id":"claude-session-3","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"tool-1","content":[{"type":"text","text":"done"}]}]}})"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls[0].result_text, std::string("done"));
	UAM_ASSERT_EQ(nlohmann::json::parse(app.chats.front().messages[0].tool_calls[0].args_json)["file_path"], nlohmann::json("README.md"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"result","subtype":"success","is_error":false,"session_id":"claude-session-3","result":"Finished.","total_cost_usd":0.1})"));
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("ready"));
#endif
}

UAM_TEST(ClaudeControlHandshakeDiscoversModelsAndRoutesPermissionsAndQuestions)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-control");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(temp.root);
	ChatSession chat;
	chat.id = "claude-control";
	chat.provider_id = "claude-cli";
	chat.command_safety_tier = "off";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	// Simulated launch captures the same catalog ownership as StartAcpProcessForChat.
	session.model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey(chat.provider_id, chat.workspace_directory);
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	session.processing = true;
	session.protocol_kind = "claude-code-stream-json";
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	const nlohmann::json initialize = runtime.OnAcpBuildInitialize(session, 7);
	UAM_ASSERT_EQ(initialize["request"]["subtype"], nlohmann::json("initialize"));
	UAM_ASSERT(!session.initialized);
	session.initialize_request_id = 7;
	session.pending_request_methods[7] = "initialize";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_response","response":{"subtype":"success","request_id":"999","response":{}}})"));
	UAM_ASSERT(!session.initialized);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_response","response":{"subtype":"success","request_id":"7","response":{"models":[{"value":"opus","displayName":"Opus","description":"Provider model"},{"value":"sonnet","displayName":"Sonnet"}]}}})"));
	UAM_ASSERT(session.initialized);
	UAM_ASSERT_EQ(session.initialize_request_id, 0);
	UAM_ASSERT_EQ(session.available_models.size(), std::size_t{2});
	UAM_ASSERT_EQ(session.available_models[0].id, std::string("opus"));
	UAM_ASSERT_EQ(app.provider_model_catalog->GetCachedProviderModels(chat.provider_id, chat.workspace_directory).size(), std::size_t{2});
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeProvider(ProviderProfileStore::DefaultClaudeProfile())["structuredPermissionControl"], nlohmann::json("uam"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"permission-1","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"bash-1","input":{"command":"printf '%s' 'C:\\new\\notes'","timeout":1000}}})"));
	UAM_ASSERT(session.waiting_for_permission);
	UAM_ASSERT_EQ(session.pending_permission.kind, std::string("execute"));
	const nlohmann::json allow = runtime.OnAcpBuildPermissionResponse(session, "allow", false);
	UAM_ASSERT_EQ(allow["response"]["request_id"], nlohmann::json("permission-1"));
	UAM_ASSERT_EQ(allow["response"]["response"]["updatedInput"], nlohmann::json::parse(session.pending_permission.provider_input_json));
	UAM_ASSERT_EQ(runtime.OnAcpBuildPermissionResponse(session, "deny", false)["response"]["response"]["behavior"], nlohmann::json("deny"));
	UAM_ASSERT_EQ(runtime.OnAcpBuildPermissionResponse(session, "allow", true)["response"]["response"]["behavior"], nlohmann::json("deny"));
	UAM_ASSERT_EQ(runtime.OnAcpBuildPermissionResponse(session, "unknown", false)["response"]["response"]["behavior"], nlohmann::json("deny"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"permission-2","request":{"subtype":"can_use_tool","tool_name":"Write","tool_use_id":"write-1","input":{"file_path":"README.md","content":"text"}}})"));
	UAM_ASSERT_EQ(session.queued_permissions.size(), std::size_t{1});
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_cancel_request","request_id":"permission-2"})"));
	UAM_ASSERT(session.queued_permissions.empty());
	UAM_ASSERT_EQ(session.pending_permission.tool_call_id, std::string("bash-1"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"question-1","request":{"subtype":"can_use_tool","tool_name":"AskUserQuestion","tool_use_id":"ask-1","input":{"questions":[{"header":"Scope","question":"Which scope?","multiSelect":false,"options":[{"label":"Focused","description":"One change"}]}]}}})"));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT_EQ(session.pending_user_input.questions.size(), std::size_t{1});
	UAM_ASSERT(!session.pending_user_input.questions.front().is_multiple);
	const nlohmann::json answer = runtime.OnAcpBuildUserInputResponse(session, {{"0", {"Focused"}}});
	UAM_ASSERT_EQ(answer["response"]["request_id"], nlohmann::json("question-1"));
	UAM_ASSERT_EQ(answer["response"]["response"]["updatedInput"]["answers"]["Which scope?"], nlohmann::json("Focused"));
	UAM_ASSERT(answer["response"]["response"]["updatedInput"].contains("questions"));
	UAM_ASSERT_EQ(runtime.OnAcpBuildUserInputResponse(session, {})["response"]["response"]["behavior"], nlohmann::json("deny"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_cancel_request","request_id":"question-1"})"));
	UAM_ASSERT(!session.waiting_for_user_input);
	UAM_ASSERT(session.waiting_for_permission);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"question-multiple","request":{"subtype":"can_use_tool","tool_name":"AskUserQuestion","tool_use_id":"ask-multiple","input":{"questions":[{"question":"Which checks?","multiSelect":true,"options":[{"label":"Focused"},{"label":"Build"}]}]}}})"));
	UAM_ASSERT(session.pending_user_input.questions.front().is_multiple);
	const nlohmann::json multiple_answer = runtime.OnAcpBuildUserInputResponse(session, {{"0", {"Focused", "Build", "Custom check"}}});
	UAM_ASSERT_EQ(multiple_answer["response"]["response"]["updatedInput"]["answers"]["Which checks?"], nlohmann::json("Focused, Build, Custom check"));
	UAM_ASSERT_EQ(multiple_answer["response"]["response"]["updatedInput"]["questions"][0]["multiSelect"], nlohmann::json(true));

#endif
}

UAM_TEST(ClaudeConcurrentQuestionAndPermissionKeepRemainingWaitActive)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const bool permission_first : {false, true})
	{
		TempDir temp("uam-claude-concurrent-input");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "claude-concurrent";
		chat.provider_id = "claude-cli";
		chat.command_safety_tier = "off";
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& session = *owned;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		session.processing = true;
		session.session_ready = true;
#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd", "/C", "more > NUL"};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
		std::string error;
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &error));
		session.running = true;
		app.acp_sessions.push_back(std::move(owned));
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"permission-1","request":{"subtype":"can_use_tool","tool_name":"Write","tool_use_id":"write-1","input":{"file_path":"file.txt","content":"test"}}})"));
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"question-1","request":{"subtype":"can_use_tool","tool_name":"AskUserQuestion","tool_use_id":"ask-1","input":{"questions":[{"question":"Which scope?","options":[{"label":"Focused"}]}]}}})"));
		const bool resolved = permission_first
		    ? uam::ResolveAcpPermission(app, chat.id, session.pending_permission.request_id_json, "allow", false, &error)
		    : uam::ResolveAcpUserInput(app, chat.id, session.pending_user_input.request_id_json, {{"0", {"Focused"}}}, &error);
		const std::string lifecycle = session.lifecycle_state;
		const bool still_waiting = permission_first ? session.waiting_for_user_input : session.waiting_for_permission;
		process.StopStdioProcess(session, true);
		process.CloseStdioProcessHandles(session);
		UAM_ASSERT(resolved);
		UAM_ASSERT(still_waiting);
		UAM_ASSERT_EQ(lifecycle, std::string(permission_first ? uam::acp_detail::kAcpLifecycleWaitingUserInput : uam::acp_detail::kAcpLifecycleWaitingPermission));
	}
#endif
}

UAM_TEST(ClaudeInstalledCliHandshakeSmoke)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	// Opt-in protocol check; starts only the selected CLI, with no inference or saved session.
	const char* executable = std::getenv("UAM_TEST_CLAUDE_CLI");
	if (executable == nullptr || *executable == '\0')
		return;
	TempDir temp("uam-claude-installed-cli");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(temp.root);
	ChatSession chat;
	chat.id = "claude-smoke";
	chat.provider_id = "claude-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	// Simulated launch captures the same catalog ownership as StartAcpProcessForChat.
	session.model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey(chat.provider_id, chat.workspace_directory);
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
	argv.front() = executable;
	argv.insert(argv.end(), {"--no-session-persistence", "--strict-mcp-config", "--mcp-config", R"({"mcpServers":{}})", "--setting-sources", ""});
	std::string error;
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &error));
	session.running = true;
	const bool sent = uam::acp_detail::SendInitialize(session, &error);
	for (int attempt = 0; sent && !session.initialized && attempt < 1000; ++attempt)
	{
		(void)uam::acp_detail::DrainStdout(app, session, app.chats.front(), nullptr);
		(void)uam::acp_detail::DrainStderr(app, session, app.chats.front());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool ready = session.initialized && uam::acp_detail::SendSessionSetupIfReady(app, session, app.chats.front());
	process.StopStdioProcess(session, true);
	process.CloseStdioProcessHandles(session);
	UAM_ASSERT(sent);
	UAM_ASSERT(ready);
	UAM_ASSERT(session.session_ready);
	UAM_ASSERT(!session.available_models.empty());
	UAM_ASSERT_EQ(app.provider_model_catalog->GetCachedProviderModels(chat.provider_id, chat.workspace_directory).size(), session.available_models.size());
	UAM_ASSERT(app.chats.front().messages.empty());
#endif
}

UAM_TEST(ClaudeFollowupSettingsRestartFailureDoesNotAcceptAnUnsentTurn)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const bool change_model : {false, true})
	{
		TempDir temp("uam-claude-followup-restart");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		// A missing destination must reject restart without stopping the captured local transport.
		app.settings.execution_hosts.clear();
		ChatSession chat;
		chat.id = "claude-followup";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = "missing-fixture-host";
		chat.workspace_directory = temp.root.string();
		chat.model_id = "opus";
		chat.approval_mode = "default";
		app.chats.push_back(chat);
		auto owned = std::make_unique<uam::AcpSessionState>();
		owned->chat_id = chat.id;
		owned->provider_id = chat.provider_id;
		owned->process_execution_host_id = "local";
		owned->protocol_kind = "claude-code-stream-json";
		owned->running = true;
		owned->initialized = true;
		owned->session_ready = true;
		owned->session_id = "fixture-session";
		owned->current_model_id = change_model ? "claude-opus-5-5" : "opus";
		owned->current_mode_id = change_model ? "default" : "plan";
#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", "more > NUL"};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
		std::string error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*owned, temp.root, argv, &error));
		uam::AcpSessionState& session = *owned;
		app.acp_sessions.push_back(std::move(owned));
		struct Cleanup
		{
			uam::AppState& app;
			~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
		} cleanup{app};
		UAM_ASSERT(!uam::SendAcpPrompt(app, chat.id, "Follow up", &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT(session.running);
		UAM_ASSERT(!session.processing);
		UAM_ASSERT(session.queued_prompt.empty());
		UAM_ASSERT(app.chats.front().messages.empty());
	}
#endif
}

UAM_TEST(ClaudeFollowupRestartsAndWritesPromptAfterSettingsChange)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && !defined(_WIN32)
	for (const bool change_model : {false, true})
	{
		TempDir temp("uam-claude-followup-delivery");
		const fs::path shim = temp.root / "claude";
		UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\nexec /bin/cat >> received.jsonl\n"));
		fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
		const char* previous_path = std::getenv("PATH");
		ScopedEnvVar path_env("PATH", temp.root.string() + ":" + (previous_path == nullptr ? "" : previous_path));
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
		app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
		app.provider_model_catalog->Initialize(temp.root);
		app.runtime_cli_versions_by_provider_id["claude-cli"].checked = true;
		app.runtime_cli_versions_by_provider_id["claude-cli"].supported = true;
		ChatSession chat;
		chat.id = "claude-followup-delivery";
		chat.provider_id = "claude-cli";
		chat.workspace_directory = temp.root.string();
		chat.model_id = "opus";
		chat.approval_mode = "default";
		app.chats.push_back(chat);
		auto owned = std::make_unique<uam::AcpSessionState>();
		owned->chat_id = chat.id;
		owned->provider_id = chat.provider_id;
		// Production launch captures the executing host before any restart.
		owned->process_execution_host_id = chat.execution_host_id;
		owned->protocol_kind = "claude-code-stream-json";
		owned->running = true;
		owned->initialized = true;
		owned->session_ready = true;
		owned->session_id = "fixture-session";
		owned->current_model_id = change_model ? "claude-opus-5-5" : "opus";
		owned->current_mode_id = change_model ? "default" : "plan";
		std::string error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*owned, temp.root, {"/bin/sh", "-c", "cat >/dev/null"}, &error));
		uam::AcpSessionState& session = *owned;
		app.acp_sessions.push_back(std::move(owned));
		struct Cleanup
		{
			uam::AppState& app;
			~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
		} cleanup{app};
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Followup delivery sentinel", &error));
		UAM_ASSERT(session.running);
		UAM_ASSERT(session.initialize_request_id != 0);
		const nlohmann::json response = {{"type", "control_response"}, {"response", {
		    {"subtype", "success"}, {"request_id", std::to_string(session.initialize_request_id)},
		    {"response", {{"models", nlohmann::json::array({{{"value", "opus"}}})}}}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), response.dump()));
		std::string received;
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			(void)uam::PollAllAcpSessions(app);
			(void)uam::io::TryReadTextFile(temp.root / "received.jsonl", received);
			if (received.find("Followup delivery sentinel") != std::string::npos) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(session.running);
		UAM_ASSERT(session.queued_prompt.empty());
		UAM_ASSERT(!app.chats.front().messages.front().acp_prompt_not_sent);
		const std::size_t sent = received.find("Followup delivery sentinel");
		UAM_ASSERT(sent != std::string::npos);
		UAM_ASSERT(received.find("Followup delivery sentinel", sent + 1) == std::string::npos);
		if (change_model)
		{
			// Restart an unsent batch whose image belongs to its first, rather than last, message.
			IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
			process.StopStdioProcess(session, true);
			process.CloseStdioProcessHandles(session);
			const fs::path image = temp.root / "batch.png";
			std::string batch_image;
			UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", batch_image));
			UAM_ASSERT(uam::io::WriteBinaryFile(image, batch_image));
			Message first;
			first.role = MessageRole::User;
			first.content = "Image batch first";
			first.acp_prompt_not_sent = true;
			first.attachments.push_back(MessageAttachment{"image-1", "batch.png", "image", "image/png", "batch.png"});
			app.chats.front().messages.push_back(std::move(first));
			app.chats.front().messages.push_back({.role = MessageRole::User, .content = "Image batch last", .acp_prompt_not_sent = true});
			session.running = false;
			session.processing = true;
			session.queued_prompt = "Recovery batch sentinel";
			session.turn_first_user_message_index = 1;
			session.turn_user_message_index = 2;
			UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
			UAM_ASSERT_EQ(session.turn_first_user_message_index, 1);
			UAM_ASSERT_EQ(session.turn_user_message_index, 2);
			nlohmann::json recovered_response = response;
			recovered_response["response"]["request_id"] = std::to_string(session.initialize_request_id);
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), recovered_response.dump()));
			for (int attempt = 0; attempt < 200; ++attempt)
			{
				uam::FlushPendingChatSaves(app);
				(void)uam::PollAllAcpSessions(app);
				(void)uam::io::TryReadTextFile(temp.root / "received.jsonl", received);
				if (received.find("Recovery batch sentinel") != std::string::npos) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			const std::size_t recovery = received.find("Recovery batch sentinel");
			UAM_ASSERT(recovery != std::string::npos);
			UAM_ASSERT(received.find(uam::base64::Encode(batch_image), recovery) != std::string::npos);
			UAM_ASSERT(!app.chats.front().messages[1].acp_prompt_not_sent);
			UAM_ASSERT(!app.chats.front().messages[2].acp_prompt_not_sent);
		}

	}
#endif
}

UAM_TEST(ClaudeThinkingErrorsAndInterruptKeepTranscriptAndSessionConsistent)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-result");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "claude-result";
	chat.provider_id = "claude-cli";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.chat_id = chat.id;
	session.running = true;
	session.processing = true;
	session.session_ready = true;
	session.protocol_kind = "claude-code-stream-json";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":"Check the file."},{"type":"tool_use","id":"read-1","name":"Read","input":{"file_path":"large.txt","offset":42}}]}})"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].thoughts, std::string("Check the file."));
	const std::string output = "  indented\n" + std::string(20000, 'x') + "\nC:\\new\\notes\n";
	const nlohmann::json tool_result = {{"type", "user"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "read-1"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", output}}})}}})}}}};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), tool_result.dump()));
	UAM_ASSERT_EQ(app.chats.front().messages[0].tool_calls[0].result_text, output);
	UAM_ASSERT_EQ(uam::StateSerializer::ToolCallContentForFrontend(session.tool_calls[0]), uam::StateSerializer::ToolCallContentForFrontend(app.chats.front().messages[0].tool_calls[0]));
	const nlohmann::json structured = nlohmann::json::array({{{"type", "text"}, {"text", "Screenshot"}}, {{"type", "image"}, {"source", {{"type", "base64"}, {"data", "image-data"}}}}});
	nlohmann::json structured_result = tool_result;
	structured_result["message"]["content"][0]["content"] = structured;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), structured_result.dump()));
	UAM_ASSERT_EQ(session.tool_calls[0].content, std::string("Screenshot\n[Image]"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"assistant","parent_tool_use_id":"agent-1","message":{"content":[{"type":"text","text":"Child response"}]}})"));
	UAM_ASSERT(app.chats.front().messages[0].content.empty());
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"result","subtype":"error_max_budget_usd","is_error":true,"errors":["Budget exceeded"]})"));
	UAM_ASSERT(session.last_error.find("Budget exceeded") != std::string::npos);

	session.processing = false;
	session.cancel_requested = true;
	session.cancel_request_id = 8;
	session.pending_request_methods[8] = "claude/interrupt";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_response","response":{"subtype":"success","request_id":"8","response":{}}})"));
	UAM_ASSERT(session.cancel_requested);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"assistant","message":{"content":[{"type":"text","text":"Late response"}]}})"));
	UAM_ASSERT(app.chats.front().messages[0].content.empty());
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"result","subtype":"success","is_error":false,"result":"Interrupted"})"));
	UAM_ASSERT(!session.cancel_requested);
	UAM_ASSERT_EQ(session.cancel_request_id, 0);
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("ready"));
#endif
}

UAM_TEST(AcpSetupDoesNotReuseRejectedDraftSessionIds)
{
	for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
	{
		if (profile.id != "gemini-cli" && profile.id != "opencode-cli" && profile.id != "copilot-cli") continue;
		for (const bool supports_resume : {false, true})
		{
			TempDir temp("uam-rejected-resume");
			uam::AppState app;
			app.data_root = temp.root;
			ChatSession chat;
			chat.id = "chat-local-draft";
			chat.provider_id = profile.id;
			chat.native_session_id = chat.id;
			chat.workspace_directory = temp.root.string();
			uam::AcpSessionState session;
			session.provider_id = profile.id;
			session.protocol_kind = ProviderRuntimeRegistry::Resolve(profile).AcpProtocolKind();
#if defined(_WIN32)
			const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
			const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, temp.root, sink_argv, &error));
			session.running = true;
			session.initialized = true;
			session.load_session_supported = true;
			session.resume_session_supported = supports_resume;
			const bool sent = uam::acp_detail::SendSessionSetupIfReady(app, session, chat);
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
			UAM_ASSERT(sent);
			UAM_ASSERT(chat.native_session_id.empty());
			UAM_ASSERT_EQ(session.pending_request_methods.at(session.session_setup_request_id), std::string(profile.id == uam::provider_ids::kCopilotCli ? "session.create" : "session/new"));
			UAM_ASSERT(session.session_id.empty());
			UAM_ASSERT(session.codex_thread_id.empty());
		}
	}
}

UAM_TEST(GeminiInvalidSessionLoadFallsBackToNewSession)
{
	TempDir temp("uam-gemini-invalid-load");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-local";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-missing";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = chat.native_session_id;

	auto session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState& raw_session = *session;
	raw_session.chat_id = chat.id;
	raw_session.provider_id = "gemini-cli";
	raw_session.protocol_kind = "gemini-acp";
	raw_session.running = true;
	raw_session.initialized = true;
	raw_session.load_session_supported = true;
	raw_session.session_id = chat.native_session_id;
	raw_session.session_setup_request_id = 2;
	raw_session.next_request_id = 3;
	raw_session.pending_request_methods[2] = "session/load";
	raw_session.lifecycle_state = "starting";

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(raw_session, temp.root, sink_argv, &launch_error));
	UAM_ASSERT(launch_error.empty());

	app.acp_sessions.push_back(std::move(session));
	ChatSession& stored_chat = app.chats.front();
	const nlohmann::json load_error = {
	    {"jsonrpc", "2.0"},
	    {"id", 2},
	    {"error",
	     {
	         {"code", -32603},
	         {"message", "Internal error"},
	         {"data",
	          {
	              {"details", "Invalid session identifier \"native-missing\". Use --list-sessions to see available sessions."},
	          }},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, raw_session, stored_chat, load_error.dump()));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(raw_session);

	UAM_ASSERT(raw_session.acp_resume_fallback_attempted);
	UAM_ASSERT_EQ(stored_chat.native_session_id, std::string(""));
	UAM_ASSERT_EQ(raw_session.session_id, std::string(""));
	UAM_ASSERT(uam::ResolveAcpSessionResumeIdForTests(app, stored_chat).empty());
	UAM_ASSERT_EQ(raw_session.session_setup_request_id, 3);
	UAM_ASSERT_EQ(raw_session.pending_request_methods[3], std::string("session/new"));
	UAM_ASSERT_EQ(raw_session.lifecycle_state, std::string("starting"));

	const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, stored_chat.id)));
	UAM_ASSERT_EQ(persisted.value("native_session_id", "missing"), std::string(""));
}

UAM_TEST(CopilotMissingSessionPreservesIdentityWhileCodexStartsFresh)
{
	for (const bool codex : {false, true})
	{
		TempDir temp("uam-copilot-missing-load");
		uam::AppState app;
		app.data_root = temp.root;

		ChatSession chat;
		chat.id = "chat-copilot-local";
		chat.provider_id = codex ? uam::provider_ids::kCodexCli : uam::provider_ids::kCopilotCli;
		chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
		chat.workspace_directory = temp.root.string();
		app.chats.push_back(chat);
		app.resolved_native_sessions_by_chat_id[chat.id] = chat.native_session_id;

		auto session = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& raw_session = *session;
		raw_session.chat_id = chat.id;
		raw_session.provider_id = chat.provider_id;
		raw_session.protocol_kind = codex ? "codex-app-server" : uam::provider_profile_constants::kProtocolCopilotSdk;
		raw_session.running = true;
		raw_session.initialized = true;
		raw_session.load_session_supported = true;
		raw_session.session_id = chat.native_session_id;
		raw_session.session_setup_request_id = 2;
		raw_session.next_request_id = 3;
		raw_session.pending_request_methods[2] = codex ? "thread/resume" : "session.resume";
		raw_session.lifecycle_state = "starting";

#if defined(_WIN32)
		const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
		const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
		std::string launch_error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(raw_session, temp.root, sink_argv, &launch_error));
		UAM_ASSERT(launch_error.empty());

		app.acp_sessions.push_back(std::move(session));
		ChatSession& stored_chat = app.chats.front();
		const nlohmann::json load_error = {
		    {"jsonrpc", "2.0"},
		    {"id", 2},
		    {"error",
		     {
		         {"code", codex ? -32600 : -32002},
		         {"message", codex ? "no rollout found for thread id" : "Resource not found: Session was not found"},
		         {"data", {{"uri", "Session was not found"}}},
		     }},
		};

		UAM_ASSERT(uam::ProcessAcpLineForTests(app, raw_session, stored_chat, load_error.dump()));

		PlatformServicesFactory::Instance().process_service.StopStdioProcess(raw_session, true);
		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(raw_session);

		const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, stored_chat.id)));
		if (codex)
		{
			UAM_ASSERT(raw_session.codex_resume_fallback_attempted);
			UAM_ASSERT(stored_chat.native_session_id.empty());
			UAM_ASSERT(raw_session.session_id.empty());
			UAM_ASSERT(uam::ResolveAcpSessionResumeIdForTests(app, stored_chat).empty());
			UAM_ASSERT_EQ(raw_session.session_setup_request_id, 3);
			UAM_ASSERT_EQ(raw_session.pending_request_methods[3], std::string("thread/start"));
			UAM_ASSERT_EQ(raw_session.lifecycle_state, std::string("starting"));
			UAM_ASSERT_EQ(persisted.value("native_session_id", "missing"), std::string{});
		}
		else
		{
			UAM_ASSERT(!raw_session.acp_resume_fallback_attempted);
			UAM_ASSERT_EQ(stored_chat.native_session_id, chat.native_session_id);
			UAM_ASSERT_EQ(raw_session.session_id, chat.native_session_id);
			UAM_ASSERT_EQ(uam::ResolveAcpSessionResumeIdForTests(app, stored_chat), chat.native_session_id);
			UAM_ASSERT_EQ(raw_session.session_setup_request_id, 0);
			UAM_ASSERT_EQ(raw_session.next_request_id, 3);
			UAM_ASSERT_EQ(raw_session.lifecycle_state, std::string("error"));
			UAM_ASSERT(raw_session.queue_held);
			UAM_ASSERT_EQ(persisted.value("native_session_id", "missing"), chat.native_session_id);
		}
	}
}
