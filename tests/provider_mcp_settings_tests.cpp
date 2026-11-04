#include "test_harness.h"
#include "app/provider_mcp_settings_service.h"
#include "app/provider_configuration_service.h"
#include "app/uam_control_service.h"
#include "common/config/central_provider_configuration.h"
#include "common/config/settings_store.h"

UAM_TEST(OpenCodeProgressiveMcpStatusPersistsExactHostChoice)
{
	uam_test::TempDir temp("opencode-progressive-mcp-settings");
	uam::AppState app;
	app.settings.central_provider_configuration.instructions = "Retain shared instructions";
	app.settings.central_provider_configuration.mcp_overrides = {
	    {"opencode-cli", "local", "opencode-progressive-mcp", false, true},
	    {"codex-cli", "ssh-fixture", "docs", false}};
	const std::filesystem::path path = temp.root / "settings.txt";
	UAM_ASSERT(SettingsStore::Save(path, app.settings));
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(path, restored).loaded);
	UAM_ASSERT_EQ(uam::central_configuration::Serialize(restored.central_provider_configuration), uam::central_configuration::Serialize(app.settings.central_provider_configuration));
	app.settings = std::move(restored);
	for (const std::string host : {"local", "ssh-fixture"})
	{
		const nlohmann::json status = uam::provider_mcp_settings::Status(app, host, "");
		for (const nlohmann::json& row : status.at("providers"))
		{
			if (row.at("providerId") == "opencode-cli")
				UAM_ASSERT_EQ(row.at("progressiveMcpDiscoveryEnabled").get<bool>(), host != "local");
			else UAM_ASSERT(!row.contains("progressiveMcpDiscoveryEnabled"));
		}
	}
}

UAM_TEST(ProviderMcpImportPreservesArgumentsAndReferencesAndReportsUnsupportedEntries)
{
	uam_test::TempDir temp("mcp-import-preview");
#if defined(_WIN32)
	const std::filesystem::path executable = temp.root / "npx.exe";
#else
	const std::filesystem::path executable = temp.root / "npx";
#endif
	UAM_ASSERT(uam::io::WriteBinaryFile(executable, "fixture, never executed"));
	std::filesystem::permissions(executable, std::filesystem::perms::owner_all);
	const nlohmann::json config = {{"mcp", {
	    {"local-tool", {{"type", "local"}, {"command", nlohmann::json::array({"npx", "-y", "some-mcp", "relative/file"})}, {"environment", {{"TOKEN", "{env:MY_TOKEN}"}}}, {"enabled", false}}},
	    {"remote-tool", {{"type", "remote"}, {"url", "https://example.test/mcp"}, {"headers", {{"Authorization", "{env:MCP_AUTH}"}}}}},
	    {"missing", {{"type", "local"}, {"command", nlohmann::json::array({"does-not-exist"})}}},
	    {"oauth", {{"type", "remote"}, {"url", "https://example.test/mcp"}, {"oauth", false}}},
	    {"relative-executable", {{"type", "local"}, {"command", nlohmann::json::array({"./npx"})}}},
	    {"uam-control", {{"type", "local"}, {"command", nlohmann::json::array({"npx"})}}},
	    {"substitution", {{"type", "local"}, {"command", nlohmann::json::array({"npx", "{env:SECRET_ARG}"})}}}
	}}};
	const nlohmann::json preview = uam::provider_mcp_settings::Preview(config, "local", "", {temp.root});
	UAM_ASSERT_EQ(preview["servers"].size(), std::size_t{2});
	UAM_ASSERT_EQ(preview["warnings"].size(), std::size_t{5});
	const std::vector<McpServerConfiguration> servers = uam::mcp_server_config::Parse(preview["servers"]);
	UAM_ASSERT_EQ(servers.front().args, (std::vector<std::string>{"-y", "some-mcp", "relative/file"}));
	UAM_ASSERT_EQ(servers.front().command, uam::paths::Utf8PathString(executable));
	UAM_ASSERT(!servers.front().enabled);
	UAM_ASSERT_EQ(servers.front().environment.front().environment_variable, std::string("MY_TOKEN"));
	UAM_ASSERT(servers.front().environment.front().value.empty());
	UAM_ASSERT_EQ(servers.back().headers.front().environment_variable, std::string("MCP_AUTH"));
	UAM_ASSERT(preview["requiresReview"].get<bool>());
}

UAM_TEST(ProviderMcpStatusRequiresCurrentChatBoundHandshakeAndRejectsStaleAuthority)
{
	uam_test::TempDir temp("mcp-handshake-status");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "mcp-chat"; chat.provider_id = "opencode-cli"; chat.execution_host_id = "local"; chat.uam_control_enabled = true;
	ChatSession other = chat; other.id = "other-chat";
	app.chats = {chat, other};
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back(); session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = true;
	UAM_ASSERT(uam::UamControlService::RequiresSessionStopForEnable(&session, false));
	UAM_ASSERT(!uam::UamControlService::RequiresSessionStopForEnable(&session, true));
	UAM_ASSERT(!uam::UamControlService::RequiresSessionStopForEnable(nullptr, false));
	UAM_ASSERT(session.running && session.uam_control_capability_id.empty() && app.uam_control_capabilities.empty());
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, chat, "session/new", setup));
	const auto status = [&](const std::string& id) { return uam::provider_mcp_settings::Status(app, "local", "", id)["providers"].front()["status"].get<std::string>(); };
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	UAM_ASSERT(!uam::UamControlService::RequiresSessionStopForEnable(&session, false));
	const auto receipt = [&](const std::string& id, const std::string& method, const std::string& connection) {
		return uam::UamControlService::HandleRequestForTests(app, session.uam_control_capability_id,
		    {{"requestId", id}, {"method", method}, {"arguments", {{"connectionId", connection}}}}, uam::time::SystemEpochMillisecondsNow());
	};
	UAM_ASSERT(receipt("handshake", "_mcp_initialized", "connection-one").value("ok", false));
	UAM_ASSERT_EQ(status(chat.id), std::string("handshake-confirmed"));
	UAM_ASSERT(!receipt("foreign-heartbeat", "_mcp_heartbeat", "foreign").value("ok", true));
	UAM_ASSERT(receipt("heartbeat", "_mcp_heartbeat", "connection-one").value("ok", false));
	UAM_ASSERT_EQ(status(chat.id), std::string("connected"));
	app.uam_control_capabilities.front().mcp_heartbeat_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() - 20001;
	UAM_ASSERT_EQ(status(chat.id), std::string("handshake-confirmed"));

	session.process_execution_host_id = "old-host";
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	session.process_execution_host_id = "local";
	UAM_ASSERT_EQ(status(other.id), std::string("configured-unverified"));
	UAM_ASSERT(!receipt("handshake", "_mcp_initialized", "connection-one").value("ok", true));
	UAM_ASSERT(receipt("replacement", "_mcp_initialized", "connection-two").value("ok", false));
	UAM_ASSERT(receipt("old-disconnect", "_mcp_disconnected", "connection-one").value("ok", false));
	UAM_ASSERT_EQ(status(chat.id), std::string("handshake-confirmed"));
	UAM_ASSERT(receipt("disconnect", "_mcp_disconnected", "connection-two").value("ok", false));
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	UAM_ASSERT(receipt("reconnect", "_mcp_initialized", "connection-three").value("ok", false));
	UAM_ASSERT_EQ(status(chat.id), std::string("handshake-confirmed"));
	const std::filesystem::path capability_path = app.uam_control_capabilities.front().directory;
	UAM_ASSERT(uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability_path));
	UAM_ASSERT_EQ(status(chat.id), std::string("missing"));
#if !defined(_WIN32)
	std::filesystem::create_directory_symlink(temp.root, capability_path);
	UAM_ASSERT_EQ(status(chat.id), std::string("missing"));
	UAM_ASSERT(!receipt("replaced-capability", "_mcp_initialized", "connection-three").value("ok", true));
	std::filesystem::remove(capability_path);
#endif
	uam::UamControlService::RevokeForSession(app, session);
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, chat, "session/new", setup));
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	session.running = false; session.last_error = "Model authentication failed.";
	UAM_ASSERT(!receipt("stopped", "_mcp_initialized", "connection-two").value("ok", true));
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	UAM_ASSERT(uam::provider_mcp_settings::Status(app, "local", "", chat.id)["providers"].front()["providerSessionFailed"].get<bool>());
	session.running = true; app.chats.front().uam_control_enabled = false;
	UAM_ASSERT_EQ(status(chat.id), std::string("disabled"));
	UAM_ASSERT(!uam::UamControlService::ValidStdioToolCallForTests({{"method", "tools/call"}, {"params", {{"name", "_mcp_initialized"}, {"arguments", {{"connectionId", "forged"}}}}}}));
	uam::UamControlService::RevokeForSession(app, session);
	UAM_ASSERT_EQ(status(chat.id), std::string("disabled"));
	app.chats.front().uam_control_enabled = true;
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	UAM_ASSERT_EQ(status(chat.id), std::string("configured-unverified"));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(ProviderMcpFileImportIsBoundedReadOnlyAndReportsUnsupportedJsonSyntax)
{
	uam_test::TempDir temp("mcp-file-import");
	const std::filesystem::path file = temp.root / "opencode.jsonc";
	const std::string original = "{ // Existing OpenCode settings\n \"mcp\": {\"remote\": {\"type\": \"remote\", \"url\": \"https://example.test/mcp\"}}}";
	UAM_ASSERT(uam::io::WriteTextFile(file, original));
	const nlohmann::json result = uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(file), "", "", "", {}, {});
	UAM_ASSERT_EQ(result["servers"].size(), std::size_t{1});
	std::string after; UAM_ASSERT(uam::io::TryReadTextFile(file, after)); UAM_ASSERT_EQ(after, original);
	const std::string jsonc = "{ // trailing commas are valid JSONC\n \"mcp\": {\"remote\": {\"type\": \"remote\", \"url\": \"https://example.test/mcp?a=,}\",},},}";
	UAM_ASSERT(uam::io::WriteTextFile(file, jsonc));
	const nlohmann::json directory = uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(temp.root), "", "{\"mcp\":{}}", "", {}, {});
	UAM_ASSERT_EQ(directory["servers"].size(), std::size_t{1});
	UAM_ASSERT_EQ(directory["sources"][0].get<std::string>(), uam::paths::Utf8PathString(file));
	UAM_ASSERT_EQ(directory["servers"][0]["url"].get<std::string>(), std::string("https://example.test/mcp?a=,}"));
	UAM_ASSERT(uam::io::TryReadTextFile(file, after)); UAM_ASSERT_EQ(after, jsonc);
	const std::filesystem::path global = temp.root / "global" / "opencode";
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(global));
	UAM_ASSERT(uam::io::WriteTextFile(global / "opencode.jsonc", jsonc));
	const nlohmann::json global_result = uam::provider_mcp_settings::ReadImport("", "", "{\"mcp\":{}}", "", global.parent_path(), {});
	UAM_ASSERT_EQ(global_result["servers"].size(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(global / "opencode.json", "{\"mcp\":{\"another\":{\"type\":\"remote\",\"url\":\"https://second.test/mcp\"}}}"));
	const nlohmann::json layered = uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(global), "", "", "", {}, {});
	UAM_ASSERT_EQ(layered["sources"].size(), std::size_t{2});
	UAM_ASSERT_EQ(layered["servers"].size(), std::size_t{2});
	for (const std::string& malformed : {std::string("{\"mcp\":{,}}"), std::string("{\"mcp\":{\"x\":,}}"), std::string("{/* unfinished")})
	{
		bool failed = false;
		try { (void)nlohmann::json::parse(uam::provider_mcp_settings::JsoncText(malformed)); }
		catch (const std::exception&) { failed = true; }
		UAM_ASSERT(failed);
	}

	bool rejected = false;
	try { (void)uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(temp.root / "missing"), "", "{\"mcp\":{}}", "", {}, {}); }
	catch (const std::runtime_error&) { rejected = true; }
	UAM_ASSERT(rejected);

	rejected = false;
	try { (void)uam::provider_mcp_settings::ReadImport("", "", std::string(uam::provider_setup::kMaxFileBytes + 1, ' '), "", {}, {}); }
	catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("2 MiB") != std::string::npos; }
	UAM_ASSERT(rejected);
	std::string bounded;
	std::filesystem::resize_file(file, uam::provider_setup::kMaxFileBytes + 1);
	UAM_ASSERT(!uam::provider_mcp_settings::ReadConfigFile(file, bounded));
	UAM_ASSERT(bounded.empty());
#if !defined(_WIN32)
	const std::filesystem::path fifo = temp.root / "config-fifo";
	UAM_ASSERT(mkfifo(fifo.c_str(), 0600) == 0);
	UAM_ASSERT(!uam::provider_mcp_settings::ReadConfigFile(fifo, bounded));
	const std::filesystem::path link = temp.root / "config-link";
	std::filesystem::create_symlink(file, link);
	UAM_ASSERT(!uam::provider_mcp_settings::ReadConfigFile(link, bounded));
#endif
}

UAM_TEST(ProviderMcpOverridesPersistOnlyForTheSelectedProviderAndHost)
{
	CentralProviderConfiguration configuration;
	std::string error;
	const nlohmann::json overrides = nlohmann::json::array({{{"providerId", "gemini-cli"}, {"executionHostId", "local"}, {"serverId", "uam-mcp"}, {"enabled", false}}});
	UAM_ASSERT(uam::central_configuration::Parse({{"uamControlEnabled", true}, {"mcpOverrides", overrides}}, configuration, error));
	const nlohmann::json serialized = uam::central_configuration::Serialize(configuration);
	UAM_ASSERT(serialized.contains("mcpOverrides"));
	UAM_ASSERT_EQ(serialized["mcpOverrides"], overrides);
	UAM_ASSERT(!uam::central_configuration::McpEnabled(configuration, "gemini-cli", "local", "uam-mcp", true));
	UAM_ASSERT(uam::central_configuration::McpEnabled(configuration, "codex-cli", "local", "uam-mcp", true));
	UAM_ASSERT(uam::central_configuration::McpEnabled(configuration, "gemini-cli", "ssh-other", "uam-mcp", true));
	const nlohmann::json duplicate = nlohmann::json::array({overrides.front(), overrides.front()});
	UAM_ASSERT(!uam::central_configuration::Parse({{"mcpOverrides", duplicate}}, configuration, error));
	UAM_ASSERT_EQ(uam::central_configuration::Serialize(configuration), serialized);
	uam_test::TempDir temp("mcp-provider-scope");
	AppSettings settings;
	McpServerConfiguration server; server.id = "docs"; server.name = "Docs"; server.transport = "http"; server.url = "https://docs.test/mcp"; server.enabled = false;
	settings.mcp_servers.push_back(server);
	settings.central_provider_configuration.mcp_overrides.push_back({"gemini-cli", "local", "docs", true});
	ChatSession chat; chat.provider_id = "gemini-cli"; chat.execution_host_id = "local";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root, settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"].size(), std::size_t{1});
	chat.provider_id = "codex-cli";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root, settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(bundle.local_directory.empty());
	UAM_ASSERT(bundle.manifest.is_null());
	UAM_ASSERT(bundle.files.empty());


}

UAM_TEST(ProviderMcpStatusDistinguishesIdleEnabledSetupFromAnActiveMissingCapability)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "idle-mcp-status";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	const auto status = [&]()
	{
		return uam::provider_mcp_settings::Status(app, "local", "", chat.id)["providers"].front();
	};
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("configured-unverified"));
	UAM_ASSERT_EQ(status()["activeSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ(status()["configuredSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ(status()["connectedSessionCount"].get<std::size_t>(), std::size_t{0});
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.process_execution_host_id = "local";
	session.running = true;
	session.lifecycle_state = "starting";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("configured-unverified"));
	UAM_ASSERT_EQ(status()["activeSessionCount"].get<std::size_t>(), std::size_t{1});
	UAM_ASSERT_EQ(status()["configuredSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ(status()["connectedSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ(status()["detail"].get<std::string>(), std::string("UAM MCP setup is in progress. No connection has been confirmed."));
	// Starting cannot excuse a capability that was already bound and is now missing.
	session.uam_control_capability_id = "missing-bound-capability";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	session.uam_control_capability_id.clear();
	// A ready session is no longer in the pre-binding setup window.
	session.session_ready = true;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	session.lifecycle_state = "ready";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	session.session_ready = false;
	session.lifecycle_state = "error";
	session.last_error = "Session setup failed.";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	session.lifecycle_state = "starting";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	session.last_error.clear();
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("configured-unverified"));
	app.chats.front().uam_control_explicitly_disabled = true;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("disabled"));
	app.chats.front().uam_control_explicitly_disabled = false;
	session.running = false;
	app.chats.front().uam_control_enabled = false;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("disabled"));
}

UAM_TEST(ProviderMcpStartingSessionDoesNotHideAnotherActiveMissingCapability)
{
	uam::AppState app;
	for (const char* id : {"starting-mcp-status", "ready-mcp-status"})
	{
		ChatSession chat;
		chat.id = id;
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.uam_control_enabled = true;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.process_execution_host_id = "local";
		session.running = true;
		session.lifecycle_state = app.acp_sessions.size() == 1 ? "starting" : "ready";
		session.session_ready = app.acp_sessions.size() != 1;
	}
	const auto status = [&](const std::string& chat_id)
	{
		return uam::provider_mcp_settings::Status(app, "local", "", chat_id)["providers"];
	};
	UAM_ASSERT_EQ(status("starting-mcp-status").front()["status"].get<std::string>(), std::string("configured-unverified"));
	UAM_ASSERT_EQ(status("ready-mcp-status").front()["status"].get<std::string>(), std::string("missing"));
	const nlohmann::json providers = status("");
	const auto codex = std::ranges::find_if(providers, [](const nlohmann::json& provider) { return provider["providerId"] == "codex-cli"; });
	UAM_ASSERT(codex != providers.end());
	UAM_ASSERT_EQ((*codex)["status"].get<std::string>(), std::string("missing"));
	UAM_ASSERT_EQ((*codex)["activeSessionCount"].get<std::size_t>(), std::size_t{2});
	UAM_ASSERT_EQ((*codex)["configuredSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ((*codex)["connectedSessionCount"].get<std::size_t>(), std::size_t{0});
}

UAM_TEST(ProviderMcpExplicitOptOutRejectsAnOtherwiseLiveLaunchOverride)
{
	uam_test::TempDir temp("mcp-opt-out-status");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "opt-out"; chat.provider_id = "codex-cli"; chat.execution_host_id = "local"; chat.uam_control_enabled = false;
	app.settings.central_provider_configuration.mcp_overrides.push_back({"codex-cli", "local", "uam-mcp", true});
	app.chats.push_back(chat);
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, app.chats.front(), "session/new", setup));
	uam::UamControlCapability& capability = app.uam_control_capabilities.front();
	UAM_ASSERT(capability.session_control_override_at_launch);
	capability.mcp_initialized = true;
	capability.mcp_initialized_at_epoch_ms = uam::time::SystemEpochMillisecondsNow();
	capability.mcp_heartbeat_at_epoch_ms = capability.mcp_initialized_at_epoch_ms;
	const auto status = [&]() { return uam::provider_mcp_settings::Status(app, "local", "", chat.id)["providers"].front(); };
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("connected"));
	app.chats.front().uam_control_explicitly_disabled = true;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("disabled"));
	UAM_ASSERT_EQ(status()["connectedSessionCount"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT(!status()["uamControlEnabled"].get<bool>());
	app.chats.front().uam_control_explicitly_disabled = false;
	capability.session_chat_id = "foreign-chat";
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	capability.session_chat_id = chat.id;
	capability.expires_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() - 1;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("missing"));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(ProviderMcpLayeredImportPreservesReferencesAndFilesOnMalformedLayer)
{
	uam_test::TempDir temp("mcp-layered-preservation");
	const std::filesystem::path first = temp.root / "config.json";
	const std::filesystem::path second = temp.root / "opencode.jsonc";
	const std::string original = R"({"mcp":{"docs":{"type":"remote","url":"https://example.test/mcp","enabled":false,"headers":{"Authorization":"{env:DOCS_TOKEN}","X-Literal":"fixture-secret"}}}})";
	const std::string overlay = R"({/* bounded layer */"mcp":{"docs":{"url":"https://other.test/mcp",},"manual":{"type":"remote","url":"https://example.test/mcp","oauth":false}}})";
	UAM_ASSERT(uam::io::WriteTextFile(first, original));
	UAM_ASSERT(uam::io::WriteTextFile(second, overlay));
	const nlohmann::json preview = uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(temp.root), uam::paths::Utf8PathString(temp.root / "workspace"), "", "", {}, {});
	UAM_ASSERT_EQ(preview["sources"].size(), std::size_t{2});
	UAM_ASSERT_EQ(preview["servers"].size(), std::size_t{1});
	const McpServerConfiguration server = uam::mcp_server_config::Parse(preview["servers"]).front();
	UAM_ASSERT_EQ(server.url, std::string("https://other.test/mcp"));
	UAM_ASSERT(!server.enabled);
	UAM_ASSERT_EQ(server.headers.front().environment_variable, std::string("DOCS_TOKEN"));
	UAM_ASSERT_EQ(server.headers.back().value, std::string("fixture-secret"));
	UAM_ASSERT_EQ(preview["warnings"].size(), std::size_t{1});
	UAM_ASSERT(preview["warnings"].dump().find("fixture-secret") == std::string::npos);
	std::string after;
	UAM_ASSERT(uam::io::TryReadTextFile(first, after)); UAM_ASSERT_EQ(after, original);
	UAM_ASSERT(uam::io::TryReadTextFile(second, after)); UAM_ASSERT_EQ(after, overlay);
	const std::string malformed = R"({"mcp": {"secret": "fixture-secret",)";
	UAM_ASSERT(uam::io::WriteTextFile(second, malformed));
	bool rejected = false;
	try { (void)uam::provider_mcp_settings::ReadImport(uam::paths::Utf8PathString(temp.root), "", "", "", {}, {}); }
	catch (const std::exception& error) { rejected = std::string(error.what()).find("fixture-secret") == std::string::npos; }
	UAM_ASSERT(rejected);
	UAM_ASSERT(uam::io::TryReadTextFile(first, after)); UAM_ASSERT_EQ(after, original);
	UAM_ASSERT(uam::io::TryReadTextFile(second, after)); UAM_ASSERT_EQ(after, malformed);
}

UAM_TEST(ProviderMcpAvailabilityKeepsAllSixProvidersAndHostSessionIdentitySeparate)
{
	uam::AppState app;
	const nlohmann::json overview = uam::provider_mcp_settings::Status(app, "local", "");
	UAM_ASSERT_EQ(overview["providers"].size(), std::size_t{6});
	for (const char* provider : uam::provider_ids::kAllCliProviderIds)
	{
		app.chats.clear(); app.acp_sessions.clear();
		ChatSession chat; chat.id = "identity-fixture"; chat.provider_id = provider; chat.execution_host_id = "fixture-host"; chat.uam_control_enabled = true;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		session.chat_id = chat.id; session.provider_id = provider; session.process_execution_host_id = "other-host"; session.running = true;
		const auto status = [&]() { return uam::provider_mcp_settings::Status(app, "fixture-host", "", chat.id)["providers"]; };
		UAM_ASSERT_EQ(status().size(), std::size_t{1});
		UAM_ASSERT_EQ(status().front()["providerId"].get<std::string>(), std::string(provider));
		UAM_ASSERT_EQ(status().front()["activeSessionCount"].get<std::size_t>(), std::size_t{0});
		session.process_execution_host_id.clear();
		UAM_ASSERT_EQ(status().front()["activeSessionCount"].get<std::size_t>(), std::size_t{0});
		session.process_execution_host_id = chat.execution_host_id;
		session.model_discovery_only = true;
		UAM_ASSERT_EQ(status().front()["activeSessionCount"].get<std::size_t>(), std::size_t{0});
		session.model_discovery_only = false;
		UAM_ASSERT_EQ(status().front()["status"].get<std::string>(), std::string("missing"));
		session.running = false; session.last_error = "Synthetic provider authentication failure";
		UAM_ASSERT_EQ(status().front()["status"].get<std::string>(), std::string("configured-unverified"));
		UAM_ASSERT(status().front()["providerSessionFailed"].get<bool>());
		bool rejected = false;
		try { (void)uam::provider_mcp_settings::Status(app, "other-host", "", chat.id); }
		catch (const std::exception&) { rejected = true; }
		UAM_ASSERT(rejected);
	}
}

UAM_TEST(ProviderMcpAdditionalServerCountsHonorProviderHostAndWorkspaceOverrides)
{
	uam::AppState app;
	McpServerConfiguration global{.id = "global", .name = "global", .transport = "http", .url = "https://fixture.test/mcp"};
	McpServerConfiguration opt_in = global; opt_in.id = "opt-in"; opt_in.name = "opt-in"; opt_in.enabled = false;
	McpServerConfiguration remote = global; remote.id = "remote"; remote.name = "remote"; remote.execution_host_id = "fixture-host";
	McpServerConfiguration workspace = global; workspace.id = "workspace"; workspace.name = "workspace"; workspace.workspace_directory = "/fixture/workspace";
	app.settings.mcp_servers = {global, opt_in, remote, workspace};
	app.settings.central_provider_configuration.mcp_overrides = {
	    {"codex-cli", "local", "global", false}, {"codex-cli", "local", "opt-in", true},
	    {"opencode-cli", "local", "global", false}, {"codex-cli", "fixture-host", "remote", false}};
	const auto count = [&](const std::string& provider, const std::string& host, const std::string& directory)
	{
		const nlohmann::json result = uam::provider_mcp_settings::Status(app, host, directory);
		for (const nlohmann::json& row : result["providers"])
			if (row["providerId"] == provider) return row["additionalServerCount"].get<std::size_t>();
		return std::size_t{999};
	};
	UAM_ASSERT_EQ(count("codex-cli", "local", ""), std::size_t{1});
	UAM_ASSERT_EQ(count("opencode-cli", "local", ""), std::size_t{0});
	UAM_ASSERT_EQ(count("claude-cli", "local", ""), std::size_t{1});
	UAM_ASSERT_EQ(count("codex-cli", "local", "/fixture/workspace"), std::size_t{2});
	UAM_ASSERT_EQ(count("codex-cli", "fixture-host", ""), std::size_t{1});
	UAM_ASSERT_EQ(count("opencode-cli", "fixture-host", ""), std::size_t{2});
}

UAM_TEST(ProviderMcpTerminalAvailabilityRejectsForeignHostStoppedAndOptedOutConnections)
{
	uam_test::TempDir temp("mcp-terminal-identity");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "terminal-mcp"; chat.provider_id = "antigravity-cli"; chat.execution_host_id = "local"; chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	UAM_ASSERT(uam::UamControlService::Initialize(app));
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.back(); terminal.running = true;
	terminal.uam_control_session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState& session = *terminal.uam_control_session;
	session.chat_id = chat.id; session.provider_id = chat.provider_id; session.process_execution_host_id = "local";
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, chat, "session/new", setup));
	uam::UamControlCapability& capability = app.uam_control_capabilities.front();
	capability.mcp_initialized = true;
	capability.mcp_initialized_at_epoch_ms = uam::time::SystemEpochMillisecondsNow();
	capability.mcp_heartbeat_at_epoch_ms = capability.mcp_initialized_at_epoch_ms;
	const auto status = [&]() { return uam::provider_mcp_settings::Status(app, "local", "", chat.id)["providers"].front(); };
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("connected"));
	session.process_execution_host_id = "foreign-host";
	UAM_ASSERT_EQ(status()["activeSessionCount"].get<std::size_t>(), std::size_t{0});
	session.process_execution_host_id = "local"; terminal.running = false;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("configured-unverified"));
	terminal.running = true; app.chats.front().uam_control_explicitly_disabled = true;
	UAM_ASSERT_EQ(status()["status"].get<std::string>(), std::string("disabled"));
	UAM_ASSERT_EQ(status()["connectedSessionCount"].get<std::size_t>(), std::size_t{0});
	uam::UamControlService::Shutdown(app);
}
