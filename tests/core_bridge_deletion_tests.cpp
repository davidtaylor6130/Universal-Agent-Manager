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

UAM_TEST(CefBridgeRequestValidationRejectsMalformedEnvelopes)
{
	const auto non_object = uam::cef::ParseBridgeRequest(R"(["getInitialState"])");
	UAM_ASSERT(!non_object.ok);
	UAM_ASSERT_EQ(non_object.status, 400);

	const auto missing_action = uam::cef::ParseBridgeRequest(R"({"payload":{}})");
	UAM_ASSERT(!missing_action.ok);
	UAM_ASSERT_EQ(missing_action.status, 400);

	const auto non_string_action = uam::cef::ParseBridgeRequest(R"({"action":7,"payload":{}})");
	UAM_ASSERT(!non_string_action.ok);
	UAM_ASSERT_EQ(non_string_action.status, 400);

	const auto blank_action = uam::cef::ParseBridgeRequest(R"({"action":"  ","payload":{}})");
	UAM_ASSERT(!blank_action.ok);
	UAM_ASSERT_EQ(blank_action.status, 400);

	const auto string_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":"bad"})");
	UAM_ASSERT(!string_payload.ok);
	UAM_ASSERT_EQ(string_payload.status, 400);

	const auto array_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":[]})");
	UAM_ASSERT(!array_payload.ok);
	UAM_ASSERT_EQ(array_payload.status, 400);
}

UAM_TEST(CefBridgeRequestValidationDefaultsMissingAndNullPayload)
{
	const auto missing_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState"})");
	UAM_ASSERT(missing_payload.ok);
	UAM_ASSERT_EQ(missing_payload.request.action, std::string("getInitialState"));
	UAM_ASSERT(missing_payload.request.payload.is_object());
	UAM_ASSERT(missing_payload.request.payload.empty());

	const auto null_payload = uam::cef::ParseBridgeRequest(R"({"action":"getInitialState","payload":null})");
	UAM_ASSERT(null_payload.ok);
	UAM_ASSERT_EQ(null_payload.request.action, std::string("getInitialState"));
	UAM_ASSERT(null_payload.request.payload.is_object());
	UAM_ASSERT(null_payload.request.payload.empty());

	const auto valid_payload = uam::cef::ParseBridgeRequest(R"({"action":"selectSession","payload":{"chatId":"chat-1"}})");
	UAM_ASSERT(valid_payload.ok);
	UAM_ASSERT_EQ(valid_payload.request.action, std::string("selectSession"));
	UAM_ASSERT_EQ(valid_payload.request.payload.value("chatId", ""), std::string("chat-1"));

	const auto spaced_action = uam::cef::ParseBridgeRequest(R"({"action":" selectSession ","payload":{}})");
	UAM_ASSERT(spaced_action.ok);
	UAM_ASSERT_EQ(spaced_action.request.action, std::string("selectSession"));

	const std::string wrapped_payload = R"(xx{"action":"selectSession","payload":{"chatId":"chat-2"}}yy)";
	const auto sliced_payload = uam::cef::ParseBridgeRequest(std::string_view(wrapped_payload).substr(2, wrapped_payload.size() - 4));
	UAM_ASSERT(sliced_payload.ok);
	UAM_ASSERT_EQ(sliced_payload.request.payload.value("chatId", ""), std::string("chat-2"));
}

UAM_TEST(CefMacOsWebAppShortcutCrashWorkaroundDisablesShortcutFeatures)
{
	const std::string disabled_features = uam::cef::MacOsWebAppShortcutCrashDisabledFeatures();

	UAM_ASSERT(disabled_features.find("WebAppEnableShortcuts") != std::string::npos);
	UAM_ASSERT(disabled_features.find("DesktopPWADeterminedInstalledByOsIntegration") != std::string::npos);
	UAM_ASSERT(disabled_features.find("WebAppSystemMediaControlsWin") != std::string::npos);
	UAM_ASSERT(disabled_features.find("WebAppEnableOsIntegrationSubManagers") != std::string::npos);
	UAM_ASSERT(disabled_features.find("DesktopPWAsRunOnOsLogin") != std::string::npos);
	UAM_ASSERT(disabled_features.find("DesktopPWAsWithoutExtensions") != std::string::npos);
	UAM_ASSERT(disabled_features.find("WebAppUniversalInstall") != std::string::npos);
}

UAM_TEST(CefTrustedUiSchemeServesOnlyRealBundledResources)
{
	TempDir bundled("uam-cef-trusted-bundled");
	const fs::path bundled_index = bundled.root / "Resources" / "UI-V2" / "dist" / "index.html";
	fs::create_directories(bundled_index.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(bundled_index, "<!doctype html>"));
	const fs::path asset = bundled_index.parent_path() / "assets" / "app.js";
	fs::create_directories(asset.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(asset, "export {};"));

	const std::string bundled_url = uam::cef::ResolveTrustedUiIndexUrl(bundled.root / "Contents" / "MacOS");
	UAM_ASSERT_EQ(bundled_url, std::string(uam::cef::kUamUiIndexUrl));
	const fs::path root = uam::cef::ResolveTrustedUiRoot(bundled.root / "Contents" / "MacOS");
	UAM_ASSERT_EQ(root, uam::paths::NormalizeExistingPath(bundled_index.parent_path()));
	const auto resolved = uam::cef::ResolveTrustedUiResourcePath(root, "uam://app/assets/app.js?v=1#x");
	UAM_ASSERT(resolved.has_value());
	UAM_ASSERT_EQ(*resolved, uam::paths::NormalizeExistingPath(asset));
	UAM_ASSERT(!uam::cef::ResolveTrustedUiResourcePath(root, "uam://app/../secret").has_value());
	UAM_ASSERT(!uam::cef::ResolveTrustedUiResourcePath(root, "uam://app/%2e%2e/secret").has_value());
	UAM_ASSERT(!uam::cef::ResolveTrustedUiResourcePath(root, "uam://other/index.html").has_value());
	UAM_ASSERT(!uam::cef::ResolveTrustedUiResourcePath(root, "file:///tmp/index.html").has_value());

	TempDir missing("uam-cef-trusted-missing");
	const std::string fallback_url = uam::cef::ResolveTrustedUiIndexUrl(missing.root / "nested" / "bin");
	UAM_ASSERT_EQ(fallback_url, std::string(uam::cef::kUamUiIndexUrl));
}

UAM_TEST(CefTrustedUiUrlIgnoresFileUrlDecorationAndLocalhostAuthority)
{
	TempDir temp("uam-cef-trusted-url");
	const fs::path index = temp.root / uam::paths::PathFromUtf8("install-\xE2\x98\x83") / "UI-V2" / "dist" / "index.html";
	fs::create_directories(index.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(index, "<!doctype html>"));

	const std::string trusted_url = uam::cef::FileUrlFromPath(index);
	constexpr std::string_view file_scheme = "file://";
	const std::string localhost_url = "file://localhost" + trusted_url.substr(file_scheme.size()) + "?v=1#root";
	const std::string mixed_case_localhost_url = "FILE://LOCALHOST" + trusted_url.substr(file_scheme.size());
	UAM_ASSERT(uam::cef::IsTrustedUiUrl(localhost_url, trusted_url));
	UAM_ASSERT(uam::cef::IsTrustedUiUrl(mixed_case_localhost_url, trusted_url));
	UAM_ASSERT(uam::cef::IsTrustedUiUrl(trusted_url + "#root", trusted_url + "?v=1"));
	UAM_ASSERT(!uam::cef::IsTrustedUiUrl("", ""));
	UAM_ASSERT(!uam::cef::IsTrustedUiUrl(trusted_url + ".bak", trusted_url));
}

UAM_TEST(CefExternalUrlPolicyAllowsOnlyExplicitSchemes)
{
	UAM_ASSERT(uam::cef::ShouldOpenExternally("HTTPS://example.test/path"));
	UAM_ASSERT(uam::cef::ShouldOpenExternally("mailto:hello@example.test"));
	UAM_ASSERT(uam::cef::ShouldOpenExternally("tel:+15551234567"));
	UAM_ASSERT(!uam::cef::ShouldOpenExternally("file:///tmp/index.html"));
	UAM_ASSERT(!uam::cef::ShouldOpenExternally("javascript:alert(1)"));
	UAM_ASSERT(!uam::cef::ShouldOpenExternally("/relative/path"));
}

UAM_TEST(CefPopupPolicyCancelsEveryPopupAndOpensOnlyExternalUrls)
{
	std::vector<std::string> opened_urls;
	const auto opener = [&](const std::string& url) {
		opened_urls.push_back(url);
		return true;
	};

	for (const std::string& url : {std::string{}, std::string("file:///tmp/index.html"), std::string("javascript:alert(1)"), std::string("/relative/path")})
	{
		UAM_ASSERT(uam::cef::CancelPopupAndOpenExternally(url, opener));
	}
	UAM_ASSERT(opened_urls.empty());
	UAM_ASSERT(uam::cef::CancelPopupAndOpenExternally("https://example.test/path", opener));
	UAM_ASSERT_EQ(opened_urls, std::vector<std::string>{"https://example.test/path"});

	using PopupHandler = bool (UamCefClient::*)(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&, const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*);
	UAM_ASSERT((std::is_same_v<decltype(&UamCefClient::OnBeforePopup), PopupHandler>));
}

UAM_TEST(FolderLifecycleKeepsWorkspaceRootsMinimal)
{
	TempDir temp("uam-folders");
	uam::AppState app;
	app.data_root = temp.root;

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));
	UAM_ASSERT(!created_id.empty());
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);

	UAM_ASSERT(!RenameFolderById(app, "  ", "Ignored", temp.root.string()));
	UAM_ASSERT_EQ(app.status_line, std::string("Folder id is required."));

	const fs::path renamed_root = temp.root / "renamed";
	fs::create_directories(renamed_root);
	UAM_ASSERT(RenameFolderById(app, " " + created_id + " ", "Renamed", renamed_root.string()));
	const ChatFolder* renamed = ChatDomainService().FindFolderById(app, created_id);
	UAM_ASSERT(renamed != nullptr);
	UAM_ASSERT_EQ(renamed->title, std::string("Renamed"));
	UAM_ASSERT_EQ(renamed->directory, renamed_root.string());

	ChatSession folder_chat;
	folder_chat.id = "chat-in-folder";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = " " + created_id + " ";
	folder_chat.title = "Folder chat";
	folder_chat.workspace_directory = renamed_root.string();
	folder_chat.created_at = "2026-01-01T00:00:00.000Z";
	folder_chat.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession general_chat;
	general_chat.id = "chat-in-general";
	general_chat.provider_id = "gemini-cli";
	general_chat.folder_id = uam::constants::kDefaultFolderId;
	general_chat.title = "General chat";
	general_chat.created_at = "2026-01-01T00:00:00.000Z";
	general_chat.updated_at = "2026-01-01T00:00:00.000Z";

	app.chats.push_back(folder_chat);
	app.chats.push_back(general_chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, folder_chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, general_chat));

	const fs::path moved_root = temp.root / "moved";
	fs::create_directories(moved_root);
	UAM_ASSERT(RenameFolderById(app, created_id, "Moved", moved_root.string()));
	UAM_ASSERT_EQ(app.chats[0].workspace_directory, moved_root.string());
	UAM_ASSERT_EQ(app.chats[1].workspace_directory, std::string());
	const std::vector<ChatSession> moved_chats = ChatRepository::LoadLocalChats(temp.root);
	const auto moved_chat = std::ranges::find_if(moved_chats, [](const ChatSession& chat) { return chat.id == "chat-in-folder"; });
	UAM_ASSERT(moved_chat != moved_chats.end());
	UAM_ASSERT_EQ(moved_chat->workspace_directory, moved_root.string());

	const fs::path folder_chat_file = AppPaths::UamChatFilePath(temp.root, folder_chat.id);
	const fs::path general_chat_file = AppPaths::UamChatFilePath(temp.root, general_chat.id);
	UAM_ASSERT(fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));

	UAM_ASSERT(DeleteFolderById(app, " " + created_id + " "));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) == nullptr);
	UAM_ASSERT(ChatDomainService().FindFolderById(app, uam::constants::kDefaultFolderId) == nullptr);
	UAM_ASSERT(ChatDomainService().FindChatById(app, folder_chat.id) == nullptr);
	const ChatSession* retained_chat = ChatDomainService().FindChatById(app, general_chat.id);
	UAM_ASSERT(retained_chat != nullptr);
	UAM_ASSERT_EQ(retained_chat->folder_id, std::string(uam::constants::kDefaultFolderId));
	UAM_ASSERT(fs::exists(renamed_root));
	UAM_ASSERT(!fs::exists(folder_chat_file));
	UAM_ASSERT(fs::exists(general_chat_file));
}

UAM_TEST(ChatFolderStoreRoundTripsEncodedFieldsAndUsesBackupFallback)
{
	TempDir temp("uam-folder-store");

	ChatFolder folder;
	folder.id = " folder-one ";
	folder.title = "Project=Alpha";
	folder.directory = "workspace\none";
	folder.collapsed = true;
	folder.execution_host_id = "ssh-lab";
	folder.custom_icon = {"text", "🛠️"};

	UAM_ASSERT(ChatFolderStore::Save(temp.root, {folder}));

	const std::vector<ChatFolder> loaded = ChatFolderStore::Load(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().id, std::string("folder-one"));
	UAM_ASSERT_EQ(loaded.front().title, folder.title);
	UAM_ASSERT_EQ(loaded.front().directory, folder.directory);
	UAM_ASSERT(loaded.front().collapsed);
	UAM_ASSERT_EQ(loaded.front().execution_host_id, folder.execution_host_id);
	UAM_ASSERT(loaded.front().custom_icon == folder.custom_icon);

	const std::string saved_folder_text = ReadFile(temp.root / "folders.txt");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "folders.txt", "not-a-folder-entry\n"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "folders.txt.bak", saved_folder_text));

	const std::vector<ChatFolder> recovered = ChatFolderStore::Load(temp.root);
	UAM_ASSERT_EQ(recovered.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(recovered.front().id, std::string("folder-one"));

	std::error_code remove_error;
	fs::remove(temp.root / "folders.txt", remove_error);
	UAM_ASSERT(!remove_error);
	const std::vector<ChatFolder> backup_only_recovered = ChatFolderStore::Load(temp.root);
	UAM_ASSERT_EQ(backup_only_recovered.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(backup_only_recovered.front().id, std::string("folder-one"));

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "folders.txt", "[folder]\r\n id = folder-two \r\n title =Second\r\ndirectory=workspace-two\r\ncollapsed= ON \r\n"));
	const std::vector<ChatFolder> crlf_loaded = ChatFolderStore::Load(temp.root);
	UAM_ASSERT_EQ(crlf_loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(crlf_loaded.front().id, std::string("folder-two"));
	UAM_ASSERT(crlf_loaded.front().collapsed);
}

UAM_TEST(MixedMachineWorkspaceMigrationSplitsChatsWithoutChangingTheirAuthority)
{
	TempDir temp("uam-workspace-host-migration");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());
	app.settings.execution_hosts = {
	    uam::execution_hosts::LocalHost(),
	    {"windows", "Windows", "ssh", "windows", "ready", "4.5.7", "windows", "x86_64", "", ""},
	    {"homelab", "Homelab", "ssh", "homelab", "ready", "4.5.7", "linux", "x86_64", "", ""},
	};
	app.folders.push_back({"mixed", "Legacy mixed workspace", "C:\\Work\\Project", false, ""});

	const auto add_chat = [&](std::string id, std::string host, std::string directory) {
		ChatSession chat;
		chat.id = std::move(id);
		chat.title = chat.id;
		chat.provider_id = "opencode-cli";
		chat.folder_id = "mixed";
		chat.execution_host_id = std::move(host);
		chat.workspace_directory = std::move(directory);
		chat.messages.push_back(Message{MessageRole::User, "preserve me"});
		chat.messages_loaded = true;
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		app.chats.push_back(std::move(chat));
	};
	add_chat("windows-1", "windows", "C:\\Work\\Project");
	add_chat("windows-2", "windows", "c:/work/project/");
	add_chat("windows-3", "windows", "C:/WORK/PROJECT");
	add_chat("homelab-1", "homelab", "/opt/containers/project");
	add_chat("local-1", "local", temp.root.string());

	UAM_ASSERT(uam::MigrateWorkspaceFolderOwnership(app));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(3));
	const ChatFolder* original = ChatDomainService().FindFolderById(app, "mixed");
	UAM_ASSERT(original != nullptr);
	UAM_ASSERT_EQ(original->execution_host_id, std::string("windows"));
	UAM_ASSERT(uam::execution_hosts::RemotePathsMatch("windows", original->directory,
	                                                  "C:\\Work\\Project"));

	for (const ChatFolder& folder : app.folders)
	{
		for (const ChatSession& chat : app.chats)
		{
			if (chat.folder_id != folder.id) continue;
			UAM_ASSERT_EQ(chat.execution_host_id,
			              uam::strings::NonEmptyOrFallback(folder.execution_host_id, "local"));
		}
	}
	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(persisted.size(), app.chats.size());
	for (const ChatSession& chat : persisted)
	{
		UAM_ASSERT_EQ(chat.messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(chat.messages.front().content, std::string("preserve me"));
	}
	const std::vector<ChatFolder> persisted_folders = ChatFolderStore::Load(temp.root);
	UAM_ASSERT_EQ(persisted_folders.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(uam::MigrateWorkspaceFolderOwnership(app));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(3));
}

UAM_TEST(ChatDeletionWaitsForRemoteStopConfirmation)
{
	TempDir temp("uam-delete-waits-for-remote-stop");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "remote-idle-stop";
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!RemoveChatById(app, chat.id));
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_pending);
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id) != nullptr);
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(RemoveChatById(app, chat.id));
}

UAM_TEST(UnconfirmedIdleRemoteStopKeepsChatUndeletable)
{
	TempDir temp("uam-delete-unconfirmed-remote-stop");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "remote-idle-stop-failed";
	chat.execution_host_id = "ssh-test";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 70"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 70"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!uam::StopAcpSession(app, chat.id));
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_unconfirmed);
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id)->remote_stop_cleanup_pending);
	UAM_ASSERT(uam::AcpStopInProgress(app, chat.id));
	UAM_ASSERT(!RemoveChatById(app, chat.id));
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id) != nullptr);
	UAM_ASSERT(!uam::EnsureAcpStopProgress(app, chat.id));
	UAM_ASSERT(app.acp_sessions.front()->recovering_remote_turn);
	UAM_ASSERT(app.acp_sessions.front()->reconnect_pending);
}

UAM_TEST(RemoveChatByIdTrimsRequestedChatId)
{
	TempDir temp("uam-remove-chat-trimmed-id");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.remember_last_chat = true;

	ChatSession chat;
	chat.id = "chat-remove-trimmed";
	chat.provider_id = "gemini-cli";
	chat.title = "Remove me";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:00.000Z";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	app.chats_with_unseen_updates.insert(chat.id);
	app.collapsed_branch_chat_ids.insert(chat.id);
	app.filtered_chat_ids.insert(chat.id);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	UAM_ASSERT(!RemoveChatById(app, "  "));
	UAM_ASSERT_EQ(app.status_line, std::string("Chat id is required."));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(RemoveChatById(app, " " + chat.id + " "));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT_EQ(app.selected_chat_index, -1);
	UAM_ASSERT(app.chats_with_unseen_updates.empty());
	UAM_ASSERT(app.collapsed_branch_chat_ids.empty());
	UAM_ASSERT(app.filtered_chat_ids.empty());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, chat.id)));
}

UAM_TEST(RemoveChatByIdDefersCommittedDeletionWithoutLosingUnloadedMessages)
{
	TempDir temp("uam-remove-chat-summary-rollback");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession target;
	target.id = "chat-with-history";
	target.provider_id = "gemini-cli";
	target.title = "Keep the transcript";
	target.messages.push_back(Message{MessageRole::User, "Do not lose this message."});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, target));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!app.chats.front().messages_loaded);

	ChatSession unsavable;
	unsavable.id = "../unsafe";
	unsavable.provider_id = "gemini-cli";
	app.chats.push_back(unsavable);
	app.selected_chat_index = 0;

	UAM_ASSERT(RemoveChatById(app, target.id));
	UAM_ASSERT(std::ranges::none_of(app.chats, [&](const ChatSession& chat) { return chat.id == target.id; }));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, target.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(restored->messages.front().content, std::string("Do not lose this message."));
}

UAM_TEST(RemoveChatByIdDoesNotResurrectRecoveryBackup)
{
	TempDir temp("uam-remove-chat-backup");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-with-backup";
	chat.provider_id = "gemini-cli";
	chat.title = "First version";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(uam::io::WriteTextFile(
	    uam::io::MakeBackupPath(AppPaths::UamChatFilePath(temp.root, chat.id)),
	    uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id))));
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(fs::exists(uam::io::MakeBackupPath(AppPaths::UamChatFilePath(temp.root, chat.id))));

	UAM_ASSERT(RemoveChatById(app, chat.id));
	UAM_ASSERT(ChatRepository::LoadLocalChats(temp.root).empty());
}

UAM_TEST(RemoveChatByIdDeletesGeminiNativeHistory)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-remove-chat-native-history");
	const fs::path gemini_home = temp.root / "gemini-home";
	const fs::path workspace_root = temp.root / "workspace";
	const fs::path source_root = gemini_home / ".gemini" / "tmp" / "workspace-source";
	const fs::path source_chats = source_root / "chats";
	fs::create_directories(workspace_root);
	fs::create_directories(source_chats);
	UAM_ASSERT(uam::io::WriteTextFile(source_root / ".project_root", workspace_root.string()));
	UAM_ASSERT(uam::io::WriteTextFile(source_chats / "native-delete.json", R"({
  "sessionId": "native-delete",
  "startTime": "2026-01-01T00:00:00.000Z",
  "lastUpdated": "2026-01-01T00:00:01.000Z",
  "messages": [{"type":"user","content":"delete native too"}]
})"));

	ScopedEnvVar gemini_home_env("GEMINI_CLI_HOME", gemini_home.string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-delete-native";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.native_session_id = "native-delete";
	chat.workspace_directory = workspace_root.string();
	chat.messages.push_back({MessageRole::User, "delete native too"});
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));

	UAM_ASSERT(RemoveChatById(app, chat.id));
	UAM_ASSERT(!fs::exists(source_chats / "native-delete.json"));
#endif
}

UAM_TEST(RemoveLocalChatDoesNotDependOnNativeTombstoneStorage)
{
	TempDir temp("uam-remove-local-chat-corrupt-tombstones");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "local-only-chat";
	chat.provider_id = uam::provider_ids::kCodexCli;
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "native-import-tombstones.json", "{"));

	UAM_ASSERT(RemoveChatById(app, chat.id));
	UAM_ASSERT(app.chats.empty());
}

UAM_TEST(RemoveChatsByIdsPreflightsTheWholeBatch)
{
	TempDir temp("uam-remove-chat-batch-preflight");
	uam::AppState app;
	app.data_root = temp.root;
	for (const std::string& id : {"chat-one", "chat-two"})
	{
		ChatSession chat;
		chat.id = id;
		chat.provider_id = "gemini-cli";
		chat.messages.push_back(Message{MessageRole::User, id});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	app.worktree_operation_chat_ids.insert("chat-two");

	UAM_ASSERT(!RemoveChatsByIds(app, {" chat-one ", "chat-two", "chat-one"}));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(temp.root).size(), static_cast<std::size_t>(2));

	app.worktree_operation_chat_ids.clear();
	UAM_ASSERT(RemoveChatsByIds(app, {" chat-one ", "chat-two", "chat-one"}));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChats(temp.root).empty());
}

UAM_TEST(RemoveChatsByIdsBlocksPendingAcpControlRequests)
{
	TempDir temp("uam-remove-chat-pending-acp-control");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-pending-control";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	session->mode_change_request_id = 42;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!RemoveChatsByIds(app, {chat.id}));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, chat.id)));
}

UAM_TEST(RemoveGoalOwnerPreflightsAndDeletesHiddenIterationChats)
{
	TempDir temp("uam-remove-goal-owner-cascade");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession owner;
	owner.id = "goal-owner";
	ChatSession iteration;
	iteration.id = "goal-iteration";
	iteration.goal_owner_chat_id = owner.id;
	iteration.goal_iteration_goal_id = "goal-1";
	app.chats = {owner, iteration};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, owner));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, iteration));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = iteration.id;
	session->running = true;
	session->processing = true;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!RemoveChatById(app, owner.id));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(temp.root).size(), static_cast<std::size_t>(2));

	app.acp_sessions.front()->running = false;
	app.acp_sessions.front()->processing = false;
	app.chats[1].execution_host_id = "ssh-test";
	app.chats[1].remote_turn_reconnect_pending = true;
	app.acp_sessions.front()->reconnect_pending = true;
	UAM_ASSERT(!RemoveChatById(app, owner.id));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	app.chats[1].remote_turn_reconnect_pending = false;
	app.acp_sessions.front()->reconnect_pending = false;
	UAM_ASSERT(RemoveChatById(app, owner.id));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(ChatRepository::LoadLocalChats(temp.root).empty());
}

UAM_TEST(RemoveOwnerPreflightsAndDeletesManagedAgentTranscripts)
{
	TempDir temp("uam-remove-managed-agent-owner-cascade");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession owner;
	owner.id = "managed-owner";
	ChatSession transcript;
	transcript.id = "managed-transcript";
	transcript.agent_run_id = "managed-run";
	ChatSession unrelated;
	unrelated.id = "unrelated-history";
	app.chats = {owner, transcript, unrelated};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, owner));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, transcript));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, unrelated));
	AgentRun run;
	run.id = transcript.agent_run_id;
	run.root_chat_id = owner.id;
	run.transcript_chat_id = transcript.id;
	run.status = "running";
	app.agent_runs.push_back(run);
	AgentRun stale_run;
	stale_run.id = "stale-run";
	stale_run.root_chat_id = owner.id;
	stale_run.transcript_chat_id = unrelated.id;
	stale_run.status = "completed";
	app.agent_runs.push_back(stale_run);

	UAM_ASSERT(!RemoveChatById(app, owner.id));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(temp.root).size(), static_cast<std::size_t>(3));

	app.agent_runs.front().status = "completed";
	UAM_ASSERT(RemoveChatById(app, owner.id));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().id, unrelated.id);
	const std::vector<ChatSession> remaining = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(remaining.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(remaining.front().id, unrelated.id);
}

UAM_TEST(DeleteFolderPreflightsAndDeletesHiddenGoalIterationChats)
{
	TempDir temp("uam-delete-folder-goal-owner-cascade");
	uam::AppState app;
	app.data_root = temp.root;
	ChatFolder folder;
	folder.id = "goal-folder";
	app.folders.push_back(folder);
	ChatSession owner;
	owner.id = "folder-goal-owner";
	owner.folder_id = folder.id;
	ChatSession iteration;
	iteration.id = "folder-goal-iteration";
	iteration.goal_owner_chat_id = owner.id;
	app.chats = {owner, iteration};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, owner));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, iteration));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = iteration.id;
	session->running = true;
	session->processing = true;
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(!DeleteFolderById(app, folder.id));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	app.acp_sessions.front()->running = false;
	app.acp_sessions.front()->processing = false;
	app.chats[1].execution_host_id = "ssh-test";
	app.chats[1].remote_turn_reconnect_pending = true;
	UAM_ASSERT(!DeleteFolderById(app, folder.id));
	app.chats[1].remote_turn_reconnect_pending = false;
	UAM_ASSERT(DeleteFolderById(app, folder.id));
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(app.folders.empty());
}

#if !defined(_WIN32)
UAM_TEST(RemoveChatByIdTombstonesCommittedDeletionWhenMetadataCleanupIsDeferred)
{
	TempDir temp("uam-remove-chat-delete-failure");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-delete-failure";
	chat.provider_id = "codex-cli";
	chat.title = "Keep me";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path chats_directory = AppPaths::UamChatFilePath(temp.root, chat.id).parent_path();
	fs::permissions(chats_directory, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

	const bool removed = RemoveChatById(app, chat.id);

	fs::permissions(chats_directory, fs::perms::owner_all, fs::perm_options::replace);
	UAM_ASSERT(removed);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
}
#endif

UAM_TEST(DefaultFolderIsNotSynthesized)
{
	uam::AppState app;
	app.new_chat_folder_id = "missing-folder";

	ChatSession chat;
	chat.id = "chat-with-missing-folder";
	chat.folder_id = "missing-folder";
	app.chats.push_back(chat);

	ChatDomainService().NormalizeChatFolderAssignments(app);

	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT_EQ(app.chats.front().folder_id, std::string("missing-folder"));
	UAM_ASSERT(app.new_chat_folder_id.empty());
}

UAM_TEST(UnsortedChatAssignmentMatchesBothDirectoryAndMachine)
{
	TempDir temp("uam-unsorted-machine-assignment");
	uam::AppState app;
	app.settings.execution_hosts = {
	    uam::execution_hosts::LocalHost(),
	    {"homelab", "Homelab", "ssh", "homelab", "ready", "4.5.7", "linux", "x86_64", "", ""},
	    {"nas", "NAS", "ssh", "nas", "ready", "4.5.7", "linux", "x86_64", "", ""},
	};
	app.folders = {
	    {"local", "Local", (temp.root / "same-name").string(), false, "local"},
	    {"homelab", "Homelab", "/srv/same-name", false, "homelab"},
	    {"nas", "NAS", "/srv/same-name", false, "nas"},
	};
	for (const std::string& host_id : {"homelab", "nas"})
	{
		ChatSession chat;
		chat.id = host_id;
		chat.execution_host_id = host_id;
		chat.workspace_directory = "/srv/same-name";
		app.chats.push_back(std::move(chat));
	}

	ChatDomainService().NormalizeChatFolderAssignments(app);
	UAM_ASSERT_EQ(app.chats[0].folder_id, std::string("homelab"));
	UAM_ASSERT_EQ(app.chats[1].folder_id, std::string("nas"));
}

UAM_TEST(UnsortedWorkspaceRecoveryPreviewsAndRebuildsAvailableFolders)
{
	TempDir temp("uam-unsorted-workspace-recovery");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	const fs::path alpha = temp.root / "Alpha";
	const fs::path existing = temp.root / "Existing";
	const fs::path worktree = temp.root / "worktrees" / "chat-worktree";
	const fs::path missing = temp.root / "Disconnected";
	const fs::path not_a_folder = temp.root / "workspace.txt";
	fs::create_directories(alpha);
	fs::create_directories(existing);
	fs::create_directories(worktree);
	UAM_ASSERT(uam::io::WriteTextFile(not_a_folder, "not a directory"));

	std::string existing_folder_id;
	UAM_ASSERT(CreateFolder(app, "Existing", existing.string(), &existing_folder_id));

	const auto add_chat = [&](std::string id, std::string folder_id, std::string workspace)
	{
		ChatSession chat;
		chat.id = std::move(id);
		chat.title = chat.id;
		chat.provider_id = "gemini-cli";
		chat.folder_id = std::move(folder_id);
		chat.workspace_directory = std::move(workspace);
		chat.created_at = "2026-01-01T00:00:00.000Z";
		chat.updated_at = chat.created_at;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(std::move(chat));
	};
	add_chat("alpha-one", "", alpha.string());
	add_chat("alpha-two", "stale-folder", (alpha / ".").string());
	add_chat("missing", "", missing.string());
	add_chat("unavailable", "", not_a_folder.string());
	add_chat("unknown", "", "");
	add_chat("worktree", "", worktree.string());
	app.chats.back().workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
	app.chats.back().workspace_source_directory = existing.string();
	app.chats.back().workspace_worktree_directory = worktree.string();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.back()));

	const uam::WorkspaceFolderRecoveryPreview preview = uam::PreviewUnsortedWorkspaceFolders(app);
	UAM_ASSERT_EQ(preview.groups.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(preview.missing.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(preview.unavailable.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(preview.no_location.size(), static_cast<std::size_t>(1));
	const auto alpha_group = std::ranges::find_if(preview.groups, [&](const auto& group) { return FolderDirectoryMatches(group.directory, alpha); });
	const auto existing_group = std::ranges::find_if(preview.groups, [&](const auto& group) { return FolderDirectoryMatches(group.directory, existing); });
	UAM_ASSERT(alpha_group != preview.groups.end());
	UAM_ASSERT_EQ(alpha_group->title, std::string("Alpha"));
	UAM_ASSERT_EQ(alpha_group->chat_ids.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(alpha_group->existing_folder_id.empty());
	UAM_ASSERT(existing_group != preview.groups.end());
	UAM_ASSERT_EQ(existing_group->existing_folder_id, existing_folder_id);
	UAM_ASSERT_EQ(existing_group->chat_ids, std::vector<std::string>{"worktree"});

	uam::WorkspaceFolderRecoveryResult result;
	UAM_ASSERT(uam::RebuildUnsortedWorkspaceFolders(app, &result));
	UAM_ASSERT_EQ(result.organized_chat_count, static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(result.created_folder_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(result.reused_folder_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(app.resource_collections.empty());

	const ChatSession* recovered_worktree = ChatDomainService().FindChatById(app, "worktree");
	UAM_ASSERT(recovered_worktree != nullptr);
	UAM_ASSERT_EQ(recovered_worktree->folder_id, existing_folder_id);
	UAM_ASSERT_EQ(recovered_worktree->workspace_directory, worktree.string());
	UAM_ASSERT_EQ(recovered_worktree->workspace_source_directory, existing.string());
	UAM_ASSERT(ChatDomainService().FindChatById(app, "missing")->folder_id.empty());
	UAM_ASSERT(ChatDomainService().FindChatById(app, "unavailable")->folder_id.empty());
	UAM_ASSERT(ChatDomainService().FindChatById(app, "unknown")->folder_id.empty());

	const std::vector<ChatSession> persisted_chats = ChatRepository::LoadLocalChats(app.data_root);
	const auto persisted_alpha = std::ranges::find_if(persisted_chats, [](const ChatSession& chat) { return chat.id == "alpha-one"; });
	UAM_ASSERT(persisted_alpha != persisted_chats.end());
	UAM_ASSERT(!persisted_alpha->folder_id.empty());
	UAM_ASSERT_EQ(ChatFolderStore::Load(app.data_root).size(), static_cast<std::size_t>(2));
}

UAM_TEST(UnsortedWorkspaceRecoveryRollsBackChatsWhenFolderSaveFails)
{
	TempDir temp("uam-unsorted-workspace-recovery-rollback");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const fs::path workspace = temp.root / "Workspace";
	fs::create_directories(workspace);

	ChatSession chat;
	chat.id = "rollback-chat";
	chat.title = "Rollback chat";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = workspace.string();
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = chat.created_at;
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));

	const fs::path folders_path = app.data_root / "folders.txt";
	const fs::path folders_backup_path = uam::io::MakeBackupPath(folders_path);
	fs::create_directories(folders_path);
	fs::create_directories(folders_backup_path);
	UAM_ASSERT(uam::io::WriteTextFile(folders_backup_path / "block", "block replacement"));

	UAM_ASSERT(!uam::RebuildUnsortedWorkspaceFolders(app));
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(app.chats.front().folder_id.empty());
	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(persisted.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(persisted.front().folder_id.empty());
}

UAM_TEST(UnsortedRemoteWorkspaceRecoveryKeepsEachMachineSeparate)
{
	TempDir temp("uam-unsorted-remote-workspace-recovery");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.execution_hosts = {
	    uam::execution_hosts::LocalHost(),
	    {"homelab", "Homelab", "ssh", "homelab", "ready", "4.5.7", "linux", "x86_64", "", ""},
	    {"nas", "NAS", "ssh", "nas", "ready", "4.5.7", "linux", "x86_64", "", ""},
	};
	for (const std::string& host_id : {"homelab", "nas"})
	{
		ChatSession chat;
		chat.id = host_id + "-chat";
		chat.title = chat.id;
		chat.provider_id = "opencode-cli";
		chat.execution_host_id = host_id;
		chat.workspace_directory = "/srv/shared-name";
		chat.created_at = "2026-01-01T00:00:00.000Z";
		chat.updated_at = chat.created_at;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(std::move(chat));
	}

	const uam::WorkspaceFolderRecoveryPreview preview =
	    uam::PreviewUnsortedWorkspaceFolders(app);
	UAM_ASSERT_EQ(preview.groups.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(preview.groups[0].directory, std::string("/srv/shared-name"));
	UAM_ASSERT(preview.groups[0].execution_host_id != preview.groups[1].execution_host_id);
	UAM_ASSERT(uam::RebuildUnsortedWorkspaceFolders(app));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(app.folders[0].execution_host_id != app.folders[1].execution_host_id);
	for (const ChatSession& chat : app.chats)
	{
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, chat.folder_id);
		UAM_ASSERT(folder != nullptr);
		UAM_ASSERT_EQ(folder->execution_host_id, chat.execution_host_id);
		UAM_ASSERT_EQ(folder->directory, chat.workspace_directory);
	}
}

UAM_TEST(DeleteLegacyDefaultFolderDeletesContainedChats)
{
	TempDir temp("uam-delete-default-folder");
	uam::AppState app;
	app.data_root = temp.root;

	ChatFolder legacy_default;
	legacy_default.id = uam::constants::kDefaultFolderId;
	legacy_default.title = uam::constants::kDefaultFolderTitle;
	legacy_default.directory = temp.root.string();
	app.folders.push_back(legacy_default);

	ChatSession chat;
	chat.id = "chat-in-default";
	chat.provider_id = "gemini-cli";
	chat.folder_id = uam::constants::kDefaultFolderId;
	chat.title = "Default chat";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:00.000Z";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path chat_file = AppPaths::UamChatFilePath(temp.root, chat.id);
	UAM_ASSERT(fs::exists(chat_file));

	UAM_ASSERT(DeleteFolderById(app, uam::constants::kDefaultFolderId));
	UAM_ASSERT(app.folders.empty());
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(!fs::exists(chat_file));
}

UAM_TEST(DeleteFolderRefreshesRememberedSelectionToFallbackChat)
{
	TempDir temp("uam-folder-delete-selection");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.remember_last_chat = true;

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));

	ChatSession folder_chat;
	folder_chat.id = "chat-in-folder";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Folder chat";
	folder_chat.created_at = "2026-01-01T00:00:00.000Z";
	folder_chat.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession fallback_chat;
	fallback_chat.id = "chat-fallback";
	fallback_chat.provider_id = "gemini-cli";
	fallback_chat.folder_id = uam::constants::kDefaultFolderId;
	fallback_chat.title = "Fallback chat";
	fallback_chat.created_at = "2026-01-01T00:00:00.000Z";
	fallback_chat.updated_at = "2026-01-01T00:00:00.000Z";

	app.chats.push_back(folder_chat);
	app.chats.push_back(fallback_chat);
	app.selected_chat_index = 0;
	app.chats_with_unseen_updates.insert(folder_chat.id);
	app.collapsed_branch_chat_ids.insert(folder_chat.id);
	app.filtered_chat_ids.insert(folder_chat.id);
	app.resolved_native_sessions_by_chat_id[folder_chat.id] = "native-session";
	app.resolved_native_sessions_by_chat_id["native-session"] = folder_chat.id;
	app.resolved_native_sessions_by_chat_id[" padded-native-session "] = " " + folder_chat.id + " ";
	app.resolved_native_sessions_by_chat_id[" " + folder_chat.id + " "] = "padded-native-session";
	ChatDomainService().RefreshRememberedSelection(app);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-in-folder"));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, folder_chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, fallback_chat));

	UAM_ASSERT(DeleteFolderById(app, created_id));
	UAM_ASSERT_EQ(app.selected_chat_index, 0);
	UAM_ASSERT_EQ(app.chats[static_cast<std::size_t>(app.selected_chat_index)].id, std::string("chat-fallback"));
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-fallback"));
	UAM_ASSERT(app.chats_with_unseen_updates.empty());
	UAM_ASSERT(app.collapsed_branch_chat_ids.empty());
	UAM_ASSERT(app.filtered_chat_ids.empty());
	UAM_ASSERT(app.resolved_native_sessions_by_chat_id.empty());
}

#if !defined(_WIN32)
UAM_TEST(DeleteFolderTombstonesCommittedDeletionWhenMetadataCleanupIsDeferred)
{
	TempDir temp("uam-folder-delete-failure");
	uam::AppState app;
	app.data_root = temp.root;

	std::string folder_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &folder_id));

	ChatSession chat;
	chat.id = "folder-chat-delete-failure";
	chat.provider_id = "codex-cli";
	chat.folder_id = folder_id;
	chat.title = "Keep me";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path chats_directory = AppPaths::UamChatFilePath(temp.root, chat.id).parent_path();
	fs::permissions(chats_directory, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

	const bool removed = DeleteFolderById(app, folder_id);

	fs::permissions(chats_directory, fs::perms::owner_all, fs::perm_options::replace);
	UAM_ASSERT(removed);
	UAM_ASSERT(ChatDomainService().FindFolderById(app, folder_id) == nullptr);
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id) == nullptr);
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
}
#endif

UAM_TEST(ChatDeletionWaitsForCancelledCliContextPreparation)
{
	TempDir temp("uam-chat-context-cancel");
	uam::AppState app;
	app.data_root = temp.root;
	std::string folder_id;
	UAM_ASSERT(CreateFolder(app, "Synthetic", temp.root.string(), &folder_id));
	ChatSession chat = ChatDomainService().CreateNewChat(folder_id, "codex-cli");
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	const std::shared_ptr<uam::CliContextPreparation> state = std::make_shared<uam::CliContextPreparation>();
	state->chat_id = chat.id;
	uam::CliContextPreparationTask task;
	task.state = state;
	app.cli_context_preparation_tasks.push_back(std::move(task));
	uam::CliTerminalState terminal;
	terminal.context_preparation = state;
	uam::StopCliTerminal(terminal, false, uam::CliTerminalStopMode::FastExit);
	UAM_ASSERT(state->cancellation.stop_requested());
	UAM_ASSERT(terminal.context_preparation == nullptr);
	UAM_ASSERT(!RemoveChatById(app, chat.id));
	UAM_ASSERT(!DeleteFolderById(app, folder_id));
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id) != nullptr);
	state->finished.store(true);
	UAM_ASSERT(RemoveChatById(app, chat.id));
	UAM_ASSERT(ChatDomainService().FindChatById(app, chat.id) == nullptr);
	UAM_ASSERT(DeleteFolderById(app, folder_id));
}

UAM_TEST(DeleteFoldersBatchPreservesUnrelatedHistoryAndReparentsSurvivors)
{
	TempDir temp("uam-workspace-batch");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders = {{"a", "A", (temp.root / "workspace-a").string(), false},
	              {"b", "B", (temp.root / "workspace-b").string(), false},
	              {"keep", "Keep", (temp.root / "workspace-keep").string(), false}};
	for (const ChatFolder& folder : app.folders) fs::create_directories(folder.directory);
	UAM_ASSERT(ChatFolderStore::Save(temp.root, app.folders));
	ChatSession keep;
	keep.id = "keep";
	keep.folder_id = "keep";
	keep.provider_id = uam::provider_ids::kCodexCli;
	ChatSession a = keep;
	a.id = "a";
	a.folder_id = "a";
	a.parent_chat_id = keep.id;
	ChatSession b = a;
	b.id = "b";
	b.folder_id = "b";
	b.parent_chat_id = a.id;
	ChatSession child = keep;
	child.id = "child";
	child.parent_chat_id = b.id;
	child.branch_from_message_index = 2;
	ChatSession hidden = a;
	hidden.id = "hidden";
	hidden.folder_id.clear();
	hidden.goal_owner_chat_id = a.id;
	app.chats = {keep, a, b, child, hidden};
	ChatBranching::Normalize(app.chats);
	for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path keep_path = AppPaths::UamChatFilePath(temp.root, keep.id);
	const fs::file_time_type sentinel = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(keep_path, sentinel);
	const std::string keep_contents = ReadFile(keep_path);
	app.selected_chat_index = 1;
	app.new_chat_folder_id = "b";
	UAM_ASSERT(DeleteFoldersByIds(app, {" a ", "b", "a"}));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(app.new_chat_folder_id.empty());
	UAM_ASSERT(fs::last_write_time(keep_path) == sentinel);
	UAM_ASSERT_EQ(ReadFile(keep_path), keep_contents);
	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	const ChatSession* saved_child = nullptr;
	for (const ChatSession& chat : saved) if (chat.id == child.id) saved_child = &chat;
	UAM_ASSERT(saved_child != nullptr);
	UAM_ASSERT_EQ(saved_child->parent_chat_id, keep.id);
	UAM_ASSERT_EQ(saved_child->branch_root_chat_id, keep.id);
	UAM_ASSERT_EQ(saved_child->branch_from_message_index, 2);
	for (const std::string& id : {a.id, b.id, hidden.id}) UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, id)));
	UAM_ASSERT(fs::exists(temp.root / "workspace-a"));
	UAM_ASSERT(fs::exists(temp.root / "workspace-b"));
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
	UAM_ASSERT(!fs::exists(temp.root / ".deletion-transaction"));
}

UAM_TEST(WorkspaceDeletionBackgroundPhasesPreserveLiveChangesAndRecoverBeforeCommit)
{
	TempDir temp("uam-workspace-background");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders = {{"delete", "Delete", temp.root.string(), false}, {"keep", "Keep", temp.root.string(), false}};
	UAM_ASSERT(ChatFolderStore::Save(temp.root, app.folders));
	ChatSession doomed;
	doomed.id = "doomed";
	doomed.folder_id = "delete";
	doomed.provider_id = uam::provider_ids::kCodexCli;
	ChatSession survivor = doomed;
	survivor.id = "survivor";
	survivor.folder_id = "keep";
	survivor.parent_chat_id = doomed.id;
	app.chats = {doomed, survivor};
	ChatBranching::Normalize(app.chats);
	for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete"}, task));
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
	bool staged = false;
	std::thread stage([&]() { staged = uam::StageWorkspaceDeletion(task); });
	stage.join();
	UAM_ASSERT(staged);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(2));
	// A pending unrelated runtime or history completion can update live state during staging.
	app.chats[1].title = "Updated while staging";
	app.chats[1].messages.push_back({MessageRole::User, "Live message", "2026-10-03T00:00:00Z"});
	app.folders.push_back({"new", "New", temp.root.string(), false});
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().title, "Updated while staging");
	UAM_ASSERT(app.chats.front().parent_chat_id.empty());
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, doomed.id)));
	UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
	bool cleaned = false;
	std::thread cleanup([&]() { cleaned = uam::CleanupWorkspaceDeletion(task); });
	cleanup.join();
	UAM_ASSERT(cleaned);
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, doomed.id)));
	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(saved.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().messages.front().content, "Live message");

	// Closing the app after background staging still leaves a complete recovery transaction.
	uam::WorkspaceDeletionTask interrupted;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"keep"}, interrupted));
	UAM_ASSERT(uam::StageWorkspaceDeletion(interrupted));
	uam::AppState recovery;
	recovery.data_root = temp.root;
	recovery.provider_profiles = app.provider_profiles;
	UAM_ASSERT(uam::RecoverPendingDeletionTransaction(recovery));
	UAM_ASSERT(ChatRepository::LoadLocalChats(temp.root).empty());
	UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root).size(), static_cast<std::size_t>(1));
}

UAM_TEST(WorkspaceDeletionStagesUnloadedTranscriptsWithoutRetainingTheirBodies)
{
	TempDir temp("uam-delete-unloaded-batch");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders = {{"delete-a", "A", temp.root.string(), false}, {"delete-b", "B", temp.root.string(), false}};
	UAM_ASSERT(ChatFolderStore::Save(temp.root, app.folders));
	for (int index = 0; index < 2; ++index)
	{
		ChatSession chat;
		chat.id = "unloaded-" + std::to_string(index);
		chat.folder_id = index == 0 ? "delete-a" : "delete-b";
		chat.provider_id = uam::provider_ids::kCodexCli;
		chat.messages.push_back({MessageRole::User, std::string(4U * 1024U * 1024U, 'x')});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
		UAM_ASSERT(summary.has_value());
		summary->title = "Latest local metadata";
		app.chats.push_back(std::move(*summary));
	}
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete-a", "delete-b"}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	for (const ChatSession& chat : task.snapshot.chats)
	{
		UAM_ASSERT(!chat.messages_loaded);
		UAM_ASSERT(chat.messages.empty());
		const std::optional<ChatSession> staged = ChatRepository::LoadLocalChat(temp.root / ".deletion-transaction", chat.id, true);
		UAM_ASSERT(staged.has_value());
		UAM_ASSERT_EQ(staged->title, std::string("Latest local metadata"));
		UAM_ASSERT_EQ(staged->messages.front().content.size(), std::size_t{4U * 1024U * 1024U});
	}
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT(app.chats.empty());
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
}

UAM_TEST(ChatDeletionStagesColdHistoryAsynchronouslyAndExpandsDependents)
{
	TempDir temp("uam-chat-delete-cold-staged");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession owner;
	owner.id = "cold-owner";
	owner.provider_id = uam::provider_ids::kCodexCli;
	for (int index = 0; index < 1000; ++index)
		owner.messages.push_back({MessageRole::User, "message-" + std::to_string(index) + std::string(1024, 'x')});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, owner));
	ChatSession dependent;
	dependent.id = "dependent-transcript";
	dependent.goal_owner_chat_id = owner.id;
	dependent.provider_id = uam::provider_ids::kCodexCli;
	dependent.messages.push_back({MessageRole::Assistant, "managed transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, dependent));
	ChatSession survivor;
	survivor.id = "survivor";
	survivor.provider_id = uam::provider_ids::kCodexCli;
	survivor.messages.push_back({MessageRole::User, "unrelated"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, survivor));
	std::optional<ChatSession> cold_owner = ChatRepository::LoadLocalChat(temp.root, owner.id, false);
	std::optional<ChatSession> cold_dependent = ChatRepository::LoadLocalChat(temp.root, dependent.id, false);
	UAM_ASSERT(cold_owner.has_value());
	UAM_ASSERT(cold_dependent.has_value());
	app.chats = {std::move(*cold_owner), std::move(*cold_dependent), survivor};
	app.selected_chat_index = 0;

	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {owner.id}, task));
	UAM_ASSERT(task.deleting_chats);
	UAM_ASSERT_EQ(task.deleted_ids.size(), std::size_t{2});
	UAM_ASSERT_EQ(task.snapshot.chats.size(), std::size_t{2});
	for (const ChatSession& chat : task.snapshot.chats)
	{
		UAM_ASSERT(!chat.messages_loaded);
		UAM_ASSERT(chat.messages.empty());
	}
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{3});
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	const std::optional<ChatSession> staged_owner = ChatRepository::LoadLocalChat(temp.root / ".deletion-transaction", owner.id, true);
	UAM_ASSERT(staged_owner.has_value());
	UAM_ASSERT_EQ(staged_owner->messages.size(), std::size_t{1000});
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{3});
	ChatDomainService().SelectChatById(app, survivor.id);
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), survivor.id);
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
	const std::vector<ChatSession> remaining = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(remaining.size(), std::size_t{1});
	UAM_ASSERT_EQ(remaining.front().id, survivor.id);
}

UAM_TEST(ChatDeletionPreparationRejectsPendingSaveWithoutStaging)
{
	TempDir temp("uam-chat-delete-pending-save");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "pending-save-chat";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	app.pending_chat_save_at_by_chat_id[chat.id] = 1.0;
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(!uam::PrepareChatDeletion(app, {chat.id}, task));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
	app.pending_chat_save_at_by_chat_id.erase(chat.id);
	uam::WorkspaceDeletionTask staged_task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, staged_task));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "deletion-transaction.json", "existing transaction"));
	UAM_ASSERT(!uam::StageWorkspaceDeletion(staged_task));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
}

UAM_TEST(ChatDeletionStagesAuthoritativeWarmTranscriptSnapshot)
{
	TempDir temp("uam-chat-delete-warm-snapshot");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "warm-chat";
	chat.messages.push_back({MessageRole::User, "persisted before current live update"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.messages.back().content = "authoritative in-memory transcript";
	chat.messages_loaded = true;
	app.chats.push_back(chat);

	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, task));
	UAM_ASSERT(task.snapshot.chats.front().messages_loaded);
	UAM_ASSERT_EQ(task.snapshot.chats.front().messages.back().content, std::string("authoritative in-memory transcript"));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	const std::optional<ChatSession> staged = ChatRepository::LoadLocalChat(temp.root / ".deletion-transaction", chat.id, true);
	UAM_ASSERT(staged.has_value());
	UAM_ASSERT_EQ(staged->messages.back().content, std::string("authoritative in-memory transcript"));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
}

UAM_TEST(ChatDeletionPersistsRootNormalizationForSurvivingBranch)
{
	TempDir temp("uam-chat-delete-normalized-survivor");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root;
	root.id = "deleted-root";
	ChatSession survivor;
	survivor.id = "surviving-branch";
	survivor.parent_chat_id = root.id;
	survivor.branch_root_chat_id = root.id;
	survivor.branch_from_message_index = 4;
	app.chats = {root, survivor};
	ChatBranching::Normalize(app.chats);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats[0]));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats[1]));

	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {root.id}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.chats.front().parent_chat_id, std::string());
	UAM_ASSERT_EQ(app.chats.front().branch_root_chat_id, survivor.id);
	UAM_ASSERT_EQ(app.chats.front().branch_from_message_index, -1);
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(temp.root, survivor.id, false);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->branch_root_chat_id, survivor.id);
	UAM_ASSERT_EQ(persisted->branch_from_message_index, -1);
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
}

UAM_TEST(WorkspaceDeletionKeepsSurvivingTranscriptStorage)
{
	TempDir temp("uam-delete-survivor-storage");
	uam::AppState app;
	app.data_root = temp.root;
	app.folders = {{"delete", "Delete", temp.root.string(), false}, {"keep", "Keep", temp.root.string(), false}};
	UAM_ASSERT(ChatFolderStore::Save(temp.root, app.folders));
	ChatSession doomed;
	doomed.id = "doomed";
	doomed.folder_id = "delete";
	ChatSession survivor;
	survivor.id = "survivor";
	survivor.folder_id = "keep";
	survivor.messages.push_back({MessageRole::User, std::string(4U * 1024U * 1024U, 'x')});
	app.chats = {doomed, survivor};
	ChatBranching::Normalize(app.chats);
	for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const char* body = app.chats[1].messages.front().content.data();
	const std::string saved = uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, survivor.id));
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete"}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	uam::CommitWorkspaceDeletion(app, task);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT(app.chats.front().messages.front().content.data() == body);
	UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, survivor.id)), saved);
}

UAM_TEST(DeletionPreservesNativeHistoryOwnedBySurvivingAliases)
{
	for (const bool folder_deletion : {false, true})
	for (const bool recovery : {false, true})
	for (const bool nested : {false, true})
	{
		TempDir temp("uam-delete-shared-native");
		const fs::path workspace = temp.root / "workspace";
		const fs::path source = temp.root / "gemini" / ".gemini" / "tmp" / "project";
		ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
		fs::create_directories(workspace / "sub");
		fs::create_directories(source / "chats");
		UAM_ASSERT(uam::io::WriteTextFile(source / ".project_root", workspace.string()));
		const fs::path native = source / "chats" / "shared-native.json";
		const std::string native_bytes = R"({"sessionId":"shared-native","messages":[{"type":"user","content":"shared"}]})";
		UAM_ASSERT(uam::io::WriteTextFile(native, native_bytes));
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		const fs::path retained_workspace = workspace / (nested ? "sub" : ".");
		app.folders = {{"delete", "Delete", workspace.string(), false}, {"alias", "Alias", retained_workspace.string(), false}};
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		ChatSession doomed;
		doomed.id = "doomed";
		doomed.folder_id = "delete";
		doomed.provider_id = "gemini-cli";
		doomed.native_session_id = "shared-native";
		doomed.workspace_directory = workspace.string();
		doomed.messages.push_back({MessageRole::User, "conflicting original"});
		ChatSession survivor = doomed;
		survivor.id = "survivor";
		survivor.folder_id = "alias";
		survivor.workspace_directory = retained_workspace.string();
		survivor.messages.front().content = "conflicting survivor";
		app.chats = {doomed, survivor};
		ChatBranching::Normalize(app.chats);
		for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		const std::string saved = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, survivor.id));
		if (recovery)
		{
			uam::WorkspaceDeletionTask task;
			if (folder_deletion) UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete"}, task));
			else UAM_ASSERT(uam::PrepareChatDeletion(app, {doomed.id}, task));
			UAM_ASSERT(uam::StageWorkspaceDeletion(task));
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		}
		else if (folder_deletion) UAM_ASSERT(DeleteFolderById(app, "delete"));
		else UAM_ASSERT(RemoveChatById(app, doomed.id));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(native), native_bytes);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, survivor.id)), saved);
		UAM_ASSERT(!fs::exists(app.data_root / "deletion-transaction.json"));
		const fs::path tombstones = app.data_root / "native-import-tombstones.json";
		UAM_ASSERT(!fs::exists(tombstones) || nlohmann::json::parse(uam::io::ReadTextFile(tombstones)).empty());
		uam::AppState reopened;
		reopened.data_root = app.data_root;
		reopened.provider_profiles = app.provider_profiles;
		reopened.folders = ChatFolderStore::Load(app.data_root);
		reopened.chats = ChatRepository::LoadLocalChats(app.data_root);
		UAM_ASSERT(RemoveChatById(reopened, survivor.id));
		UAM_ASSERT(!fs::exists(native));
	}
}

UAM_TEST(DeletionDoesNotTombstoneSurvivingNativeOwners)
{
	for (const std::string provider : {std::string("gemini-cli"), std::string("codex-cli")})
	for (const bool recovery : {false, true})
	{
		TempDir temp("uam-delete-shared-tombstone");
		ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession doomed;
		doomed.id = "doomed";
		doomed.provider_id = provider;
		doomed.native_session_id = "019c3c97-3c20-75d1-be43-2a6e4e67b02d";
		doomed.workspace_directory = (temp.root / "workspace").string();
		doomed.messages.push_back({MessageRole::User, "original"});
		ChatSession survivor = doomed;
		survivor.id = "survivor";
		survivor.messages.front().content = "different history";
		app.chats = {doomed, survivor};
		ChatBranching::Normalize(app.chats);
		for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		const std::string saved = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, survivor.id));
		if (recovery)
		{
			uam::WorkspaceDeletionTask task;
			UAM_ASSERT(uam::PrepareChatDeletion(app, {doomed.id}, task));
			UAM_ASSERT(uam::StageWorkspaceDeletion(task));
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		}
		else UAM_ASSERT(RemoveChatById(app, doomed.id));
		const fs::path tombstones = app.data_root / "native-import-tombstones.json";
		UAM_ASSERT(!fs::exists(tombstones) || nlohmann::json::parse(uam::io::ReadTextFile(tombstones)).empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, survivor.id)), saved);
		const std::vector<ChatSession> reopened = ChatRepository::LoadLocalChats(app.data_root);
		UAM_ASSERT_EQ(reopened.size(), std::size_t{1});
		UAM_ASSERT_EQ(reopened.front().native_session_id, survivor.native_session_id);
		uam::AppState final_owner;
		final_owner.data_root = app.data_root;
		final_owner.provider_profiles = app.provider_profiles;
		final_owner.chats = reopened;
		UAM_ASSERT(RemoveChatById(final_owner, survivor.id));
		UAM_ASSERT_EQ(nlohmann::json::parse(uam::io::ReadTextFile(tombstones)).size(), std::size_t{1});
	}
}

UAM_TEST(NativeDeletionNeverFallsBackToAnotherHostOrWorkspace)
{
	TempDir temp("uam-delete-native-scope");
	ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	const fs::path workspace = temp.root / "local-workspace";
	const fs::path source = temp.root / "gemini" / ".gemini" / "tmp" / "project";
	fs::create_directories(workspace);
	fs::create_directories(source / "chats");
	UAM_ASSERT(uam::io::WriteTextFile(source / ".project_root", workspace.string()));
	const fs::path native = source / "chats" / "same-id.json";
	const std::string bytes = R"({"sessionId":"same-id","messages":[{"type":"user","content":"local sentinel"}]})";
	UAM_ASSERT(uam::io::WriteTextFile(native, bytes));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	for (const bool remote : {true, false})
	{
		ChatSession chat;
		chat.id = "doomed";
		chat.provider_id = "gemini-cli";
		chat.native_session_id = "same-id";
		chat.execution_host_id = remote ? "remote-host" : "local";
		chat.workspace_directory = (temp.root / "other-workspace").string();
		std::error_code error;
		UAM_ASSERT(!ChatHistorySyncService().DeleteNativeSessionFileForChat(app, chat, &error));
		UAM_ASSERT(!error);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(native), bytes);
	}
	ChatSession local;
	local.id = "local-owner";
	local.provider_id = "gemini-cli";
	local.native_session_id = "same-id";
	local.workspace_directory = workspace.string();
	std::error_code error;
	UAM_ASSERT(ChatHistorySyncService().DeleteNativeSessionFileForChat(app, local, &error));
	UAM_ASSERT(!error);
	UAM_ASSERT(!fs::exists(native));
}

UAM_TEST(DeletionProtectsResolvedNativeOwnersBeforeAndAfterStaging)
{
	for (const bool resolve_late : {false, true})
	for (const bool native_id_collision : {false, true})
	for (const int interruption : {0, 1, 2})
	{
		TempDir temp("uam-delete-resolved-owner");
		ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
		const fs::path workspace = temp.root / "workspace";
		const fs::path source = temp.root / "gemini" / ".gemini" / "tmp" / "project";
		fs::create_directories(workspace / "sub");
		fs::create_directories(source / "chats");
		UAM_ASSERT(uam::io::WriteTextFile(source / ".project_root", workspace.string()));
		const fs::path native = source / "chats" / "shared.json";
		const std::string bytes = R"({"sessionId":"shared","messages":[{"type":"user","content":"keep"}]})";
		UAM_ASSERT(uam::io::WriteTextFile(native, bytes));
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession doomed;
		doomed.id = native_id_collision ? "shared" : "chat-doomed";
		doomed.provider_id = "gemini-cli";
		doomed.native_session_id = "shared";
		doomed.workspace_directory = workspace.string();
		ChatSession survivor;
		survivor.id = "chat-survivor";
		survivor.provider_id = "gemini-cli";
		survivor.workspace_directory = (workspace / "sub").string();
		app.chats = {doomed, survivor};
		ChatBranching::Normalize(app.chats);
		for (const ChatSession& chat : app.chats) UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		if (!resolve_late) app.resolved_native_sessions_by_chat_id[survivor.id] = "shared";
		uam::WorkspaceDeletionTask task;
		UAM_ASSERT(uam::PrepareChatDeletion(app, {doomed.id}, task));
		UAM_ASSERT(uam::StageWorkspaceDeletion(task));
		if (resolve_late) app.resolved_native_sessions_by_chat_id[survivor.id] = "shared";
		if (interruption != 1) uam::CommitWorkspaceDeletion(app, task);
		if (interruption == 0) UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
		else
		{
			uam::AppState restarted;
			restarted.data_root = app.data_root;
			restarted.provider_profiles = app.provider_profiles;
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(restarted));
		}
		UAM_ASSERT_EQ(uam::io::ReadTextFile(native), bytes);
		const fs::path tombstones = app.data_root / "native-import-tombstones.json";
		UAM_ASSERT(!fs::exists(tombstones) || nlohmann::json::parse(uam::io::ReadTextFile(tombstones)).empty());
		UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id.at(survivor.id), std::string("shared"));
	}
}

UAM_TEST(ChatDeletionPersistsFolderWorkspaceWithoutUsingCleanupWorkingDirectory)
{
	for (const bool cold : {false, true})
	for (const bool recovery : {false, true})
	{
		TempDir temp("uam-delete-folder-location");
		ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
		const fs::path intended = temp.root / "intended";
		const fs::path unrelated = temp.root / "unrelated";
		for (const std::string name : {std::string("intended"), std::string("unrelated")})
		{
			fs::create_directories(temp.root / name);
			const fs::path store = temp.root / "gemini" / ".gemini" / "tmp" / name;
			fs::create_directories(store / "chats");
			UAM_ASSERT(uam::io::WriteTextFile(store / ".project_root", (temp.root / name).string()));
			UAM_ASSERT(uam::io::WriteTextFile(store / "chats" / "same-id.json", R"({"sessionId":"same-id","messages":[{"type":"user","content":"sentinel"}]})"));
		}
		struct RestoreDirectory
		{
			fs::path previous = fs::current_path();
			~RestoreDirectory() { std::error_code error; fs::current_path(previous, error); }
		} restore;
		fs::current_path(unrelated);
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.folders = {{"intended", "Intended", intended.string(), false}};
		UAM_ASSERT(ChatFolderStore::Save(app.data_root, app.folders));
		ChatSession chat;
		chat.id = "chat-folder-location";
		chat.folder_id = "intended";
		chat.provider_id = "gemini-cli";
		chat.native_session_id = "same-id";
		chat.messages.push_back({MessageRole::User, "intended transcript"});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		if (cold) chat = *ChatRepository::LoadLocalChat(app.data_root, chat.id, false);
		app.chats.push_back(chat);
		uam::WorkspaceDeletionTask task;
		UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, task));
		UAM_ASSERT(uam::StageWorkspaceDeletion(task));
		const std::optional<ChatSession> staged = ChatRepository::LoadLocalChat(app.data_root / ".deletion-transaction", chat.id);
		UAM_ASSERT(staged.has_value());
		UAM_ASSERT_EQ(staged->workspace_directory, intended.string());
		if (recovery) UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		else
		{
			uam::CommitWorkspaceDeletion(app, task);
			UAM_ASSERT(uam::CleanupWorkspaceDeletion(task));
		}
		UAM_ASSERT(!fs::exists(temp.root / "gemini" / ".gemini" / "tmp" / "intended" / "chats" / "same-id.json"));
		UAM_ASSERT(fs::exists(temp.root / "gemini" / ".gemini" / "tmp" / "unrelated" / "chats" / "same-id.json"));
	}
}

UAM_TEST(DeletionRejectsOwnershipPublicationFailureBeforeChangingLiveHistory)
{
	for (const bool primary_failure : {false, true})
	{
		TempDir temp("uam-delete-ownership-save-failure");
		ScopedEnvVar home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "chat-keep-on-failure";
		chat.messages.push_back({MessageRole::User, "keep"});
		app.chats.push_back(chat);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const std::string saved = uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id));
		uam::WorkspaceDeletionTask task;
		UAM_ASSERT(uam::PrepareChatDeletion(app, {chat.id}, task));
		UAM_ASSERT(uam::StageWorkspaceDeletion(task));
		const fs::path intent_path = temp.root / "deletion-transaction.json";
		const fs::path backup = primary_failure ? intent_path : uam::io::MakeBackupPath(intent_path);
		if (primary_failure) fs::rename(intent_path, temp.root / "staged-intent.json");
		fs::create_directory(backup);
		UAM_ASSERT(uam::io::WriteTextFile(backup / "blocker", "keep backup collision"));
		uam::CommitWorkspaceDeletion(app, task);
		UAM_ASSERT(task.commit_rejected);
		UAM_ASSERT(app.worktree_operation_chat_ids.contains(chat.id));
		UAM_ASSERT(!task.metadata_saved);
		UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id)), saved);
		UAM_ASSERT(!uam::CleanupWorkspaceDeletion(task));
		UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
	}
}

UAM_TEST(DeletionRecoveryRejectsMalformedRetainedOwnershipWithoutDeletingHistory)
{
	for (const int malformed : {0, 1, 2, 3})
	{
		TempDir temp("uam-delete-ownership-malformed");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "chat-keep-on-corruption";
		chat.messages.push_back({MessageRole::User, "keep"});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const std::string saved = uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id));
		nlohmann::json intent = {{"version", 2}, {"complete", true}, {"folderIds", nlohmann::json::array()},
		    {"chatIds", {chat.id}}, {"retainedNativeKeys", nlohmann::json::array()},
		    {"retainedNativeOwners", nlohmann::json::array()}, {"ownershipFinalized", true}};
		if (malformed == 0) intent["retainedNativeKeys"] = nlohmann::json::array({42});
		if (malformed == 1) intent["retainedNativeOwners"] = nlohmann::json::array({{{"directory", 42}, {"sessionId", "s"}}});
		if (malformed == 2) intent["ownershipFinalized"] = "yes";
		if (malformed == 3) intent.erase("retainedNativeOwners");
		UAM_ASSERT(uam::io::WriteTextFile(temp.root / "deletion-transaction.json", intent.dump()));
		UAM_ASSERT(!uam::RecoverPendingDeletionTransaction(app));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id)), saved);
		UAM_ASSERT(fs::exists(temp.root / "deletion-transaction.json"));
	}
}

UAM_TEST(WorkspaceDeletionLargeBatchStagesAndCleansOutsideTheUiPhases)
{
	TempDir temp("uam-workspace-background-large");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders = {{"delete", "Delete", temp.root.string(), false}, {"keep", "Keep", temp.root.string(), false}};
	UAM_ASSERT(ChatFolderStore::Save(temp.root, app.folders));
	for (int index = 0; index < 1000; ++index)
	{
		ChatSession chat;
		chat.id = "large-" + std::to_string(index);
		chat.folder_id = index < 500 ? "delete" : "keep";
		chat.provider_id = uam::provider_ids::kCodexCli;
		chat.messages.push_back({MessageRole::User, std::string(2048, 'x')});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		app.chats.push_back(std::move(chat));
	}
	ChatBranching::Normalize(app.chats);
	uam::WorkspaceDeletionTask task;
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	UAM_ASSERT(uam::PrepareWorkspaceDeletion(app, {"delete"}, task));
	const std::chrono::steady_clock::time_point prepared = std::chrono::steady_clock::now();
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
	bool staged = false;
	std::thread stage([&]() { staged = uam::StageWorkspaceDeletion(task); });
	stage.join();
	UAM_ASSERT(staged);
	const std::chrono::steady_clock::time_point staged_at = std::chrono::steady_clock::now();
	uam::CommitWorkspaceDeletion(app, task);
	const std::chrono::steady_clock::time_point committed = std::chrono::steady_clock::now();
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(500));
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, "large-0")));
	bool cleaned = false;
	std::thread cleanup([&]() { cleaned = uam::CleanupWorkspaceDeletion(task); });
	cleanup.join();
	UAM_ASSERT(cleaned);
	const std::chrono::steady_clock::time_point finished = std::chrono::steady_clock::now();
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, "large-0")));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChats(temp.root).size(), static_cast<std::size_t>(500));
	const double ui_ms = std::chrono::duration<double, std::milli>(prepared - start + committed - staged_at).count();
	const double disk_ms = std::chrono::duration<double, std::milli>(staged_at - prepared + finished - committed).count();
	std::cout << "[PERF] Workspace deletion 500 chats: UI phases=" << ui_ms << "ms, background disk phases=" << disk_ms << "ms\n";
}

UAM_TEST(DeleteFoldersBatchPreflightsEveryWorkspaceBeforeDeletion)
{
	TempDir temp("uam-workspace-batch-blocked");
	uam::AppState app;
	app.data_root = temp.root;
	app.folders = {{"a", "A", temp.root.string(), false}, {"b", "B", temp.root.string(), false}};
	ChatSession a;
	a.id = "a";
	a.folder_id = "a";
	ChatSession hidden;
	hidden.id = "hidden";
	hidden.goal_owner_chat_id = a.id;
	app.chats = {a, hidden};
	UAM_ASSERT(!DeleteFoldersByIds(app, {}));
	UAM_ASSERT(!DeleteFoldersByIds(app, {"a", "missing"}));
	app.worktree_operation_chat_ids.insert(hidden.id);
	UAM_ASSERT(!DeleteFoldersByIds(app, {"a", "b"}));
	app.worktree_operation_chat_ids.clear();
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = hidden.id;
	session->running = true;
	session->processing = true;
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(!DeleteFoldersByIds(app, {"b", "a"}));
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
}

UAM_TEST(DeleteFoldersBatchRecoversInterruptedCleanupAtEveryStage)
{
	for (int stage = 0; stage < 4; ++stage)
	{
		TempDir temp("uam-workspace-batch-recovery-" + std::to_string(stage));
		const std::vector<ChatFolder> folders = {{"a", "A", temp.root.string(), false}, {"b", "B", temp.root.string(), false}};
		UAM_ASSERT(ChatFolderStore::Save(temp.root, folders));
		ChatSession a;
		a.id = "a";
		a.folder_id = "a";
		a.provider_id = uam::provider_ids::kCodexCli;
		ChatSession b = a;
		b.id = "b";
		b.folder_id = "b";
		ChatSession child = a;
		child.id = "child";
		child.folder_id.clear();
		child.parent_chat_id = a.id;
		child.branch_root_chat_id = a.id;
		child.branch_from_message_index = 1;
		const fs::path staging = temp.root / ".deletion-transaction";
		for (const ChatSession& chat : {a, b, child}) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		for (const ChatSession& chat : {a, b}) UAM_ASSERT(ChatRepository::SaveChat(staging, chat));
		UAM_ASSERT(ChatFolderStore::Save(staging, folders));
		const nlohmann::json intent = {{"version", 2}, {"complete", true}, {"folderIds", {"a", "b"}}, {"chatIds", {"a", "b"}}};
		UAM_ASSERT(uam::io::WriteTextFileWithBackup(temp.root / "deletion-transaction.json", intent.dump()));
		if (stage >= 1) UAM_ASSERT(ChatFolderStore::Save(temp.root, {}));
		if (stage >= 2) UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, a.id).Failed());
		if (stage >= 3) UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, b.id).Failed());
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		UAM_ASSERT(ChatFolderStore::Load(temp.root).empty());
		const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
		UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
		UAM_ASSERT(saved.front().parent_chat_id.empty());
		UAM_ASSERT_EQ(saved.front().branch_root_chat_id, child.id);
		UAM_ASSERT_EQ(saved.front().branch_from_message_index, -1);
		UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
	}
}

UAM_TEST(DeleteFolderBlocksWhenContainedChatIsRunning)
{
	TempDir temp("uam-folder-pending-delete");
	uam::AppState app;
	app.data_root = temp.root;

	std::string created_id;
	UAM_ASSERT(CreateFolder(app, "Project", temp.root.string(), &created_id));

	ChatSession folder_chat;
	folder_chat.id = "chat-running";
	folder_chat.provider_id = "gemini-cli";
	folder_chat.folder_id = created_id;
	folder_chat.title = "Running chat";
	app.chats.push_back(folder_chat);

	std::unique_ptr<uam::AcpSessionState> busy_session = std::make_unique<uam::AcpSessionState>();
	busy_session->chat_id = folder_chat.id;
	busy_session->running = true;
	busy_session->processing = true;
	app.acp_sessions.push_back(std::move(busy_session));

	UAM_ASSERT(!DeleteFolderById(app, created_id));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, created_id) != nullptr);
	UAM_ASSERT(ChatDomainService().FindChatById(app, folder_chat.id) != nullptr);
}

UAM_TEST(DeletionTransactionsRollForwardIdempotentlyFromEveryDurableStage)
{
	for (const bool delete_folder : {false, true})
	{
		for (int stage = 0; stage < 5; ++stage)
		{
			TempDir temp("uam-delete-transaction-" + std::to_string(delete_folder) + "-" + std::to_string(stage));
			const std::string folder_id = "folder-delete";
			ChatFolder folder{folder_id, "Delete", temp.root.string(), false};
			UAM_ASSERT(ChatFolderStore::Save(temp.root, {folder}));

			ChatSession first;
			first.id = "delete-one";
			first.provider_id = uam::provider_ids::kCodexCli;
			first.folder_id = delete_folder ? folder_id : "";
			first.messages.push_back(Message{MessageRole::User, "one"});
			ChatSession second = first;
			second.id = "delete-two";
			second.messages.front().content = "two";
			ChatSession survivor;
			survivor.id = "survivor";
			survivor.provider_id = uam::provider_ids::kCodexCli;
			survivor.parent_chat_id = first.id;
			for (const ChatSession& chat : {first, second, survivor}) UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

			const fs::path staging = temp.root / ".deletion-transaction";
			for (const ChatSession& chat : {first, second}) UAM_ASSERT(ChatRepository::SaveChat(staging, chat));
			if (delete_folder) UAM_ASSERT(ChatFolderStore::Save(staging, {folder}));
			const nlohmann::json intent = {
			    {"version", 1}, {"complete", true},
			    {"folderId", delete_folder ? folder_id : ""},
			    {"chatIds", {first.id, second.id}},
			};
			UAM_ASSERT(uam::io::WriteTextFileWithBackup(temp.root / "deletion-transaction.json", intent.dump()));

			if (stage >= 1)
			{
				survivor.parent_chat_id.clear();
				UAM_ASSERT(ChatRepository::SaveChat(temp.root, survivor));
			}
			if (stage >= 2 && delete_folder) UAM_ASSERT(ChatFolderStore::Save(temp.root, {}));
			if (stage >= 3) UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, first.id).Failed());
			if (stage >= 4) UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, second.id).Failed());

			uam::AppState app;
			app.data_root = temp.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
			const auto chats = ChatRepository::LoadLocalChats(temp.root);
			UAM_ASSERT_EQ(chats.size(), static_cast<std::size_t>(1));
			UAM_ASSERT_EQ(chats.front().id, survivor.id);
			UAM_ASSERT(chats.front().parent_chat_id.empty());
			UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root).empty(), delete_folder);
			UAM_ASSERT(!fs::exists(temp.root / "deletion-transaction.json"));
			UAM_ASSERT(!fs::exists(staging));
		}
	}
}

UAM_TEST(DeletionTransactionRecoversWrongTypedPrimaryFromValidatedBackup)
{
	for (const std::string_view wrong_field : {"version", "complete", "folderId"})
	{
		TempDir temp("uam-delete-intent-wrong-type-" + std::string(wrong_field));
		ChatSession chat;
		chat.id = "delete-one";
		chat.provider_id = uam::provider_ids::kCodexCli;
		chat.messages.push_back(Message{MessageRole::User, "delete me"});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const fs::path staging = temp.root / ".deletion-transaction";
		UAM_ASSERT(ChatRepository::SaveChat(staging, chat));

		const fs::path intent_path = temp.root / "deletion-transaction.json";
		const nlohmann::json valid_intent = {
		    {"version", 1}, {"complete", true}, {"folderId", ""}, {"chatIds", {chat.id}},
		};
		UAM_ASSERT(uam::io::WriteTextFileWithBackup(intent_path, valid_intent.dump()));
		UAM_ASSERT(uam::io::WriteTextFileWithBackup(intent_path, valid_intent.dump()));
		nlohmann::json malformed = valid_intent;
		malformed[std::string(wrong_field)] = wrong_field == "folderId" ? nlohmann::json(1) : nlohmann::json("wrong-type");
		UAM_ASSERT(uam::io::WriteTextFile(intent_path, malformed.dump()));

		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
		UAM_ASSERT(ChatRepository::LoadLocalChats(temp.root).empty());
		UAM_ASSERT(!fs::exists(intent_path));
		UAM_ASSERT(!fs::exists(staging));
	}
}
