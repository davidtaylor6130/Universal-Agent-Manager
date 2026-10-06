#include "test_harness.h"
#include <barrier>
#include <thread>
#include "app/provider_configuration_service.h"
#include "common/config/central_provider_configuration.h"
#include "common/config/settings_store.h"
#include "remote/runner_state.h"
#include "remote/runner_client.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "common/runtime/terminal/terminal_provider_cli.h"

using namespace uam_test;

UAM_TEST(CentralConfigurationPersistsAndRejectsMalformedPaths)
{
	TempDir temp("uam-central-settings");
	AppSettings settings;
	settings.central_provider_configuration = {true, "Shared instructions", {(temp.root / "AGENTS.md").string()}, {(temp.root / "skills").string()}, "plan", true};
	const fs::path path = temp.root / "settings.txt";
	UAM_ASSERT(SettingsStore::Save(path, settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(path, loaded).loaded);
	UAM_ASSERT_EQ(uam::central_configuration::Serialize(loaded.central_provider_configuration), uam::central_configuration::Serialize(settings.central_provider_configuration));
	std::string error;
	const nlohmann::json malformed = {{"instructionFiles", nlohmann::json::array({"relative.md"})}};
	UAM_ASSERT(!uam::central_configuration::Parse(malformed, loaded.central_provider_configuration, error));
	UAM_ASSERT_EQ(loaded.central_provider_configuration.instructions, std::string("Shared instructions"));
}

UAM_TEST(CentralResourcesPreserveFullSkillAssetsAndDetectTampering)
{
	TempDir temp("uam-central-assets");
	fs::create_directories(temp.root / "skills" / "example" / "references");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skills/example/SKILL.md", "Read references/data.bin"));
	const std::string binary("a\0b", 3);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skills/example/references/data.bin", binary));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.skill_directories = {(temp.root / "skills").string()};
	ChatSession chat;
	chat.id = "chat";
	chat.uam_agent_id = "build";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	if (!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
	UAM_ASSERT_EQ(ReadFile(bundle.local_directory / "skills/example/references/data.bin"), binary);
	std::vector<std::string> missing;
	UAM_ASSERT(uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, error));
	UAM_ASSERT(missing.empty());
	UAM_ASSERT(uam::io::WriteTextFile(bundle.local_directory / "skills/example/SKILL.md", "changed"));
	UAM_ASSERT(!uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, error));
	UAM_ASSERT(error.find("differs") != std::string::npos);
	UAM_ASSERT(!uam::provider_setup::SafeRelativePath("../outside"));
	UAM_ASSERT(!uam::provider_setup::SafeRelativePath("C:\\outside"));
}

UAM_TEST(CentralSkillManifestPathsStayPortableForDirectoriesAndImportedLibraries)
{
	TempDir temp("uam-central-portable-assets");
	const fs::path skill = temp.root / "example";
	const std::string relative_asset = "references/nested/caf\xc3\xa9.bin";
	const fs::path asset = skill / uam::paths::PathFromUtf8(relative_asset);
	fs::create_directories(asset.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(skill / "SKILL.md", "Read the nested reference."));
	const std::string binary("a\0b", 3);
	UAM_ASSERT(uam::io::WriteTextFile(asset, binary));
	for (const bool imported : {false, true})
	{
		AppSettings settings;
		settings.central_provider_configuration.enabled = true;
		std::string skill_name = "example";
		std::string error;
		if (imported)
		{
			const fs::path library = temp.root / "library";
			fs::create_directories(library);
			const std::vector<MarkdownStoreService::ImportResult> results = MarkdownStoreService::ImportEntries(
			    library, {{"claude", skill / "SKILL.md", MarkdownStoreService::ImportConflictAction::Skip}}, &error);
			UAM_ASSERT_EQ(results.size(), std::size_t(1));
			UAM_ASSERT_EQ(results.front().status, std::string("imported"));
			settings.markdown_store_directory = uam::paths::Utf8PathString(library);
			skill_name = results.front().entry.command_name;
			settings.central_provider_configuration.default_skills = {skill_name};
		}
		else settings.central_provider_configuration.skill_directories = {uam::paths::Utf8PathString(skill)};
		ChatSession chat;
		ExecutionHost host;
		uam::ProviderConfigurationBundle bundle;
		if (!uam::PrepareProviderConfiguration(temp.root / (imported ? "imported-data" : "direct-data"), settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
		const std::string key = "skills/" + skill_name + "/" + relative_asset;
		UAM_ASSERT(bundle.files.contains(key));
		UAM_ASSERT_EQ(bundle.files.at(key), binary);
		UAM_ASSERT(bundle.manifest["files"].contains(key));
		UAM_ASSERT_EQ(ReadFile(bundle.local_directory / uam::paths::PathFromUtf8(key)), binary);
		for (const std::pair<const std::string, std::string>& file : bundle.files)
		{
			UAM_ASSERT(file.first.find('\\') == std::string::npos);
			UAM_ASSERT(uam::provider_setup::SafeRelativePath(file.first));
		}
	}
}

UAM_TEST(CentralLaunchContractsCoverAllFiveProvidersAndPreserveOpenCodeConfig)
{
	TempDir temp("uam-central-contracts");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Use shared instructions";
	ChatSession chat;
	chat.id = "chat";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	if (!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
	for (const std::string& provider : {"codex-cli", "claude-cli", "gemini-cli", "opencode-cli", "copilot-cli"})
	{
		std::vector<std::string> argv{provider};
		uam::provider_setup::Environment environment;
		if (provider == "opencode-cli") environment.emplace_back("OPENCODE_CONFIG_CONTENT", R"({"theme":"existing","instructions":["existing.md"],"permission":{"edit":"deny"}})");
		UAM_ASSERT(uam::provider_setup::Apply(provider, bundle.local_directory, temp.root, argv, environment, error));
		if (provider == "codex-cli") UAM_ASSERT(std::ranges::any_of(argv, [](const std::string& arg) { return arg.starts_with("developer_instructions=") && arg.find("Use shared instructions") != std::string::npos && arg.find("__UAM_RESOURCE_ROOT__") == std::string::npos; }));
		if (provider == "claude-cli") UAM_ASSERT(std::ranges::find(argv, "--append-system-prompt-file") != argv.end());
		if (provider == "gemini-cli") UAM_ASSERT(!uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH").empty());
		if (provider == "copilot-cli") UAM_ASSERT(!uam::provider_setup::EnvironmentValue(environment, "COPILOT_CUSTOM_INSTRUCTIONS_DIRS").empty());
		if (provider == "opencode-cli")
		{
			const nlohmann::json configuration = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
			UAM_ASSERT_EQ(configuration["theme"], nlohmann::json("existing"));
			UAM_ASSERT_EQ(configuration["permission"]["edit"], nlohmann::json("deny"));
			UAM_ASSERT_EQ(configuration["instructions"].size(), std::size_t{2});
		}
	}
}

UAM_TEST(CentralSshPreparationIsResumableAndRejectsUnownedDirectories)
{
	TempDir temp("uam-central-ssh");
	uam::remote::RunnerState runner;
	const std::string bytes = "shared";
	nlohmann::json manifest = {{"format", 1}, {"files", {{"AGENTS.md", {{"size", bytes.size()}, {"digest", uam::provider_setup::Digest(bytes)}}}}}};
	const fs::path directory = temp.root / ".UAM/provider-setup" / uam::provider_setup::Digest(manifest.dump());
	const nlohmann::json request = {{"id", "configuration"}, {"type", "configuration.prepare"}, {"directory", directory.string()}, {"manifest", manifest}};
	const nlohmann::json prepared = runner.HandleProcessRequest(request);
	if (!prepared.value("ok", false)) throw std::runtime_error(prepared.dump());
	UAM_ASSERT_EQ(prepared["result"]["missing"].size(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(directory / "AGENTS.md", bytes));
	UAM_ASSERT(runner.HandleProcessRequest(request)["result"]["missing"].empty());
	manifest["files"]["../outside"] = {{"size", std::size_t{0}}, {"digest", uam::provider_setup::Digest("")}};
	UAM_ASSERT(!runner.HandleProcessRequest({{"id", "configuration"}, {"type", "configuration.prepare"}, {"directory", directory.string()}, {"manifest", manifest}}).value("ok", false));
}

UAM_TEST(CentralMcpScopeOverridesGlobalsAndResolvesSecretsOnlyAtLaunch)
{
	TempDir temp("uam-central-mcp-secrets");
	McpServerConfiguration global;
	global.id = "global";
	global.name = "example";
	global.transport = "http";
	global.url = "http://localhost:5000/mcp";
	global.headers = {{"Authorization", "UAM_CENTRAL_TEST_SECRET"}};
	McpServerConfiguration remote = global;
	remote.id = "remote";
	remote.execution_host_id = "ssh-test";
	remote.url = "http://localhost:6000/mcp";
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.mcp_servers = {remote, global};
	ExecutionHost host;
	host.id = "ssh-test";
	host.transport = "ssh";
	ChatSession chat;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	ScopedEnvVar unset_secret("UAM_CENTRAL_TEST_SECRET", "");
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"].size(), std::size_t{1});
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["url"], nlohmann::json(remote.url));
	std::vector<std::string> argv{"codex"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(!uam::provider_setup::Apply("codex-cli", bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(error.find("UAM_CENTRAL_TEST_SECRET") != std::string::npos);
	ScopedEnvVar target_secret("UAM_CENTRAL_TEST_SECRET", "target-only-value");
	argv = {"codex"};
	UAM_ASSERT(uam::provider_setup::Apply("codex-cli", bundle.local_directory, temp.root, argv, environment, error));
	for (const std::string& argument : argv) UAM_ASSERT(argument.find("target-only-value") == std::string::npos);
	UAM_ASSERT(bundle.manifest.dump().find("target-only-value") == std::string::npos);
	UAM_ASSERT(std::ranges::any_of(environment, [](const auto& entry) { return entry.second == "target-only-value"; }));
}

#if !defined(_WIN32)
UAM_TEST(CentralSshClientTransfersRealAssetsAndPreservesHostContext)
{
	TempDir temp("uam-central-real-runner");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Controller instruction";
	ChatSession chat;
	ExecutionHost host;
	host.id = "ssh-test";
	host.transport = "ssh";
	host.instruction_file = (temp.root / "host.md").string();
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "host.md", "Host instruction"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "AGENTS.md", "Workspace instruction"));
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	const fs::path target = temp.root / ".UAM/provider-setup" / bundle.local_directory.filename();
	uam::remote::RunnerClient client(service, {runner.string(), "bridge-direct"});
	if (!client.InstallProviderConfiguration(bundle.local_directory, target, &error)) throw std::runtime_error(error);
	UAM_ASSERT(client.InstallProviderConfiguration(bundle.local_directory, target, &error));
	std::vector<std::string> argv{"claude"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply("claude-cli", target, temp.root, argv, environment, error));
	const std::string installed = ReadFile(uam::paths::PathFromUtf8(argv.back()));
	UAM_ASSERT(installed.find("Controller instruction") != std::string::npos);
	UAM_ASSERT(installed.find("Host instruction") != std::string::npos);
	UAM_ASSERT(installed.find("Workspace instruction") != std::string::npos);
	UAM_ASSERT(installed.find(target.string()) != std::string::npos);
	client.Disconnect();
}

UAM_TEST(CentralResourcesRejectSymlinkedAssetsAndPreserveClaudeHandoff)
{
	TempDir temp("uam-central-symbolic-links");
	fs::create_directories(temp.root / "skill");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/SKILL.md", "Skill"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "private.md", "Outside"));
	fs::create_symlink(temp.root / "private.md", temp.root / "skill/reference.md");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.skill_directories = {(temp.root / "skill").string()};
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle rejected;
	std::string error;
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, rejected, error));
	settings.central_provider_configuration.skill_directories.clear();
	uam::ProviderConfigurationBundle bundle;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	std::vector<std::string> argv{"claude", "--append-system-prompt-file", (temp.root / "private.md").string()};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply("claude-cli", bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT_EQ(std::count(argv.begin(), argv.end(), "--append-system-prompt-file"), 1);
	UAM_ASSERT(ReadFile(uam::paths::PathFromUtf8(argv.back())).find("Outside") != std::string::npos);
}
#endif

UAM_TEST(CentralNativeUamControlAuthorityEndsWithTerminalAndKeepsChatCapability)
{
	TempDir temp("uam-central-native-control");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "native-control";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& structured = *app.acp_sessions.back();
	structured.chat_id = chat.id;
	structured.provider_id = chat.provider_id;
	structured.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, structured, chat, "session/new", setup, &error));
	const std::string structured_id = structured.uam_control_capability_id;
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.back();
	nlohmann::json server;
	UAM_ASSERT(uam::UamControlService::PrepareTerminalMcpServer(app, terminal, chat, server, error));
	terminal.running = true;
	const std::string capability_id = terminal.uam_control_session->uam_control_capability_id;
	const nlohmann::json request = {{"requestId", "native-skill-list"}, {"method", "skill_list"}, {"arguments", nlohmann::json::object()}};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, capability_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", false));
	UAM_ASSERT(capability_id != structured_id);
	structured.chat_id = "other-chat";
	const nlohmann::json question = {{"requestId", "native-question"}, {"method", "user_question"}, {"arguments", {{"question", "Proceed?"}, {"options", {"Yes", "No"}}}}};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, capability_id, question, uam::time::SystemEpochMillisecondsNow()).value("pendingApproval", false));
	const nlohmann::json state = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(state["chats"][0]["acpSession"]["pendingUserInput"]["questions"][0]["question"], nlohmann::json("Proceed?"));
	const auto pending = uam::UamControlService::PendingApprovalForChat(app, chat.id);
	UAM_ASSERT(pending.has_value());
	UAM_ASSERT(uam::UamControlService::ResolveApproval(app, chat.id, pending->request_id_json, {{"userQuestion", {"No"}}}, &error));
	structured.chat_id = chat.id;
	terminal.uam_control_session->active_uam_agent_workspace_access = "read";
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(app, capability_id, {{"requestId", "read-only-goal"}, {"method", "goal_create"}, {"arguments", {{"objective", "Write changes"}, {"idempotencyKey", "read-only"}}}}, uam::time::SystemEpochMillisecondsNow()).value("ok", true));
	terminal.running = false;
	terminal.uam_control_session.reset();
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(app, capability_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", true));
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, structured_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", false));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(CentralSkillToolsReadInstalledAssetsAndKeepCustomAgentScope)
{
	TempDir temp("uam-central-skill-tools");
	fs::create_directories(temp.root / "skill" / "references");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/SKILL.md", "Read references/example.md"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/references/example.md", "Full supporting asset"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.central_provider_configuration.enabled = true;
	app.settings.central_provider_configuration.skill_directories = {(temp.root / "skill").string()};
	ChatSession chat;
	chat.id = "central-skill-tools";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(app.data_root, app.settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.provider_configuration_directory = bundle.local_directory;
	session.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, chat, "session/new", setup, &error));
	const std::string id = session.uam_control_capability_id;
	const std::int64_t now = uam::time::SystemEpochMillisecondsNow();
	const nlohmann::json listing = uam::UamControlService::HandleRequestForTests(app, id, {{"requestId", "list-skills"}, {"method", "skill_list"}, {"arguments", nlohmann::json::object()}}, now);
	UAM_ASSERT(listing.value("ok", false));
	UAM_ASSERT_EQ(listing["result"][0]["id"], nlohmann::json("skill"));
	const nlohmann::json read = {{"requestId", "read-asset"}, {"method", "skill_read"}, {"arguments", {{"id", "skill"}, {"path", "references/example.md"}}}};
	const nlohmann::json asset = uam::UamControlService::HandleRequestForTests(app, id, read, now);
	UAM_ASSERT(asset.value("ok", false));
	UAM_ASSERT_EQ(asset["result"]["body"], nlohmann::json("Full supporting asset"));
	app.uam_control_capabilities.back().central_skills_enabled = false;
	const nlohmann::json denied = uam::UamControlService::HandleRequestForTests(app, id, {{"requestId", "read-denied"}, {"method", "skill_read"}, {"arguments", {{"id", "skill"}}}}, now);
	UAM_ASSERT(!denied.value("ok", true));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(SavedMcpSettingsReachAllFiveRuntimesWithoutEnablingCentralInstructions)
{
	TempDir temp("uam-mcp-propagation");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "shared-http";
	server.name = "shared-http";
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	server.headers = {{"Authorization", "UAM_PROPAGATION_TEST_SECRET"}};
	settings.mcp_servers = {server};
	// The Settings store is the only source. No provider home files are written.
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT(!loaded.central_provider_configuration.enabled);
	ScopedEnvVar secret("UAM_PROPAGATION_TEST_SECRET", "target-secret");
	for (const std::string& host_id : {"local", "ssh-linux", "ssh-windows"})
	{
		ExecutionHost host;
		host.id = host_id;
		host.transport = host_id == "local" ? "local" : "ssh";
		ChatSession chat;
		chat.id = "mcp-propagation";
		chat.uam_agent_id = "missing-agent-must-not-block-mcp";
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / host_id, loaded, chat, host, temp.root, bundle, error));
		UAM_ASSERT(!bundle.local_directory.empty());
		UAM_ASSERT(bundle.manifest.value("mcpOnly", false));
		UAM_ASSERT(bundle.manifest["files"].empty());
		UAM_ASSERT(bundle.manifest.dump().find("target-secret") == std::string::npos);
		for (const std::string& provider : {"codex-cli", "claude-cli", "gemini-cli", "opencode-cli", "copilot-cli"})
		{
			std::vector<std::string> argv{"provider", "--append-system-prompt", "existing instructions"};
			uam::provider_setup::Environment environment;
			UAM_ASSERT(uam::provider_setup::Apply(provider, bundle.local_directory, temp.root, argv, environment, error));
			UAM_ASSERT(std::ranges::find(argv, "existing instructions") != argv.end());
			UAM_ASSERT(std::ranges::find(argv, "--append-system-prompt-file") == argv.end());
			if (provider == "codex-cli")
			{
				UAM_ASSERT(std::ranges::any_of(argv, [](const std::string& arg) { return arg.find("mcp_servers.") != std::string::npos && arg.find(".url=") != std::string::npos; }));
				UAM_ASSERT(std::ranges::none_of(argv, [](const std::string& arg) { return arg.find("target-secret") != std::string::npos || arg.starts_with("developer_instructions="); }));
			}
			else if (provider == "opencode-cli")
			{
				const nlohmann::json config = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
				UAM_ASSERT_EQ(config["mcp"]["shared-http"]["url"], nlohmann::json(server.url));
				UAM_ASSERT_EQ(config["mcp"]["shared-http"]["headers"]["Authorization"], nlohmann::json("target-secret"));
			}
			else
			{
				std::string configuration_path;
				if (provider == "gemini-cli") configuration_path = uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH");
				else
				{
					const auto flag = std::ranges::find(argv, provider == "claude-cli" ? "--mcp-config" : "--additional-mcp-config");
					UAM_ASSERT(flag != argv.end());
					configuration_path = *(flag + 1);
					if (configuration_path.starts_with("@")) configuration_path.erase(0, 1);
				}
				const nlohmann::json config = nlohmann::json::parse(ReadFile(uam::paths::PathFromUtf8(configuration_path)));
				const nlohmann::json native = config["mcpServers"]["shared-http"];
				UAM_ASSERT_EQ(native[provider == "gemini-cli" ? "httpUrl" : "url"], nlohmann::json(server.url));
				UAM_ASSERT_EQ(native["headers"]["Authorization"], nlohmann::json("target-secret"));
			}
		}
	}
}

UAM_TEST(SharedMcpFollowsSourceWorkspaceIntoAnIsolatedWorktree)
{
	TempDir temp("uam-mcp-worktree-scope");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "source-server";
	server.name = "source-server";
	server.workspace_directory = (temp.root / "source").string();
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	settings.mcp_servers = {server};
	ChatSession chat;
	chat.workspace_source_directory = server.workspace_directory;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root / "worktree", bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"].size(), std::size_t{1});
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["name"], nlohmann::json(server.name));
	McpServerConfiguration override = server;
	override.id = "worktree-override";
	override.workspace_directory = (temp.root / "worktree").string();
	override.url = "http://localhost:5001/mcp";
	for (const bool source_first : {false, true})
	{
		settings.mcp_servers = source_first ? std::vector<McpServerConfiguration>{server, override} : std::vector<McpServerConfiguration>{override, server};
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root / "worktree", bundle, error));
		UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["url"], nlohmann::json(override.url));
	}
	bundle.manifest["mcpOnly"] = "yes";
	UAM_ASSERT(!uam::provider_setup::ValidateManifest(bundle.manifest, error));
}

UAM_TEST(McpOnlySetupPreservesGeminiContextConfiguration)
{
	TempDir temp("uam-mcp-gemini-preservation");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "shared-http";
	server.name = "shared-http";
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	settings.mcp_servers = {server};
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	const nlohmann::json original = {{"context", {{"fileName", "CUSTOM.md"}, {"loadMemoryFromIncludeDirectories", false}}}};
	const fs::path system_settings = temp.root / "system-settings.json";
	UAM_ASSERT(uam::io::WriteTextFile(system_settings, original.dump()));
	std::vector<std::string> argv{"gemini"};
	uam::provider_setup::Environment environment{{"GEMINI_CLI_SYSTEM_SETTINGS_PATH", system_settings.string()}};
	UAM_ASSERT(uam::provider_setup::Apply("gemini-cli", bundle.local_directory, temp.root, argv, environment, error));
	const nlohmann::json generated = nlohmann::json::parse(ReadFile(uam::paths::PathFromUtf8(uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH"))));
	UAM_ASSERT_EQ(generated["context"], original["context"]);
	UAM_ASSERT_EQ(generated["mcpServers"][server.name]["httpUrl"], nlohmann::json(server.url));
	UAM_ASSERT_EQ(argv, std::vector<std::string>{"gemini"});
}

UAM_TEST(CliViewPreparesMcpInheritedFromSourceWorkspace)
{
	TempDir temp("uam-mcp-cli-source");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "source-mcp-cli";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = false;
	chat.workspace_directory = (temp.root / "worktree").string();
	chat.workspace_source_directory = (temp.root / "source").string();
	fs::create_directories(chat.workspace_directory);
	McpServerConfiguration server;
	server.id = "source-mcp";
	server.name = "source-mcp";
	server.workspace_directory = chat.workspace_source_directory;
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	app.settings.mcp_servers = {server};
	ExecutionHost host;
	uam::CliTerminalState terminal;
	std::vector<std::string> argv{"opencode"};
	uam::provider_setup::Environment environment;
	std::string channel;
	std::string error;
	UAM_ASSERT(!uam::PrepareCliProviderHandoffAsync(app, terminal, chat, host, argv, environment, channel, error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(terminal.context_preparation != nullptr);
	app.cli_context_preparation_tasks.back().worker->join();
	UAM_ASSERT(uam::PrepareCliProviderHandoffAsync(app, terminal, chat, host, argv, environment, channel, error));
	const nlohmann::json generated = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
	UAM_ASSERT_EQ(generated["mcp"][server.name]["url"], nlohmann::json(server.url));
}

UAM_TEST(McpWindowsWorkspaceScopeMatchesAcrossControllerPlatforms)
{
	McpServerConfiguration global;
	global.id = "global";
	global.name = "shared";
	global.transport = "http";
	global.url = "http://localhost:5000/mcp";
	McpServerConfiguration scoped = global;
	scoped.id = "windows-workspace";
	scoped.execution_host_id = "ssh-windows";
	scoped.workspace_directory = R"(C:\Projects\Example)";
	scoped.url = "http://localhost:5001/mcp";
	std::vector<McpServerConfiguration> servers{global, scoped};
	std::string error;
	UAM_ASSERT(uam::mcp_server_config::NormalizeAndValidate(servers, &error));
	const std::vector<McpServerConfiguration> selected = uam::mcp_server_config::SelectForWorkspace(servers, "c:/projects/example/", "ssh-windows");
	UAM_ASSERT_EQ(selected.size(), std::size_t{1});
	UAM_ASSERT_EQ(selected.front().id, scoped.id);
	UAM_ASSERT_EQ(uam::mcp_server_config::SelectForWorkspace(servers, "C:/Projects/Other", "ssh-windows").front().id, global.id);
	UAM_ASSERT_EQ(uam::mcp_server_config::SelectForWorkspace(servers, "C:/Projects/Example", "local").front().id, global.id);
}

UAM_TEST(ConcurrentCentralPreparationPublishesCompleteBundles)
{
	TempDir temp("uam-central-concurrent");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = std::string(256 * 1024, 'x');
	ChatSession chat;
	ExecutionHost host;
	for (int round = 0; round < 4; ++round)
	{
		std::barrier start(12);
		std::vector<std::jthread> workers;
		std::vector<std::string> errors(12);
		for (int index = 0; index < 12; ++index)
			workers.emplace_back([&, index]
			{
				start.arrive_and_wait();
				uam::ProviderConfigurationBundle bundle;
				if (!uam::PrepareProviderConfiguration(temp.root / std::to_string(round), settings, chat, host, temp.root, bundle, errors[index])) return;
				std::vector<std::string> missing;
				if (!uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, errors[index])) return;
				if (!missing.empty()) errors[index] = "Published bundle is incomplete.";
			});
		workers.clear();
		for (const std::string& error : errors) if (!error.empty()) throw std::runtime_error(error);
	}
}

UAM_TEST(CentralPreparationRecoversInterruptedOwnershipPublication)
{
	TempDir temp("uam-central-interrupted-owner");
	const nlohmann::json manifest = {{"format", 1}, {"files", nlohmann::json::object()}};
	const fs::path directory = temp.root / uam::provider_setup::Digest(manifest.dump());
	fs::create_directories(directory);
	std::vector<std::string> missing;
	std::string error;
	UAM_ASSERT(uam::provider_setup::Prepare(directory, manifest, missing, error));
	UAM_ASSERT_EQ(ReadFile(directory / "owner.json"), manifest.dump());
}

UAM_TEST(CentralPreparationRecoversStagingFilesAndRejectsUnownedData)
{
	TempDir temp("uam-central-staging-recovery");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Shared resource";
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	const fs::path target = temp.root / "interrupted" / bundle.local_directory.filename();
	fs::create_directories(target);
	UAM_ASSERT(uam::io::WriteTextFile(target / "context-settings-123.json", "unfinished"));
	std::vector<std::string> missing;
	UAM_ASSERT(uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT_EQ(missing.size(), bundle.files.size());
	for (const auto& [name, bytes] : bundle.files)
	{
		fs::create_directories((target / name).parent_path());
		UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target / name, bytes, error));
	}
	UAM_ASSERT(uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT(missing.empty());
	const fs::path foreign = temp.root / "foreign" / bundle.local_directory.filename();
	fs::create_directories(foreign);
	UAM_ASSERT(uam::io::WriteTextFile(foreign / "user.txt", "keep"));
	UAM_ASSERT(!uam::provider_setup::Prepare(foreign, bundle.manifest, missing, error));
	UAM_ASSERT(!fs::exists(foreign / "owner.json"));
	UAM_ASSERT_EQ(ReadFile(foreign / "user.txt"), std::string("keep"));
	UAM_ASSERT(uam::io::WriteTextFile(target / "owner.json", "other owner"));
	UAM_ASSERT(!uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT_EQ(ReadFile(target / "owner.json"), std::string("other owner"));
}

UAM_TEST(ImmutableCentralPublicationKeepsExistingData)
{
	TempDir temp("uam-central-immutable-publication");
	const fs::path target = temp.root / "resource.md";
	std::string error;
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "published", error, false));
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "published", error, false));
	UAM_ASSERT(!uam::provider_setup::WriteRuntimeFile(target, "different", error, false));
	UAM_ASSERT_EQ(ReadFile(target), std::string("published"));
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "updated runtime setting", error));
	UAM_ASSERT_EQ(ReadFile(target), std::string("updated runtime setting"));
}

UAM_TEST(CentralNativeSetupInstallsTheQueuedAgentSnapshot)
{
	TempDir temp("uam-central-native-snapshot");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "SHARED_NATIVE_INSTRUCTIONS";
	ChatSession chat;
	chat.uam_agent_id = "build";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	const std::string snapshot = "QUEUED_NATIVE_AGENT_SNAPSHOT";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, &snapshot));
	UAM_ASSERT(bundle.files.at("AGENTS.md").find(snapshot) != std::string::npos);
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("SHARED_NATIVE_INSTRUCTIONS") != std::string::npos);
	UAM_ASSERT_EQ(bundle.files.at("agents/build.md"), snapshot);
	UAM_ASSERT_EQ(ReadFile(bundle.local_directory / "agents/build.md"), snapshot);
}

UAM_TEST(McpServersAcceptLanUrlsAndLiteralValuesButRejectCredentialUrls)
{
	McpServerConfiguration remote{.id = "penpot", .name = "penpot", .transport = "http", .url = "http://main.homelab.com:9001/mcp/stream?userToken=abc"};
	remote.headers = {{"Authorization", "", "Bearer token"}};
	McpServerConfiguration local{.id = "searxng", .name = "searxng", .transport = "stdio", .command = "/bin/sh"};
	local.environment = {{"SEARXNG_URL", "", "http://main.homelab.com:8081"}};
	std::vector<McpServerConfiguration> servers{remote, local};
	std::string error;
	UAM_ASSERT(uam::mcp_server_config::NormalizeAndValidate(servers, &error));
	const std::vector<McpServerConfiguration> round_trip = uam::mcp_server_config::Parse(uam::mcp_server_config::Serialize(servers));
	UAM_ASSERT_EQ(round_trip[1].environment.front().value, std::string("http://main.homelab.com:8081"));
	const nlohmann::json resolved = uam::mcp_server_config::ResolveForWorkspace(round_trip, "/any", true, true, &error);
	UAM_ASSERT(resolved.is_array() && resolved.size() == 2);
	UAM_ASSERT_EQ(resolved[0]["headers"][0]["value"].get<std::string>(), std::string("Bearer token"));
	UAM_ASSERT_EQ(resolved[1]["env"][0]["value"].get<std::string>(), std::string("http://main.homelab.com:8081"));
	std::vector<McpServerConfiguration> credentialed{remote};
	credentialed.front().url = "http://user:pass@main.homelab.com/mcp";
	UAM_ASSERT(!uam::mcp_server_config::NormalizeAndValidate(credentialed, &error));
	std::vector<McpServerConfiguration> both{local};
	both.front().environment.front().environment_variable = "SEARXNG_URL";
	UAM_ASSERT(!uam::mcp_server_config::NormalizeAndValidate(both, &error));
}
