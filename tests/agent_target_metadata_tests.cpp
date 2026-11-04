#include "test_harness.h"
#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/uam_control_service.h"
#include "common/chat/acp_prompt_recovery.h"
#include "common/chat/chat_message_page_index.h"
#include "common/runtime/acp/acp_session_runtime.h"
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
			const auto worker = std::ranges::find_if(listed["result"], [](const nlohmann::json& agent) { return agent.value("id", "") == "worker"; });
			UAM_ASSERT(worker != listed["result"].end());
			return worker->at("providerTargets");
		}
	};
}

UAM_TEST(BuiltinAgentsListAndDelegateWithoutCustomDefinitionsAcrossProviders)
{
	for (const ProviderProfile& provider : ProviderProfileStore::BuiltInProfiles())
	{
		TargetMetadataFixture fixture;
		uam::AppState& app = fixture.app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.chats.front().command_safety_tier = "yolo";
		fs::remove(app.data_root / "agents" / "worker.md");
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "agents" / "primary-only.md",
		    "---\nversion: 1\nname: primary-only\ndescription: Chat only\nmode: primary\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nChat only.\n"));
		const auto listed = uam::UamControlService::HandleRequestForTests(app, "target-capability",
		    {{"requestId", "defaults"}, {"method", "agent_list"}, {"arguments", nlohmann::json::object()}}, 1000);
		UAM_ASSERT(listed.value("ok", false));
		UAM_ASSERT_EQ(listed["result"].size(), std::size_t{2});
		for (const std::string& id : {"build", "plan"})
		{
			const auto found = std::ranges::find_if(listed["result"], [&](const nlohmann::json& agent) { return agent.value("id", "") == id; });
			UAM_ASSERT(found != listed["result"].end());
			UAM_ASSERT_EQ(found->at("workspaceAccess").get<std::string>(), std::string(id == "plan" ? "read" : "write"));
			const auto delegated = uam::UamControlService::HandleRequestForTests(app, "target-capability",
			    {{"requestId", "delegate-" + id}, {"method", "agent_delegate"},
			     {"arguments", {{"agentId", id}, {"providerId", provider.id}, {"modelId", "requested-model"}, {"task", "Do the assigned task."}}}}, 1001);
			UAM_ASSERT(delegated.value("ok", false));
			const AgentRun& run = app.agent_runs.back();
			UAM_ASSERT_EQ(run.id, delegated["result"].at("runId").get<std::string>());
			UAM_ASSERT_EQ(run.agent_id, id);
			UAM_ASSERT_EQ(run.provider_id, provider.id);
			UAM_ASSERT_EQ(run.model_id, std::string("requested-model"));
			UAM_ASSERT_EQ(run.effective_workspace_access, std::string(id == "plan" ? "read" : "write"));
			UAM_ASSERT_EQ(app.chats.back().execution_host_id, app.chats.front().execution_host_id);
			UAM_ASSERT_EQ(app.chats.back().workspace_directory, app.chats.front().workspace_directory);
			if (id == "plan") UAM_ASSERT_EQ(app.chats.back().approval_mode, std::string("plan"));
		}
		std::string error;
		UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(app, "root", "", "primary-only", "Do work.", nullptr, &error));
		UAM_ASSERT(error.find("unavailable for delegation") != std::string::npos);
	}
}

UAM_TEST(BuiltinBuildDelegationRetainsReadOnlyCeilingsAndRemoteHost)
{
	for (const bool active_read_only : {false, true})
	{
		TargetMetadataFixture fixture;
		uam::AppState& app = fixture.app;
		app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());
		app.chats.front().execution_host_id = "ssh-windows";
		if (active_read_only) app.acp_sessions.front()->active_uam_agent_workspace_access = "read";
		else app.chats.front().approval_mode = "plan";
		std::string run_id, error;
		UAM_ASSERT(uam::AgentRunScheduler::Enqueue(app, "root", "", "build", "Implement the task.", &run_id, &error,
		    "opencode-cli", "opencode/glm-5.3-flash-free"));
		UAM_ASSERT_EQ(app.agent_runs.back().effective_workspace_access, std::string("read"));
		UAM_ASSERT_EQ(app.chats.back().approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(app.chats.back().execution_host_id, std::string("ssh-windows"));
	}
}

UAM_TEST(BuiltinCapturedDefinitionsRoundTripWithoutPromotingLegacySnapshots)
{
	TempDir temp("uam-builtin-capture-versions");
	fs::create_directories(temp.root / "agents");
	for (const std::string& id : {"build", "plan"})
	{
		uam::SelectedPrimaryAgentSnapshot selected;
		std::string error;
		UAM_ASSERT(uam::AgentDefinitionService::CaptureSelectedPrimary(temp.root, {}, id, selected, error));
		UAM_ASSERT_EQ(selected.definition.mode, std::string("both"));
		UAM_ASSERT_EQ(selected.definition.definition_hash, "builtin-" + id + "-v2");
		UAM_ASSERT_EQ(selected.definition.markdown_snapshot, "builtin:" + id + ":v2");
		for (const std::string& version : {"v1", "v2"})
		{
			uam::AgentDefinition decoded;
			UAM_ASSERT(uam::AgentDefinitionService::DecodeCapturedPrimary(id, "builtin:" + id + ":" + version, decoded, &error));
			UAM_ASSERT_EQ(decoded.id, id);
			UAM_ASSERT_EQ(decoded.definition_hash, "builtin-" + id + "-" + version);
			UAM_ASSERT_EQ(decoded.markdown_snapshot, "builtin:" + id + ":" + version);
			UAM_ASSERT_EQ(decoded.mode, std::string(version == "v1" ? "primary" : "both"));
			UAM_ASSERT_EQ(decoded.instructions, selected.definition.instructions);
			UAM_ASSERT_EQ(decoded.workspace_access, std::string(id == "plan" ? "read" : "write"));
			UAM_ASSERT(decoded.built_in && decoded.skills.empty() && decoded.delegates.empty());
		}
		for (const std::string& snapshot : {"builtin:" + id + ":v0", "builtin:" + id + ":v3",
		         "builtin:" + id + ":v01", "builtin:" + id + ":v2\n",
		         std::string("builtin:") + (id == "build" ? "plan" : "build") + ":v1",
		         std::string("builtin:") + (id == "build" ? "plan" : "build") + ":v2"})
		{
			uam::AgentDefinition decoded;
			UAM_ASSERT(!uam::AgentDefinitionService::DecodeCapturedPrimary(id, snapshot, decoded, &error));
			UAM_ASSERT(!error.empty());
		}
	}
}

UAM_TEST(BuiltinCapturedRecoveryRejectsForeignSnapshotsAndHashes)
{
	for (const std::string& id : {"build", "plan"})
		for (const std::string& version : {"v1", "v2"})
			for (const std::string& mutation : {"", "foreign-snapshot", "foreign-hash"})
			{
				TempDir temp("uam-builtin-recovery-version");
				uam::AppState app;
				app.data_root = temp.root;
				app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
				ChatSession chat = ChatDomainService().CreateNewChat({}, "codex-cli");
				chat.workspace_directory = temp.root.string();
				ChatDomainService().AddMessage(chat, MessageRole::User, "Keep this unsent input.");
				chat.messages.back().acp_prompt_not_sent = true;
				chat.messages.back().interrupted = true;
				uam::AgentDefinition definition;
				std::string error;
				UAM_ASSERT(uam::AgentDefinitionService::DecodeCapturedPrimary(id, "builtin:" + id + ":" + version, definition, &error));
				uam::AcpQueuedUserPromptState prompt;
				prompt.id = "captured-builtin-input";
				prompt.revision = 1;
				prompt.uam_agent_id = id;
				prompt.uam_agent_definition_hash = definition.definition_hash;
				prompt.uam_agent_definition_snapshot = definition.markdown_snapshot;
				prompt.uam_agent_instructions = definition.instructions;
				prompt.uam_agent_skills = definition.skills;
				prompt.uam_agent_delegates = definition.delegates;
				prompt.uam_agent_workspace_access = definition.workspace_access;
				prompt.uam_agent_execution_capability = "uam-prompt-injected";
				if (mutation == "foreign-snapshot") prompt.uam_agent_definition_snapshot = std::string("builtin:") + (id == "build" ? "plan" : "build") + ":" + version;
				if (mutation == "foreign-hash") prompt.uam_agent_definition_hash = "builtin-" + id + (version == "v1" ? "-v2" : "-v1");
				chat.acp_unsent_prompt_recoveries.push_back(uam::prompt_recovery::Capture(chat, 0, prompt));
				UAM_ASSERT(uam::prompt_recovery::BindingsMatch(chat));
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
				const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
				UAM_ASSERT(saved && saved->acp_unsent_prompt_recoveries == chat.acp_unsent_prompt_recoveries);
				app.chats.push_back(*saved);
				UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(app, app.chats.front()), std::vector<int>({0}));
				std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;
				for (const Message& row : app.chats.front().messages) uam::chat_pages::AppendMessageDigest(hash, row);
				const std::string digest = uam::chat_pages::FinishMessageDigest(hash, app.chats.front().messages.size());
				const bool edited = uam::EditUnsentAcpMessage(app, chat.id, 0, digest, "Edited explicit unsent input.", &error);
				UAM_ASSERT_EQ(edited, mutation.empty());
				UAM_ASSERT_EQ(error.empty(), edited);
				UAM_ASSERT(app.acp_sessions.empty());
				const auto durable = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
				UAM_ASSERT(durable && durable->created_at == saved->created_at);
				if (!edited)
				{
					UAM_ASSERT_EQ(app.chats.front().messages, saved->messages);
					UAM_ASSERT_EQ(durable->messages, saved->messages);
					UAM_ASSERT_EQ(durable->acp_unsent_prompt_recoveries, saved->acp_unsent_prompt_recoveries);
				}
				else
				{
					UAM_ASSERT_EQ(durable->messages.front().content, std::string("Edited explicit unsent input."));
					UAM_ASSERT(durable->messages.front().acp_prompt_not_sent && !durable->messages.front().acp_prompt_discarded);
					const auto& retained = durable->acp_unsent_prompt_recoveries.front();
					UAM_ASSERT_EQ(retained.uam_agent_definition_hash, definition.definition_hash);
					UAM_ASSERT_EQ(retained.uam_agent_definition_snapshot, definition.markdown_snapshot);
					UAM_ASSERT_EQ(retained.uam_agent_instructions, definition.instructions);
					UAM_ASSERT_EQ(retained.uam_agent_workspace_access, definition.workspace_access);
					UAM_ASSERT(uam::prompt_recovery::BindingsMatch(*durable));
				}
			}
}

UAM_TEST(AgentListExposesScopedCrossProviderModelsAndTruthfulAuthentication)
{
	TargetMetadataFixture fixture;
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "claude-scoped"}}}), fixture.app.chats.front().workspace_directory));
	UAM_ASSERT(fixture.app.provider_model_catalog->RememberSuccessfulModels("claude-cli", nlohmann::json::array({{{"id", "wrong-workspace"}}}), (fixture.temp.root / "other").string()));
	const nlohmann::json targets = fixture.Targets();
	UAM_ASSERT_EQ(targets.size(), std::size_t{3});
	UAM_ASSERT_EQ(targets[0].value("providerId", ""), std::string("codex-cli"));
	UAM_ASSERT_EQ(targets[0].value("defaultModelId", ""), std::string("parent-model"));
	UAM_ASSERT(targets[0].value("eligible", false));
	UAM_ASSERT_EQ(targets[0].value("authentication", ""), std::string("unverified"));
	UAM_ASSERT_EQ(targets[1].value("defaultModelId", "wrong"), std::string{});
	UAM_ASSERT_EQ(targets[1]["models"][0].value("id", ""), std::string("claude-scoped"));
	UAM_ASSERT_EQ(targets[1].value("availability", ""), std::string("installed-compatible"));
}

UAM_TEST(AgentListIncludesManagedAntigravityWithoutClaimingToolFreeTextWorkers)
{
	TargetMetadataFixture fixture;
	fixture.Ready("antigravity-cli");
	const auto targets = fixture.Targets();
	const auto target = std::ranges::find_if(targets, [](const nlohmann::json& value) { return value.value("providerId", "") == "antigravity-cli"; });
	UAM_ASSERT(target != targets.end());
	UAM_ASSERT(target->value("eligible", false));
	UAM_ASSERT(!ProviderRuntimeRegistry::ResolveById("antigravity-cli").SupportsTextWorkers());
}

UAM_TEST(ManagedAntigravityDelegationPreservesParentPlanAndPermissionCeilings)
{
	TargetMetadataFixture fixture;
	fixture.app.chats.front().approval_mode = "plan";
	fixture.app.chats.front().command_safety_tier = "aiReview";
	std::string run_id, error;
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(fixture.app, "root", {}, "worker", "Inspect only.",
	    &run_id, &error, "antigravity-cli"));
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	const auto& run = fixture.app.agent_runs.front();
	const auto child = std::ranges::find(fixture.app.chats, run.transcript_chat_id, &ChatSession::id);
	UAM_ASSERT(child != fixture.app.chats.end());
	UAM_ASSERT_EQ(child->provider_id, std::string("antigravity-cli"));
	UAM_ASSERT_EQ(child->approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(child->command_safety_tier, std::string("aiReview"));
	UAM_ASSERT_EQ(run.effective_workspace_access, std::string("read"));
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

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
#include "app/opencode_native_activity_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "remote/runner_proxy.h"
#include "remote/runner_protocol.h"
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include "common/platform/observed_process_stop.h"
#include <cerrno>

namespace
{
	struct ManagedOpenCodePrivateRunnerService
	{
		uam::platform::StdioProcessPlatformFields process;
		pid_t pid = -1;
		bool started = false;
		void Start(const fs::path& runner, const fs::path& socket_path, const fs::path& root)
		{
			UAM_ASSERT(!started && !fs::exists(socket_path));
			const std::string endpoint = socket_path.string();
			sockaddr_un address{};
			UAM_ASSERT(socket_path.is_absolute() && endpoint.size() < sizeof(address.sun_path));
			address.sun_family = AF_UNIX;
			std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
			    process, root, {runner.string(), "serve", "--socket", endpoint}, &error));
			started = true; pid = process.child_pid;
			UAM_ASSERT(pid > 0);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			bool reachable = false;
			while (!reachable && std::chrono::steady_clock::now() < deadline)
			{
				const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
				UAM_ASSERT(probe >= 0);
				if (fcntl(probe, F_SETFL, O_NONBLOCK) == 0)
					reachable = connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
				close(probe);
				if (!reachable)
				{
					int exit = -1;
					if (PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(process, &exit)) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}
			UAM_ASSERT(reachable);
		}
		void Stop() noexcept
		{
			if (!started) return;
			try
			{
				auto& service = PlatformServicesFactory::Instance().process_service;
				// Runtime, native futures, bridges and commands are drained before this owner is terminated.
				const auto result = uam::platform::StopStdioProcessObserved(service, process, std::chrono::milliseconds(0));
				if (!result.exit_confirmed) { std::cerr << "FATAL_PRIVATE_MANAGED_RUNNER_SERVICE_NOT_STOPPED" << std::endl; std::abort(); }
				service.CloseStdioProcessHandles(process);
				siginfo_t info{}; errno = 0;
				if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != -1 || errno != ECHILD)
				{ std::cerr << "FATAL_PRIVATE_MANAGED_RUNNER_SERVICE_NOT_REAPED" << std::endl; std::abort(); }
				started = false;
			}
			catch (...) { std::cerr << "FATAL_PRIVATE_MANAGED_RUNNER_SERVICE_CLEANUP" << std::endl; std::abort(); }
		}
		~ManagedOpenCodePrivateRunnerService() noexcept { Stop(); }
	};
	// Actual private Mac helper/stdio/HTTP peers. No installed provider or SSH target is invoked.
	struct ManagedOpenCodeSshFixture
	{
		TempDir temp{"uam-mos"};
		std::vector<std::unique_ptr<ScopedEnvVar>> environment;
		ManagedOpenCodePrivateRunnerService runner_service; // Destroyed before environment/TempDir even on constructor failure.
		uam::AppState app;
		std::string run_id, transcript_id;
		std::vector<pid_t> proxy_pids;
		nlohmann::json states = nlohmann::json::array();
		bool stopped = false;
		const std::string native = "ses_private_managed_ssh_first_native";
		const std::string model = "private/managed-selected";
		const std::string task = "Private managed SSH diagnostic task.";

		ManagedOpenCodeSshFixture(bool duplicate)
		{
			using Json = nlohmann::json;
			const auto bin = temp.root / "bin", home = temp.root / "home";
			const auto workspace = fs::weakly_canonical(temp.root) / "workspace";
			fs::create_directories(bin); fs::create_directories(home / "tmp"); fs::create_directories(workspace);
			const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
			UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
			const auto manifest = Json::parse(ReadFile(fs::path(runner.string() + ".manifest.json")));
			const auto version = uam::strings::Trim(ReadFile(fs::path(runner.string() + ".version")));
			UAM_ASSERT_EQ(version, std::string(uam::constants::kAppVersion).substr(1));
			UAM_ASSERT_EQ(manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
			// Root's current declared dependency, not a remote folder-name inference.
			const auto fingerprint = manifest.at("sourceFingerprint").get<std::string>();
			UAM_ASSERT(fingerprint.size() == 64 && fingerprint.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos);
			const auto generated_header = runner.parent_path() / "generated" / "uam_runner_build.h";
			UAM_ASSERT(fs::is_regular_file(generated_header));
			UAM_ASSERT(("\n" + ReadFile(generated_header) + "\n").find("\n#define UAM_RUNNER_SOURCE_FINGERPRINT \"" + fingerprint + "\"\n") != std::string::npos);
			UAM_ASSERT(ReadFile(runner).find(manifest.at("sourceFingerprint").get<std::string>()) != std::string::npos);
			const auto ssh = bin / "ssh", shell = bin / "sh", peer = bin / "opencode";
			UAM_ASSERT(uam::io::WriteTextFile(shell, "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
			UAM_ASSERT(uam::io::WriteTextFile(ssh, R"SSH_PEER(#!/usr/bin/python3 -I -S
import json, os, pathlib, struct, subprocess, sys, time
root = pathlib.Path(os.environ['UAM_TEST_MANAGED_SSH_ROOT'])
assert 'private-managed-opencode-host' in sys.argv[1:], sys.argv
# Only the actual locally packaged helper executes; all requested target shell text is inert data.
child = subprocess.Popen([os.environ['UAM_TEST_MANAGED_RUNNER'], 'bridge', '--socket', os.environ['UAM_TEST_MANAGED_SOCKET']], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
audit = root / ('runner-%d.jsonl' % os.getpid())
count = 0
def frame(stream):
    header = stream.read(4)
    if not header: return None
    assert len(header) == 4
    size = struct.unpack('>I', header)[0]
    assert 0 < size <= 1048576
    body = stream.read(size)
    assert len(body) == size
    return header + body, json.loads(body)
def record(direction, obj):
    global count
    count += 1
    assert count <= 8192 and (not audit.exists() or audit.stat().st_size < 16777216)
    with audit.open('a') as out:
        out.write(json.dumps(dict(direction=direction, at=time.clock_gettime_ns(time.CLOCK_MONOTONIC), frame=obj), separators=(',', ':')) + '\n')
        out.flush()
try:
    while True:
        incoming = frame(sys.stdin.buffer)
        if incoming is None: break
        wire, request = incoming
        record('request', request)
        child.stdin.write(wire); child.stdin.flush()
        answer = frame(child.stdout)
        if answer is None: break
        wire, response = answer
        record('response', dict(requestType=request.get('type'), request=request, response=response))
        sys.stdout.buffer.write(wire); sys.stdout.buffer.flush()
finally:
    child.stdin.close()
    try: child.wait(timeout=2)
    except subprocess.TimeoutExpired:
        child.terminate()
        try: child.wait(timeout=2)
        except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=2)
)SSH_PEER"));
			UAM_ASSERT(uam::io::WriteTextFile(peer, R"OPENCODE_PEER(#!/usr/bin/python3 -I -S
import base64, http.server, json, os, pathlib, signal, sys, threading, time, urllib.parse
root = pathlib.Path(os.environ['UAM_TEST_MANAGED_SSH_ROOT'])
config = json.loads((root / 'case.json').read_text())
lock = threading.Lock()
def store(name, value):
    path = root / name
    temporary = root / (name + '.tmp')
    with temporary.open('w') as out:
        json.dump(value, out, separators=(',', ':')); out.flush(); os.fsync(out.fileno())
    os.replace(temporary, path)
def rawlog(name, value):
    with lock:
        path = root / name
        assert not path.exists() or path.stat().st_size < 1048576
        with path.open('a') as out:
            out.write(json.dumps(value, separators=(',', ':')) + '\n'); out.flush(); os.fsync(out.fileno())
def emit(request, result):
    reply = dict(jsonrpc='2.0', id=request['id'], result=result)
    rawlog('peer-stdout.jsonl', reply)
    print(json.dumps(reply, separators=(',', ':')), flush=True)
def saved():
    files = list((root / 'controller' / 'chats').glob('*.json'))
    matches = [json.loads(path.read_text()) for path in files if path.stem == config['transcript']]
    assert len(matches) == 1 and matches[0]['native_session_id'] == config['native'], matches
    return matches[0]
def terminate(signum, frame):
    store('native-exit.json', dict(pid=os.getpid(), signal=signum)); sys.exit(0)
if sys.argv[1:] == ['--version']:
    rawlog('version.jsonl', dict(pid=os.getpid(), argv=sys.argv[1:]))
    print('1.19.3', flush=True); sys.exit(0)
args = sys.argv[1:]
assert args[0] == 'acp' and len(args) == 6 and args[1:3] == ['--hostname', '127.0.0.1'] and args[3] == '--port' and args[5] == '--no-mdns', args
assert os.environ.get('OPENCODE_SERVER_USERNAME') == 'opencode'
password = os.environ['OPENCODE_SERVER_PASSWORD']
auth = 'Basic ' + base64.b64encode(('opencode:' + password).encode()).decode()
current_native = ''
permission = [dict(permission='task', pattern='*', action='deny')]
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def request(self):
        global permission
        assert self.headers.get('Authorization') == auth
        parsed = urllib.parse.urlparse(self.path)
        assert urllib.parse.parse_qs(parsed.query).get('directory') == [os.getcwd()]
        body = None
        if self.command == 'PATCH':
            size = int(self.headers.get('Content-Length', '0')); assert 0 < size <= 4096
            body = json.loads(self.rfile.read(size))
            assert body == dict(permission=[dict(permission='task', pattern='*', action='ask')]), body
            permission = body['permission']
        assert current_native == config['native']
        if parsed.path == '/session/' + current_native:
            value = dict(id=current_native, directory=os.getcwd(), permission=permission)
        elif parsed.path == '/permission': value = []
        elif parsed.path == '/session/status': value = {}
        else: raise RuntimeError('Unexpected native HTTP route: ' + self.path)
        rawlog('native-http.jsonl', dict(method=self.command, route=parsed.path, body=body, value=value, pid=os.getpid()))
        wire = json.dumps(value, separators=(',', ':')).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(wire))); self.end_headers(); self.wfile.write(wire)
    do_GET = request
    do_PATCH = request
server = http.server.HTTPServer(('127.0.0.1', int(args[4])), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
signal.signal(signal.SIGTERM, terminate); signal.signal(signal.SIGINT, terminate)
store('native-launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), version='1.19.3'))
model_selected = False
mode_selected = False
prompt_count = 0
for line in sys.stdin:
    received_at = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    assert len(line) <= 1048576
    request = json.loads(line)
    rawlog('peer-stdin.jsonl', request)
    method = request.get('method')
    if method == 'initialize':
        assert request['id'] == 1 and request['params']['protocolVersion'] == 1
        emit(request, dict(protocolVersion=1, agentCapabilities=dict(loadSession=True, mcpCapabilities=dict(http=True, sse=True)), agentInfo=dict(name='opencode', title='Private OpenCode ACP', version='1.19.3')))
    elif method == 'session/new':
        assert request['id'] == 2 and request['params']['cwd'] == os.getcwd() and not current_native
        current_native = config['native']
        result = dict(sessionId=current_native, configOptions=[dict(id='model', category='model', type='select', currentValue='private/baseline', options=[dict(value='private/baseline', name='Baseline'), dict(value=config['model'], name='Selected')]), dict(id='mode', category='mode', type='select', currentValue='build', options=[dict(value='build', name='Build'), dict(value='plan', name='Plan')])])
        emit(request, result)
        if config['duplicate']: emit(request, result)
        store('native-created.json', dict(pid=os.getpid(), native=current_native))
    elif method == 'session/set_mode':
        assert request['params'] == dict(sessionId=current_native, modeId='build') and not mode_selected
        original = saved()
        mode_selected = True
        store('mode-input.json', dict(pid=os.getpid(), at=received_at, request=request, saved=original))
        emit(request, {})
    elif method == 'session/set_model':
        assert request['params'] == dict(sessionId=current_native, modelId=config['model']) and not model_selected
        original = saved()
        model_selected = True
        store('model-input.json', dict(pid=os.getpid(), at=received_at, request=request, saved=original))
        emit(request, {})
    elif method == 'session/prompt':
        prompt_count += 1
        assert prompt_count == 1 and mode_selected and model_selected and request['params']['sessionId'] == current_native
        blocks = request['params']['prompt']; assert any(config['task'] in part.get('text', '') for part in blocks)
        assert permission == [dict(permission='task', pattern='*', action='ask')]
        store('prompt-input.json', dict(pid=os.getpid(), request=request, saved=saved(), permission=permission))
        while not (root / 'release-result').exists(): time.sleep(0.005)
        event = dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=current_native, update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text='Private managed SSH completed.'))))
        rawlog('peer-stdout.jsonl', event); print(json.dumps(event, separators=(',', ':')), flush=True)
        emit(request, dict(stopReason='end_turn'))
    elif method == 'session/cancel':
        store('cancel.json', dict(pid=os.getpid(), request=request))
        if 'id' in request: emit(request, {})
    else:
        store('unexpected.json', request); raise RuntimeError('Unexpected ACP method: ' + str(method))
store('native-exit.json', dict(pid=os.getpid(), eof=True))
)OPENCODE_PEER"));
			for (const auto& executable : {ssh, shell, peer}) fs::permissions(executable, fs::perms::owner_all);
			const auto env = [&](const std::string& key, const std::string& value) { environment.push_back(std::make_unique<ScopedEnvVar>(key, value)); };
			env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", home.string()); env("USERPROFILE", home.string());
			env("XDG_CONFIG_HOME", (home / "config").string()); env("XDG_DATA_HOME", (home / "data").string()); env("XDG_CACHE_HOME", (home / "cache").string());
			env("TMPDIR", (home / "tmp").string()); env("UAM_DATA_DIR", (temp.root / "controller").string());
			env("OPENCODE_CONFIG_CONTENT", "{}"); env("OPENCODE_CONFIG_DIR", (home / "opencode").string());
			env("UAM_TEST_MANAGED_RUNNER", runner.string()); env("UAM_TEST_MANAGED_SSH_ROOT", temp.root.string());
			env("UAM_TEST_MANAGED_SOCKET", (temp.root / "s").string());
			env("NO_PROXY", "127.0.0.1,localhost"); env("no_proxy", "127.0.0.1,localhost");
			app.data_root = temp.root / "controller";
			app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
			app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = app.settings.computer_use_globally_enabled = false;
			ExecutionHost host; host.id = "private-managed-opencode"; host.label = "Private Mac shared remote path";
			host.transport = "ssh"; host.ssh_alias = "private-managed-opencode-host"; host.platform = "macos";
			host.runner_status = "ready"; host.runner_version = version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
			app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
			ChatSession root = ChatDomainService().CreateNewChat("", "opencode-cli", false);
			root.execution_host_id = host.id; root.workspace_directory = workspace.string(); root.model_id = model;
			root.approval_mode = "default"; root.command_safety_tier = "yolo";
			root.uam_control_enabled = root.memory_enabled = root.computer_use_enabled = false;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root)); app.chats.push_back(root);
			std::string error;
			UAM_ASSERT(uam::AgentRunScheduler::Enqueue(app, root.id, {}, "build", task, &run_id, &error, "opencode-cli", model));
			UAM_ASSERT_EQ(app.agent_runs.size(), std::size_t{1});
			const auto& run = app.agent_runs.front(); transcript_id = run.transcript_chat_id;
			UAM_ASSERT_EQ(run.status, std::string("queued")); UAM_ASSERT_EQ(run.definition_hash, std::string("builtin-build-v2"));
			const auto fresh = ChatRepository::LoadLocalChat(app.data_root, transcript_id, true);
			UAM_ASSERT(fresh && fresh->native_session_id.empty() && fresh->execution_host_id == host.id && fresh->workspace_directory == workspace.string());
			UAM_ASSERT(uam::FindAcpSessionForChat(app, transcript_id) == nullptr);
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "case.json", Json{{"transcript", transcript_id}, {"native", native}, {"model", model}, {"task", task}, {"duplicate", duplicate}}.dump()));
			runner_service.Start(runner, temp.root / "s", temp.root);
		}

		nlohmann::json Snapshot()
		{
			using Json = nlohmann::json;
			const auto* session = uam::FindAcpSessionForChat(app, transcript_id);
			const auto* chat = ChatDomainService().FindChatById(app, transcript_id);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, transcript_id, true);
			Json state{{"helper_service_pid", runner_service.pid}, {"helper_service_alive", runner_service.pid > 0 && kill(runner_service.pid, 0) == 0}, {"session", session != nullptr}, {"live_native", chat ? chat->native_session_id : ""}, {"saved_native", saved ? saved->native_session_id : ""}, {"run_status", app.agent_runs.front().status}, {"run_code", app.agent_runs.front().diagnostic_code}, {"stop_tasks", app.acp_process_stop_tasks.size()}, {"remote_stops", app.pending_acp_remote_stops.size()}, {"save_workers", app.pending_chat_save_tasks.size()}, {"scheduled_saves", app.pending_chat_save_at_by_chat_id.contains(transcript_id)}};
			if (session)
			{
				if (session->child_pid > 0 && std::ranges::find(proxy_pids, session->child_pid) == proxy_pids.end()) proxy_pids.push_back(session->child_pid);
				Json methods = Json::array(); for (const auto& [id, method] : session->pending_request_methods) methods.push_back(Json{{"id", id}, {"method", method}});
				state["runtime"] = {{"running", session->running}, {"processing", session->processing}, {"reconnect", session->reconnect_pending}, {"recovering_turn", session->recovering_remote_turn}, {"recovering_process", session->recovering_remote_process}, {"lifecycle", session->lifecycle_state}, {"error", session->last_error}, {"ownership_error", session->transport_ownership_error}, {"process", session->last_process_id}, {"proxy_pid", session->child_pid}, {"proxy_alive", session->child_pid > 0 && kill(session->child_pid, 0) == 0}, {"native", session->session_id}, {"model", session->current_model_id}, {"launch_attempted", session->managed_launch_attempted}, {"agent_input", bool(session->pending_agent_input)}, {"local_launch", bool(session->pending_local_launch)}, {"local_receipt", bool(session->pending_local_session_receipt)}, {"delivery", bool(session->pending_prompt_delivery)}, {"local_stop", session->local_stop_pending}, {"remote_stop", session->remote_stop_pending}, {"stop_unconfirmed", session->remote_stop_unconfirmed}, {"worktree", session->worktree_launch_pending}, {"queued_prompt", !session->queued_prompt.empty()}, {"queue_size", session->queued_user_prompts.size()}, {"setup_id", session->session_setup_request_id}, {"prompt_id", session->prompt_request_id}, {"model_id", session->startup_model_request_id}, {"pending_methods", methods}, {"stdout_poll", session->stdout_poll_pending}, {"stderr_poll", session->stderr_poll_pending}, {"output_ack", !session->pending_remote_output_ack_line.empty()}, {"stdout_cursor", session->pending_remote_stdout_cursor}, {"stderr_cursor", session->pending_remote_stderr_cursor}, {"source_exit", session->remote_source_exit_reported}, {"source_code", session->remote_source_exit_code}, {"source_ack", !session->pending_remote_source_exit_ack_line.empty()}, {"exit_observed", session->process_exit_observed}, {"observed_exit_code", session->observed_process_exit_code}, {"has_exit_code", session->has_last_exit_code}, {"exit_code", session->last_exit_code}, {"stderr", session->recent_stderr}};
			}
			if (chat) state["remote"] = {{"process_exists", chat->remote_process_exists}, {"pending_requests", chat->remote_pending_requests.size()}, {"source_exit", chat->remote_source_exit_pending}, {"stop_cleanup", chat->remote_stop_cleanup_pending}, {"restart", chat->remote_restart_pending}};
			if (fs::exists(temp.root / "native-launch.json"))
			{
				const auto native_pid = Json::parse(ReadFile(temp.root / "native-launch.json")).at("pid").get<pid_t>();
				state["native_pid"] = native_pid; state["native_alive"] = native_pid > 0 && kill(native_pid, 0) == 0;
			}
			return state;
		}
		void BeforeScheduler()
		{
			const auto state = Snapshot();
			if (states.empty() || states.back() != state)
			{
				if (states.size() == 160) states.erase(states.begin());
				states.push_back(state);
			}
			// This diagnostic is captured BEFORE Poll can FinishRun/Stop/remove the runtime.
			if (const auto* session = uam::FindAcpSessionForChat(app, transcript_id); session && session->managed_launch_attempted && !session->running && !session->processing && !session->reconnect_pending)
				std::cerr << "PRIVATE_MANAGED_OPENCODE_BEFORE_EXIT " << state.dump() << std::endl;
		}
		void Pulse()
		{
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			ProviderCliCompatibilityService().Poll(app);
			if (app.opencode_native_activity) (void)app.opencode_native_activity->Poll(app);
			BeforeScheduler();
			(void)uam::AgentRunScheduler::Poll(app);
		}
		bool Await(const std::function<bool()>& done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && app.agent_runs.front().status != "failed" && std::chrono::steady_clock::now() < deadline)
			{
				Pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!done()) std::cerr << "PRIVATE_MANAGED_OPENCODE_STATES " << states.dump() << std::endl;
			return done();
		}
		nlohmann::json Audit()
		{
			nlohmann::json result = nlohmann::json::array();
			for (const auto& entry : fs::directory_iterator(temp.root))
				if (entry.path().filename().string().starts_with("runner-") && entry.path().extension() == ".jsonl")
				{
					const std::string captured = ReadFile(entry.path());
					const auto complete = captured.find_last_of('\n');
					if (complete == std::string::npos) continue;
					// Concurrent append may expose a partial final record; only complete frames are evidence.
					std::istringstream input(captured.substr(0, complete + 1)); std::string line;
					while (std::getline(input, line)) { UAM_ASSERT(result.size() < 16384); result.push_back(nlohmann::json::parse(line)); }
				}
			return result;
		}
		void Stop()
		{
			if (stopped) return;
			(void)uam::io::WriteTextFile(temp.root / "release-result", "Release actual peer completion for bounded cleanup.\n");
			// Stop actual runtime ownership first; keep the default service alive to harvest its real futures.
			(void)uam::StopAcpSession(app, transcript_id);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			const auto quiet = [&]()
			{
				const auto* session = uam::FindAcpSessionForChat(app, transcript_id);
				return (!app.opencode_native_activity || !app.opencode_native_activity->HasPendingWork()) && app.pending_acp_remote_stops.empty() && app.acp_process_stop_tasks.empty() && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty() && app.remote_context_tasks.empty() && app.cli_context_preparation_tasks.empty() && !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker && app.runtime_cli_version_check_queue.empty() && (!session || (!session->running && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending));
			};
			while (!quiet() && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::PollAllAcpSessions(app); (void)uam::FlushPendingChatSaves(app); ProviderCliCompatibilityService().Poll(app);
				const auto* runtime = uam::FindAcpSessionForChat(app, transcript_id);
				if (runtime && runtime->running) (void)uam::StopAcpSession(app, transcript_id);
				// Stopped/absent runtime makes existing snapshots unowned and prevents new snapshot/image admission.
				if ((!runtime || !runtime->running) && app.opencode_native_activity) (void)app.opencode_native_activity->Poll(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!quiet()) { std::cerr << "FATAL_PRIVATE_MANAGED_OPENCODE_CLEANUP " << Snapshot().dump() << std::endl; std::abort(); }
			for (const pid_t pid : proxy_pids)
			{
				siginfo_t info{}; errno = 0;
				if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != -1 || errno != ECHILD) { std::cerr << "FATAL_PRIVATE_MANAGED_OPENCODE_PROXY_NOT_REAPED" << std::endl; std::abort(); }
			}
			// Only now have ready futures been consumed; Stop must not clear the evidence before this point.
			if (app.opencode_native_activity) { app.opencode_native_activity->Stop(); app.opencode_native_activity.reset(); }
			if (fs::exists(temp.root / "native-launch.json"))
			{
				const auto native_pid = nlohmann::json::parse(ReadFile(temp.root / "native-launch.json")).at("pid").get<pid_t>();
				errno = 0;
				if (native_pid <= 0 || kill(native_pid, 0) != -1 || errno != ESRCH)
				{ std::cerr << "FATAL_PRIVATE_MANAGED_NATIVE_NOT_REAPED_BEFORE_SERVICE_STOP" << std::endl; std::abort(); }
			}
			runner_service.Stop(); // Exact owned service dies/reaps while private environment and files still exist.
			stopped = true;
		}
		~ManagedOpenCodeSshFixture() noexcept
		{
			try { Stop(); } catch (...) { std::cerr << "FATAL_PRIVATE_MANAGED_OPENCODE_CLEANUP_EXCEPTION" << std::endl; std::abort(); }
		}
	};

	void CheckManagedOpenCodeSshStartup(bool duplicate)
	{
		using Json = nlohmann::json;
		ManagedOpenCodeSshFixture fixture(duplicate);
		UAM_ASSERT(fixture.Await([&] { return fs::exists(fixture.temp.root / "native-created.json"); }));
		UAM_ASSERT(fixture.Await([&]
		{
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.transcript_id, true);
			const auto* session = uam::FindAcpSessionForChat(fixture.app, fixture.transcript_id);
			return saved && saved->native_session_id == fixture.native && session && session->session_id == fixture.native && session->session_setup_request_id == 0;
		}));
		UAM_ASSERT(fixture.Await([&] { return fs::exists(fixture.temp.root / "prompt-input.json"); }));
		const auto snapshot = fixture.Snapshot();
		UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("running"));
		UAM_ASSERT(snapshot.at("runtime").at("running") == true && snapshot.at("runtime").at("proxy_alive") == true && snapshot.at("native_alive") == true);
		UAM_ASSERT_EQ(snapshot.at("saved_native"), Json(fixture.native));
		UAM_ASSERT(snapshot.at("runtime").at("source_exit") == false);
		UAM_ASSERT(snapshot.at("helper_service_alive") == true);
		UAM_ASSERT(fs::exists(fixture.temp.root / "model-input.json") && fs::exists(fixture.temp.root / "native-http.jsonl"));
		UAM_ASSERT(!fs::exists(fixture.temp.root / "unexpected.json") && !fs::exists(fixture.temp.root / "native-exit.json"));
		const auto prompt = Json::parse(ReadFile(fixture.temp.root / "prompt-input.json"));
		UAM_ASSERT_EQ(prompt.at("saved").at("native_session_id"), Json(fixture.native));
		UAM_ASSERT_EQ(prompt.at("saved").at("execution_host_id"), Json("private-managed-opencode"));
		UAM_ASSERT_EQ(prompt.at("saved").at("agent_run_id"), Json(fixture.run_id));
		std::size_t initialize = 0, create = 0, models = 0, inputs = 0;
		std::istringstream raw(ReadFile(fixture.temp.root / "peer-stdin.jsonl")); std::string line;
		while (std::getline(raw, line))
		{
			const auto method = Json::parse(line).at("method").get<std::string>();
			initialize += method == "initialize"; create += method == "session/new"; models += method == "session/set_model"; inputs += method == "session/prompt";
		}
		UAM_ASSERT_EQ(initialize, std::size_t{1}); UAM_ASSERT_EQ(create, std::size_t{1}); UAM_ASSERT_EQ(models, std::size_t{1}); UAM_ASSERT_EQ(inputs, std::size_t{1});
		std::uintmax_t creation_cursor = 0;
		std::uint64_t creation_poll_at = 0, creation_ack_at = 0;
		const auto audit = fixture.Audit();
		for (const auto& event : audit)
			if (event.at("direction") == "response")
			{
				const auto& frame = event.at("frame");
				if (frame.at("requestType") != "process.poll" || frame.at("request").value("sessionId", "") != "acp-" + fixture.transcript_id || !frame.at("response").value("ok", false)) continue;
				const auto& result = frame.at("response").at("result");
				std::string output; UAM_ASSERT(uam::base64::Decode(result.value("stdoutBase64", ""), output));
				std::istringstream lines(output); std::string rpc_line;
				while (std::getline(lines, rpc_line))
				{
					const auto rpc = Json::parse(rpc_line, nullptr, false);
					if (rpc.is_object() && rpc.value("id", Json()) == 2 && rpc.contains("result") && rpc.at("result").value("sessionId", "") == fixture.native)
					{
						const auto at = event.at("at").get<std::uint64_t>();
						if (!creation_poll_at || at < creation_poll_at) { creation_poll_at = at; creation_cursor = result.at("stdoutCursor").get<std::uintmax_t>(); }
					}
				}
			}
		bool native_query = false;
		for (const auto& event : audit)
			if (event.at("direction") == "response")
			{
				const auto& frame = event.at("frame");
				if (creation_cursor && frame.at("requestType") == "process.ack" && frame.at("request").value("sessionId", "") == "acp-" + fixture.transcript_id && frame.at("response").value("ok", false) && frame.at("request").value("stdoutCursor", std::uintmax_t{0}) >= creation_cursor)
				{
					const auto at = event.at("at").get<std::uint64_t>();
					if (at >= creation_poll_at && (!creation_ack_at || at < creation_ack_at)) creation_ack_at = at;
				}
				if (frame.at("requestType") == "opencode.native" && frame.at("request").value("sessionId", "") == "acp-" + fixture.transcript_id && frame.at("response").value("ok", false)) native_query = true;
			}
		UAM_ASSERT(creation_cursor > 0 && creation_poll_at > 0 && creation_ack_at >= creation_poll_at && native_query);
		const auto model_input = Json::parse(ReadFile(fixture.temp.root / "model-input.json"));
		// Explicit CLOCK_MONOTONIC is system-wide on macOS, including Python3.9; no process-local monotonic epoch comparison.
		UAM_ASSERT(creation_ack_at <= model_input.at("at").get<std::uint64_t>());
		std::cerr << "PRIVATE_MANAGED_OPENCODE_HEALTHY " << Json{{"duplicate_creation_reply", duplicate}, {"actual_before_completion", snapshot}, {"creation_cursor", creation_cursor}, {"creation_poll_at", creation_poll_at}, {"creation_ack_at", creation_ack_at}, {"model_received_at", model_input.at("at")}}.dump() << std::endl;
		UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "release-result", "Release actual provider result.\n"));
		UAM_ASSERT(fixture.Await([&] { return fixture.app.agent_runs.front().status == "completed"; }));
		UAM_ASSERT(fixture.app.agent_runs.front().diagnostic_code.empty());
		const auto completed = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.transcript_id, true);
		UAM_ASSERT(completed && completed->native_session_id == fixture.native);
		UAM_ASSERT(std::ranges::any_of(completed->messages, [&](const Message& message) { return message.role == MessageRole::Assistant && message.content == "Private managed SSH completed."; }));
		fixture.Stop();
	}
}

UAM_TEST(ManagedOpenCodeSshFreshNativeSetupReachesFirstPromptThroughActualHelper)
{
	CheckManagedOpenCodeSshStartup(false);
}
UAM_TEST(ManagedOpenCodeSshDuplicateCreationReplyCannotFalseExitBeforeFirstPrompt)
{
	CheckManagedOpenCodeSshStartup(true);
}
#endif

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI && defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
	// Real private native stream peers; the public controller tool dispatcher is invoked below.
	// This does not impersonate a provider's MCP client or contact an installed/account-backed CLI.
	struct OrdinaryAntigravityPublicLifecycleFixture
	{
		TempDir temp{"uam-antigravity-public-lifecycle"};
		std::vector<std::unique_ptr<ScopedEnvVar>> environment;
		uam::AppState app;
		uam::AgentDefinition captured;
		std::string root_id;
		std::set<pid_t> owned_pids;
		bool stopped = false;
		const std::string parent_model = "private-parent-model";
		const std::string child_model = "private-child-model";
		const std::string parent_task = "Start an ordinary private Antigravity conversation.";
		const std::string child_task = "Inspect the private captured definition and return its bounded answer.";
		const std::string parent_answer = "PRIVATE-ANTIGRAVITY-PARENT-READY";
		const std::string child_answer = "PRIVATE-ANTIGRAVITY-CHILD-COMPLETE";
		const std::string parent_native = "a061729b-d042-46ae-96e2-6804da990321";
		const std::string child_native = "f2418669-80a2-4b7f-97eb-bc3d86440518";

		OrdinaryAntigravityPublicLifecycleFixture()
		{
			UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
			const auto home = temp.root / "home", bin = temp.root / "bin", workspace = temp.root / "workspace";
			fs::create_directories(bin); fs::create_directories(home / "tmp"); fs::create_directories(workspace);
			const auto env = [&](const std::string& name, const std::string& value)
			{
				environment.push_back(std::make_unique<ScopedEnvVar>(name, value));
			};
			env("PATH", bin.string()); env("HOME", home.string()); env("USERPROFILE", home.string());
			env("XDG_CONFIG_HOME", (home / "config").string()); env("XDG_DATA_HOME", (home / "data").string());
			env("XDG_CACHE_HOME", (home / "cache").string()); env("TMPDIR", (home / "tmp").string());
			env("UAM_DATA_DIR", (temp.root / "controller").string());
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", nlohmann::json{
			    {"workspace", workspace.string()}, {"parent_model", parent_model}, {"child_model", child_model},
			    {"parent_task", parent_task}, {"child_task", child_task}, {"parent_answer", parent_answer},
			    {"child_answer", child_answer}, {"parent_native", parent_native}, {"child_native", child_native}}.dump()));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer.py", R"AGY_PUBLIC(import json, os, pathlib, sys, time
root = pathlib.Path(__file__).parent
config = json.loads((root / 'peer-config.json').read_text())
args = sys.argv[1:]
if args == ['--version']:
    print('Antigravity CLI 1.4.0', flush=True)
    sys.exit(0)
def option(name):
    assert args.count(name) == 1, 'missing or repeated ' + name
    return args[args.index(name) + 1]
assert option('--input-format') == 'stream-json'
assert option('--output-format') == 'stream-json'
model = option('--model')
assert model in [config['parent_model'], config['child_model']]
kind = 'parent' if model == config['parent_model'] else 'child'
def store(name, value):
    path = root / (kind + '-' + name + '.json')
    temporary = root / (kind + '-' + name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)
try:
    store('launch', dict(pid=os.getpid(), cwd=os.getcwd(), argv=args, model=model))
    assert os.path.samefile(os.getcwd(), config['workspace']), 'native cwd is not the configured private workspace'
    assert '--conversation' not in args, 'fresh ordinary launch unexpectedly resumed another conversation'
    if kind == 'child':
        assert option('--mode') == 'plan'
        assert not any(arg.startswith('--dangerously-skip-permissions') for arg in args)
    prompts = 0
    for line in sys.stdin:
        assert len(line) <= 1048576
        request = json.loads(line)
        assert request.get('event') == 'user' and 'method' not in request
        prompts += 1
        assert prompts == 1, 'ordinary first input duplicated'
        text = request['message']['content']
        assert isinstance(text, str) and text.count(config[kind + '_task']) == 1
        store('input', dict(pid=os.getpid(), cwd=os.getcwd(), argv=args, model=model, request=request, count=prompts))
        if kind == 'child':
            deadline = time.monotonic() + 45
            while not (root / 'release-child').exists():
                assert time.monotonic() < deadline, 'test did not release the private result'
                time.sleep(0.005)
        native = config[kind + '_native']
        answer = config[kind + '_answer']
        emit(dict(event='init', conversation_id=native))
        emit(dict(event='step_update', step_update=dict(conversation_id=native, step_index=1,
            state='DONE', step_type='agent_response', text_delta=answer, usage=dict(input_tokens=7, output_tokens=3))))
        emit(dict(event='result', result=dict(conversation_id=native, status='SUCCESS', response=answer, num_turns=1)))
        store('result', dict(pid=os.getpid(), native=native, answer=answer))
    store('exit', dict(pid=os.getpid(), eof=True))
except Exception as error:
    trace = error.__traceback__
    while trace is not None and trace.tb_next is not None:
        trace = trace.tb_next
    store('error', dict(error=str(error), exceptionType=type(error).__name__, line=trace.tb_lineno if trace is not None else 0))
    sys.exit(70)
)AGY_PUBLIC"));
			const auto executable = bin / "agy";
			UAM_ASSERT(uam::io::WriteTextFile(executable, "#!/bin/sh\nexec /usr/bin/python3 -I -S " + ShellQuoteForTest((temp.root / "peer.py").string()) + " \"$@\"\n"));
			fs::permissions(executable, fs::perms::owner_all, fs::perm_options::replace);
			UAM_ASSERT(fs::is_regular_file(executable) && !fs::is_symlink(executable));
			UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
			app.data_root = temp.root / "controller";
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.git_pull_before_chat = false;
			app.settings.git_snapshot_plain_folders = app.settings.git_snapshot_svn = app.settings.svn_enabled = false;
			app.settings.memory_enabled_default = app.settings.computer_use_globally_enabled = false;
			app.settings.memory_level_default = "off"; app.settings.memory_recall_budget_bytes = 0;
			app.provider_profiles = {ProviderProfileStore::DefaultAntigravityProfile()};
			app.provider_profiles.front().interactive_command = ShellQuoteForTest(executable.string());
			captured.id = "private-native-reader"; captured.description = "Private ordinary delegated peer";
			captured.mode = "subagent"; captured.workspace_access = "read";
			captured.instructions = "PRIVATE-CAPTURED-READ-CEILING: Read only; return the assigned answer without tools or delegates.";
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(app.data_root, captured, &error));
			const auto catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
			const auto loaded = std::ranges::find(catalog.definitions, captured.id, &uam::AgentDefinition::id);
			UAM_ASSERT(loaded != catalog.definitions.end()); captured = *loaded;
			ChatSession parent = ChatDomainService().CreateNewChat({}, "antigravity-cli");
			root_id = parent.id; parent.execution_host_id = "local"; parent.routing_mode = "explicit";
			parent.workspace_directory = workspace.string(); parent.model_id = parent_model;
			parent.approval_mode = "default"; parent.command_safety_tier = "yolo"; parent.uam_agent_id = "build";
			parent.uam_control_enabled = true; parent.uam_control_explicitly_disabled = false;
			parent.memory_enabled = parent.computer_use_enabled = false; parent.memory_level = "off";
			parent.reasoning_effort.clear(); parent.service_tier.clear(); parent.service_tier_explicit = false;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, parent)); app.chats.push_back(std::move(parent));
			auto& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("antigravity-cli", "local")];
			version.checked = version.supported = true; version.installed_version = "1.4.0";
			version.execution_host = app.settings.execution_hosts.front();
			app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
			UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
		}
		void Observe()
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->child_pid > 0) owned_pids.insert(session->child_pid);
			for (const auto& kind : {"parent", "child"})
			{
				const auto path = temp.root / (std::string(kind) + "-launch.json");
				if (fs::exists(path)) owned_pids.insert(nlohmann::json::parse(ReadFile(path)).at("pid").get<pid_t>());
			}
		}
		void Pulse()
		{
			Observe();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			ProviderCliCompatibilityService().Poll(app);
			(void)uam::AgentRunScheduler::Poll(app);
			Observe();
			for (const auto& kind : {"parent", "child"})
			{
				const auto path = temp.root / (std::string(kind) + "-error.json");
				if (fs::exists(path)) throw TestFailure("Private Antigravity peer: " + ReadFile(path).substr(0, 512));
			}
		}
		template <typename Predicate> void Await(Predicate done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				Pulse();
				if (!app.agent_runs.empty() && app.agent_runs.front().status == "failed")
					throw TestFailure("Private Antigravity child: " + app.agent_runs.front().diagnostic_code + ": " + app.agent_runs.front().diagnostic);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			UAM_ASSERT(done());
		}
		nlohmann::json Tool(const std::string& capability, const std::string& id, const std::string& method, nlohmann::json arguments)
		{
			return uam::UamControlService::HandleRequestForTests(app, capability,
			    {{"requestId", id}, {"method", method}, {"arguments", std::move(arguments)}}, uam::time::SystemEpochMillisecondsNow());
		}
		void Stop()
		{
			if (stopped) return;
			(void)uam::io::WriteTextFile(temp.root / "release-child", "Allow private child cleanup.\n");
			Observe();
			uam::FastStopAcpSessionsForExit(app);
			auto& process = PlatformServicesFactory::Instance().process_service;
			for (const auto& session : app.acp_sessions)
				if (session) { process.StopStdioProcess(*session, true); process.CloseStdioProcessHandles(*session); }
			uam::ResetAsyncCommandTask(app.runtime_cli_version_check_task);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app, true); std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
			UAM_ASSERT(app.acp_process_stop_tasks.empty() && app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty());
			Observe();
			for (const pid_t pid : owned_pids)
			{
				UAM_ASSERT(pid > 0);
				int status = 0; errno = 0;
				const auto waited = waitpid(pid, &status, WNOHANG);
				if (waited == pid) continue;
				UAM_ASSERT(waited == -1 && errno == ECHILD);
				errno = 0; UAM_ASSERT(kill(pid, 0) == -1 && errno == ESRCH);
			}
			uam::UamControlService::Shutdown(app);
			stopped = true;
		}
		~OrdinaryAntigravityPublicLifecycleFixture() noexcept
		{
			try { Stop(); }
			catch (...) { std::cerr << "FATAL_PRIVATE_ANTIGRAVITY_PUBLIC_LIFECYCLE_CLEANUP" << std::endl; std::abort(); }
		}
	};
}

UAM_TEST(AntigravityOrdinaryPublicDelegateRunsPrivateChildAndReturnsOwnedStatus)
{
	using Json = nlohmann::json;
	OrdinaryAntigravityPublicLifecycleFixture fixture;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.root_id, fixture.parent_task, &error));
	fixture.Await([&]
	{
		const auto* session = uam::FindAcpSessionForChat(fixture.app, fixture.root_id);
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id, true);
		return session && session->running && !session->processing && session->last_turn_outcome == "completed" && session->session_id == fixture.parent_native && saved && saved->native_session_id == fixture.parent_native && std::ranges::any_of(saved->messages, [&](const Message& item) { return item.role == MessageRole::Assistant && item.content == fixture.parent_answer; }) && fixture.app.pending_chat_save_tasks.empty() && fixture.app.pending_chat_save_at_by_chat_id.empty();
	});
	const auto* parent_runtime = uam::FindAcpSessionForChat(fixture.app, fixture.root_id);
	UAM_ASSERT(parent_runtime && parent_runtime->active_uam_agent_workspace_access != "read");
	const std::string capability = parent_runtime->uam_control_capability_id;
	UAM_ASSERT(!capability.empty());
	const auto parent_before = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id, true);
	UAM_ASSERT(parent_before && parent_before->native_session_id == fixture.parent_native);
	UAM_ASSERT(std::ranges::any_of(parent_before->messages, [&](const Message& item) { return item.role == MessageRole::Assistant && item.content == fixture.parent_answer; }));
	const auto delegated = fixture.Tool(capability, "private-delegate", "agent_delegate",
	    {{"agentId", fixture.captured.id}, {"task", fixture.child_task}, {"providerId", "antigravity-cli"}, {"modelId", fixture.child_model}});
	if (!delegated.value("ok", false)) throw TestFailure("Public Antigravity delegation: " + delegated.dump());
	UAM_ASSERT_EQ(delegated.at("result").at("status"), Json("queued"));
	const std::string run_id = delegated.at("result").at("runId").get<std::string>();
	UAM_ASSERT_EQ(fixture.app.agent_runs.size(), std::size_t{1});
	const AgentRun queued = fixture.app.agent_runs.front();
	UAM_ASSERT_EQ(queued.id, run_id); UAM_ASSERT_EQ(queued.root_chat_id, fixture.root_id);
	UAM_ASSERT(queued.parent_run_id.empty() && queued.depth == 1 && !queued.managed_continuation);
	UAM_ASSERT_EQ(queued.provider_id, std::string("antigravity-cli")); UAM_ASSERT_EQ(queued.model_id, fixture.child_model);
	UAM_ASSERT_EQ(queued.definition_hash, fixture.captured.definition_hash);
	UAM_ASSERT_EQ(queued.definition_snapshot, fixture.captured.markdown_snapshot);
	UAM_ASSERT_EQ(queued.definition_instructions, fixture.captured.instructions);
	UAM_ASSERT_EQ(queued.effective_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(queued.task, fixture.child_task);
	const auto before_launch = ChatRepository::LoadLocalChat(fixture.app.data_root, queued.transcript_chat_id, true);
	UAM_ASSERT(before_launch && before_launch->native_session_id.empty());
	UAM_ASSERT_EQ(before_launch->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(before_launch->workspace_directory, parent_before->workspace_directory);
	UAM_ASSERT_EQ(before_launch->model_id, fixture.child_model);
	UAM_ASSERT_EQ(before_launch->approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(before_launch->command_safety_tier, parent_before->command_safety_tier);
	// A later edit must not widen the already captured read ceiling or replace its instructions.
	uam::AgentDefinition edited = fixture.captured;
	edited.workspace_access = "write"; edited.instructions = "PRIVATE-LATER-WRITE-DEFINITION: This is not the captured delegate.";
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(fixture.app.data_root, edited, &error));
	fixture.Await([&] { return fs::exists(fixture.temp.root / "child-input.json"); });
	const auto input = Json::parse(ReadFile(fixture.temp.root / "child-input.json"));
	UAM_ASSERT_EQ(input.at("model"), Json(fixture.child_model));
	UAM_ASSERT(fs::equivalent(fs::path(input.at("cwd").get<std::string>()), fs::path(parent_before->workspace_directory)));
	UAM_ASSERT_EQ(input.at("count"), Json(1));
	const std::string delivered = input.at("request").at("message").at("content").get<std::string>();
	UAM_ASSERT(delivered.find(fixture.captured.instructions) != std::string::npos);
	UAM_ASSERT(delivered.find(edited.instructions) == std::string::npos);
	const auto* child_runtime = uam::FindAcpSessionForChat(fixture.app, queued.transcript_chat_id);
	UAM_ASSERT(child_runtime && child_runtime->running && child_runtime->processing && child_runtime->managed_launch_attempted);
	UAM_ASSERT_EQ(child_runtime->provider_id, std::string("antigravity-cli"));
	UAM_ASSERT_EQ(child_runtime->active_uam_agent_definition_hash, queued.definition_hash);
	UAM_ASSERT_EQ(child_runtime->active_uam_agent_definition_snapshot, queued.definition_snapshot);
	UAM_ASSERT_EQ(child_runtime->active_uam_agent_workspace_access, std::string("read"));
	const auto running = fixture.Tool(capability, "private-running", "agent_status", {{"runId", run_id}});
	UAM_ASSERT(running.value("ok", false)); UAM_ASSERT_EQ(running.at("result").at("status"), Json("running"));
	UAM_ASSERT_EQ(running.at("result").at("result"), Json(""));
	UAM_ASSERT(!fs::exists(fixture.temp.root / "child-result.json"));
	UAM_ASSERT(uam::io::WriteTextFile(fixture.temp.root / "release-child", "Release the actual native child result.\n"));
	fixture.Await([&]
	{
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, queued.transcript_chat_id, true);
		return fixture.app.agent_runs.front().status == "completed" && saved && saved->native_session_id == fixture.child_native && std::ranges::any_of(saved->messages, [&](const Message& item) { return item.role == MessageRole::Assistant && item.content == fixture.child_answer; });
	});
	const auto terminal = fixture.Tool(capability, "private-terminal", "agent_status", {{"runId", run_id}});
	UAM_ASSERT(terminal.value("ok", false));
	const auto& result = terminal.at("result");
	UAM_ASSERT_EQ(result.at("runId"), Json(run_id)); UAM_ASSERT_EQ(result.at("status"), Json("completed"));
	UAM_ASSERT_EQ(result.at("providerId"), Json("antigravity-cli")); UAM_ASSERT_EQ(result.at("modelId"), Json(fixture.child_model));
	UAM_ASSERT_EQ(result.at("definitionHash"), Json(queued.definition_hash));
	UAM_ASSERT_EQ(result.at("transcriptChatId"), Json(queued.transcript_chat_id));
	UAM_ASSERT_EQ(result.at("result"), Json(fixture.child_answer)); UAM_ASSERT_EQ(result.at("diagnosticCode"), Json(""));
	const auto ledger = uam::AgentRunLedger::LoadAllReadOnly(fixture.app.data_root);
	UAM_ASSERT(ledger.errors.empty() && ledger.runs.size() == 1);
	UAM_ASSERT_EQ(ledger.runs.front().status, std::string("completed"));
	UAM_ASSERT_EQ(ledger.runs.front().result_excerpt, fixture.child_answer);
	const auto completed = ChatRepository::LoadLocalChat(fixture.app.data_root, queued.transcript_chat_id, true);
	UAM_ASSERT(completed && completed->native_session_id == fixture.child_native);
	UAM_ASSERT(completed->native_session_id != parent_before->native_session_id);
	UAM_ASSERT_EQ(std::ranges::count_if(completed->messages, [&](const Message& item) { return item.role == MessageRole::User && item.content == fixture.child_task; }), 1);
	UAM_ASSERT_EQ(std::ranges::count_if(completed->messages, [&](const Message& item) { return item.role == MessageRole::Assistant && item.content == fixture.child_answer; }), 1);
	const auto parent_after = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.root_id, true);
	UAM_ASSERT(parent_after && parent_after->native_session_id == fixture.parent_native && parent_after->model_id == fixture.parent_model);
	UAM_ASSERT(parent_after->messages == parent_before->messages); // Tool audit is metadata; ordinary child result is read via agent_status.
	fixture.Stop();
	UAM_ASSERT_EQ(fixture.owned_pids.size(), std::size_t{2});
}
#endif

UAM_TEST(AgentListIncludesSameScopeOpenCodeConfigOnlyNativeModels)
{
	TargetMetadataFixture fixture;
	auto& app = fixture.app;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultOpenCodeProfile());
	auto& host = app.settings.execution_hosts.front();
	host.id = "windows-target"; host.transport = "ssh"; host.platform = "windows";
	host.architecture = "x86_64"; host.runner_status = "ready"; host.runner_directory = "owned-helper";
	app.chats.front().execution_host_id = host.id;
	app.chats.front().workspace_directory = "C:/workspaces/owned";
	fixture.Ready("opencode-cli");
	app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", host.id)].installed_version = "1.19.3";
	app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
	app.provider_model_catalog->SetExecutionHostContext(app.settings.execution_hosts);
	ChatSession producer; producer.id = "catalog-producer"; producer.provider_id = "opencode-cli";
	producer.execution_host_id = host.id; producer.workspace_directory = app.chats.front().workspace_directory;
	app.chats.push_back(producer);
	uam::AcpSessionState session; session.chat_id = producer.id; session.provider_id = "opencode-cli";
	session.session_setup_request_id = 1; session.pending_request_methods[1] = "session/new";
	session.model_catalog_scope_key = app.provider_model_catalog->CaptureScopeKey("opencode-cli", producer.workspace_directory, host.id);
	const auto options = nlohmann::json::array({{{"id", "model"}, {"category", "model"}, {"currentValue", "not-an-advertised-default"},
	    {"options", nlohmann::json::array({{{"value", "example-provider/fast"}, {"name", "Fast label"}}, {{"value", "example-provider/normal"}, {"name", "Normal"}}, {{"value", "example-provider/qwen2.5-0.5b-instruct"}, {"name", "Qwen label"}}})}}});
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.back(), nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"sessionId", "native-catalog"}, {"configOptions", options}}}}.dump()));
	UAM_ASSERT(app.provider_model_catalog->GetCachedProviderModels("opencode-cli", producer.workspace_directory, host.id).empty());
	UAM_ASSERT_EQ(app.provider_model_catalog->GetCachedProviderConfigOptions("opencode-cli", producer.workspace_directory, host.id).size(), std::size_t{1});
	const auto targets = fixture.Targets();
	const auto target = std::ranges::find_if(targets, [](const auto& item) { return item.value("providerId", "") == "opencode-cli"; });
	UAM_ASSERT(target != targets.end()); UAM_ASSERT(target->value("eligible", false));
	UAM_ASSERT_EQ(target->value("defaultModelId", "unexpected"), std::string{});
	UAM_ASSERT_EQ(target->value("authentication", ""), std::string("unverified"));
	UAM_ASSERT_EQ(target->at("models"), (nlohmann::json::array({{{"id", "example-provider/fast"}}, {{"id", "example-provider/normal"}}, {{"id", "example-provider/qwen2.5-0.5b-instruct"}}})));
}

UAM_TEST(AgentListConfigModelChoicesRespectNativePrecedenceAndBounds)
{
	for (const auto& provider : ProviderProfileStore::BuiltInProfiles())
		for (int variant = 0; variant != 7; ++variant)
		{
			TargetMetadataFixture fixture; auto& app = fixture.app;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles(); fixture.Ready(provider.id);
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
			const std::string workspace = app.chats.front().workspace_directory;
			nlohmann::json legacy = nlohmann::json::array();
			for (int i = 0; i != 32; ++i) legacy.push_back({{"id", "legacy-" + std::to_string(i)}});
			nlohmann::json choices = nlohmann::json::array({{{"value", "  native/one  "}, {"name", "NeverUseLabel"}}, {{"value", "native/two"}}, {{"value", "native/one"}}, {{"name", "NoValue"}}, {{"value", 7}}, {{"value", std::string("native\0bad", 10)}}, {{"value", std::string(257, 'x')}}});
			nlohmann::json option = {{"id", " model "}, {"category", " model "}, {"currentValue", "NeverUseCurrent"}, {"options", choices}};
			nlohmann::json options = nlohmann::json::array({option});
			nlohmann::json expected = nlohmann::json::array({{{"id", "native/one"}}, {{"id", "native/two"}}});
			if (variant == 1) { options[0]["options"] = nlohmann::json::array(); expected = nlohmann::json::array(); }
			if (variant == 2)
			{
				// Native parser skips empty IDs even if category says model.
				options.insert(options.begin(), nlohmann::json{{"id", " "}, {"category", "model"}, {"options", nlohmann::json::array({{{"value", "must-skip-empty-id"}}})}});
				options[1]["id"] = "custom-model-id";
			}
			if (variant == 3) options.push_back({{"id", "model"}, {"options", nlohmann::json::array({{{"value", "must-not-replace-first"}}})}});
			if (variant == 4)
			{
				options = nlohmann::json::array();
				for (int i = 0; i != 64; ++i) options.push_back({{"id", "nonmodel-" + std::to_string(i)}});
				options.push_back(option); expected = nlohmann::json::array(); // Explicit oversized-cache fail-closed.
			}
			if (variant == 5)
			{
				options[0]["options"] = nlohmann::json::array();
				for (int i = 0; i != 129; ++i) options[0]["options"].push_back({{"value", "native-" + std::to_string(i)}});
				expected = nlohmann::json::array();
			}
			if (variant == 6)
			{
				options[0]["options"] = nlohmann::json::array(); expected = nlohmann::json::array();
				for (int i = 0; i != 40; ++i)
				{
					options[0]["options"].push_back({{"value", "native-" + std::to_string(i)}});
					if (i < 32) expected.push_back({{"id", "native-" + std::to_string(i)}});
				}
			}
			UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider.id, legacy, workspace, options));
			UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider.id, nlohmann::json::array({{{"id", "wrong-workspace"}}}), workspace + "-other", options));
			const auto targets = fixture.Targets(); const auto found = std::ranges::find_if(targets, [&](const auto& item) { return item.value("providerId", "") == provider.id; });
			UAM_ASSERT(found != targets.end()); UAM_ASSERT_EQ(found->at("models"), expected);
			app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles, "different-profile-context");
			const auto rebound = fixture.Targets(); const auto changed = std::ranges::find_if(rebound, [&](const auto& item) { return item.value("providerId", "") == provider.id; });
			UAM_ASSERT(changed != rebound.end()); UAM_ASSERT(changed->at("models").empty());
		}
}
