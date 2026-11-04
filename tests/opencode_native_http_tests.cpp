#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "test_harness.h"
#include "common/provider/opencode_native_http.h"

using namespace uam_test;

UAM_TEST(OpenCodeNativeHttpUsesOnlyTypedLoopbackRoutesAndPipedAuthentication)
{
	const uam::opencode_native::Endpoint endpoint{49152, "private-native-token"};
	std::string url, method, config, error;
	UAM_ASSERT(uam::opencode_native::BuildRequest(endpoint, "/workspace/a b#c", {{"operation", "session"}, {"nativeSessionId", "ses_owned"}}, url, method, config, error));
	UAM_ASSERT_EQ(url, std::string("http://127.0.0.1:49152/session/ses_owned?directory=%2Fworkspace%2Fa%20b%23c"));
	UAM_ASSERT_EQ(method, std::string("GET"));
	UAM_ASSERT(url.find(endpoint.token) == std::string::npos);
	UAM_ASSERT(config.find(uam::base64::Encode("opencode:" + endpoint.token)) != std::string::npos);
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "session"}, {"nativeSessionId", "../foreign"}}, url, method, config, error));
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "http://other-host"}}, url, method, config, error));
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, std::string("/work\0space", 11), {{"operation", "status"}}, url, method, config, error));
	UAM_ASSERT(!uam::opencode_native::BuildRequest({80, ""}, "/workspace", {{"operation", "status"}}, url, method, config, error));
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", 5}}, url, method, config, error));
}

UAM_TEST(OpenCodeNativeHttpEnablesOnlyTaskAskAndOffersOnlyOneTimePermissionReplies)
{
	const uam::opencode_native::Endpoint endpoint{49152, "private-native-token"};
	std::string url, method, config, error;
	UAM_ASSERT(uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "enableTask"}, {"nativeSessionId", "ses_owned"}}, url, method, config, error));
	UAM_ASSERT_EQ(method, std::string("PATCH"));
	UAM_ASSERT(config.find("task") != std::string::npos);
	UAM_ASSERT(config.find("ask") != std::string::npos);
	UAM_ASSERT(config.find("allow") == std::string::npos);
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "enableTask"}, {"nativeSessionId", "ses_owned"}, {"readOnly", true}}, url, method, config, error));
	UAM_ASSERT(uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "disableTask"}, {"nativeSessionId", "ses_owned"}}, url, method, config, error));
	UAM_ASSERT(config.find("deny") != std::string::npos && config.find("edit") == std::string::npos && config.find("bash") == std::string::npos);
	for (const std::string reply : {"once", "reject"})
	{
		UAM_ASSERT(uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "reply"}, {"requestId", "per_owned"}, {"reply", reply}}, url, method, config, error));
		UAM_ASSERT_EQ(method, std::string("POST"));
		UAM_ASSERT(url.find("/permission/per_owned/reply?") != std::string::npos);
	}
	UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace", {{"operation", "reply"}, {"requestId", "per_owned"}, {"reply", "always"}}, url, method, config, error));
}

UAM_TEST(OpenCodeNativeRunnerControlRequiresExistingPrivateProcessAuthority)
{
	TempDir temp("uam-native-http-authority");
	uam::remote::RunnerState state;
	nlohmann::json request{{"id", "native"}, {"type", "opencode.native"}, {"sessionId", "acp-owned"},
	    {"controlToken", "not-the-owner"}, {"operation", {{"operation", "permissions"}}}};
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
	const std::string executable = uam::paths::Utf8PathString(PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath());
	const auto started = state.HandleProcessRequest({{"id", "start"}, {"type", "process.start"}, {"sessionId", "acp-owned"},
	    {"controlToken", "actual-private-owner"}, {"cwd", uam::paths::Utf8PathString(temp.root)},
	    {"argv", nlohmann::json::array({executable, "--uam-test-codex-setup", "acp", "--hostname", "127.0.0.1", "--port", "49152"})},
	    {"environment", {{"OPENCODE_SERVER_PASSWORD", "private-native-token"}}}});
	UAM_ASSERT(started.value("ok", false));
	const auto rejected = state.HandleProcessRequest(request);
	UAM_ASSERT(!rejected.value("ok", true));
	UAM_ASSERT_EQ(rejected["error"]["code"], nlohmann::json("unauthorized"));
}

UAM_TEST(OpenCodeNativeCompanionLaunchRequiresNativeExecutableAndUnclaimedNetworkPolicy)
{
	using uam::opencode_native::SupportsCompanionLaunch;
	UAM_ASSERT(SupportsCompanionLaunch({"opencode", "acp"}, "macos"));
	UAM_ASSERT(SupportsCompanionLaunch({"/opt/homebrew/bin/opencode", "--pure", "acp"}, "macos"));
	UAM_ASSERT(SupportsCompanionLaunch({"C:\\Program Files\\OpenCode\\OpenCode.exe", "acp"}, "windows"));
	UAM_ASSERT(SupportsCompanionLaunch({"C:\\node\\OPENCODE.CMD", "acp"}, "windows"));
	for (const std::string extension : {".sh", ".py", ".wrapper", ".ps1", ".exe.sh"})
	{
		UAM_ASSERT(!SupportsCompanionLaunch({"/tmp/opencode" + extension, "acp"}, "macos"));
		UAM_ASSERT(!SupportsCompanionLaunch({"C:\\tools\\opencode" + extension, "acp"}, "windows"));
	}
	UAM_ASSERT(!SupportsCompanionLaunch({"/tmp/opencode.exe", "acp"}, "macos"));
	UAM_ASSERT(!SupportsCompanionLaunch({}, "macos"));
	UAM_ASSERT(!SupportsCompanionLaunch({"opencode"}, "macos"));
	UAM_ASSERT(!SupportsCompanionLaunch({"/bin/sh", "wrapper.sh", "acp"}, "macos"));
	UAM_ASSERT(!SupportsCompanionLaunch({"opencode-wrapper", "acp"}, "macos"));
	for (const std::string option : {"--port", "--port=5111", "--hostname=localhost", "--mini", "--cors", "--mdns", "--no-mdns", "attach"})
		UAM_ASSERT(!SupportsCompanionLaunch({"opencode", "acp", option}, "macos"));
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
UAM_TEST(OpenCodeTextWorkerSelectsCheckedLocalV2WithoutLegacyFlags)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	const auto profile = ProviderProfileStore::DefaultOpenCodeProfile();
	uam::AppState app;
	auto& version = app.runtime_cli_versions_by_provider_id["opencode-cli"];
	const std::vector<std::string> original{"opencode", "run", "--pure", "--dir", "/owned-worker", "--model", "mock/demo/high", "--"};
	auto argv = original; std::string error;
	UAM_ASSERT(!runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
	version.checked = true; version.installed_version = "1.19.4";
	UAM_ASSERT(!runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
	version.supported = true;
	UAM_ASSERT(runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
	version.installed_version = "2.1.2";
	bool private_isolation = false;
	UAM_ASSERT(runtime.PrepareLocalWorkerLaunch(app, profile, argv, error, &private_isolation)); UAM_ASSERT(private_isolation);
	UAM_ASSERT_EQ(argv.size(), std::size_t(5));
	UAM_ASSERT_EQ(argv[1], std::string("--uam-opencode-v2-text-worker"));
	UAM_ASSERT_EQ(argv[2], std::string("mock/demo/high"));
	argv = original; version.installed_version = "2.2.0";
	UAM_ASSERT(!runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
	version.installed_version = "2.1.2"; version.execution_host.transport = "ssh";
	UAM_ASSERT(!runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
	version.execution_host = {}; version.check_error = "probe failed";
	UAM_ASSERT(!runtime.PrepareLocalWorkerLaunch(app, profile, argv, error)); UAM_ASSERT_EQ(argv, original);
}

UAM_TEST(OpenCodeStructuredCompatibilitySeparatesV1FromUnverifiedNewMajors)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	const auto profile = ProviderProfileStore::DefaultOpenCodeProfile();
	uam::AppState app;
	auto& version = app.runtime_cli_versions_by_provider_id["opencode-cli"];
	version.checked = true; version.supported = true;
	for (const std::string installed : {"1.15.13", "1.19.4", "1.99.99"})
	{
		version.installed_version = installed;
		UAM_ASSERT(runtime.StructuredCliVersionError(profile, installed).empty());
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).empty());
	}
	for (const std::string installed : {"2.0.0", "2.2.0", "v2.1.2", "2.0.0-beta.1", "3.0.0"})
	{
		version.installed_version = installed;
		UAM_ASSERT(!runtime.StructuredCliVersionError(profile, installed).empty());
		UAM_ASSERT(runtime.StructuredCliVersionError(profile, installed).find("protocol version") != std::string::npos);
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).empty());
	}
	for (const std::string command : {"/opt/other/bin/opencode", "C:\\other\\opencode.exe", "opencode-wrapper", "/bin/sh wrapper.sh"})
	{
		auto custom = profile; custom.interactive_command = command;
		UAM_ASSERT(runtime.StructuredCliVersionError(custom, "2.1.2").empty());
	}
	UAM_ASSERT(runtime.StructuredCliVersionError(profile, "2.1.2").empty());
	UAM_ASSERT(!runtime.StructuredCliVersionError(profile, "2.1.2", true).empty());
	// Installation minimums remain separate from native protocol selection.
	UAM_ASSERT(ProviderCliCompatibilityService().IsSupportedVersionForProvider("opencode-cli", "2.1.2"));
	ChatSession chat; chat.native_session_id = "ses_saved_v1";
	UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(ProviderProfileStore::DefaultOpenCodeProfile(), chat), (std::vector<std::string>{"opencode", "acp"}));
	const auto environment = runtime.BuildStructuredLaunchEnvironment(ProviderProfileStore::DefaultOpenCodeProfile(), chat);
	const auto permission = std::find_if(environment.begin(), environment.end(), [](const auto& entry) { return entry.first == "OPENCODE_PERMISSION"; });
	UAM_ASSERT(permission != environment.end());
	const auto policy = nlohmann::json::parse(permission->second);
	UAM_ASSERT_EQ(policy["*"], nlohmann::json("ask"));
	UAM_ASSERT_EQ(policy["task"], nlohmann::json("deny"));
}
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && !defined(_WIN32)
#include "common/runtime/acp/acp_session_internal.h"
#include "app/opencode_native_activity_service.h"

UAM_TEST(OpenCodeUnverifiedV2RefusesLegacyAcpBeforeLaunchingOrChangingSession)
{
	TempDir temp("uam-opencode-v2-compatibility");
	ScopedEnvVar path("PATH", temp.root.string() + ":/usr/bin:/bin");
	const auto marker = temp.root / "launched", script = temp.root / "opencode";
	UAM_ASSERT(uam::io::WriteTextFile(script, "#!/bin/sh\nprintf 'DESCRIPTION\\n' > \"${0%/*}/launched\"\n"));
	fs::permissions(script, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.git_auto_worktree = false;
	app.settings.central_provider_configuration.enabled = false;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	app.provider_profiles.push_back(provider);
	ChatSession chat;
	chat.id = "opencode-v2-blocked"; chat.provider_id = provider.id; chat.execution_host_id = "local";
	chat.native_session_id = "ses_retained_v1"; chat.workspace_directory = temp.root.string(); chat.uam_control_enabled = false;
	app.chats.push_back(chat);
	auto& version = app.runtime_cli_versions_by_provider_id[provider.id];
	version.execution_host = uam::execution_hosts::LocalHost();
	version.checked = true; version.supported = true; version.installed_version = "2.2.0";
	uam::AcpSessionState session;
	session.queued_prompt = "retain authored input";
	std::string error;
	const bool started = uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error);
	// Also settle the private fixture if the regression returns: never leave a child running.
	auto& processes = PlatformServicesFactory::Instance().process_service;
	processes.StopStdioProcess(session, true); processes.CloseStdioProcessHandles(session);
	UAM_ASSERT(!started);
	UAM_ASSERT(error.find("V2") != std::string::npos);
	UAM_ASSERT_EQ(session.last_error, error);
	UAM_ASSERT(!fs::exists(marker));
	UAM_ASSERT(!session.running && !session.opencode_native_companion);
	UAM_ASSERT(session.provider_configuration_directory.empty());
	UAM_ASSERT_EQ(session.queued_prompt, std::string("retain authored input"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("ses_retained_v1"));
	UAM_ASSERT(app.runtime_cli_install_tasks.empty());
}

UAM_TEST(OpenCodeV2SelectsOwnedLocalBridgeAndCapturesInputRestrictions)
{
	TempDir temp("uam-opencode-v2-selection");
	const auto profile = ProviderProfileStore::DefaultOpenCodeProfile();
	const auto& runtime = ProviderRuntimeRegistry::Resolve(profile);
	uam::AcpSessionState session;
	std::vector<std::string> argv{"opencode", "acp"};
	std::vector<std::pair<std::string, std::string>> environment{{"OPENCODE_PERMISSION", "legacy"}, {"OPENCODE_CONFIG_CONTENT", "{}"}};
	std::string error;
	session.saved_image_retry = std::make_shared<uam::SavedImageRetryPermission>();
	UAM_ASSERT(!runtime.PrepareStructuredLaunch(session, profile, "2.1.2", false, argv, environment, error));
	UAM_ASSERT_EQ(argv, (std::vector<std::string>{"opencode", "acp"}));
	session.saved_image_retry.reset();
	UAM_ASSERT(runtime.PrepareStructuredLaunch(session, profile, "2.1.2", false, argv, environment, error));
	UAM_ASSERT(session.opencode_v2_native);
	UAM_ASSERT_EQ(argv.size(), std::size_t{4});
	UAM_ASSERT_EQ(argv[1], std::string("--uam-opencode-v2-bridge"));
	UAM_ASSERT_EQ(argv.back(), std::string("opencode"));
	UAM_ASSERT(std::ranges::none_of(environment, [](const auto& entry) { return entry.first == "OPENCODE_PERMISSION"; }));
	ChatSession chat; chat.native_session_id = "ses_exact";
	session.active_uam_agent_workspace_access = "read"; session.session_id = chat.native_session_id;
	std::string method;
	const auto setup = runtime.OnAcpBuildSessionSetupRequest(session, 1, chat, "/workspace", true, method);
	UAM_ASSERT_EQ(method, std::string("session/load"));
	UAM_ASSERT_EQ(setup["params"]["sessionId"], nlohmann::json("ses_exact"));
	UAM_ASSERT_EQ(setup["params"]["_meta"]["uamPolicy"]["readOnly"], nlohmann::json(true));
	const auto prompt = runtime.OnAcpBuildPrompt(session, 2, "captured input", chat, method);
	UAM_ASSERT_EQ(prompt["params"]["_meta"]["uamPolicy"]["readOnly"], nlohmann::json(true));
	UAM_ASSERT(!runtime.SupportsSavedImageRetry(session));
	session.opencode_v2_native = false; session.agent_version = "2.1.2";
	uam::AppState app;
	app.data_root = temp.root / "data";
	UAM_ASSERT(runtime.OnAcpPrepareSetup(app, session, chat, "/workspace", error) == AcpRequestPreflight::Rejected);
	chat.id = "owned-v2-binding"; chat.provider_id = "opencode-cli";
	session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = true; session.opencode_v2_native = true;
	(void)runtime.OnAcpHandleResult(app, session, chat, "session/load", "owned", {{"sessionId", "ses_exact"}});
	UAM_ASSERT(chat.opencode_v2_session_id == std::optional<std::string>{"ses_exact"});
	session.opencode_v2_native = false;
	session.agent_version = "1.19.4"; chat.opencode_v2_session_id = chat.native_session_id;
	UAM_ASSERT(!runtime.StructuredCliVersionError(profile, "1.19.4", false, &chat).empty());
	UAM_ASSERT(runtime.OnAcpPrepareSetup(app, session, chat, "/workspace", error) == AcpRequestPreflight::Rejected);
	UAM_ASSERT_EQ(chat.native_session_id, std::string("ses_exact"));
	chat.opencode_v2_session_id = "ses_different_binding";
	UAM_ASSERT(runtime.StructuredCliVersionError(profile, "1.19.4", false, &chat).empty());
	UAM_ASSERT(runtime.OnAcpPrepareSetup(app, session, chat, "/workspace", error) == AcpRequestPreflight::Ready);
	UAM_ASSERT(!runtime.PrepareStructuredLaunch(session, profile, "2.1.2", true, argv, environment, error));
	UAM_ASSERT(error.find("SSH helper") != std::string::npos);
	argv = {"opencode", "acp"};
	UAM_ASSERT(runtime.PrepareStructuredLaunch(session, profile, "1.19.4", false, argv, environment, error));
	UAM_ASSERT(!session.opencode_v2_native);
	UAM_ASSERT_EQ(argv, (std::vector<std::string>{"opencode", "acp"}));
}

UAM_TEST(OpenCodeV2OriginHintRoundTripsWithoutGrantingLegacyResume)
{
	TempDir temp("uam-opencode-v2-origin");
	ChatSession chat; chat.id = "chat-opencode-v2-origin"; chat.provider_id = "opencode-cli";
	chat.native_session_id = "ses_exact_v2"; chat.opencode_v2_session_id = chat.native_session_id;
	chat.created_at = "2026-10-23T00:00:00.000Z"; chat.updated_at = chat.created_at;
	Message message; message.role = MessageRole::User; message.content = "Retain exact authored input"; message.created_at = chat.created_at;
	chat.messages.push_back(message);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	auto metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata && metadata->opencode_v2_session_id == chat.opencode_v2_session_id);
	metadata->title = "Metadata-only update";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *metadata));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded && loaded->opencode_v2_session_id == chat.opencode_v2_session_id);
	UAM_ASSERT_EQ(loaded->native_session_id, chat.native_session_id);
	UAM_ASSERT_EQ(loaded->messages, chat.messages);
}

UAM_TEST(OpenCodeCustomAcpWrapperKeepsBasicLaunchWithoutNativeCompanionGate)
{
	TempDir temp("uam-opencode-custom-acp");
	const auto script = temp.root / "capture.sh", capture = temp.root / "launch.txt";
	UAM_ASSERT(uam::io::WriteTextFile(script,
	    "#!/bin/sh\nprintf '%s\\n' \"$OPENCODE_PERMISSION\" \"$OPENCODE_SERVER_PASSWORD\" \"$@\" > \"$1\"\nIFS= read -r line\n"));
	ScopedEnvVar password("OPENCODE_SERVER_PASSWORD", "existing-user-password");
	uam::AppState app; app.data_root = temp.root / "data"; app.settings.central_provider_configuration.enabled = false;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	provider.interactive_command = "/bin/sh \"" + script.string() + "\" \"" + capture.string() + "\"";
	app.provider_profiles.push_back(provider);
	ChatSession chat; chat.id = "custom-opencode-basic"; chat.provider_id = provider.id; chat.execution_host_id = "local";
	chat.workspace_directory = temp.root.string(); chat.uam_control_enabled = false;
	chat.remote_opencode_native_port = 50000; chat.remote_opencode_native_token = "stale-previous-native-companion";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	std::string error;
	const bool started = uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error);
	std::string bytes;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (std::chrono::steady_clock::now() < deadline && bytes.empty())
	{
		(void)uam::io::TryReadTextFile(capture, bytes);
		if (bytes.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	auto& processes = PlatformServicesFactory::Instance().process_service;
	processes.StopStdioProcess(session, true); processes.CloseStdioProcessHandles(session);
	UAM_ASSERT(started);
	UAM_ASSERT(!session.opencode_native_companion);
	UAM_ASSERT_EQ(app.chats.front().remote_opencode_native_port, 0);
	UAM_ASSERT(app.chats.front().remote_opencode_native_token.empty());
	const auto first_line = bytes.find('\n'); UAM_ASSERT(first_line != std::string::npos);
	const auto permission = nlohmann::json::parse(bytes.substr(0, first_line), nullptr, false);
	UAM_ASSERT(permission.is_object()); UAM_ASSERT_EQ(permission["task"], nlohmann::json("deny"));
	UAM_ASSERT(bytes.find("existing-user-password") != std::string::npos);
	UAM_ASSERT(bytes.find("--hostname") == std::string::npos && bytes.find("--port") == std::string::npos && bytes.find("--no-mdns") == std::string::npos);
	uam::OpenCodeNativeActivityService service;
	UAM_ASSERT(service.BeforeDispatch(app, session, app.chats.front(), error) == uam::OpenCodeNativeGateState::NotApplicable);
}
#endif

UAM_TEST(OpenCodeTerminalOccupiedPortFallsBackWithoutChangingUserLaunchOptions)
{
#if defined(_WIN32)
	WSADATA startup{};
	UAM_ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &startup), 0);
	const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	UAM_ASSERT(listener != INVALID_SOCKET);
	struct Guard { SOCKET socket; ~Guard() { closesocket(socket); WSACleanup(); } } guard{listener};
	const BOOL exclusive = TRUE;
	UAM_ASSERT_EQ(setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)), 0);
#else
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	UAM_ASSERT(listener >= 0);
	struct Guard { int socket; ~Guard() { close(socket); } } guard{listener};
#endif
	sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
	UAM_ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
	UAM_ASSERT_EQ(listen(listener, 1), 0);
#if defined(_WIN32)
	int size = sizeof(address);
#else
	socklen_t size = sizeof(address);
#endif
	UAM_ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size), 0);
	const auto original_argv = std::vector<std::string>{"opencode", "--session", "ses_exact", "--model", "custom/model"};
	const auto original_env = std::vector<std::pair<std::string, std::string>>{{"OPENCODE_SERVER_PASSWORD", "user-owned-password"}, {"USER_SETTING", "preserved"}};
	auto argv = original_argv; auto environment = original_env;
	argv.insert(argv.end(), {"--hostname", "127.0.0.1", "--port", std::to_string(ntohs(address.sin_port)), "--no-mdns"});
	environment.emplace_back("OPENCODE_SERVER_USERNAME", "opencode"); environment.emplace_back("OPENCODE_SERVER_PASSWORD", "private-observer-credential");
	const auto observed_argv = argv; const auto observed_env = environment;
	UAM_ASSERT(!uam::opencode_native::RemoveConflictingTerminalObserver(argv, environment, "linux", [](int) { return true; }));
	UAM_ASSERT_EQ(argv, observed_argv); UAM_ASSERT_EQ(environment, observed_env);
	UAM_ASSERT(uam::opencode_native::RemoveConflictingTerminalObserver(argv, environment, "linux"));
	UAM_ASSERT_EQ(argv, original_argv); UAM_ASSERT_EQ(environment, original_env);
	auto custom = std::vector<std::string>{"opencode", "--hostname", "user-host", "--port", "6000"}; const auto original_custom = custom;
	UAM_ASSERT(!uam::opencode_native::RemoveConflictingTerminalObserver(custom, environment, "linux", [](int) { return false; }));
	UAM_ASSERT_EQ(custom, original_custom); UAM_ASSERT_EQ(environment, original_env);
}


UAM_TEST(OpenCodeImageEvidenceQueriesKeepFixedReadOnlyAuthenticatedRoutes)
{
	const uam::opencode_native::Endpoint endpoint{49152, "private-image-evidence-token"};
	std::string url, method, config, error;
	for (const auto& [operation, expected] : std::vector<std::pair<std::string, std::string>>{
	    {"health", "http://127.0.0.1:49152/global/health"},
	    {"configProviders", "http://127.0.0.1:49152/config/providers?directory=%2Fworkspace%2Fa%20b%23c"}})
	{
		const nlohmann::json request{{"operation", operation}, {"url", "https://unowned.example/"},
		    {"method", "POST"}, {"body", {{"permission", "allow"}}}};
		UAM_ASSERT(uam::opencode_native::BuildRequest(endpoint, "/workspace/a b#c", request, url, method, config, error));
		UAM_ASSERT_EQ(url, expected);
		UAM_ASSERT_EQ(method, std::string("GET"));
		UAM_ASSERT(url.find(endpoint.token) == std::string::npos);
		UAM_ASSERT(config.find(uam::base64::Encode("opencode:" + endpoint.token)) != std::string::npos);
		UAM_ASSERT(config.find("data =") == std::string::npos);
		UAM_ASSERT(url.find("unowned") == std::string::npos && config.find("allow") == std::string::npos);
	}
	for (const std::string operation : {"health", "configProviders"})
	{
		UAM_ASSERT(!uam::opencode_native::BuildRequest({49152, ""}, "/workspace", {{"operation", operation}}, url, method, config, error));
		UAM_ASSERT(!uam::opencode_native::BuildRequest(endpoint, "/workspace\nother", {{"operation", operation}}, url, method, config, error));
	}
}


#if defined(__APPLE__)
UAM_TEST(OpenCodeNativeHttpWaitsForActualDelayedOwnedResponse)
{
	TempDir temp("uam-native-http-would-block");
	const auto script = temp.root / "owned-http.py";
	UAM_ASSERT(uam::io::WriteTextFile(script, R"PY(import base64, http.server, json, os, sys, time, urllib.parse
root = sys.argv[1]
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        authorized = self.headers.get("Authorization", "") == "Basic " + base64.b64encode(b"opencode:private-delayed-http-token").decode()
        with open(os.path.join(root, "received.tmp"), "w", encoding="utf-8") as stream:
            json.dump({"path": parsed.path, "authorized": authorized}, stream)
        os.replace(os.path.join(root, "received.tmp"), os.path.join(root, "received"))
        deadline = time.monotonic() + 2
        while not os.path.isfile(os.path.join(root, "release")) and time.monotonic() < deadline:
            time.sleep(0.002)
        value = {"id": "ses_owned", "directory": urllib.parse.parse_qs(parsed.query).get("directory", [""])[0]}
        body = json.dumps(value).encode()
        self.send_response(200 if authorized and parsed.path == "/session/ses_owned" else 403)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
with open(os.path.join(root, "port.tmp"), "w", encoding="ascii") as stream:
    stream.write(str(server.server_address[1]))
os.replace(os.path.join(root, "port.tmp"), os.path.join(root, "port"))
server.serve_forever()
)PY"));
	auto& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields server;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(server, temp.root, {"/usr/bin/python3", "-I", script.string(), temp.root.string()}, &error));
	struct Guard
	{
		IPlatformProcessService& service;
		uam::platform::StdioProcessPlatformFields& process;
		~Guard() { service.StopStdioProcess(process, true); service.CloseStdioProcessHandles(process); }
	} guard{service, server};
	std::string port;
	const auto startup = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (port.empty() && std::chrono::steady_clock::now() < startup)
	{
		(void)uam::io::TryReadTextFile(temp.root / "port", port);
		if (port.empty()) std::this_thread::yield();
	}
	UAM_ASSERT(!port.empty());
	const uam::opencode_native::Endpoint endpoint{std::stoi(port), "private-delayed-http-token"};
	std::atomic_bool finished = false;
	bool ok = false;
	nlohmann::json result;
	std::jthread query([&](std::stop_token cancel)
	{
		ok = uam::opencode_native::Query(service, endpoint, temp.root.string(),
		    {{"operation", "session"}, {"nativeSessionId", "ses_owned"}}, result, error, cancel);
		finished.store(true, std::memory_order_release);
	});
	const auto requested = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!fs::exists(temp.root / "received") && !finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < requested) std::this_thread::yield();
	const bool received = fs::exists(temp.root / "received");
	const bool waiting = received && !finished.load(std::memory_order_acquire);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release", "release actual HTTP response"));
	query.join();
	if (!ok) throw uam_test::TestFailure("Owned delayed HTTP response was not awaited: " + error);
	UAM_ASSERT(received && waiting);
	const auto request = nlohmann::json::parse(uam_test::ReadFile(temp.root / "received"));
	UAM_ASSERT(request["authorized"].get<bool>() && request["path"] == "/session/ses_owned");
	UAM_ASSERT_EQ(result["id"], nlohmann::json("ses_owned"));
	UAM_ASSERT_EQ(result["directory"], nlohmann::json(temp.root.string()));
}
#endif


#include <cstring>
namespace
{
	// Only the platform read/exit contract is scripted. No OS process, ACP/session, permission or runtime state is created.
	class OpenCodeFinalDrainProcessR80 final : public IPlatformProcessService
	{
	  public:
		struct Read
		{
			int status; // -2 would-block, -1 error, 0 EOF, 1 the supplied real bytes.
			std::string bytes;
		};
		std::vector<Read> reads;
		int exit_code = 0;
		std::stop_source* cancel_on_poll = nullptr;
		mutable std::vector<std::string> calls;
		mutable std::size_t position = 0;
		mutable uam::platform::StdioProcessPlatformFields* owned = nullptr;

		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields& process, const std::filesystem::path& directory,
		    const std::vector<std::string>& argv, std::string_view input, std::string*,
		    const std::vector<std::pair<std::string, std::string>>& environment) const override
		{
			UAM_ASSERT(calls.empty() && owned == nullptr && directory.empty() && environment.empty());
			UAM_ASSERT(!argv.empty() && fs::path(argv.front()).is_absolute());
#if defined(_WIN32)
			UAM_ASSERT_EQ(fs::path(argv.front()).filename().string(), std::string("curl.exe"));
#else
			UAM_ASSERT_EQ(argv.front(), std::string("/usr/bin/curl"));
#endif
			const std::vector<std::string> expected{"-q", "--silent", "--noproxy", "*", "--proto", "=http", "--connect-timeout", "1",
			    "--max-time", "2", "--max-filesize", "262144", "--request", "GET", "--header", "Content-Type: application/json",
			    "--config", "-", "--write-out", "\n%{http_code}", "http://127.0.0.1:49152/session/ses_owned?directory=%2Fworkspace%2Fa%20b%23c"};
			UAM_ASSERT_EQ(std::vector<std::string>(argv.begin() + 1, argv.end()), expected);
			const std::string secret = "private-final-drain-token";
			const std::string encoded = uam::base64::Encode("opencode:" + secret);
			UAM_ASSERT_EQ(std::string(input), "header = \"Authorization: Basic " + encoded + "\"\n");
			for (const auto& argument : argv) UAM_ASSERT(argument.find(secret) == std::string::npos && argument.find(encoded) == std::string::npos);
			owned = &process;
			calls.push_back("start");
			return true;
		}
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t size, std::string*) const override
		{
			UAM_ASSERT(owned == &process && position < reads.size());
			const auto& next = reads[position++];
			if (next.status == 1)
			{
				UAM_ASSERT(!next.bytes.empty() && next.bytes.size() <= size);
				std::memcpy(buffer, next.bytes.data(), next.bytes.size());
				calls.push_back("read:bytes");
				return static_cast<std::ptrdiff_t>(next.bytes.size());
			}
			UAM_ASSERT(next.status == -2 || next.status == -1 || next.status == 0);
			calls.push_back(next.status == -2 ? "read:would-block" : next.status == -1 ? "read:error" : "read:eof");
			return next.status;
		}
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process, int* code) const override
		{
			UAM_ASSERT(owned == &process && position > 0);
			calls.push_back("poll:exited");
			if (code) *code = exit_code;
			if (cancel_on_poll) cancel_on_poll->request_stop();
			return true; // Exit observation does not consume the separately scripted pipe bytes.
		}
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast) const override
		{
			UAM_ASSERT(owned == &process && fast);
			calls.push_back("stop");
		}
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
		{
			UAM_ASSERT(owned == &process && !calls.empty() && calls.back() == "stop");
			calls.push_back("close");
		}
		[[noreturn]] static void Unexpected() { throw uam_test::TestFailure("Unexpected operation in private native-HTTP reader contract"); }
		bool SupportsDetachedProcesses() const override { Unexpected(); }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { Unexpected(); }
		std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path&, const std::string&) const override { Unexpected(); }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { Unexpected(); }
		int NormalizeCapturedCommandExitCode(int) const override { Unexpected(); }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { Unexpected(); }
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields&, const std::filesystem::path&, const std::vector<std::string>&, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { Unexpected(); }
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields&, const char*, std::size_t, std::string*) const override { Unexpected(); }
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields&) const override { Unexpected(); }
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields&, bool) const override { Unexpected(); }
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields&, char*, std::size_t, std::string*) const override { Unexpected(); }
		std::filesystem::path ResolveCurrentExecutablePath() const override { Unexpected(); }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path&, std::string*) const override { Unexpected(); }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { Unexpected(); }
		std::size_t NativeGeminiSessionMaxMessages() const override { Unexpected(); }
		std::string GenerateUuid() const override { Unexpected(); }
		bool LaunchShellAt(const std::filesystem::path&, std::string*) const override { Unexpected(); }
		void SetKeepSystemAwake(bool) const override { Unexpected(); }
		bool EmbeddedBrowserUsesMockKeychain() const override { Unexpected(); }
	};
}

UAM_TEST(OpenCodeNativeHttpDrainsFinalBytesAfterObservedProcessExit)
{
	for (const bool split : {false, true})
	{
		OpenCodeFinalDrainProcessR80 service;
		if (split)
			service.reads = {{-2, {}}, {1, "{\"id\":"}, {1, "\"ses_owned\",\"directory\":\"/workspace/a b#c\"}"}, {1, "\n2"}, {1, "00"}, {0, {}}};
		else
			service.reads = {{-2, {}}, {1, "{\"id\":\"ses_owned\",\"directory\":\"/workspace/a b#c\"}\n200"}, {0, {}}};
		nlohmann::json result = "must be replaced";
		std::string error;
		const bool ok = uam::opencode_native::Query(service, {49152, "private-final-drain-token"}, "/workspace/a b#c",
		    {{"operation", "session"}, {"nativeSessionId", "ses_owned"}}, result, error);
		UAM_ASSERT(ok && error.empty()); // Intended RED after only the sentinel fix: exit must not discard the unread final response.
		UAM_ASSERT_EQ(result, (nlohmann::json{{"id", "ses_owned"}, {"directory", "/workspace/a b#c"}}));
		const std::vector<std::string> expected = split
		    ? std::vector<std::string>{"start", "read:would-block", "poll:exited", "read:bytes", "read:bytes", "read:bytes", "read:bytes", "read:eof", "poll:exited", "stop", "close"}
		    : std::vector<std::string>{"start", "read:would-block", "poll:exited", "read:bytes", "read:eof", "poll:exited", "stop", "close"};
		UAM_ASSERT_EQ(service.calls, expected);
		UAM_ASSERT_EQ(service.position, service.reads.size());
	}
}

UAM_TEST(OpenCodeNativeHttpFinalDrainRetainsStrictFailuresAndCancellation)
{
	for (const std::string variant : {"non200", "malformed", "nonzero-exit", "read-error", "cancel-after-exit", "cancel-before-start", "invalid-endpoint"})
	{
		OpenCodeFinalDrainProcessR80 service;
		service.reads = {{1, "{\"id\":\"ses_owned\"}\n200"}, {0, {}}};
		std::stop_source cancel;
		if (variant == "non200") service.reads[0].bytes = "{\"id\":\"ses_owned\"}\n403";
		else if (variant == "malformed") service.reads[0].bytes = "not-json\n200";
		else if (variant == "nonzero-exit") service.exit_code = 7;
		else if (variant == "read-error") service.reads = {{-1, {}}};
		else if (variant == "cancel-after-exit") { service.reads = {{-2, {}}, {1, "{\"id\":\"must-not-read\"}\n200"}, {0, {}}}; service.cancel_on_poll = &cancel; }
		else if (variant == "cancel-before-start") cancel.request_stop();
		nlohmann::json result = "must be cleared";
		std::string error;
		const uam::opencode_native::Endpoint endpoint{49152, variant == "invalid-endpoint" ? "" : "private-final-drain-token"};
		UAM_ASSERT(!uam::opencode_native::Query(service, endpoint, "/workspace/a b#c",
		    {{"operation", "session"}, {"nativeSessionId", "ses_owned"}}, result, error, cancel.get_token()));
		UAM_ASSERT(result.is_null() && !error.empty());
		if (variant == "cancel-before-start" || variant == "invalid-endpoint") UAM_ASSERT(service.calls.empty());
		else if (variant == "read-error") UAM_ASSERT_EQ(service.calls, (std::vector<std::string>{"start", "read:error", "stop", "close"}));
		else if (variant == "cancel-after-exit")
		{
			UAM_ASSERT_EQ(service.calls, (std::vector<std::string>{"start", "read:would-block", "poll:exited", "stop", "close"}));
			UAM_ASSERT_EQ(service.position, std::size_t{1}); // Cancellation forbids consuming the still-readable positive response.
		}
		else UAM_ASSERT_EQ(service.calls, (std::vector<std::string>{"start", "read:bytes", "read:eof", "poll:exited", "stop", "close"}));
	}
}
