#include "test_harness.h"
#include "app/uam_control_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

UAM_TEST(ComputerUseGlobalPolicyDefaultsAndPersistsWithoutChatConsent)
{
	TempDir temp("uam-cu-global-settings");
	AppSettings settings;
	UAM_ASSERT(settings.computer_use_globally_enabled);
	settings.computer_use_globally_enabled = false;
	settings.computer_use_allowlist_enabled = true;
	settings.computer_use_allowed_applications.push_back({"bundleid", "com.example.app"});
	const fs::path path = AppPaths::SettingsFilePath(temp.root);
	UAM_ASSERT(SettingsStore::Save(path, settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(path, loaded).loaded);
	UAM_ASSERT(!loaded.computer_use_globally_enabled);
	UAM_ASSERT(loaded.computer_use_allowlist_enabled);
	UAM_ASSERT_EQ(loaded.computer_use_allowed_applications, settings.computer_use_allowed_applications);
	UAM_ASSERT(!uam::settings_frontend_json::SerializeCommonSettingsFields(loaded)["computerUseGloballyEnabled"].get<bool>());

	UAM_ASSERT(uam::io::WriteTextFile(path, "computer_use_allowlist_enabled=true\n"));
	AppSettings legacy;
	UAM_ASSERT(SettingsStore::Load(path, legacy).loaded);
	UAM_ASSERT(legacy.computer_use_globally_enabled);

	ChatSession chat;
	chat.id = "global-policy-snapshot";
	chat.provider_id = "codex-cli";
	chat.computer_use_globally_enabled = false;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const nlohmann::json stored = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT(!stored.contains("computer_use_globally_enabled"));
	UAM_ASSERT(!stored.contains("computerUseGloballyEnabled"));
}

UAM_TEST(ComputerUseGlobalOffSuppressesEveryMcpInjection)
{
	ChatSession chat;
	chat.id = "global-off-injection";
	chat.provider_id = "codex-cli";
	chat.computer_use_enabled = true;
	chat.computer_use_globally_enabled = false;
	UAM_ASSERT(!uam::computer_use::AvailableForChat(chat));
	UAM_ASSERT(uam::computer_use::AcpMcpServers(chat).empty());
	UAM_ASSERT(nlohmann::json::parse(uam::computer_use::ClaudeMcpConfig(chat))["mcpServers"].empty());
	UAM_ASSERT(uam::acp_detail::BuildNewSessionRequest(1, ".", &chat)["params"]["mcpServers"].empty());
	UAM_ASSERT(uam::acp_detail::BuildLoadSessionRequest(2, "session", ".", &chat)["params"]["mcpServers"].empty());
	UAM_ASSERT(uam::acp_detail::BuildResumeSessionRequest(3, "session", ".", &chat)["params"]["mcpServers"].empty());
	std::vector<std::string> argv;
	uam::computer_use::AppendCodexMcpLaunchArguments(argv, chat);
	UAM_ASSERT_EQ(argv, (std::vector<std::string>{"--disable", "computer_use"}));
	argv.clear();
	uam::computer_use::AppendClaudeMcpLaunchArguments(argv, chat);
	uam::computer_use::AppendGeminiMcpLaunchArguments(argv, chat);
	UAM_ASSERT(argv.empty());
	chat.computer_use_backend = "provider";
	uam::computer_use::AppendCodexMcpLaunchArguments(argv, chat);
	UAM_ASSERT_EQ(argv, (std::vector<std::string>{"--disable", "computer_use"}));
	ProviderProfile profile;
	const auto copilot = ProviderRuntimeRegistry::ResolveById("copilot-cli").BuildStructuredLaunchArgv(profile, chat);
	UAM_ASSERT(std::ranges::none_of(copilot, [](const std::string& argument) { return argument.find("uam-computer") != std::string::npos; }));
	const auto opencode = ProviderRuntimeRegistry::ResolveById("opencode-cli").BuildStructuredLaunchEnvironment(profile, chat);
	UAM_ASSERT_EQ(opencode.size(), static_cast<std::size_t>(1));
	const nlohmann::json permissions = nlohmann::json::parse(opencode.front().second);
	UAM_ASSERT_EQ(permissions["uam-computer_computer_action"].get<std::string>(), std::string("deny"));
}

UAM_TEST(ComputerUseGlobalOffRejectsTasksControlsAndStalePollGrants)
{
	TempDir temp("uam-cu-global-control");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.computer_use_globally_enabled = false;
	ChatSession chat;
	chat.id = "global-control";
	chat.execution_host_id = "local";
	chat.computer_use_enabled = true;
	app.chats.push_back(chat);
	const fs::path directory = temp.root / "computer-use" / chat.id;
	fs::create_directories(directory);
	UAM_ASSERT(uam::io::WriteTextFile(directory / "control.json", "{\"state\":\"armed\"}\n"));
	std::string error;
	UAM_ASSERT(!uam::ComputerUseService::SetControlState(app, chat.id, "armed", &error));
	UAM_ASSERT(!uam::ComputerUseService::SetControlState(app, chat.id, "running", &error));
	UAM_ASSERT(!uam::ComputerUseService::PersistTrustedTask(app, chat.id, "Open Example", &error));
	UAM_ASSERT(!fs::exists(directory / "task.json"));
	UAM_ASSERT(uam::ComputerUseService::Poll(app));
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	UAM_ASSERT(uam::ComputerUseService::ResetControlsForStartup(app));
	app.settings.computer_use_globally_enabled = true;
	(void)uam::ComputerUseService::Poll(app);
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(directory / "control.json"))["state"].get<std::string>(), std::string("stopped"));
}

UAM_TEST(ComputerUseDeliveryRechecksPolicyWithoutBlockingOrdinaryInputs)
{
	uam::AppState app;
	app.settings.computer_use_globally_enabled = false;
	ChatSession chat;
	chat.id = "global-delivery";
	chat.execution_host_id = "local";
	chat.computer_use_enabled = true;
	uam::AcpSessionState session;
	session.turn_computer_use_mode = true;
	session.queued_prompt = "prepared computer use task";
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT_EQ(session.last_error, std::string("Computer Use was disabled before prompt delivery."));
	UAM_ASSERT(session.queue_held);

	uam::AcpSessionState ordinary;
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, ordinary, chat));
	UAM_ASSERT(ordinary.last_error.empty());
	UAM_ASSERT(!ordinary.queue_held);
}

UAM_TEST(ComputerUseGlobalOffRejectsAdmissionBeforeProviderStart)
{
	TempDir temp("uam-cu-global-admission");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.computer_use_globally_enabled = false;
	ChatSession chat;
	chat.id = "global-admission";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.computer_use_enabled = true;
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(app, chat.id, "Open Example", {}, {}, false, &error, {}, true));
	UAM_ASSERT(error.find("Computer Use") != std::string::npos);
	UAM_ASSERT(!uam::SteerAcpPrompt(app, chat.id, "Open Example", {}, {}, false, &error, {}, true));
	UAM_ASSERT(app.acp_sessions.empty());
}

UAM_TEST(ComputerUseGlobalOffRejectsAgentConsentRequestsAndPendingApproval)
{
	TempDir temp("uam-cu-global-agent-consent");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", "opencode-cli");
	chat.id = "global-agent-consent";
	chat.execution_host_id = "local";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = "opencode-cli";
	session->running = true;
	uam::AcpSessionState* raw = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, *raw, app.chats.front(), "session/new", setup, &error));
	const std::string capability_id = app.uam_control_capabilities.front().id;
	const int64_t now = uam::time::SystemEpochMillisecondsNow();
	const nlohmann::json request{{"requestId", "global-consent"}, {"method", "computer_use_request"}, {"arguments", {{"reason", "Open the requested application."}}}};
	const nlohmann::json pending = uam::UamControlService::HandleRequestForTests(app, capability_id, request, now);
	UAM_ASSERT(pending.value("pendingApproval", false));
	const auto question = uam::UamControlService::PendingApprovalForChat(app, chat.id);
	UAM_ASSERT(question.has_value());
	app.settings.computer_use_globally_enabled = false;
	UAM_ASSERT(!uam::UamControlService::ResolveApproval(app, chat.id, question->request_id_json, {{"computerUse", {"Allow"}}}, &error));
	UAM_ASSERT_EQ(error, std::string("Computer Use is disabled globally."));
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	nlohmann::json blocked_request = request;
	blocked_request["requestId"] = "blocked-global-consent";
	const nlohmann::json blocked = uam::UamControlService::HandleRequestForTests(app, capability_id, blocked_request, now + 1);
	UAM_ASSERT(!blocked.value("ok", false));
	UAM_ASSERT(!blocked.value("pendingApproval", false));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(ComputerUseGlobalOffDisablesNativeCodexInteractiveFreshAndResume)
{
	AppSettings settings;
	settings.computer_use_globally_enabled = false;
	ProviderProfile profile;
	profile.id = "codex-cli";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.interactive_command = "codex --enable computer_use";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.computer_use_enabled = true;
	for (const std::string& native_id : {std::string{}, std::string("12345678-1234-1234-1234-123456789abc")})
	{
		chat.native_session_id = native_id;
		const std::vector<std::string> argv = ProviderRuntimeRegistry::ResolveById("codex-cli").BuildInteractiveArgv(profile, chat, settings);
		UAM_ASSERT(argv.size() >= 2);
		UAM_ASSERT_EQ(argv[argv.size() - 2], std::string("--disable"));
		UAM_ASSERT_EQ(argv.back(), std::string("computer_use"));
	}
}

UAM_TEST(ComputerUseFailedStartupResetBlocksStaleGrantAndDeletedCleanup)
{
	TempDir temp("uam-cu-startup-revoke");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "startup-revoke"; chat.execution_host_id = "local";
	app.chats.push_back(chat);
	const fs::path directory = temp.root / "computer-use" / chat.id;
	fs::create_directories(directory / "control.json");
	UAM_ASSERT(!uam::ComputerUseService::ResetControlsForStartup(app));
	UAM_ASSERT(app.computer_use_revocation_pending_chat_ids.contains(chat.id));
	fs::remove(directory / "control.json");
	UAM_ASSERT(uam::io::WriteTextFile(directory / "control.json", "{\"state\":\"armed\"}\n"));
	(void)uam::ComputerUseService::Poll(app);
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	std::string error;
	UAM_ASSERT(!uam::ComputerUseService::SetControlState(app, chat.id, "armed", &error));
	app.chats.front().computer_use_enabled = true;
	UAM_ASSERT(!uam::ComputerUseService::PersistTrustedTask(app, chat.id, "Open Example", &error));
	app.chats.clear();
	UAM_ASSERT(uam::ComputerUseService::SetControlState(app, chat.id, "stopped", &error));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(directory / "control.json"))["state"].get<std::string>(), std::string("stopped"));
	// Runtime shutdown confirmation owns removal of the pending latch.
	UAM_ASSERT(app.computer_use_revocation_pending_chat_ids.contains(chat.id));
}
