#include "test_harness.h"
#include "app/agent_definition_service.h"
#include "app/uam_control_service.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/io_utils.h"

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
