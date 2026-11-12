#include "test_harness.h"
#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "computer_use/computer_use_mcp_config.h"

using namespace uam_test;

namespace
{
	using Argv = std::vector<std::string>;
	using Environment = std::vector<std::pair<std::string, std::string>>;

	bool HasRun(const Argv& argv, const Argv& run)
	{
		return std::ranges::search(argv, run).begin() != argv.end();
	}

	bool HasAny(const Argv& argv, std::string_view needle)
	{
		return std::ranges::any_of(argv, [&](const std::string& argument) { return argument.find(needle) != std::string::npos; });
	}

	std::string EnvValue(const Environment& environment, std::string_view name)
	{
		const auto found = std::ranges::find(environment, name, &std::pair<std::string, std::string>::first);
		return found == environment.end() ? std::string("<unset>") : found->second;
	}

	ChatSession LaunchChat(std::string provider, std::string approval_mode)
	{
		ChatSession chat;
		chat.id = "permission-launch";
		chat.provider_id = std::move(provider);
		chat.approval_mode = std::move(approval_mode);
		chat.computer_use_globally_enabled = true;
		return chat;
	}

	Argv Filter(std::string_view flags, bool ceiling)
	{
		AppSettings settings;
		settings.provider_extra_flags = std::string(flags);
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, ceiling);
	}
}

UAM_TEST(ProviderPermissionFlagsStripBypassAlwaysAndGrantsUnderReadCeiling)
{
	// Every bypass spelling is removed with its value, with or without a read ceiling.
	const std::string bypass =
	    "--dangerously-bypass-approvals-and-sandbox -s danger-full-access --sandbox=danger-full-access -sdanger-full-access "
	    "--config sandbox_mode=\"danger-full-access\" -c approval_policy=never -a never --ask-for-approval=never -y --yolo "
	    "--approval-mode yolo --permission-mode bypassPermissions --allow-dangerously-skip-permissions --dangerously-skip-permissions "
	    "-dangerously-skip-permissions --allow-all-tools --allow-all-urls --full-auto -csandbox_mode=danger-full-access -capproval_policy=never --keep";
	for (const bool ceiling : {false, true})
		UAM_ASSERT_EQ(Filter(bypass, ceiling), (Argv{"--keep"}));
	AppSettings bypass_settings;
	bypass_settings.provider_extra_flags = "-y";
	UAM_ASSERT(uam::provider_runtime_internal::HasPermissionBypassExtraFlags(bypass_settings));

	// Grants stay in ordinary launches and are removed, values included, under a read ceiling.
	const std::string grants =
	    "--allowedTools Bash(npm run *) Edit --allowed-tools=Write --allow-tool shell(git) write --allow-url example.com "
	    "--settings ./s.json --setting-sources=user,project --permission-mode acceptEdits --permission-mode=dontAsk "
	    "--approval-mode auto_edit --policy ./p.toml --admin-policy=./a.toml -a on-request -s workspace-write "
	    "-c sandbox_workspace_write.network_access=true -c mcp_servers.x.default_tools_approval_mode=approve --mode accept-edits "
	    "--autopilot --assisted-approval --profile wide --model m";
	UAM_ASSERT_EQ(Filter(grants, false), uam::command_line::SplitWords(grants));
	UAM_ASSERT_EQ(Filter(grants, true), (Argv{"--model", "m"}));
	AppSettings grant_settings;
	grant_settings.provider_extra_flags = grants;
	UAM_ASSERT(!uam::provider_runtime_internal::HasPermissionBypassExtraFlags(grant_settings));

	// Restrictive options stay, and nothing after a bare "--" is provider option text.
	UAM_ASSERT_EQ(Filter("--sandbox --disallowedTools Bash -s read-only --allowed-mcp-server-names a -- --yolo --allowedTools Edit", true),
	    (Argv{"--sandbox", "--disallowedTools", "Bash", "-s", "read-only", "--allowed-mcp-server-names", "a", "--", "--yolo", "--allowedTools", "Edit"}));
}

UAM_TEST(ProviderReadCeilingCliViewArgvAddsNativePlanAndStripsGrants)
{
	AppSettings settings;
	settings.provider_extra_flags = "--allowedTools Edit --allow-tool write --settings {} --yolo";
	settings.computer_use_globally_enabled = true;
	for (const std::string mode : {"plan", "default"})
	{
		const bool plan = mode == "plan";
		const auto argv = [&](const ProviderProfile& profile, const std::string& provider)
		{ return ProviderRuntimeRegistry::Resolve(profile).BuildInteractiveArgv(profile, LaunchChat(provider, mode), settings); };
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		const Argv codex = argv(ProviderProfileStore::DefaultCodexProfile(), uam::provider_ids::kCodexCli);
		UAM_ASSERT_EQ(HasRun(codex, {"--sandbox", "read-only", "--ask-for-approval", "on-request"}), plan);
		UAM_ASSERT_EQ(HasRun(codex, {"--disable", "computer_use"}), plan || !uam::computer_use::AvailableForChat(LaunchChat(uam::provider_ids::kCodexCli, mode)));
		UAM_ASSERT_EQ(HasAny(codex, "--allowedTools"), !plan);
		UAM_ASSERT(!HasAny(codex, "--yolo"));
		// Codex refuses repeated sandbox options, so UAM's own replace a kept restrictive one, before positional data.
		AppSettings restrictive;
		restrictive.provider_extra_flags = "-s read-only --ask-for-approval=untrusted -- prompt";
		const Argv codex_restrictive = ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).BuildInteractiveArgv(
		    ProviderProfileStore::DefaultCodexProfile(), LaunchChat(uam::provider_ids::kCodexCli, mode), restrictive);
		UAM_ASSERT_EQ(static_cast<std::size_t>(std::ranges::count(codex_restrictive, "--sandbox") + std::ranges::count(codex_restrictive, "-s")), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(codex_restrictive.back(), std::string("prompt"));
		if (plan) UAM_ASSERT(std::ranges::find(codex_restrictive, "--sandbox") < std::ranges::find(codex_restrictive, "--"));
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		const Argv gemini = argv(ProviderProfileStore::DefaultGeminiProfile(), uam::provider_ids::kGeminiCli);
		UAM_ASSERT_EQ(HasRun(gemini, {"--approval-mode", "plan"}), plan);
		UAM_ASSERT_EQ(HasAny(gemini, "--settings"), !plan);
		AppSettings positional;
		positional.provider_extra_flags = "-- prompt";
		const Argv gemini_positional = ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultGeminiProfile()).BuildInteractiveArgv(
		    ProviderProfileStore::DefaultGeminiProfile(), LaunchChat(uam::provider_ids::kGeminiCli, mode), positional);
		UAM_ASSERT_EQ(HasRun(gemini_positional, {"--approval-mode", "plan", "--"}), plan);
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		const Argv claude = argv(ProviderProfileStore::DefaultClaudeProfile(), uam::provider_ids::kClaudeCli);
		UAM_ASSERT_EQ(HasRun(claude, {"--permission-mode", "plan"}), plan);
		UAM_ASSERT_EQ(HasAny(claude, "--allowedTools"), !plan);
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		const Argv copilot = argv(ProviderProfileStore::DefaultCopilotProfile(), uam::provider_ids::kCopilotCli);
		UAM_ASSERT_EQ(HasAny(copilot, "--allow-tool"), !plan);
		UAM_ASSERT(!HasAny(copilot, "--yolo"));
#endif
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
		const Argv antigravity = argv(ProviderProfileStore::DefaultAntigravityProfile(), uam::provider_ids::kAntigravityCli);
		UAM_ASSERT_EQ(HasRun(antigravity, {"--mode", "plan"}), plan);
		UAM_ASSERT(!HasAny(antigravity, "--yolo"));
#endif
	}
}

UAM_TEST(ProviderReadOnlyAgentCliViewMapsToPlanWithoutCentralConfiguration)
{
	TempDir temp("uam-read-agent-cli-view");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace / ".uam" / "agents");
	UAM_ASSERT(uam::io::WriteTextFile(workspace / ".uam" / "agents" / "reader.md",
	    "---\nversion: 1\nname: reader\ndescription: Reader\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nRead only.\n"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(!app.settings.central_provider_configuration.enabled);
	ChatSession chat = LaunchChat(uam::provider_ids::kCodexCli, "default");
	chat.workspace_directory = uam::paths::Utf8PathString(workspace);
	chat.uam_agent_id = "reader";
	UAM_ASSERT(!chat.uam_control_enabled);
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	UAM_ASSERT(HasRun(uam::BuildProviderInteractiveArgv(app, chat), {"--sandbox", "read-only", "--ask-for-approval", "on-request"}));
	UAM_ASSERT(uam::ProviderInteractiveCeilingError(app, chat).empty());
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	// OpenCode's terminal has no Plan option UAM can enforce, so a Plan or read-only chat is refused.
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	UAM_ASSERT(uam::ProviderInteractiveCeilingError(app, chat).find("OpenCode CLI View") != std::string::npos);
	chat.uam_agent_id.clear();
	UAM_ASSERT(uam::ProviderInteractiveCeilingError(app, chat).empty());
	chat.approval_mode = "plan";
	UAM_ASSERT(!uam::ProviderInteractiveCeilingError(app, chat).empty());
#endif
}

UAM_TEST(ProviderReadCeilingStructuredLaunchNeutralisesProviderConfiguration)
{
	ProviderProfile profile;
	for (const std::string mode : {"plan", "default"})
	{
		const bool plan = mode == "plan";
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		// Claude: no user, project or local settings, only UAM's MCP servers, and no Computer Use pre-allow.
		const Argv claude = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kClaudeCli).BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kClaudeCli, mode));
		UAM_ASSERT_EQ(HasRun(claude, {"--setting-sources", "", "--strict-mcp-config"}), plan);
		UAM_ASSERT_EQ(HasRun(claude, {"--permission-mode", "plan"}), plan);
		UAM_ASSERT_EQ(HasAny(claude, "uam-computer"), !plan && uam::computer_use::AvailableForChat(LaunchChat(uam::provider_ids::kClaudeCli, mode)));
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		// Gemini: the Plan-only admin policy outranks project and user rules; Default keeps settings adminPolicyPaths.
		const Argv gemini = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kGeminiCli).BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kGeminiCli, mode));
		UAM_ASSERT_EQ(HasRun(gemini, {"--admin-policy", uam::paths::Utf8PathString(GeminiPlanCeilingPolicyPath())}), plan);
		UAM_ASSERT_EQ(HasRun(gemini, {"--approval-mode", "plan"}), plan);
		UAM_ASSERT(!plan || !HasAny(gemini, "gemini-policy.toml"));
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		// OpenCode: the plan agent's own permission keys are replaced, and repository config is off under a ceiling.
		const Environment opencode = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kOpenCodeCli).BuildStructuredLaunchEnvironment(profile, LaunchChat(uam::provider_ids::kOpenCodeCli, mode));
		const nlohmann::json permission = nlohmann::json::parse(EnvValue(opencode, "OPENCODE_PERMISSION"));
		UAM_ASSERT_EQ(permission["*"], nlohmann::json("ask"));
		UAM_ASSERT_EQ(permission["uam-computer_computer_action"], nlohmann::json(plan || !uam::computer_use::AvailableForChat(LaunchChat(uam::provider_ids::kOpenCodeCli, mode)) ? "deny" : "allow"));
		UAM_ASSERT_EQ(EnvValue(opencode, "OPENCODE_DISABLE_PROJECT_CONFIG"), std::string(plan ? "1" : "<unset>"));
		if (!plan)
		{
			UAM_ASSERT_EQ(EnvValue(opencode, "OPENCODE_CONFIG_CONTENT"), std::string("<unset>"));
			continue;
		}
		const nlohmann::json inline_config = nlohmann::json::parse(EnvValue(opencode, "OPENCODE_CONFIG_CONTENT"));
		const nlohmann::json& agent_plan = inline_config["agent"]["plan"]["permission"];
		UAM_ASSERT_EQ(agent_plan["bash"], nlohmann::json("ask"));
		UAM_ASSERT_EQ(agent_plan["edit"], nlohmann::json("ask"));
		UAM_ASSERT_EQ(agent_plan["task"], nlohmann::json("deny"));
		UAM_ASSERT_EQ(agent_plan["uam-computer_computer_action"], nlohmann::json("deny"));
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		// Copilot: deny rules beat saved approvals and allow rules; inherited allow-all is switched off.
		const IProviderRuntime& copilot_runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
		const Argv copilot = copilot_runtime.BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kCopilotCli, mode));
		UAM_ASSERT_EQ(HasRun(copilot, {"--deny-tool=shell", "--deny-tool=write", "--deny-tool=memory"}), plan);
		UAM_ASSERT(!plan || !HasAny(copilot, "--allow-tool"));
		UAM_ASSERT_EQ(EnvValue(copilot_runtime.BuildStructuredLaunchEnvironment(profile, LaunchChat(uam::provider_ids::kCopilotCli, mode)), "COPILOT_ALLOW_ALL"), std::string("false"));
#endif
	}
	// Computer Use is never attached to a Plan, read-only or review launch (all mapped to Plan).
	ChatSession plan = LaunchChat(uam::provider_ids::kCodexCli, "plan");
	UAM_ASSERT(!uam::computer_use::AttachableForLaunch(plan));
	UAM_ASSERT(uam::computer_use::AcpMcpServers(plan).empty());
	UAM_ASSERT(nlohmann::json::parse(uam::computer_use::ClaudeMcpConfig(plan))["mcpServers"].empty());
	Argv codex_mcp;
	uam::computer_use::AppendCodexMcpLaunchArguments(codex_mcp, plan);
	UAM_ASSERT_EQ(codex_mcp, (Argv{"--disable", "computer_use"}));
}

UAM_TEST(ClaudeReadCeilingLaunchCarriesOnlyLocalAuthSettings)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-read-ceiling-auth");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "settings.json",
	    R"json({"env":{"ANTHROPIC_BASE_URL":"https://proxy.example"},"apiKeyHelper":"/bin/key","permissions":{"allow":["Bash(npm run *)","Edit"],"deny":["Read(./.env)"]},"hooks":{"PreToolUse":[]},"enabledPlugins":{"x@y":true}})json"));
	ScopedEnvVar config_dir("CLAUDE_CONFIG_DIR", temp.root.string());
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kClaudeCli);
	ProviderProfile profile;
	uam::AcpSessionState session;
	Environment environment;
	std::string error;

	Argv local = runtime.BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kClaudeCli, "plan"));
	UAM_ASSERT(runtime.PrepareStructuredLaunch(session, profile, "", false, local, environment, error));
	const auto settings = std::ranges::find(local, "--settings");
	UAM_ASSERT(settings != local.end() && std::next(settings) != local.end());
	// Allow rules, hooks and plugins stay behind; the user's own deny rules still restrict the read turn.
	UAM_ASSERT_EQ(nlohmann::json::parse(*std::next(settings)),
	    (nlohmann::json{{"apiKeyHelper", "/bin/key"}, {"permissions", {{"deny", {"Read(./.env)"}}}}}));
	UAM_ASSERT(HasRun(local, {"--setting-sources", "", "--settings"}));
	// Settings env reaches the process environment, never the command line or launch diagnostics.
	UAM_ASSERT_EQ(EnvValue(environment, "ANTHROPIC_BASE_URL"), std::string("https://proxy.example"));
	UAM_ASSERT(!HasAny(local, "proxy.example"));
	UAM_ASSERT(uam::acp_detail::JoinAcpArgvForDiagnostics(local).find("/bin/key") == std::string::npos);

	Argv remote = runtime.BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kClaudeCli, "plan"));
	const Argv remote_before = remote;
	UAM_ASSERT(runtime.PrepareStructuredLaunch(session, profile, "", true, remote, environment, error));
	UAM_ASSERT_EQ(remote, remote_before);

	Argv ordinary = runtime.BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kClaudeCli, "default"));
	const Argv ordinary_before = ordinary;
	UAM_ASSERT(runtime.PrepareStructuredLaunch(session, profile, "", false, ordinary, environment, error));
	UAM_ASSERT_EQ(ordinary, ordinary_before);
	UAM_ASSERT_EQ(ClaudeReadCeilingSettingsJson("not json"), std::string("{}"));
#endif
}

UAM_TEST(GeminiReadCeilingLaunchRefusesWithoutItsPlanPolicy)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kGeminiCli);
	ProviderProfile profile;
	Environment environment;
	const fs::path policy = GeminiPlanCeilingPolicyPath();
	const fs::path aside = policy.string() + ".aside";
	const bool installed = fs::exists(policy);
	if (installed) fs::rename(policy, aside);
	const auto prepare = [&](const std::string& mode, bool remote, bool review, Argv& argv, std::string& error)
	{
		uam::AcpSessionState session;
		session.goal_review_turn = review;
		argv = runtime.BuildStructuredLaunchArgv(profile, LaunchChat(uam::provider_ids::kGeminiCli, mode));
		error.clear();
		return runtime.PrepareStructuredLaunch(session, profile, "", remote, argv, environment, error);
	};
	Argv argv;
	std::string error;
	// SSH hosts lack the policy file: Plan and review launches are refused, Default launches drop the flag.
	UAM_ASSERT(!prepare("plan", true, false, argv, error));
	UAM_ASSERT(error.find("SSH") != std::string::npos);
	UAM_ASSERT(!prepare("default", true, true, argv, error));
	UAM_ASSERT(prepare("default", true, false, argv, error));
	UAM_ASSERT(!HasAny(argv, "--admin-policy"));
	// Locally a missing policy file refuses a Plan launch rather than launching without it.
	const bool refused_without_file = !prepare("plan", false, false, argv, error) && error.find("missing") != std::string::npos;
	if (installed) fs::rename(aside, policy);
	else UAM_ASSERT(uam::io::WriteTextFile(policy, "# test\n"));
	UAM_ASSERT(refused_without_file);
	UAM_ASSERT(prepare("plan", false, false, argv, error));
	UAM_ASSERT(HasRun(argv, {"--admin-policy", uam::paths::Utf8PathString(policy)}));
	if (!installed) fs::remove(policy);
#endif
}
