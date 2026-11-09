#include "test_harness.h"
#include "app/provider_configuration_service.h"
#include "remote/host_context.h"
#include "common/provider/provider_runtime.h"

using namespace uam_test;

UAM_TEST(RemoteInlineInstructionsPreserveLegacyEmptyAndTextStates)
{
	ExecutionHost host;
	host.id = "inline-fixture";
	host.transport = "ssh";
	host.ssh_alias = "never-connect";
	host.platform = "linux";
	host.instruction_file = "/legacy/AGENTS.md";
	const nlohmann::json legacy = uam::execution_hosts::Serialize({host});
	UAM_ASSERT(!uam::execution_hosts::Parse(legacy).back().instruction_text);
	nlohmann::json absent = legacy;
	absent.back().erase("instructionText");
	UAM_ASSERT(!uam::execution_hosts::Parse(absent).back().instruction_text);
	for (const std::string text : {std::string{}, std::string("Quoted \"text\"\nUnicode: ✓\n  preserved  ")})
	{
		host.instruction_text = text;
		const ExecutionHost restored = uam::execution_hosts::Parse(uam::execution_hosts::Serialize({host})).back();
		UAM_ASSERT(restored.instruction_text.has_value());
		UAM_ASSERT_EQ(*restored.instruction_text, text);
		UAM_ASSERT_EQ(restored.instruction_file, std::string("/legacy/AGENTS.md"));
		std::string context;
		std::string error;
		UAM_ASSERT(uam::remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service, restored, "/workspace", 0, context, error));
		// Host instructions arrive through launch configuration, so no prompt text can repeat them.
		UAM_ASSERT_EQ(context, std::string{});
	}
	ExecutionHost changed = host;
	changed.instruction_text = "changed";
	UAM_ASSERT(!uam::execution_hosts::SameConnection(host, changed));
}

UAM_TEST(RemoteInlineInstructionsRejectMalformedAndOversizedText)
{
	ExecutionHost host;
	host.id = "inline-fixture";
	host.transport = "ssh";
	host.ssh_alias = "never-connect";
	host.platform = "linux";
	host.instruction_text = std::string(uam::execution_hosts::kMaxInstructionTextBytes, 'x');
	std::string error;
	UAM_ASSERT(uam::execution_hosts::NormalizeRemote(host, &error));
	nlohmann::json serialized = uam::execution_hosts::Serialize({host});
	UAM_ASSERT_EQ(uam::execution_hosts::Parse(serialized).size(), std::size_t(2));
	for (const nlohmann::json& malformed : {nlohmann::json(1), nlohmann::json(false), nlohmann::json::object(), nlohmann::json(std::string(uam::execution_hosts::kMaxInstructionTextBytes + 1, 'x'))})
	{
		serialized.back()["instructionText"] = malformed;
		UAM_ASSERT_EQ(uam::execution_hosts::Parse(serialized).size(), std::size_t(1));
	}
	host.instruction_text->push_back('x');
	UAM_ASSERT(!uam::execution_hosts::NormalizeRemote(host, &error));
	std::string context;
	UAM_ASSERT(!uam::remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service, host, "/workspace", 0, context, error));
}

UAM_TEST(RemoteInlineInstructionsReachRunnerWithoutLegacyFileReads)
{
	TempDir temp("uam-inline-host-instructions");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Controller instruction";
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCodexCli;
	ExecutionHost host;
	host.id = "ssh-inline";
	host.transport = "ssh";
	host.instruction_file = (temp.root / "missing-legacy.md").string();
	for (const std::string text : {std::string("Inline host marker\n"), std::string{}})
	{
		host.instruction_text = text;
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
		UAM_ASSERT_EQ(bundle.manifest["hostInstructionFile"].get<std::string>(), std::string{});
		UAM_ASSERT(!bundle.manifest.contains("hostInstructionText"));
		std::vector<std::string> argv{"codex", "app-server", "--listen", "stdio://"};
		uam::provider_setup::Environment environment;
		std::string instructions;
		UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error, nlohmann::json::array(), &instructions));
		UAM_ASSERT_EQ(instructions.find("Inline host marker") != std::string::npos, !text.empty());
		UAM_ASSERT(!fs::exists(temp.root / "missing-legacy.md"));
	}
}

UAM_TEST(RemoteInlineInstructionsBoundCombinedProviderPayload)
{
	TempDir temp("uam-inline-host-size");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = std::string(uam::provider_setup::kMaxFileBytes - 1024, 'g');
	ChatSession chat;
	ExecutionHost host;
	host.id = "ssh-inline";
	host.transport = "ssh";
	host.instruction_text = std::string(2048, 'h');
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(error, std::string("Combined provider instructions exceed 2 MiB."));
	settings.central_provider_configuration.instructions.clear();
	host.instruction_text = std::string(uam::execution_hosts::kMaxInstructionTextBytes + 1, 'h');
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(error, std::string("Host instruction text exceeds 256 KiB."));
}

namespace
{
	std::string RuntimeInstructionsFor(const std::filesystem::path& directory, const std::string& provider)
	{
		for (const std::filesystem::directory_entry& entry : fs::recursive_directory_iterator(directory / "runtime" / provider))
			if (entry.is_regular_file() && entry.path().filename() == "AGENTS.md")
				return uam::io::ReadTextFile(entry.path());
		throw std::runtime_error("No runtime instructions for " + provider);
	}

	bool ArgvContains(const std::vector<std::string>& argv, std::string_view value)
	{
		return std::ranges::any_of(argv, [value](const std::string& argument) { return argument.find(value) != std::string::npos; });
	}

	ExecutionHost SshHost()
	{
		ExecutionHost host;
		host.id = "ssh-host";
		host.transport = "ssh";
		host.platform = "linux";
		return host;
	}
}

UAM_TEST(RemoteHostInstructionsReachEveryProviderLaunchWhileCentralSetupIsOff)
{
	TempDir temp("uam-host-only-launch");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "AGENTS.md", "Workspace marker"));
	AppSettings settings;
	ExecutionHost host = SshHost();
	host.instruction_text = "Inline host marker";
	for (const std::string& provider : {std::string(uam::provider_ids::kClaudeCli), std::string(uam::provider_ids::kCodexCli), std::string(uam::provider_ids::kGeminiCli),
	         std::string(uam::provider_ids::kOpenCodeCli), std::string(uam::provider_ids::kCopilotCli), std::string(uam::provider_ids::kAntigravityCli)})
	{
		ChatSession chat;
		chat.provider_id = provider;
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, workspace, bundle, error));
		UAM_ASSERT(!bundle.local_directory.empty());
		UAM_ASSERT(!bundle.manifest["mcpOnly"].get<bool>());
		UAM_ASSERT(bundle.manifest["hostInstructionsOnly"].get<bool>());
		std::vector<std::string> argv{"provider"};
		if (provider == uam::provider_ids::kCodexCli) argv = {"codex", "app-server", "--listen", "stdio://"};
		uam::provider_setup::Environment environment;
		if (provider == uam::provider_ids::kGeminiCli) environment.emplace_back("GEMINI_CLI_HOME", (temp.root / "gemini-home").string());
		std::string structured;
		UAM_ASSERT(uam::provider_setup::Apply(provider, bundle.local_directory, workspace, argv, environment, error, nlohmann::json::array(), &structured));
		const std::string delivered = RuntimeInstructionsFor(bundle.local_directory, provider);
		UAM_ASSERT(delivered.find("Inline host marker") != std::string::npos);
		// Central Setup off must not start injecting the workspace file.
		UAM_ASSERT(delivered.find("Workspace marker") == std::string::npos);
		if (provider == uam::provider_ids::kClaudeCli) UAM_ASSERT(ArgvContains(argv, "--append-system-prompt-file"));
		if (provider == uam::provider_ids::kCodexCli) UAM_ASSERT(structured.find("Inline host marker") != std::string::npos);
		if (provider == uam::provider_ids::kCopilotCli) UAM_ASSERT(!uam::provider_setup::EnvironmentValue(environment, "COPILOT_CUSTOM_INSTRUCTIONS_DIRS").empty());
		if (provider == uam::provider_ids::kOpenCodeCli) UAM_ASSERT(nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"))["instructions"].size() == 1);
		if (provider == uam::provider_ids::kGeminiCli)
		{
			UAM_ASSERT(ArgvContains(argv, "--include-directories"));
			const nlohmann::json geminiSettings = nlohmann::json::parse(uam::io::ReadTextFile(uam::paths::PathFromUtf8(uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH"))));
			// Listing AGENTS.md would make Gemini read the workspace file as well.
			UAM_ASSERT_EQ(geminiSettings["context"]["fileName"], nlohmann::json::array({"GEMINI.md"}));
			UAM_ASSERT(uam::io::ReadTextFile(uam::paths::PathFromUtf8(argv.back()) / "GEMINI.md").find("Inline host marker") != std::string::npos);
			// Stops Gemini's upward context walk from also reading the bundle's AGENTS.md.
			UAM_ASSERT(fs::is_directory(uam::paths::PathFromUtf8(argv.back()) / ".git"));
			// Replacing the system-settings path must not move the host's own system defaults.
			const std::string defaults = uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_DEFAULTS_PATH");
			UAM_ASSERT(defaults.ends_with("system-defaults.json"));
			UAM_ASSERT(defaults.find(bundle.local_directory.string()) == std::string::npos);
		}
		if (provider == uam::provider_ids::kAntigravityCli) UAM_ASSERT(ArgvContains(argv, "--add-dir"));
	}
	// CLI View has no protocol channel, so Codex must carry them on its command line.
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCodexCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, workspace, bundle, error));
	std::vector<std::string> argv{"codex"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error));
	UAM_ASSERT(ArgvContains(argv, "Inline host marker"));
}

UAM_TEST(GeminiChatLeavesHostInstructionsOutOfItsLaunchButCliViewKeepsThem)
{
	TempDir temp("uam-gemini-first-prompt");
	AppSettings settings;
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kGeminiCli;
	ExecutionHost host = SshHost();
	host.instruction_text = "Host marker";
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	// Structured Chat: nothing about the host belongs in the launch.
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error, nullptr, {}, false, true));
	UAM_ASSERT(bundle.local_directory.empty());
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Central marker";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error, nullptr, {}, false, true));
	UAM_ASSERT(bundle.files["AGENTS.md"].find("Central marker") != std::string::npos);
	UAM_ASSERT(bundle.files["AGENTS.md"].find("Host marker") == std::string::npos);
	host.instruction_text.reset();
	host.instruction_file = "/legacy/host.md";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error, nullptr, {}, false, true));
	UAM_ASSERT_EQ(bundle.manifest["hostInstructionFile"].get<std::string>(), std::string{});
	// CLI View shows its own trust prompt, so it keeps launch delivery.
	settings.central_provider_configuration = {};
	host.instruction_file.clear();
	host.instruction_text = "Host marker";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(bundle.files["AGENTS.md"].find("Host marker") != std::string::npos);
	UAM_ASSERT(ProviderRuntimeRegistry::ResolveById("gemini-cli").DeliversHostInstructionsInFirstPrompt());
	UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById("claude-cli").DeliversHostInstructionsInFirstPrompt());
}

UAM_TEST(RemoteHostContextReturnsHostTextSeparatelyWhenAskedFor)
{
	ExecutionHost host = SshHost();
	host.ssh_alias = "never-connect";
	host.instruction_text = "Host marker";
	std::string context;
	std::string host_text;
	std::string error;
	UAM_ASSERT(uam::remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service, host, "/workspace", 0, context, error, {}, &host_text));
	UAM_ASSERT_EQ(host_text, std::string("Host marker"));
	UAM_ASSERT_EQ(context, std::string{});
	UAM_ASSERT_EQ(uam::remote::WrapHostInstructions(host_text), std::string("--- BEGIN HOST INSTRUCTIONS ---\nHost marker\n--- END HOST INSTRUCTIONS ---\n\n"));
}

UAM_TEST(RemoteHostOnlyGeminiLaunchDoesNotRepeatTextWhenAgentsMdIsAlreadyListed)
{
	TempDir temp("uam-host-only-gemini-names");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	const fs::path home = temp.root / "gemini-home";
	fs::create_directories(home / ".gemini");
	UAM_ASSERT(uam::io::WriteTextFile(home / ".gemini" / "settings.json", R"({"context":{"fileName":["GEMINI.md","AGENTS.md"]}})"));
	AppSettings settings;
	ExecutionHost host = SshHost();
	host.instruction_text = "Inline host marker";
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kGeminiCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, workspace, bundle, error));
	std::vector<std::string> argv{"gemini"};
	uam::provider_setup::Environment environment{{"GEMINI_CLI_HOME", home.string()}};
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error));
	const fs::path runtime = uam::paths::PathFromUtf8(argv.back());
	UAM_ASSERT(uam::io::ReadTextFile(runtime / "AGENTS.md").find("Inline host marker") != std::string::npos);
	UAM_ASSERT(!fs::exists(runtime / "GEMINI.md"));
}

UAM_TEST(RemoteHostOnlyCodexCliViewNamesHostInstructionsWhenOverTheLaunchLimit)
{
	TempDir temp("uam-host-only-codex-limit");
	AppSettings settings;
	ExecutionHost host = SshHost();
	host.instruction_text = std::string(128 * 1024, 'h');
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCodexCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	std::vector<std::string> argv{"codex"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(!uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT_EQ(error, std::string("Host instructions exceed the OS launch limit for this provider. Shorten them."));
}

UAM_TEST(ManagedWorkspaceResourceAcceptsAWorkspaceWithAFinalSeparator)
{
	const fs::path setup = fs::path("/srv/app/.UAM/provider-setup/0123456789abcdef");
	UAM_ASSERT(uam::provider_setup::IsManagedWorkspaceResource(setup, "provider-setup", "/srv/app"));
	UAM_ASSERT(uam::provider_setup::IsManagedWorkspaceResource(setup, "provider-setup", "/srv/app/"));
	UAM_ASSERT(!uam::provider_setup::IsManagedWorkspaceResource(setup, "provider-setup", "/srv/other/"));
	UAM_ASSERT(!uam::provider_setup::IsManagedWorkspaceResource(setup, "context", "/srv/app/"));
	UAM_ASSERT(uam::provider_setup::IsManagedWorkspaceResource("/srv/app/.UAM/context/0123456789abcdef", "context", "/srv/app/"));
}

#if !defined(_WIN32)
UAM_TEST(ProviderSetupNamesASymbolicLinkInTheLaunchFolderPath)
{
	TempDir temp("uam-host-only-linked-workspace");
	const fs::path real = temp.root / "real";
	fs::create_directories(real);
	const fs::path linked = temp.root / "linked";
	fs::create_directory_symlink(real, linked);
	const nlohmann::json manifest{{"format", 1}, {"mcpOnly", false}, {"hostInstructionsOnly", true}, {"files", nlohmann::json::object()}};
	std::vector<std::string> missing;
	std::string error;
	UAM_ASSERT(!uam::provider_setup::Prepare(linked / uam::provider_setup::Digest(manifest.dump()), manifest, missing, error));
	UAM_ASSERT(error.find("symbolic link") != std::string::npos);
}
#endif

UAM_TEST(RemoteLegacyHostFileStopsAHostOnlyLaunchWhenUnreadable)
{
	TempDir temp("uam-host-only-legacy");
	const fs::path legacy = temp.root / "legacy.md";
	AppSettings settings;
	ExecutionHost host = SshHost();
	host.instruction_file = legacy.string();
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kClaudeCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["hostInstructionFile"].get<std::string>(), legacy.string());
	std::vector<std::string> argv{"claude"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(!uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(uam::io::WriteTextFile(legacy, "Legacy host marker"));
	error.clear();
	argv = {"claude"};
	environment.clear();
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(RuntimeInstructionsFor(bundle.local_directory, chat.provider_id).find("Legacy host marker") != std::string::npos);
}

UAM_TEST(RemoteHostInstructionsKeepDocumentedOrderWithCentralSetup)
{
	TempDir temp("uam-host-order");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "AGENTS.md", "Workspace marker"));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Central marker";
	ExecutionHost host = SshHost();
	host.instruction_text = "Inline host marker";
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kClaudeCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, workspace, bundle, error));
	UAM_ASSERT(!bundle.manifest.contains("hostInstructionsOnly"));
	std::vector<std::string> argv{"claude"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error));
	const std::string delivered = RuntimeInstructionsFor(bundle.local_directory, chat.provider_id);
	UAM_ASSERT(delivered.find("Central marker") < delivered.find("Inline host marker"));
	UAM_ASSERT(delivered.find("Inline host marker") < delivered.find("Workspace marker"));
}

UAM_TEST(LocalHostIgnoresHostInstructionsAtLaunch)
{
	TempDir temp("uam-host-local");
	AppSettings settings;
	ExecutionHost host;
	host.instruction_text = "Never for local chats";
	host.instruction_file = (temp.root / "legacy.md").string();
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kClaudeCli;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(bundle.local_directory.empty());
}
