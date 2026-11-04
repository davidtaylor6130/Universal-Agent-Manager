#include "test_harness.h"
#include "common/chat/chat_repository.h"
#include "common/state/app_state.h"
#include "app/execution_routing_integration.h"
#include "common/provider/provider_profile.h"
#include "common/config/host_scoped_settings.h"
#include "common/config/settings_store.h"

using namespace uam_test;

UAM_TEST(HostScopedSettingsInheritWithoutLeakingAcrossProvidersOrHosts)
{
	AppSettings settings;
	settings.provider_chat_defaults["codex-cli"].model_id = "global-model";
	settings.host_scoped_settings["lab"].provider_chat_defaults["codex-cli"].model_id = "lab-model";
	settings.host_scoped_settings["lab"].default_new_chat_provider_id = "claude-code";
	UAM_ASSERT(uam::provider_chat_defaults::ForProvider(settings, "codex", "lab").model_id == "lab-model");
	UAM_ASSERT(uam::provider_chat_defaults::ForProvider(settings, "codex", "other").model_id == "global-model");
	UAM_ASSERT(uam::provider_chat_defaults::ForProvider(settings, "gemini-cli", "lab").model_id.empty());
	UAM_ASSERT(uam::host_scoped_settings::DefaultProvider(settings, "other") == settings.default_new_chat_provider_id);
	UAM_ASSERT(uam::host_scoped_settings::DefaultProvider(settings, "lab") == "claude-code");
}

UAM_TEST(HostScopedSettingsExplicitEmptyMcpOverridesGlobalList)
{
	AppSettings settings;
	settings.mcp_servers.push_back(McpServerConfiguration{});
	UAM_ASSERT(uam::host_scoped_settings::McpServers(settings, "lab").size() == 1);
	settings.host_scoped_settings["lab"].mcp_servers = std::vector<McpServerConfiguration>{};
	UAM_ASSERT(uam::host_scoped_settings::McpServers(settings, "lab").empty());
	UAM_ASSERT(uam::host_scoped_settings::McpServers(settings, "other").size() == 1);
}

UAM_TEST(HostScopedSettingsRejectUnknownLocalAndMalformedSaveScopes)
{
	AppSettings settings;
	ExecutionHost host;
	host.id = "lab";
	host.transport = "ssh";
	settings.execution_hosts = {uam::execution_hosts::LocalHost(), host};
	std::string scope;
	std::string error;
	UAM_ASSERT(uam::host_scoped_settings::ParseScope(settings, {{"scopeExecutionHostId", "lab"}}, scope, error));
	UAM_ASSERT(scope == "lab");
	for (const nlohmann::json& value : {nlohmann::json("missing"), nlohmann::json("local"), nlohmann::json(4), nlohmann::json(nullptr)})
		UAM_ASSERT(!uam::host_scoped_settings::ParseScope(settings, {{"scopeExecutionHostId", value}}, scope, error));
	UAM_ASSERT(uam::host_scoped_settings::ParseScope(settings, {{"scopeExecutionHostId", "*"}}, scope, error));
	UAM_ASSERT(scope.empty());
	UAM_ASSERT(settings.host_scoped_settings.empty());
}

UAM_TEST(HostScopedSettingsRoundTripKeepsEmptyOverrideAndAliases)
{
	TempDir temp{"uam-host-settings"};
	AppSettings settings;
	ExecutionHost host;
	host.id = "lab";
	host.transport = "ssh";
	host.ssh_alias = "private-fixture";
	settings.execution_hosts = {uam::execution_hosts::LocalHost(), host};
	settings.host_scoped_settings["lab"].provider_chat_defaults["codex"].model_id = "lab-model";
	settings.host_scoped_settings["lab"].mcp_servers = std::vector<McpServerConfiguration>{};
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT(uam::provider_chat_defaults::ForProvider(loaded, "codex-cli", "lab").model_id == "lab-model");
	UAM_ASSERT(loaded.host_scoped_settings.at("lab").mcp_servers.has_value());
	UAM_ASSERT(uam::host_scoped_settings::McpServers(loaded, "lab").empty());
}

UAM_TEST(HostScopedRoutingIdentityChangesOnlyTheConfiguredHost)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ProviderProfile& provider = app.provider_profiles.front();
	const std::string local_before = uam::ExecutionRoutingProviderIdentity(app, provider, "local");
	const std::string remote_before = uam::ExecutionRoutingProviderIdentity(app, provider, "lab");
	app.settings.host_scoped_settings["lab"].provider_chat_defaults[provider.id].model_id = "lab-model";
	UAM_ASSERT(uam::ExecutionRoutingProviderIdentity(app, provider, "lab") != remote_before);
	UAM_ASSERT(uam::ExecutionRoutingProviderIdentity(app, provider, "local") == local_before);
	app.settings.host_scoped_settings["lab"].mcp_servers = std::vector<McpServerConfiguration>{};
	const std::string remote_empty = uam::ExecutionRoutingProviderIdentity(app, provider, "lab");
	McpServerConfiguration server;
	server.id = "global-fixture";
	server.command = "never-launched";
	app.settings.mcp_servers.push_back(server);
	UAM_ASSERT(uam::ExecutionRoutingProviderIdentity(app, provider, "lab") == remote_empty);
	UAM_ASSERT(uam::ExecutionRoutingProviderIdentity(app, provider, "local") != local_before);
}

UAM_TEST(HostScopedRoutingDefaultsPreserveExplicitChoicesAndLegacyChats)
{
	AppSettings settings;
	ProviderChatDefaults& defaults = settings.host_scoped_settings["lab"].provider_chat_defaults["codex-cli"];
	defaults.model_id = "host-model";
	defaults.reviewer_model_id = "host-reviewer";
	defaults.reasoning_effort = "high";
	defaults.service_tier = "fast";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "lab";
	chat.routing_inherited_default_fields = {"modelId", "reviewerModelId", "reasoningEffort", "serviceTier"};
	chat.model_id = "explicit-edit-before-assignment";
	chat.approval_mode = "plan";
	chat.memory_enabled = false;
	chat.memory_level = "off";
	uam::host_scoped_settings::ApplyInheritedRoutingDefaults(settings, chat);
	UAM_ASSERT(chat.model_id == "explicit-edit-before-assignment");
	UAM_ASSERT(chat.reviewer_model_id == "host-reviewer");
	UAM_ASSERT(chat.reasoning_effort == "high");
	UAM_ASSERT(chat.approval_mode == "plan");
	UAM_ASSERT(!chat.memory_enabled && chat.memory_level == "off");
	ChatSession legacy;
	legacy.provider_id = "codex-cli";
	legacy.execution_host_id = "lab";
	uam::host_scoped_settings::ApplyInheritedRoutingDefaults(settings, legacy);
	UAM_ASSERT(legacy.model_id.empty() && legacy.reviewer_model_id.empty());
}

UAM_TEST(HostScopedRoutingInheritedIntentSurvivesPrivateSaveAndSummaryReload)
{
	TempDir temp{"uam-routing-settings-intent"};
	ChatSession chat;
	chat.id = "pending-route-fixture";
	chat.provider_id = "codex-cli";
	chat.routing_mode = "automatic";
	chat.routing_inherited_default_fields = {"modelId", "reviewerModelId", "reasoningEffort", "serviceTier"};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->routing_inherited_default_fields == chat.routing_inherited_default_fields);
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id);
	UAM_ASSERT(summary.has_value());
	UAM_ASSERT(summary->routing_inherited_default_fields == chat.routing_inherited_default_fields);
}

UAM_TEST(HostScopedSettingsProjectionKeepsDisabledServersAndExcludesForeignHosts)
{
	AppSettings settings;
	McpServerConfiguration shared;
	shared.id = "shared-fixture";
	shared.enabled = false;
	settings.mcp_servers.push_back(shared);
	McpServerConfiguration local;
	local.id = "local-fixture";
	local.execution_host_id = "local";
	settings.mcp_servers.push_back(local);
	McpServerConfiguration remote;
	remote.id = "remote-fixture";
	remote.execution_host_id = "lab";
	settings.mcp_servers.push_back(remote);
	const std::vector<McpServerConfiguration> visible = uam::host_scoped_settings::VisibleMcpServers(settings, "lab");
	UAM_ASSERT(visible.size() == 2);
	UAM_ASSERT(visible.front().id == "shared-fixture" && !visible.front().enabled);
	UAM_ASSERT(visible.back().id == "remote-fixture");
	UAM_ASSERT(settings.mcp_servers.size() == 3);
}
