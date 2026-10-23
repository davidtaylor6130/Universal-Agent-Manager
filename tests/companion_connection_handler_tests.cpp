#include "test_harness.h"
#include "cef_handler_fixture.h"
#include "cef/uam_query_handler.h"
#include "cef/companion_server.h"
#include "common/platform/companion_network_platform.h"
#include "include/cef_app.h"

#if defined(__APPLE__)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace uam_test;

struct CompanionConnectionHandlerTestAccess
{
	static void Set(UamQueryHandler& handler, bool enabled, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleSetCompanionEnabled(nullptr, {{"enabled", enabled}}, callback);
	}
	static void Token(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleGetCompanionToken(nullptr, nlohmann::json::object(), callback);
	}
	static void Get(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleGetCompanionSettings(nullptr, nlohmann::json::object(), callback);
	}
	static void Restart(UamQueryHandler& handler, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		handler.HandleRestartCompanion(nullptr, nlohmann::json::object(), callback);
	}
	static void Stop(UamQueryHandler& handler)
	{
		if (handler.m_companion) handler.m_companion->Stop();
	}
};

namespace
{
	class ConnectionCallback final : public UamQueryHandler::Callback
	{
	public:
		nlohmann::json response;
		int failure = 0;
		void Success(const CefString& value) override { response = nlohmann::json::parse(value.ToString()); }
		void Success(const void*, size_t) override { failure = 500; }
		void Failure(int value, const CefString&) override { failure = value; }
	private:
		IMPLEMENT_REFCOUNTING(ConnectionCallback);
	};

#if defined(__APPLE__)
	bool PumpConnectionUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(10))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		do
		{
			CefDoMessageLoopWork();
			if (predicate()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		} while (std::chrono::steady_clock::now() < deadline);
		return predicate();
	}

	// Hold an OS-assigned loopback port until the fixture is ready to publish it.
	class ConnectionPortReservation
	{
	public:
		ConnectionPortReservation()
		{
			m_socket = socket(AF_INET, SOCK_STREAM, 0);
			UAM_ASSERT(m_socket >= 0);
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
			{
				Release();
				UAM_ASSERT(false);
			}
			socklen_t size = sizeof(address);
			if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &size) != 0)
			{
				Release();
				UAM_ASSERT(false);
			}
			port = ntohs(address.sin_port);
			UAM_ASSERT(port > 1024);
		}
		~ConnectionPortReservation() { Release(); }
		void Release() { if (m_socket >= 0) { close(m_socket); m_socket = -1; } }
		int port = 0;
	private:
		int m_socket = -1;
	};

	bool ConnectionListenerOpen(int port)
	{
		return uam::platform::ProbeCompanionTcpListener("127.0.0.1", port, std::chrono::milliseconds(20));
	}

	struct ConnectionRuntimeCleanup
	{
		UamQueryHandler& handler;
		~ConnectionRuntimeCleanup()
		{
			CompanionConnectionHandlerTestAccess::Stop(handler);
			// Keep fixture environment, PKI, handler and CEF alive until its worker releases all CEF refs.
			if (!PumpConnectionUntil([] { return UamCompanionService::AllStopped(); }))
			{
				std::cerr << "Companion fixture shutdown fence timed out; refusing unsafe CEF teardown.\n";
				std::abort();
			}
		}
	};

	struct ConnectionCurlResult
	{
		int status = 0;
		nlohmann::json body;
	};

	ConnectionCurlResult ReadConnectionApi(const fs::path& root, const fs::path& certificate, const std::string& endpoint, const std::string& origin, const std::string& token, const nlohmann::json& request = {{"action", "getCompanionCapabilities"}, {"payload", nlohmann::json::object()}})
	{
		const std::string request_body = request.dump();
		UAM_ASSERT(request_body.size() <= 60 * 1024);
		const auto body_file = root / "curl-response.json";
		const std::vector<std::string> argv{
		    "/usr/bin/curl", "--noproxy", "*", "--silent", "--show-error", "--connect-timeout", "2", "--max-time", "5",
		    "--cacert", certificate.string(), "--output", body_file.string(), "--write-out", "%{http_code}",
		    "--header", "Authorization: Bearer " + token, "--header", "Origin: " + origin,
		    "--header", "Content-Type: application/json", "--data", request_body, endpoint + "/api"};
		auto& service = PlatformServicesFactory::Instance().process_service;
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		UAM_ASSERT(service.StartStdioProcess(process, root, argv, &error));
		struct Cleanup
		{
			uam::platform::StdioProcessPlatformFields& process;
			~Cleanup() { PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true); }
		} cleanup{process};
		service.CloseStdioProcessInput(process);
		std::string output, diagnostics;
		auto drain = [&]
		{
			char buffer[4096];
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const auto count = service.ReadStdioProcessStdout(process, buffer, sizeof(buffer));
				if (count <= 0) break;
				output.append(buffer, static_cast<size_t>(count));
				UAM_ASSERT(output.size() <= 32768);
			}
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const auto count = service.ReadStdioProcessStderr(process, buffer, sizeof(buffer));
				if (count <= 0) break;
				diagnostics.append(buffer, static_cast<size_t>(count));
				UAM_ASSERT(diagnostics.size() <= 32768);
			}
		};
		int exit_code = -1;
		UAM_ASSERT(PumpConnectionUntil([&] { drain(); return service.PollStdioProcessExited(process, &exit_code); }, std::chrono::seconds(7)));
		drain();
		if (exit_code != 0) std::cerr << "Fixture HTTPS curl failed: " << diagnostics << '\n';
		UAM_ASSERT_EQ(exit_code, 0);
		UAM_ASSERT_EQ(output.size(), size_t(3));
		UAM_ASSERT(fs::file_size(body_file) <= 65536);
		return {std::stoi(output), nlohmann::json::parse(ReadFile(body_file))};
	}
#endif
}

UAM_TEST(CompanionToggleReturnsCompleteObservedConnectionState)
{
	TempDir temporary("uam-companion-observed-toggle");
	uam::AppState app;
	app.data_root = temporary.root;
	const auto token_file = temporary.root / "token.txt";
	UAM_ASSERT(uam::io::WriteTextFile(token_file, std::string(64, 'a')));
	const nlohmann::json config{{"enabled", true}, {"token_file", token_file.string()}, {"origin", "https://192.168.1.10:58950"}};
	UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "companion.json", config.dump()));
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///fixture/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<ConnectionCallback> callback = new ConnectionCallback;
	CompanionConnectionHandlerTestAccess::Set(handler, false, callback);
	UAM_ASSERT_EQ(callback->failure, 0);
	for (const char* field : {"configured", "enabled", "state", "reasonCode", "reason", "desiredUrl", "effectiveUrl", "url", "proxyRunning", "loopbackRunning", "localAddresses", "restartRequired", "generation"})
		UAM_ASSERT(callback->response.contains(field));
	UAM_ASSERT_EQ(callback->response["enabled"], false);
	UAM_ASSERT_EQ(callback->response["proxyRunning"], false);
	UAM_ASSERT_EQ(callback->response["effectiveUrl"], "");
	UAM_ASSERT_EQ(ReadFile(token_file), std::string(64, 'a'));
}

UAM_TEST(CompanionPairingTokenNeverPublishesAnUnreadySavedAddress)
{
	TempDir temporary("uam-companion-unready-pairing");
	uam::AppState app;
	app.data_root = temporary.root;
	const auto token_file = temporary.root / "token.txt";
	UAM_ASSERT(uam::io::WriteTextFile(token_file, std::string(64, 'b')));
	const nlohmann::json config{{"enabled", true}, {"token_file", token_file.string()}, {"origin", "https://192.168.1.10:58950"}};
	UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "companion.json", config.dump()));
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///fixture/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<ConnectionCallback> callback = new ConnectionCallback;
	CompanionConnectionHandlerTestAccess::Token(handler, callback);
	UAM_ASSERT_EQ(callback->failure, 0);
	UAM_ASSERT_EQ(callback->response["token"], std::string(64, 'b'));
	UAM_ASSERT_EQ(callback->response["url"], "");
	UAM_ASSERT_EQ(ReadFile(token_file), std::string(64, 'b'));
}

UAM_TEST(CompanionCorruptSettingsReportFailureWithoutInventedEnablement)
{
	TempDir temporary("uam-companion-corrupt-status");
	ScopedEnvVar token("UAM_COMPANION_TOKEN_FILE", ""), port("UAM_COMPANION_PORT", ""), origin("UAM_COMPANION_ORIGIN", "");
	uam::AppState app;
	app.data_root = temporary.root;
	UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "companion.json", "{broken"));
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///fixture/index.html");
	auto& handler = *handler_owner;
	CefRefPtr<ConnectionCallback> callback = new ConnectionCallback;
	CompanionConnectionHandlerTestAccess::Get(handler, callback);
	UAM_ASSERT_EQ(callback->failure, 0);
	UAM_ASSERT_EQ(callback->response["state"], "failed");
	UAM_ASSERT_EQ(callback->response["reasonCode"], "configurationInvalid");
	UAM_ASSERT_EQ(callback->response["configured"], false);
	UAM_ASSERT_EQ(callback->response["enabled"], false);
	UAM_ASSERT_EQ(callback->response["effectiveUrl"], "");
	UAM_ASSERT_EQ(ReadFile(temporary.root / "companion.json"), "{broken");
}

UAM_TEST(CompanionLiveRuntimeToggleRepairAndOriginReplacement)
{
#if defined(__APPLE__)
	const char* caddy_environment = std::getenv("UAM_TEST_CADDY");
	if (!caddy_environment || !*caddy_environment)
	{
		std::cout << "[SKIP] Set UAM_TEST_CADDY and UAM_TEST_CEF_APP_BUNDLE for isolated real CEF+Caddy coverage.\n";
		return;
	}
	const fs::path caddy(caddy_environment);
	UAM_ASSERT(caddy.is_absolute() && fs::is_regular_file(caddy) && access(caddy.c_str(), X_OK) == 0);
	const char* bundle = std::getenv("UAM_TEST_CEF_APP_BUNDLE");
	UAM_ASSERT(bundle && fs::is_directory(bundle));
	TempDir temporary("uam-companion-live-connection");
	ScopedEnvVar xdg_config("XDG_CONFIG_HOME", (temporary.root / "caddy-config").string());
	ScopedEnvVar xdg_data("XDG_DATA_HOME", (temporary.root / "caddy-env-data").string());
	ScopedEnvVar token_override("UAM_COMPANION_TOKEN_FILE", ""), port_override("UAM_COMPANION_PORT", ""), origin_override("UAM_COMPANION_ORIGIN", "");
	ConnectionPortReservation api, https_a, https_b;
	const std::string origin_a = "https://127.0.0.1:" + std::to_string(https_a.port);
	const std::string origin_b = "https://127.0.0.1:" + std::to_string(https_b.port);
	const std::string fake_token(64, 'd');
	const auto token_file = temporary.root / "token.txt";
	const auto proxy_file = temporary.root / "proxy.json";
	const auto certificate_file = temporary.root / "proxy-data/pki/authorities/local/root.crt";
	UAM_ASSERT(uam::io::WriteTextFile(token_file, fake_token));
	nlohmann::json config{{"enabled", false}, {"token_file", token_file.string()}, {"port", api.port}, {"origin", origin_a}, {"proxy_executable", caddy.string()}, {"proxy_config", proxy_file.string()}};
	auto write_configuration = [&](int https_port)
	{
		// No admin listener, global autosave, trust-store writes, production data, or external hosts.
		const nlohmann::json proxy{
		    {"admin", {{"disabled", true}}},
		    {"storage", {{"module", "file_system"}, {"root", (temporary.root / "proxy-data").string()}}},
		    {"apps", {
		        {"pki", {{"certificate_authorities", {{"local", {{"install_trust", false}}}}}}},
		        {"tls", {{"automation", {{"policies", nlohmann::json::array({{{"subjects", nlohmann::json::array({"127.0.0.1"})}, {"issuers", nlohmann::json::array({{{"module", "internal"}}})}}})}}}}},
		        {"http", {{"servers", {{"fixture", {
		            {"listen", nlohmann::json::array({"127.0.0.1:" + std::to_string(https_port)})},
		            {"tls_connection_policies", nlohmann::json::array({nlohmann::json::object()})},
		            {"automatic_https", {{"disable_redirects", true}}},
		            {"routes", nlohmann::json::array({{
		                {"match", nlohmann::json::array({{{"host", nlohmann::json::array({"127.0.0.1"})}}})},
		                {"handle", nlohmann::json::array({{{"handler", "reverse_proxy"}, {"upstreams", nlohmann::json::array({{{"dial", "127.0.0.1:" + std::to_string(api.port)}}})}}})},
		                {"terminal", true}}})}}}}}}}
		    }}};
		UAM_ASSERT(uam::io::WriteTextFile(proxy_file, proxy.dump()));
		UAM_ASSERT(uam::io::WriteTextFile(temporary.root / "companion.json", config.dump()));
	};
	write_configuration(https_a.port);
	uam::AppState app;
	app.data_root = temporary.root;
	auto handler_owner = UamQueryHandlerTestAccess::Create(app, "file:///fixture/index.html");
	auto& handler = *handler_owner;
	ConnectionRuntimeCleanup cleanup{handler};
	auto status = [&]
	{
		CefRefPtr<ConnectionCallback> callback = new ConnectionCallback;
		CompanionConnectionHandlerTestAccess::Get(handler, callback);
		UAM_ASSERT_EQ(callback->failure, 0);
		return callback->response;
	};
	auto set_enabled = [&](bool enabled)
	{
		CefRefPtr<ConnectionCallback> callback = new ConnectionCallback;
		CompanionConnectionHandlerTestAccess::Set(handler, enabled, callback);
		UAM_ASSERT_EQ(callback->failure, 0);
		UAM_ASSERT_EQ(callback->response["enabled"], enabled);
		UAM_ASSERT_EQ(callback->response["restartRequired"], false);
		config["enabled"] = enabled;
	};
	auto await_ready = [&](const std::string& origin)
	{
		nlohmann::json observed;
		const bool ready = PumpConnectionUntil([&]
		{
			observed = status();
			return observed["state"] == "ready" && observed["desiredUrl"] == origin + "/companion" && observed["effectiveUrl"] == origin + "/companion" && observed["proxyRunning"] == true && observed["loopbackRunning"] == true;
		});
		if (!ready) std::cerr << "Fixture readiness timed out: " << observed.value("state", "unknown") << '/' << observed.value("reasonCode", "unknown") << '\n';
		UAM_ASSERT(ready);
	};
	auto await_disabled = [&]
	{
		UAM_ASSERT(PumpConnectionUntil([&]
		{
			const auto observed = status();
			return observed["state"] == "disabled" && observed["effectiveUrl"] == "" && observed["proxyRunning"] == false && observed["loopbackRunning"] == false && !ConnectionListenerOpen(api.port) && !ConnectionListenerOpen(https_a.port) && !ConnectionListenerOpen(https_b.port);
		}));
	};
	api.Release();
	https_a.Release();
	UAM_ASSERT(!ConnectionListenerOpen(api.port) && !ConnectionListenerOpen(https_a.port));
	set_enabled(true);
	await_ready(origin_a);
	UAM_ASSERT(PumpConnectionUntil([&] { return fs::is_regular_file(certificate_file); }));
	const auto public_certificate = ReadFile(certificate_file);
	UAM_ASSERT(!public_certificate.empty());
	const auto initial_read = ReadConnectionApi(temporary.root, certificate_file, origin_a, origin_a, fake_token);
	UAM_ASSERT_EQ(initial_read.status, 200);
	UAM_ASSERT_EQ(initial_read.body["protocolVersion"], 1);

	// Exercise actual authenticated HTTP -> policy -> CEF -> native branch persistence.
	// The skill identity deliberately has no host file: only its captured source snapshot is authority.
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.git_enabled = false;
	app.settings.git_auto_worktree = false;
	const auto workspace = temporary.root / "branch-workspace";
	fs::create_directories(workspace);
	const std::string skill_file = (temporary.root / "captured-skill-not-on-disk.md").string();
	const std::string skill_snapshot = "  ORIGINAL CAPTURED SKILL\nretain exact spaces\n";
	ChatSession source = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
	const std::string source_id = source.id;
	source.title = "Private HTTP branch skill source";
	source.execution_host_id = "local";
	source.workspace_directory = workspace.string();
	source.messages = {{MessageRole::User, "Source user input with captured skill."}};
	source.messages[0].provider = source.provider_id;
	source.messages[0].created_at = "2026-10-18T12:00:00Z";
	source.messages[0].markdown_store_files = {skill_file};
	source.messages[0].markdown_store_prompt_blocks = {skill_snapshot};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, source));
	const auto source_primary = AppPaths::UamChatFilePath(app.data_root, source_id);
	const std::string source_before = ReadFile(source_primary);
	app.chats.push_back(std::move(source));
	ChatSession desktop = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
	const std::string desktop_id = desktop.id;
	desktop.title = "Unrelated desktop selection";
	desktop.execution_host_id = "local";
	desktop.workspace_directory = workspace.string();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, desktop));
	app.chats.push_back(std::move(desktop));
	ChatDomainService().SelectChatById(app, desktop_id);
	app.composer_text = "Unrelated unsaved desktop draft";
	const auto assert_source_and_desktop = [&]
	{
		UAM_ASSERT_EQ(ReadFile(source_primary), source_before);
		const auto* current_source = ChatDomainService().FindChatById(app, source_id);
		UAM_ASSERT(current_source && current_source->messages.size() == 1);
		UAM_ASSERT_EQ(current_source->messages[0].content, "Source user input with captured skill.");
		UAM_ASSERT_EQ(current_source->messages[0].markdown_store_files, std::vector<std::string>{skill_file});
		UAM_ASSERT_EQ(current_source->messages[0].markdown_store_prompt_blocks, std::vector<std::string>{skill_snapshot});
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), desktop_id);
		UAM_ASSERT_EQ(app.composer_text, "Unrelated unsaved desktop draft");
		UAM_ASSERT_EQ(app.settings.last_selected_chat_id, desktop_id);
		UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty() && app.worktree_operation_chat_ids.empty());
		UAM_ASSERT(!fs::exists(skill_file));
	};
	std::vector<std::string> branch_ids;
	for (const bool retain_skill : {true, false})
	{
		const nlohmann::json request{{"action", "branchFromMessage"}, {"payload", {
		    {"chatId", source_id}, {"messageIndex", 0}, {"content", "Edited through authenticated companion HTTP."},
		    {"operationId", retain_skill ? "http-retained-skill" : "http-remove-skill"}, {"regenerate", false},
		    {"retainedAttachmentIds", nlohmann::json::array()},
		    {"retainedMarkdownStoreFiles", retain_skill ? nlohmann::json::array({skill_file}) : nlohmann::json::array()}}}};
		const auto branched = ReadConnectionApi(temporary.root, certificate_file, origin_a, origin_a, fake_token, request);
		UAM_ASSERT_EQ(branched.status, 200);
		const std::string branch_id = branched.body.at("chatId").get<std::string>();
		UAM_ASSERT(!branch_id.empty() && branch_id != source_id && branch_id != desktop_id);
		UAM_ASSERT(std::ranges::find(branch_ids, branch_id) == branch_ids.end());
		branch_ids.push_back(branch_id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, branch_id, true);
		UAM_ASSERT(saved && saved->messages.size() == 1);
		UAM_ASSERT_EQ(saved->parent_chat_id, source_id);
		UAM_ASSERT_EQ(saved->messages[0].content, "Edited through authenticated companion HTTP.");
		UAM_ASSERT_EQ(saved->messages[0].markdown_store_files, retain_skill ? std::vector<std::string>{skill_file} : std::vector<std::string>{});
		UAM_ASSERT_EQ(saved->messages[0].markdown_store_prompt_blocks, retain_skill ? std::vector<std::string>{skill_snapshot} : std::vector<std::string>{});
		assert_source_and_desktop();
	}
	const auto chat_count = app.chats.size();
	const auto forged = ReadConnectionApi(temporary.root, certificate_file, origin_a, origin_a, fake_token,
	    {{"action", "branchFromMessage"}, {"payload", {{"chatId", source_id}, {"messageIndex", 0},
	        {"content", "Must not attach a new host skill."}, {"operationId", "http-forged-skill"}, {"regenerate", false},
	        {"retainedMarkdownStoreFiles", nlohmann::json::array({(temporary.root / "not-captured.md").string()})}}}});
	UAM_ASSERT_EQ(forged.status, 409);
	UAM_ASSERT_EQ(app.chats.size(), chat_count);
	const auto saved_chats = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(saved_chats.size(), chat_count);
	for (const auto& saved : saved_chats) UAM_ASSERT(saved.branch_operation_id != "http-forged-skill");
	assert_source_and_desktop();
	set_enabled(false);
	await_disabled();
	set_enabled(true);
	await_ready(origin_a);
	UAM_ASSERT_EQ(ReadConnectionApi(temporary.root, certificate_file, origin_a, origin_a, fake_token).status, 200);

	// Replace an active publication through the same recovery handler used by settings.
	const auto previous_generation = status()["generation"].get<uint64_t>();
	config["origin"] = origin_b;
	write_configuration(https_b.port);
	https_b.Release();
	CefRefPtr<ConnectionCallback> restarted = new ConnectionCallback;
	CompanionConnectionHandlerTestAccess::Restart(handler, restarted);
	UAM_ASSERT_EQ(restarted->failure, 0);
	UAM_ASSERT(restarted->response["generation"].get<uint64_t>() > previous_generation);
	UAM_ASSERT_EQ(restarted->response["desiredUrl"], origin_b + "/companion");
	UAM_ASSERT(PumpConnectionUntil([&]
	{
		// Recheck the old listener after observing the new one to avoid a transition sampling race.
		if (ConnectionListenerOpen(https_b.port)) UAM_ASSERT(!ConnectionListenerOpen(https_a.port));
		return status()["state"] == "ready" && status()["effectiveUrl"] == origin_b + "/companion";
	}));
	await_ready(origin_b);
	UAM_ASSERT(!ConnectionListenerOpen(https_a.port));
	UAM_ASSERT(ConnectionListenerOpen(api.port) && ConnectionListenerOpen(https_b.port));
	UAM_ASSERT_EQ(ReadConnectionApi(temporary.root, certificate_file, origin_b, origin_b, fake_token).status, 200);
	// Origin mismatch deliberately shares the transport's existing authentication rejection.
	UAM_ASSERT_EQ(ReadConnectionApi(temporary.root, certificate_file, origin_b, origin_a, fake_token).status, 401);
	UAM_ASSERT_EQ(ReadFile(token_file), fake_token);
	UAM_ASSERT_EQ(ReadFile(certificate_file), public_certificate);
	CefRefPtr<ConnectionCallback> pairing = new ConnectionCallback;
	CompanionConnectionHandlerTestAccess::Token(handler, pairing);
	UAM_ASSERT_EQ(pairing->failure, 0);
	UAM_ASSERT_EQ(pairing->response["token"], fake_token);
	UAM_ASSERT_EQ(pairing->response["url"], origin_b + "/companion");
	UAM_ASSERT(!pairing->response.value("certificate", std::string{}).empty());
	set_enabled(false);
	await_disabled();
#else
	std::cout << "[SKIP] Real CEF+Caddy connection coverage currently requires the macOS fixture.\n";
#endif
}
