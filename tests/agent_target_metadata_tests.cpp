#include "test_harness.h"
#include "app/agent_definition_service.h"
#include "app/uam_control_service.h"
#include "common/provider/provider_ids.h"
#include "common/platform/platform_services.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/io_utils.h"

#include <cstdlib>

using namespace uam_test;

namespace
{
	struct TargetMetadataFixture
	{
		TempDir temp{"uam-agent-target-metadata"};
		ScopedEnvVar codex{"CODEX_HOME", (temp.root / "codex").string()};
		ScopedEnvVar claude{"CLAUDE_CONFIG_DIR", (temp.root / "claude").string()};
		ScopedEnvVar config{"XDG_CONFIG_HOME", (temp.root / "config").string()};
		uam::AppState app;
		TargetMetadataFixture()
		{
			const fs::path workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root / "agents");
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "agents" / "worker.md",
			    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nDo work.\n"));
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile(), ProviderProfileStore::DefaultClaudeProfile(), ProviderProfileStore::DefaultAntigravityProfile()};
			ChatSession chat;
			chat.id = "root";
			chat.provider_id = "codex";
			chat.model_id = "parent-model";
			chat.workspace_directory = workspace.string();
			chat.uam_control_enabled = true;
			app.chats.push_back(chat);
			uam::UamControlCapability capability;
			capability.id = "target-capability";
			capability.directory = temp.root / "capability";
			fs::create_directories(capability.directory);
			capability.chat_id = chat.id;
			capability.session_chat_id = chat.id;
			capability.provider_id = chat.provider_id;
			capability.expires_at_epoch_ms = 100000;
			app.uam_control_capabilities.push_back(capability);
			std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = chat.id;
			session->provider_id = chat.provider_id;
			session->running = true;
			session->last_error = "Previously stopped by user.";
			session->uam_control_capability_id = capability.id;
			app.acp_sessions.push_back(std::move(session));
			app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
			Ready("codex-cli");
			Ready("claude-cli");
		}
		void Ready(const std::string& provider)
		{
			uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(provider, app.chats.front().execution_host_id)];
			version.checked = true;
			version.supported = true;
			version.installed_version = "1.2.3";
			version.execution_host = app.settings.execution_hosts.front();
		}
		nlohmann::json Targets()
		{
			const nlohmann::json listed = uam::UamControlService::HandleRequestForTests(app, "target-capability",
			    {{"requestId", "list-" + std::to_string(app.uam_control_capabilities.front().seen_request_ids.size())}, {"method", "agent_list"}, {"arguments", nlohmann::json::object()}}, 1000);
			UAM_ASSERT(listed.value("ok", false));
			UAM_ASSERT_EQ(listed["result"].size(), std::size_t{1});
			return listed["result"][0].at("providerTargets");
		}
	};
}

UAM_TEST(AgentListExposesScopedCrossProviderModelsAndTruthfulAuthentication)
{
	TargetMetadataFixture fixture;
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "claude-scoped"}}}), fixture.app.chats.front().workspace_directory));
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "wrong-workspace"}}}), (fixture.temp.root / "other").string()));
	const nlohmann::json targets = fixture.Targets();
	UAM_ASSERT_EQ(targets.size(), std::size_t{2});
	UAM_ASSERT_EQ(targets[0].value("providerId", ""), std::string("codex-cli"));
	UAM_ASSERT_EQ(targets[0].value("defaultModelId", ""), std::string("parent-model"));
	UAM_ASSERT(targets[0].value("eligible", false));
	UAM_ASSERT_EQ(targets[0].value("authentication", ""), std::string("unverified"));
	UAM_ASSERT_EQ(targets[1].value("defaultModelId", "wrong"), std::string{});
	UAM_ASSERT_EQ(targets[1]["models"][0].value("id", ""), std::string("claude-scoped"));
	UAM_ASSERT_EQ(targets[1].value("availability", ""), std::string("installed-compatible"));
}

UAM_TEST(AgentListRejectsStaleHostVersionAndUnreadyOrUpdatingTargetMetadata)
{
	TargetMetadataFixture fixture;
	fixture.app.runtime_cli_versions_by_provider_id["claude-cli"].checked = false;
	UAM_ASSERT(!fixture.Targets()[1].value("eligible", true));
	fixture.Ready("claude-cli");
	fixture.app.runtime_cli_install_tasks["claude-cli"].running = true;
	UAM_ASSERT(!fixture.Targets()[1].value("eligible", true));
	fixture.app.runtime_cli_install_tasks.clear();
	fixture.app.chats.front().execution_host_id = "remote";
	fixture.app.settings.execution_hosts.front().id = "remote";
	fixture.app.settings.execution_hosts.front().transport = "ssh";
	fixture.app.settings.execution_hosts.front().platform = "linux";
	fixture.app.settings.execution_hosts.front().runner_status = "ready";
	// Remote metadata needs a Linux path, rather than the controller's temporary workspace.
	fixture.app.chats.front().workspace_directory = "/workspace/agent-target-metadata";
	fixture.Ready("codex-cli");
	fixture.Ready("claude-cli");
	fixture.app.settings.execution_hosts.front().runner_directory = "new-runner";
	const nlohmann::json stale = fixture.Targets();
	UAM_ASSERT(!stale[0].value("eligible", true));
	UAM_ASSERT(stale[0].value("unavailableReason", "").find("different host") != std::string::npos);
	fixture.Ready("codex-cli");
	fixture.app.settings.execution_hosts.front().runner_status = "disconnected";
	UAM_ASSERT(!fixture.Targets()[0].value("eligible", true));
}

UAM_TEST(OpenCodeAgentImportDefaultsOmittedModeToBoth)
{
	TempDir temp("uam-opencode-agent-default-mode");
	const fs::path source_root = temp.root / "native";
	fs::create_directories(source_root);
	const fs::path source = source_root / "reviewer.md";
	UAM_ASSERT(uam::io::WriteTextFile(source,
	    "---\ndescription: Reviews code\n---\nReview the code and report findings.\n"));

	const uam::ProviderAgentImportPreview preview =
	    uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.mode, std::string("both"));

	uam::ProviderAgentImportRequest request;
	request.provider_id = uam::provider_ids::kOpenCodeCli;
	request.source_path = source;
	request.workspace_access = "read";
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(
	    temp.root / "data", {}, request, &imported, &error));
	UAM_ASSERT_EQ(imported.mode, std::string("both"));

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root / "data", {});
	const auto loaded = std::ranges::find(catalog.definitions, std::string("reviewer"), &uam::AgentDefinition::id);
	UAM_ASSERT(loaded != catalog.definitions.end());
	UAM_ASSERT_EQ(loaded->mode, std::string("both"));

	const fs::path empty_mode_source = source_root / "empty-mode.md";
	UAM_ASSERT(uam::io::WriteTextFile(empty_mode_source,
	    "---\ndescription: Invalid mode\nmode:\n---\nReview.\n"));
	const uam::ProviderAgentImportPreview empty_mode_preview =
	    uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, empty_mode_source);
	UAM_ASSERT(!empty_mode_preview.supported);
}

UAM_TEST(ProviderAgentImportRejectsDuplicateSkillsWithoutCreatingInvalidFile)
{
	TempDir temp("uam-agent-import-duplicate-skills");
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Reviews code\n---\nReview.\n"));
	uam::ProviderAgentImportRequest request;
	request.provider_id = "opencode";
	request.source_path = source;
	request.workspace_access = "read";
	request.skills = {"review", "review"};
	std::string error;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(!fs::exists(temp.root / "data" / "agents" / "agent.md"));
}

UAM_TEST(ProviderAgentImportFailedReplacementPreservesPreviousBytes)
{
	TempDir temp("uam-agent-import-preserve-replacement");
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Reviews code\n---\nReview.\n"));
	uam::ProviderAgentImportRequest request;
	request.provider_id = "opencode";
	request.source_path = source;
	request.workspace_access = "read";
	request.skills = {"review"};
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	const fs::path target = temp.root / "data" / "agents" / "agent.md";
	const std::string previous = uam::io::ReadTextFile(target);
	request.replace_existing = true;
	request.skills.push_back("review");
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), previous);
}

UAM_TEST(ProviderAgentImportExpandsDirectHomeMarkdownPath)
{
	TempDir temp("uam-agent-import-home-path");
	const std::optional<fs::path> home = PlatformServicesFactory::Instance().path_service.ResolveUserHomePath();
	UAM_ASSERT(home.has_value());
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Reviews code\n---\nReview.\n"));
	const fs::path home_relative_source = fs::path("~") / source.lexically_relative(*home);
	const uam::ProviderAgentImportPreview preview = uam::AgentDefinitionService::PreviewProviderAgentImport("open-code", home_relative_source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.source_path, source);
	UAM_ASSERT_EQ(preview.suggested_id, std::string("agent"));
	uam::ProviderAgentImportRequest request;
	request.provider_id = "open-code";
	request.source_path = home_relative_source;
	request.workspace_access = "read";
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, &imported, &error));
	UAM_ASSERT_EQ(imported.id, std::string("agent"));
}

UAM_TEST(ProviderAgentAliasesImportGloballyWithSelectedSkillsAndExplicitAccess)
{
	TempDir temp("uam-agent-import-aliases");
	const fs::path source = temp.root / "agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(source,
	    "---\nname: agent\ndescription: Reviews code\n---\nReview.\n"));
	for (const std::string& provider : {"opencode", "github-copilot", "gemini", "claude-code"})
	{
		const uam::ProviderAgentImportPreview preview = uam::AgentDefinitionService::PreviewProviderAgentImport(provider, source);
		UAM_ASSERT(preview.supported);
		UAM_ASSERT_EQ(preview.provider_id, uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider));
		uam::ProviderAgentImportRequest request;
		request.provider_id = provider;
		request.source_path = source;
		request.workspace_access = "read";
		request.skills = {"review", "docs"};
		request.acknowledge_ignored_fields = !preview.ignored_fields.empty();
		uam::AgentDefinition imported;
		std::string error;
		UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / provider, {}, request, &imported, &error));
		UAM_ASSERT_EQ(imported.skills, request.skills);
		UAM_ASSERT_EQ(imported.workspace_access, std::string("read"));
		UAM_ASSERT_EQ(imported.source_path, temp.root / provider / "agents" / "agent.md");
	}
}

namespace
{
	std::string BoundAgentMarkdown(const std::string& policy = R"({"edit":"deny","bash":"ask"})")
	{
		return "---\nversion: 1\nname: reviewer\ndescription: Review\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\nopenCodePermission: " + policy + "\n---\nReview code.\n";
	}
}

UAM_TEST(OpenCodePermissionImportPreservesSourceRulesAndRequiresExplicitReadAccess)
{
	TempDir temp("uam-opencode-policy-import");
	const fs::path source = temp.root / "reviewer.md";
	const std::string original = "---\nname: read-only\ndescription: Reviews code\nmode: primary\npermission:\n  bash: ask\n  edit: deny\n---\nReview code.\n";
	UAM_ASSERT(uam::io::WriteTextFile(source, original));
	const uam::ProviderAgentImportPreview preview = uam::AgentDefinitionService::PreviewProviderAgentImport("opencode", source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(preview.security_fields.empty());
	UAM_ASSERT_EQ(preview.ignored_fields, std::vector<std::string>{"name"});
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"bash":"ask","edit":"deny"})"));
	uam::ProviderAgentImportRequest request;
	request.provider_id = "opencode";
	request.source_path = source;
	request.workspace_access = "read";
	std::string error;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	request.acknowledge_ignored_fields = true;
	request.workspace_access = "write";
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(!fs::exists(temp.root / "data" / "agents" / "reviewer.md"));
	request.workspace_access = "read";
	uam::AgentDefinition imported;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, &imported, &error));
	UAM_ASSERT_EQ(imported.open_code_permission_json, preview.open_code_permission_json);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(source), original);
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root / "data", {});
	const auto loaded = std::ranges::find(catalog.definitions, std::string("reviewer"), &uam::AgentDefinition::id);
	UAM_ASSERT(loaded != catalog.definitions.end());
	UAM_ASSERT_EQ(loaded->open_code_permission_json, imported.open_code_permission_json);
	// Re-preview the source, rather than trusting a previously displayed supported preview.
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Review\npermission:\n  edit: allow\n---\nReview.\n"));
	request.replace_existing = true;
	const std::string previous = uam::io::ReadTextFile(imported.source_path);
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(imported.source_path), previous);
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Review\n---\nReview.\n"));
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(imported.source_path), previous);
}

UAM_TEST(OpenCodePermissionImportRejectsUnsupportedYamlAndOtherSecurityFields)
{
	TempDir temp("uam-opencode-policy-rejections");
	const fs::path source = temp.root / "reviewer.md";
	const std::vector<std::string> rules{
	    "permission:\n  edit: allow", "permission:\n  bash: deny", "permission: deny",
	    "permission:\n  edit: deny\n  edit: allow", "permission:\n  edit:\n    '*': deny",
	    "permission:\n  edit: deny\n  bash:\n    '*': ask", "permission:\n  edit: deny\n  bash: allow",
	    "permission: &policy\n  edit: deny", "permission: *policy", "permission:\n  <<: *policy\n  edit: deny",
	    "permission:\n  edit: deny\n  task: deny", "permission:\n\tedit: deny", "permission:\n    edit: deny",
	    "permission:\n  edit: deny # unsupported comments", "permission:\n  edit: deny\ntools: [read]",
	    "  permission:\n    edit: deny", "'permission': deny",
	    R"("\u0070ermission": { edit: deny })", R"("\u0074ools": [read])",
	    R"("permission\u002dmode": plan)"};
	for (const std::string& rule : rules)
	{
		UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Review\n" + rule + "\n---\nReview.\n"));
		const uam::ProviderAgentImportPreview blocked = uam::AgentDefinitionService::PreviewProviderAgentImport("opencode", source);
		UAM_ASSERT(!blocked.supported);
		if (rule.starts_with("permission:"))
		{
			UAM_ASSERT(!blocked.security_fields.empty());
			UAM_ASSERT(blocked.error.find("security/tool fields") != std::string::npos);
		}
	}
	UAM_ASSERT(uam::io::WriteTextFile(source, "---\ndescription: Review\npermission:\n  edit: deny\n---\nReview.\n"));
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport("opencode", source).supported);
	for (const std::string& provider : {"claude", "copilot", "gemini"})
		UAM_ASSERT(!uam::AgentDefinitionService::PreviewProviderAgentImport(provider, source).supported);
}

UAM_TEST(CapturedOpenCodePermissionFailsClosedForMalformedSnapshotsAndIncompatibleRoutes)
{
	std::string permission, error;
	const std::string valid = BoundAgentMarkdown();
	UAM_ASSERT(uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", valid, permission, &error));
	UAM_ASSERT_EQ(permission, std::string(R"({"edit":"deny","bash":"ask"})"));
	UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("reviewer", valid, "open-code", "opencode-native-agent-config", &error));
	for (const std::string& capability : {"uam-prompt-injected", "uam-central-native-setup", "terminal", "copilot-native-agent-plugin"})
		UAM_ASSERT(!uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("reviewer", valid, "opencode", capability, &error));
	UAM_ASSERT(!uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("reviewer", valid, "codex", "opencode-native-agent-config", &error));
	UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("wrong-id", valid, permission, &error));
	UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("../reviewer", valid, permission, &error));
	UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", "openCodePermission: {}", permission, &error));
	for (const std::string& invalid : {R"({"edit":"deny","edit":"allow"})", R"({"edit":"allow"})", R"({"edit":{"*":"deny"}})", "null", "[]", "broken"})
		UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", BoundAgentMarkdown(invalid), permission, &error));
	std::string duplicate = valid;
	duplicate.insert(duplicate.find("openCodePermission:"), "openCodePermission: {}\n");
	UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", duplicate, permission, &error));
	std::string writable = valid;
	writable.replace(writable.find("workspaceAccess: read"), std::string("workspaceAccess: read").size(), "workspaceAccess: write");
	UAM_ASSERT(!uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", writable, permission, &error));
	UAM_ASSERT(uam::AgentDefinitionService::CapturedOpenCodePermission("build", "builtin:build:v1", permission, &error));
	UAM_ASSERT(permission.empty());
}

UAM_TEST(EditingImportedOpenCodeAgentPreservesPolicyAndRejectsWidening)
{
	TempDir temp("uam-opencode-policy-edit");
	const fs::path root = temp.root / "agents";
	fs::create_directories(root);
	const fs::path target = root / "reviewer.md";
	UAM_ASSERT(uam::io::WriteTextFile(target, BoundAgentMarkdown()));
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root, {});
	const auto loaded = std::ranges::find(catalog.definitions, std::string("reviewer"), &uam::AgentDefinition::id);
	UAM_ASSERT(loaded != catalog.definitions.end());
	uam::AgentDefinition edited = *loaded;
	edited.open_code_permission_json.clear(); // Older editor DTO has no policy property.
	edited.description = "Updated review";
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(temp.root, edited, &error));
	std::string retained;
	UAM_ASSERT(uam::AgentDefinitionService::CapturedOpenCodePermission("reviewer", uam::io::ReadTextFile(target), retained, &error));
	UAM_ASSERT_EQ(retained, loaded->open_code_permission_json);
	const std::string previous = uam::io::ReadTextFile(target);
	edited.workspace_access = "write";
	UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(temp.root, edited, &error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), previous);
	edited.workspace_access = "read";
	edited.open_code_permission_json = R"({"edit":"deny","bash":"deny"})";
	UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(temp.root, edited, &error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), previous);
}

UAM_TEST(OpenCodeImportedPolicyAdapterUsesCapturedRulesWithoutBroaderWildcardOverrides)
{
	TempDir temp("uam-opencode-policy-adapter");
	const std::string snapshot = BoundAgentMarkdown(R"({"bash":"deny","edit":"deny"})");
	uam::ProviderAgentRuntimeAdapter adapter;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::PrepareRuntimeAdapter(temp.root, "chat", "opencode", "reviewer", "Captured instructions.", &adapter, &error, &snapshot));
	const nlohmann::ordered_json config = nlohmann::ordered_json::parse(uam::io::ReadTextFile(adapter.directory / "opencode.json"));
	const nlohmann::ordered_json& permission = config.at("permission");
	UAM_ASSERT_EQ(permission.begin().key(), std::string("bash"));
	UAM_ASSERT_EQ(permission.at("edit").get<std::string>(), std::string("deny"));
	UAM_ASSERT_EQ(permission.at("bash").get<std::string>(), std::string("deny"));
	UAM_ASSERT_EQ(permission.at("task").get<std::string>(), std::string("deny"));
	UAM_ASSERT_EQ(permission.at("uam-computer_computer_action").get<std::string>(), std::string("deny"));
	UAM_ASSERT(!permission.contains("*"));
	const std::string agent = uam::io::ReadTextFile(adapter.directory / "agent" / (config.at("default_agent").get<std::string>() + ".md"));
	UAM_ASSERT(agent.find("permission: " + permission.dump()) != std::string::npos);
	UAM_ASSERT(!adapter.inject_in_prompt);
	UAM_ASSERT(!uam::AgentDefinitionService::PrepareRuntimeAdapter(temp.root, "other", "copilot", "reviewer", "Instructions.", &adapter, &error, &snapshot));
	UAM_ASSERT(!fs::exists(temp.root / "runtime" / "agent-adapters" / "other"));
}

UAM_TEST(OpenCodeBoundPolicyRejectsInheritedInlineAgentOverridesWithoutChangingConfiguration)
{
	const std::string snapshot = BoundAgentMarkdown();
	std::string error;
	const std::vector<std::string> blocked{
	    R"({"agent":{"uam-fixture":{"permission":{"edit":"allow"}}}})",
	    R"({"default_agent":"build"})", R"({"mode":{"review":{"permission":{"edit":"allow"}}}})",
	    R"({"\u0061gent":{}})", "broken", "[]", "null", std::string(16 * 1024 + 1, ' ')};
	for (const std::string& config : blocked)
	{
		ScopedEnvVar inline_config{"OPENCODE_CONFIG_CONTENT", config};
		UAM_ASSERT(!uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("reviewer", snapshot, "opencode", "opencode-native-agent-config", &error));
		UAM_ASSERT(error.find("OPENCODE_CONFIG_CONTENT") != std::string::npos);
		UAM_ASSERT_EQ(std::string(std::getenv("OPENCODE_CONFIG_CONTENT")), config);
		// Unbound agents retain their existing inline configuration behavior.
		UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("build", "builtin:build:v1", "opencode", "opencode-native-agent-config", &error));
	}
	for (const std::string& config : {"", "{}", R"({"permission":{"edit":"allow","bash":"allow"}})",
	                                 R"({"mcp":{"fixture":{"type":"local","command":["fixture"]}}})"})
	{
		ScopedEnvVar inline_config{"OPENCODE_CONFIG_CONTENT", config};
		UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("reviewer", snapshot, "opencode", "opencode-native-agent-config", &error));
		const char* retained = std::getenv("OPENCODE_CONFIG_CONTENT");
		UAM_ASSERT_EQ(retained == nullptr ? std::string{} : std::string(retained), config);
	}
}
