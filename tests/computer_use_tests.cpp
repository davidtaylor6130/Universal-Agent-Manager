#include "test_harness.h"

using namespace uam_test;

UAM_TEST(ComputerUseBackendPreferenceAndEffectiveRoutingAreConservative)
{
	UAM_ASSERT_EQ(uam::computer_use::BackendPreference("auto"), std::string("auto"));
	UAM_ASSERT_EQ(uam::computer_use::BackendPreference("provider"), std::string("provider"));
	UAM_ASSERT_EQ(uam::computer_use::BackendPreference("uam"), std::string("uam"));
	UAM_ASSERT_EQ(uam::computer_use::BackendPreference("native"), std::string("auto"));
	UAM_ASSERT_EQ(uam::computer_use::BackendPreference(" provider "), std::string("auto"));
	UAM_ASSERT(uam::computer_use::ProviderBackendAvailable("codex-cli"));
	UAM_ASSERT(!uam::computer_use::ProviderBackendAvailable("claude-cli"));

	const auto effective_backend = [](std::string provider_id, std::string preference)
	{
		ChatSession chat;
		chat.provider_id = std::move(provider_id);
		chat.computer_use_backend = std::move(preference);
		return uam::computer_use::EffectiveBackend(chat);
	};
	for (const std::string provider : {
	         "codex-cli", "claude-cli", "gemini-cli", "opencode-cli", "copilot-cli"})
		UAM_ASSERT_EQ(effective_backend(provider, "auto"), std::string("uam"));
	UAM_ASSERT_EQ(effective_backend("claude-cli", "provider"), std::string("uam"));
	UAM_ASSERT_EQ(effective_backend("codex-cli", "provider"), std::string("provider"));
}

UAM_TEST(RemoteComputerUseFailsClosedBeforeProviderLaunchOrInput)
{
	ChatSession chat;
	chat.id = "remote-chat";
	chat.execution_host_id = "lab";
	chat.provider_id = "codex-cli";
	chat.computer_use_backend = "provider";
	chat.computer_use_enabled = true;
	UAM_ASSERT(!uam::computer_use::AvailableForChat(chat));

	const auto arguments = uam::BuildAcpLaunchArgvForTests(chat);
	const auto has_pair = [&arguments](std::string_view first, std::string_view second)
	{
		for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
			if (arguments[index] == first && arguments[index + 1] == second) return true;
		return false;
	};
	UAM_ASSERT(has_pair("--disable", "computer_use"));
	UAM_ASSERT(!std::ranges::any_of(arguments, [](const std::string& argument)
	    { return argument.find("mcp_servers.uam-computer.") != std::string::npos; }));
	UAM_ASSERT(uam::computer_use::AcpMcpServers(chat).empty());

	TempDir temp("uam-remote-computer-use-disabled");
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::ComputerUseService::SetControlState(app, chat.id, "running", &error));
	UAM_ASSERT(error.find("remote") != std::string::npos);
	UAM_ASSERT(!fs::exists(temp.root / "computer-use" / chat.id));
}

UAM_TEST(ExecutionHostsNormalizeAndPersistWithoutCredentials)
{
	UAM_ASSERT_EQ(uam::execution_hosts::Find({}, "local")->id, std::string("local"));
	UAM_ASSERT_EQ(uam::execution_hosts::Find({}, "")->id, std::string("local"));
	UAM_ASSERT(uam::execution_hosts::Find({}, "missing-remote") == nullptr);
	std::vector<ExecutionHost> hosts = {
	    {.id = "local", .label = "spoof", .transport = "ssh", .ssh_alias = "bad"},
	    {.id = "lab", .label = "Home lab", .transport = "wrong", .ssh_alias = "uam-lab"},
	    {.id = "bad id", .label = "Dropped", .transport = "ssh", .ssh_alias = "bad"},
	    {.id = "lab", .label = "Duplicate", .transport = "ssh", .ssh_alias = "other"},
	};
	uam::execution_hosts::Normalize(hosts);
	UAM_ASSERT_EQ(hosts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(hosts[0].id, std::string("local"));
	UAM_ASSERT_EQ(hosts[0].transport, std::string("local"));
	UAM_ASSERT_EQ(hosts[1].id, std::string("lab"));
	UAM_ASSERT_EQ(hosts[1].transport, std::string("ssh"));

	TempDir temp("uam-execution-host-settings");
	AppSettings settings;
	settings.execution_hosts = hosts;
	const fs::path path = temp.root / "settings.txt";
	UAM_ASSERT(SettingsStore::Save(path, settings));
	const std::string persisted = ReadFile(path);
	UAM_ASSERT(persisted.find("uam-lab") != std::string::npos);
	UAM_ASSERT(persisted.find("private") == std::string::npos);

	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(path, loaded).loaded);
	UAM_ASSERT_EQ(loaded.execution_hosts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded.execution_hosts[1].ssh_alias, std::string("uam-lab"));
}

UAM_TEST(ComputerUseCodexLaunchSelectsExactlyOneController)
{
	const auto has_pair = [](const std::vector<std::string>& arguments,
	                          std::string_view first, std::string_view second)
	{
		for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
			if (arguments[index] == first && arguments[index + 1] == second) return true;
		return false;
	};
	const auto injects_uam = [](const std::vector<std::string>& arguments)
	{
		return std::ranges::any_of(arguments, [](const std::string& argument)
		    { return argument.find("mcp_servers.uam-computer.") != std::string::npos; });
	};
	const auto auto_approves_uam = [](const std::vector<std::string>& arguments)
	{
		return std::ranges::find(arguments,
		    "mcp_servers.uam-computer.default_tools_approval_mode=\"approve\"") !=
		    arguments.end();
	};
	const auto waits_for_uam_approval = [](const std::vector<std::string>& arguments)
	{
		return std::ranges::find(arguments,
		    "mcp_servers.uam-computer.tool_timeout_sec=300") != arguments.end();
	};

	ChatSession chat;
	chat.id = "computer-chat";
	chat.provider_id = "codex-cli";
	chat.computer_use_enabled = true;
	chat.computer_use_target_id = "42";
	chat.computer_use_target_process_id = "7";

	chat.computer_use_backend = "auto";
	const auto automatic = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT(has_pair(automatic, "--disable", "computer_use"));
	UAM_ASSERT(!has_pair(automatic, "--enable", "computer_use"));
	UAM_ASSERT(injects_uam(automatic));
	UAM_ASSERT(auto_approves_uam(automatic));
	UAM_ASSERT(waits_for_uam_approval(automatic));

	chat.computer_use_backend = "provider";
	const auto provider = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT(has_pair(provider, "--enable", "computer_use"));
	UAM_ASSERT(!has_pair(provider, "--disable", "computer_use"));
	UAM_ASSERT(!injects_uam(provider));

	chat.computer_use_backend = "uam";
	const auto uam = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT(has_pair(uam, "--disable", "computer_use"));
	UAM_ASSERT(injects_uam(uam));
	UAM_ASSERT(auto_approves_uam(uam));
	UAM_ASSERT(waits_for_uam_approval(uam));

	chat.computer_use_enabled = false;
	const auto disabled = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT(has_pair(disabled, "--disable", "computer_use"));
	UAM_ASSERT(injects_uam(disabled));

	chat.computer_use_backend = "provider";
	const auto provider_disabled = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT(has_pair(provider_disabled, "--disable", "computer_use"));
	UAM_ASSERT(!injects_uam(provider_disabled));
}

UAM_TEST(ComputerUseStructuredFallbackProvidersInjectUam)
{
	ChatSession chat;
	chat.id = "computer-chat";
	chat.computer_use_enabled = true;
	chat.computer_use_target_id = "42";
	chat.computer_use_target_process_id = "7";

	chat.provider_id = "claude-cli";
	for (const std::string backend : {"auto", "uam"})
	{
		chat.computer_use_backend = backend;
		const auto arguments = uam::BuildAcpLaunchArgvForTests(chat);
		const auto mcp_flag = std::ranges::find(arguments, std::string("--mcp-config"));
		UAM_ASSERT(mcp_flag != arguments.end() && mcp_flag + 1 != arguments.end());
		const nlohmann::json config = nlohmann::json::parse(*(mcp_flag + 1));
		UAM_ASSERT(config["mcpServers"].contains(uam::computer_use::kMcpServerName));
		const auto allowed_tools = std::ranges::find(arguments, std::string("--allowedTools"));
		UAM_ASSERT(allowed_tools != arguments.end() && allowed_tools + 1 != arguments.end());
		UAM_ASSERT_EQ(*(allowed_tools + 1), std::string("mcp__uam-computer__computer_observe,mcp__uam-computer__computer_action"));
	}

	chat.computer_use_backend = "auto";
	for (const std::string provider_id : {"gemini-cli", "opencode-cli", "copilot-cli"})
	{
		chat.provider_id = provider_id;
		std::string method;
		const nlohmann::json setup = ProviderRuntimeRegistry::ResolveById(provider_id)
		    .OnAcpBuildSetupRequest(41, chat, "/tmp/project", true, method);
		const nlohmann::json& servers = setup["params"]["mcpServers"];
		UAM_ASSERT_EQ(servers.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(servers[0].value("name", ""),
		    std::string(uam::computer_use::kMcpServerName));
		if (provider_id == "copilot-cli")
		{
			const auto arguments = uam::BuildAcpLaunchArgvForTests(chat);
			UAM_ASSERT(uam::ranges::Contains(arguments,
			    "--allow-tool=uam-computer(computer_observe),uam-computer(computer_action)"));
		}
		if (provider_id == "gemini-cli")
		{
			const auto arguments = uam::BuildAcpLaunchArgvForTests(chat);
			const auto policy = std::ranges::find(arguments, std::string("--policy"));
			UAM_ASSERT(policy != arguments.end() && policy + 1 != arguments.end());
			UAM_ASSERT_EQ(std::filesystem::path(*(policy + 1)).filename().string(),
			    std::string("gemini-policy.toml"));
		}
	}
}

UAM_TEST(ComputerUseToolSurfaceIsSmallBoundedAndAnnotated)
{
	const nlohmann::json tools = uam::computer_use::ToolDefinitionsForTests();
	UAM_ASSERT_EQ(tools.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(tools[0].value("name", ""), std::string("computer_observe"));
	UAM_ASSERT_EQ(tools[1].value("name", ""), std::string("computer_action"));
	UAM_ASSERT(tools[0]["annotations"].value("readOnlyHint", false));
	UAM_ASSERT(!tools[1]["annotations"].value("readOnlyHint", true));
	UAM_ASSERT(!tools[0]["annotations"].value("idempotentHint", true));
	UAM_ASSERT(!tools[1]["annotations"].value("idempotentHint", true));
	UAM_ASSERT(tools[1]["annotations"].value("destructiveHint", false));
	UAM_ASSERT(tools[0]["inputSchema"]["additionalProperties"] == false);
	UAM_ASSERT(tools[0]["inputSchema"]["properties"].contains("target"));
	UAM_ASSERT_EQ(tools[0]["inputSchema"]["properties"]["target"].value("maxLength", 0), 160);
	UAM_ASSERT_EQ(tools[1]["inputSchema"]["properties"]["durationMs"].value("maximum", 0), 2000);
	UAM_ASSERT(tools[1]["inputSchema"]["properties"].contains("elementId"));
	UAM_ASSERT(tools[1]["inputSchema"]["properties"].contains("clickCount"));
	UAM_ASSERT(tools[1]["inputSchema"]["properties"]["action"]["enum"] ==
	    nlohmann::json::array({"move", "click", "drag", "scroll", "type", "hotkey", "wait"}));
}

UAM_TEST(ComputerUseMcpRejectsStorageUnsafeChatIds)
{
	TempDir temp("uam-computer-use-unsafe-id");
	ScopedEnvVar data_root("UAM_DATA_DIR", temp.root.string());

	UAM_ASSERT(uam::computer_use::IsPortableMcpChatId("chat_123-ABC"));
	UAM_ASSERT(!uam::computer_use::IsPortableMcpChatId(std::string(129, 'a')));
	for (const std::string chat_id : {"..", "a..b", "D:payload", "a?b", "CON", "com1", "LPT9"})
	{
		UAM_ASSERT(!uam::computer_use::IsPortableMcpChatId(chat_id));
		std::ostringstream error_output;
		std::streambuf* previous_error = std::cerr.rdbuf(error_output.rdbuf());
		const int exit_code = uam::computer_use::RunMcpServer({
		    uam::computer_use::kMcpServerFlag, "--chat-id", chat_id,
		    "--target-id", "1", "--target-pid", "1"});
		std::cerr.rdbuf(previous_error);
		UAM_ASSERT_EQ(exit_code, 2);
	}
	UAM_ASSERT(!fs::exists(temp.root / "computer-use"));
}

UAM_TEST(ComputerUseMcpNegotiatesAndListsOnlyItsSmallToolSurface)
{
	TempDir temp("uam-computer-use-mcp");
	ScopedEnvVar data_root("UAM_DATA_DIR", temp.root.string());
	std::string requests((1024 * 1024) + 1, 'x');
	requests += "\n"
	            R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{}})"
	            "\n"
	            R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})"
	            "\n"
	            R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2099-01-01"}})"
	            "\n";
	std::istringstream input(std::move(requests));
	std::ostringstream output;
	std::streambuf* previous_input = std::cin.rdbuf(input.rdbuf());
	std::streambuf* previous_output = std::cout.rdbuf(output.rdbuf());
	const int exit_code = uam::computer_use::RunMcpServer({
	    uam::computer_use::kMcpServerFlag, "--chat-id", "test-chat"});
	std::cin.rdbuf(previous_input);
	std::cin.clear();
	std::cout.rdbuf(previous_output);

	UAM_ASSERT_EQ(exit_code, 0);
	std::vector<nlohmann::json> responses;
	std::istringstream response_stream(output.str());
	std::string line;
	while (std::getline(response_stream, line)) responses.push_back(nlohmann::json::parse(line));
	UAM_ASSERT_EQ(responses.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(responses[0]["error"].value("code", 0), -32700);
	UAM_ASSERT(responses[1]["result"].is_object());
	UAM_ASSERT_EQ(responses[2]["result"]["tools"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(responses[3]["result"].value("protocolVersion", ""),
	    std::string("2025-06-18"));
}

UAM_TEST(ComputerUseSecondChatServerCanRunWhileControllerLockIsHeld)
{
	TempDir temp("uam-computer-use-multiple-chats");
	ScopedEnvVar data_root("UAM_DATA_DIR", temp.root.string());
	std::string lock_error;
	UAM_ASSERT(uam::computer_use::AcquireControllerLock(&lock_error));

	std::istringstream input;
	std::streambuf* previous_input = std::cin.rdbuf(input.rdbuf());
	const int exit_code = uam::computer_use::RunMcpServer({
	    uam::computer_use::kMcpServerFlag, "--chat-id", "second-chat",
	    "--target-id", "42", "--target-pid", "7"});
	std::cin.rdbuf(previous_input);
	std::cin.clear();
	uam::computer_use::ReleaseControllerLock();

	UAM_ASSERT_EQ(exit_code, 0);
}

UAM_TEST(ComputerUseControlStateIsIsolatedPerChat)
{
	TempDir temp("uam-computer-use-chat-isolation");
	uam::AppState app;
	app.data_root = temp.root;
	for (const std::string id : {"first-chat", "second-chat"})
	{
		ChatSession chat;
		chat.id = id;
		app.chats.push_back(chat);
		fs::create_directories(temp.root / "computer-use" / id);
	}
	UAM_ASSERT(uam::io::WriteTextFile(
	    temp.root / "computer-use" / "first-chat" / "control.json",
	    R"({"state":"running","targetKind":"window","targetId":"11","targetProcessId":"101","targetTitle":"First target","targetInputMode":"foreground"})" "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    temp.root / "computer-use" / "second-chat" / "control.json",
	    R"({"state":"running","targetKind":"window","targetId":"22","targetProcessId":"202","targetTitle":"Second target","targetInputMode":"foreground"})" "\n"));

	UAM_ASSERT(uam::ComputerUseService::Poll(app));
	UAM_ASSERT(app.chats[0].computer_use_enabled);
	UAM_ASSERT(app.chats[1].computer_use_enabled);
	UAM_ASSERT_EQ(app.chats[0].computer_use_target_id, std::string("11"));
	UAM_ASSERT_EQ(app.chats[1].computer_use_target_id, std::string("22"));

	std::string error;
	UAM_ASSERT(uam::ComputerUseService::SetControlState(app, "first-chat", "paused", &error));
	UAM_ASSERT_EQ(app.computer_use_by_chat_id.at("first-chat").state, std::string("paused"));
	UAM_ASSERT_EQ(app.computer_use_by_chat_id.at("second-chat").state, std::string("running"));
	UAM_ASSERT(app.chats[1].computer_use_enabled);

	UAM_ASSERT(uam::ComputerUseService::SetControlState(app, "first-chat", "stopped", &error));
	UAM_ASSERT(!app.chats[0].computer_use_enabled);
	UAM_ASSERT(app.chats[1].computer_use_enabled);
	UAM_ASSERT_EQ(app.chats[1].computer_use_target_id, std::string("22"));
}

UAM_TEST(ComputerUseActionResultsPreventDuplicateInput)
{
	const nlohmann::json failure = uam::computer_use::ActionAppliedFailureForTests(
	    "Action completed, but capture failed.", "8");
	UAM_ASSERT(failure.value("isError", false));
	UAM_ASSERT(failure["structuredContent"].value("actionApplied", false));
	UAM_ASSERT_EQ(failure["structuredContent"].value("frameId", ""), std::string("8"));
	UAM_ASSERT(failure["content"][0].value("text", "").find("frameId: 8") != std::string::npos);
	UAM_ASSERT(failure["content"][0].value("text", "").find("actionApplied: true") != std::string::npos);

	const nlohmann::json observation =
	    uam::computer_use::ObservationSuccessForTests("12");
	UAM_ASSERT_EQ(observation["structuredContent"].value("frameId", ""),
	    std::string("12"));
	UAM_ASSERT(observation["content"][0].value("text", "").find("frameId: 12") !=
	    std::string::npos);
	UAM_ASSERT(observation["content"][0].value("text", "").find("actionApplied: false") !=
	    std::string::npos);

	const nlohmann::json wait = uam::computer_use::WaitSuccessForTests("9");
	UAM_ASSERT(!wait.value("isError", true));
	UAM_ASSERT(!wait["structuredContent"].value("actionApplied", true));
	UAM_ASSERT(wait["content"][0].value("text", "").find("computer_observe") != std::string::npos);
}

UAM_TEST(ComputerUseModelMustNameItsTargetBeforeUserApproval)
{
	TempDir temp("uam-computer-use-request");
	ScopedEnvVar data_root("UAM_DATA_DIR", temp.root.string());
	std::istringstream input(
	    R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"computer_observe","arguments":{}}})"
	    "\n");
	std::ostringstream output;
	std::streambuf* previous_input = std::cin.rdbuf(input.rdbuf());
	std::streambuf* previous_output = std::cout.rdbuf(output.rdbuf());
	const int exit_code = uam::computer_use::RunMcpServer({
	    uam::computer_use::kMcpServerFlag, "--chat-id", "request-chat"});
	std::cin.rdbuf(previous_input);
	std::cin.clear();
	std::cout.rdbuf(previous_output);

	UAM_ASSERT_EQ(exit_code, 0);
	const nlohmann::json response = nlohmann::json::parse(output.str());
	UAM_ASSERT(response["result"].value("isError", false));
	UAM_ASSERT(response["result"]["content"][0].value("text", "").find(
	               "Retry computer_observe with target") != std::string::npos);
	const fs::path request_path =
	    temp.root / "computer-use" / "request-chat" / "request.json";
	UAM_ASSERT(!fs::exists(request_path));
}

UAM_TEST(ComputerUseControlStateAndRedactedHistoryAreBounded)
{
	TempDir temp("uam-computer-use");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-computer";
	app.chats.push_back(chat);

	std::string error;
	fs::create_directories(temp.root / "computer-use" / chat.id);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "computer-use" / chat.id / "control.json",
	    R"({"state":"running","targetKind":"window","targetId":"42","targetProcessId":"7","targetTitle":"TextEdit — Fixture","targetInputMode":"foreground"})" "\n"));
	UAM_ASSERT(uam::ComputerUseService::Poll(app));
	UAM_ASSERT(app.chats[0].computer_use_enabled);
	UAM_ASSERT_EQ(app.chats[0].computer_use_target_id, std::string("42"));
	UAM_ASSERT_EQ(app.chats[0].computer_use_target_process_id, std::string("7"));
	UAM_ASSERT_EQ(app.chats[0].computer_use_target_title, std::string("TextEdit — Fixture"));
	UAM_ASSERT(uam::ComputerUseService::SetControlState(app, chat.id, "paused", &error));
	UAM_ASSERT_EQ(app.computer_use_by_chat_id.at(chat.id).state, std::string("paused"));
	UAM_ASSERT(app.chats[0].computer_use_enabled);
	UAM_ASSERT(!uam::ComputerUseService::SetControlState(app, chat.id, "invalid", &error));
	UAM_ASSERT(uam::ComputerUseService::SetControlState(app, chat.id, "stopped", &error));
	UAM_ASSERT(!app.chats[0].computer_use_enabled);
	const nlohmann::json stopped_control = nlohmann::json::parse(
	    ReadFile(temp.root / "computer-use" / chat.id / "control.json"));
	UAM_ASSERT_EQ(stopped_control.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(stopped_control.value("state", ""), std::string("stopped"));

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "computer-use" / chat.id / "history.jsonl",
	    R"({"time":"now","action":"type","status":"completed","detail":"Typed text (content redacted)."})" "\n"));
	UAM_ASSERT(uam::ComputerUseService::Poll(app));
	UAM_ASSERT_EQ(app.computer_use_by_chat_id.at(chat.id).history[0].detail,
	    std::string("Typed text (content redacted)."));

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "computer-use" / chat.id / "control.json",
	    std::string(4097, 'x')));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "computer-use" / chat.id / "history.jsonl",
	    std::string((512 * 1024) + 1, 'x')));
	UAM_ASSERT(uam::ComputerUseService::Poll(app));
	UAM_ASSERT_EQ(app.computer_use_by_chat_id.at(chat.id).state, std::string("stopped"));
	UAM_ASSERT(app.computer_use_by_chat_id.at(chat.id).history.empty());
	UAM_ASSERT(!app.chats[0].computer_use_enabled);

	UAM_ASSERT(uam::ComputerUseService::ResetControlsForStartup(app));
	UAM_ASSERT(!app.chats[0].computer_use_enabled);
	UAM_ASSERT(app.computer_use_by_chat_id.empty());
}

UAM_TEST(ChatRepositoryPersistsOnlyComputerUseBackendAndDropsRuntimeGrant)
{
	TempDir temp("uam-computer-use-persistence");
	ChatSession chat;
	chat.id = "chat-computer";
	chat.provider_id = "codex-cli";
	chat.computer_use_backend = "uam";
	chat.computer_use_enabled = true;
	chat.computer_use_target_kind = "screen";
	chat.computer_use_target_id = "123";
	chat.computer_use_target_process_id = "456";
	chat.computer_use_target_title = "Full display 1";
	chat.computer_use_target_input_mode = "foreground";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const nlohmann::json persisted = nlohmann::json::parse(
	    ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT_EQ(persisted.value("computer_use_backend", ""), std::string("uam"));
	for (const std::string field : {
	         "computer_use_enabled", "computer_use_target_kind", "computer_use_target_id",
	         "computer_use_target_process_id", "computer_use_target_title",
	         "computer_use_target_input_mode"})
		UAM_ASSERT(!persisted.contains(field));

	const auto loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded[0].computer_use_backend, std::string("uam"));
	UAM_ASSERT(!loaded[0].computer_use_enabled);
	UAM_ASSERT(loaded[0].computer_use_target_id.empty());
}
