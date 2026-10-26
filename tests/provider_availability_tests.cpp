#include "test_harness.h"
#include "app/model_discovery_service.h"
#include "common/config/line_value_codec.h"
#include "common/config/settings_frontend_json.h"
#include "app/provider_worker_command.h"
#include "app/shell_action_service.h"
#include "app/chat_lifecycle_service.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace
{
	struct AvailabilityFixture
	{
		TempDir temp{"uam-provider-availability"};
		ScopedEnvVar codex_home{"CODEX_HOME", (temp.root / "codex").string()};
		uam::AppState app;

		AvailabilityFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			ExecutionHost remote;
			remote.id = "lab";
			remote.transport = "ssh";
			remote.ssh_alias = "private-fixture";
			remote.platform = "linux";
			app.settings.execution_hosts.push_back(remote);
			const fs::path settings = temp.root / "settings.txt";
			UAM_ASSERT(SettingsStore::Save(settings, app.settings));
			const nlohmann::json overrides = nlohmann::json::array({
			    {{"providerId", "codex"}, {"executionHostId", "local"}, {"enabled", false}},
			    {{"providerId", "gemini-cli"}, {"executionHostId", "lab"}, {"enabled", false}}});
			UAM_ASSERT(uam::io::WriteTextFile(settings, ReadFile(settings) + "provider_availability=" + uam::EncodeLineValue(overrides.dump()) + "\n"));
			UAM_ASSERT(SettingsStore::Load(settings, app.settings).loaded);
		}

		ChatSession Chat(const std::string& provider, const std::string& host = "local")
		{
			ChatSession chat;
			chat.id = "availability-chat";
			chat.provider_id = provider;
			chat.execution_host_id = host;
			chat.workspace_directory = temp.root.string();
			chat.memory_enabled = false;
			return chat;
		}
	};
}

UAM_TEST(ProviderAvailabilityPersistsAliasesAndIndependentHostScopes)
{
	AvailabilityFixture fixture;
	const ProviderResolutionService providers;
	UAM_ASSERT(!providers.ChatProviderIsAvailable(fixture.app, fixture.Chat("codex-cli")));
	UAM_ASSERT(providers.ChatProviderIsAvailable(fixture.app, fixture.Chat("codex-cli", "lab")));
	UAM_ASSERT(providers.ChatProviderIsAvailable(fixture.app, fixture.Chat("gemini-cli")));
	UAM_ASSERT(!providers.ChatProviderIsAvailable(fixture.app, fixture.Chat("gemini-cli", "lab")));
	UAM_ASSERT(providers.ChatProviderUnavailableReason(fixture.app, fixture.Chat("codex-cli")).find("disabled") != std::string::npos);
	const fs::path settings = fixture.temp.root / "roundtrip.txt";
	UAM_ASSERT(SettingsStore::Save(settings, fixture.app.settings));
	AppSettings restored;
	UAM_ASSERT(SettingsStore::Load(settings, restored).loaded);
	fixture.app.settings = restored;
	UAM_ASSERT(!providers.ChatProviderIsAvailable(fixture.app, fixture.Chat("codex")));
	const nlohmann::json wire = uam::settings_frontend_json::SerializeCommonSettingsFields(restored);
	UAM_ASSERT(wire.contains("providerAvailability") && wire["providerAvailability"].is_array());
	UAM_ASSERT_EQ(wire["providerAvailability"].size(), std::size_t{2});
}

UAM_TEST(DisabledProviderAdmissionRejectsBeforeVersionOrProcessWork)
{
	AvailabilityFixture fixture;
	fixture.app.chats.push_back(fixture.Chat("codex-cli"));
	// Old code stops at compatibility, so a failing regression cannot invoke a provider.
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].supported = false;
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].installed_version = "0.0.1";
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, fixture.app.chats.front().id, "Private fixture prompt", &error));
	UAM_ASSERT(error.find("disabled") != std::string::npos);
	UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty());
	UAM_ASSERT(std::ranges::none_of(fixture.app.acp_sessions, [](const std::unique_ptr<uam::AcpSessionState>& session) { return session->running; }));
}

UAM_TEST(DisabledProviderStartupDiscoveryNeverLaunchesItsScope)
{
	AvailabilityFixture fixture;
	fixture.app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	fixture.app.settings.default_new_chat_provider_id = "codex-cli";
	fixture.app.folders.push_back(ChatFolder{"folder", "Private", fixture.temp.root.string(), false});
	fixture.app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	fixture.app.provider_model_catalog->Initialize(fixture.app.data_root, fixture.app.provider_profiles);
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	fixture.app.runtime_cli_versions_by_provider_id["codex-cli"].supported = true;
	int launched = 0;
	uam::StartupModelDiscoveryService discovery([&](uam::AppState&, const std::string&, const std::string&, const std::string&, std::string*) { ++launched; return false; });
	discovery.Initialize(fixture.app);
	(void)discovery.Poll(fixture.app);
	UAM_ASSERT_EQ(launched, 0);
}

UAM_TEST(ProviderAvailabilityInvalidPolicyIsAtomicAndReenableKeepsOtherHosts)
{
	AvailabilityFixture fixture;
	std::vector<ProviderAvailabilityOverride> parsed = fixture.app.settings.provider_availability;
	const nlohmann::json before = uam::provider_availability::Serialize(parsed);
	UAM_ASSERT(!uam::provider_availability::Parse(nlohmann::json::array({
	    {{"providerId", "codex"}, {"executionHostId", "local"}, {"enabled", "false"}}}), parsed));
	UAM_ASSERT_EQ(uam::provider_availability::Serialize(parsed), before);
	UAM_ASSERT(uam::provider_availability::Set(fixture.app.settings, "codex", "lab", false));
	UAM_ASSERT(uam::provider_availability::Set(fixture.app.settings, "codex-cli", "local", true));
	UAM_ASSERT(uam::provider_availability::IsEnabled(fixture.app.settings, "codex", "local"));
	UAM_ASSERT(!uam::provider_availability::IsEnabled(fixture.app.settings, "codex", "lab"));
	UAM_ASSERT(!uam::provider_availability::Set(fixture.app.settings, "unknown-provider", "local", false));
}

UAM_TEST(DisabledProviderWorkersAndExplicitDiscoveryRejectWithoutLaunch)
{
	AvailabilityFixture fixture;
	std::string error;
	const ProviderProfile* profile = ProviderProfileStore::FindById(fixture.app.provider_profiles, "codex-cli");
	UAM_ASSERT(profile != nullptr);
	UAM_ASSERT(uam::BuildProviderWorkerInvocation(fixture.app, *profile, fixture.app.settings, "private fixture", "", uam::ProviderWorkerPathMode::BasePath, &error).Empty());
	UAM_ASSERT(error.find("disabled") != std::string::npos);
	UAM_ASSERT(ProviderResolutionService().WorkerProviderForChat(fixture.app, fixture.Chat("codex")) == nullptr);
	UAM_ASSERT(ProviderResolutionService().WorkerProviderForChat(fixture.app, fixture.Chat("codex", "lab")) != nullptr);
	UAM_ASSERT(!uam::StartEphemeralAcpModelDiscovery(fixture.app, "codex", fixture.temp.root.string(), "local", &error));
	UAM_ASSERT(error.find("disabled") != std::string::npos);
	UAM_ASSERT(fixture.app.model_discovery_chats.empty());
}

UAM_TEST(DisabledProviderKeepsAcceptedQueuesAndRunningSessionAttached)
{
	AvailabilityFixture fixture;
	ChatSession chat = fixture.Chat("codex-cli");
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.running = true;
	session.session_ready = true;
	session.queued_prompt = "accepted but not delivered";
	uam::AcpQueuedUserPromptState entry;
	entry.id = "queued-intent";
	entry.text = "accepted next turn";
	session.queued_user_prompts.push_back(entry);
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(fixture.app, session, chat));
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, session, chat));
	UAM_ASSERT_EQ(session.queued_prompt, std::string("accepted but not delivered"));
	UAM_ASSERT_EQ(session.queued_user_prompts.front().id, entry.id);
	UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{0});
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	std::string error;
	UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(fixture.app, session, chat, &error));
	UAM_ASSERT(session.running);
	UAM_ASSERT(uam::provider_availability::Set(fixture.app.settings, "codex", "local", true));
	UAM_ASSERT(ProviderResolutionService().ChatProviderIsAvailable(fixture.app, chat));
}

UAM_TEST(DisabledProviderShellActionCreatesNoWorkspaceOrChat)
{
	AvailabilityFixture fixture;
	const fs::path workspace = fixture.temp.root / "shell-workspace";
	fs::create_directories(workspace);
	fs::create_directories(fixture.app.data_root);
	ShellAction action;
	action.id = "open-disabled";
	action.label = "Open in UAM";
	action.provider_id = "codex-cli";
	action.open_workspace = true;
	action.accepts_files = false;
	action.accepts_folders = true;
	fixture.app.shell_actions.push_back(action);
	bool recognized = false;
	std::string error;
	UAM_ASSERT(ShellActionService::QueueLaunchRequest(fixture.app.data_root, {"uam", "--uam-shell-action", action.id, uam::paths::Utf8PathString(workspace)}, &recognized, &error));
	(void)ShellActionService::ProcessPendingRequests(fixture.app);
	UAM_ASSERT(fixture.app.folders.empty());
	UAM_ASSERT(fixture.app.chats.empty());
	UAM_ASSERT(fixture.app.shell_action_notification.find("disabled") != std::string::npos);
}

UAM_TEST(DisabledProviderRegeneratingBranchIsRefusedBeforeSaving)
{
	AvailabilityFixture fixture;
	ChatSession chat = fixture.Chat("codex-cli");
	chat.messages.push_back({.role = MessageRole::User, .content = "private fixture prompt"});
	chat.messages_loaded = true;
	fixture.app.chats.push_back(chat);
	std::string branch_id, error;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(fixture.app, chat.id, 0, std::string("edited prompt"), &branch_id, &error));
	UAM_ASSERT(error.find("disabled") != std::string::npos);
	UAM_ASSERT(branch_id.empty());
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
}

UAM_TEST(DisabledProviderIsNeitherVersionCheckedNorAutomaticallyUpdated)
{
	AvailabilityFixture fixture;
	// Every other local provider is already checked, so the sweep can only start the disabled one.
	for (const ProviderProfile& profile : fixture.app.provider_profiles)
	{
		const std::string id = uam::provider_ids::CanonicalCliProviderLookupId(profile.id);
		if (id == "codex-cli") continue;
		uam::CliProviderVersionState& state = fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey(id, "local")];
		state.provider_id = id;
		state.execution_host = uam::execution_hosts::LocalHost();
		state.checked = true;
	}
	ProviderCliCompatibilityService().StartVersionCheck(fixture.app, false);
	UAM_ASSERT(!fixture.app.runtime_cli_version_check_task.running);
	UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.empty());
	UAM_ASSERT(!fixture.app.runtime_cli_versions_by_provider_id.contains(CliProviderVersionStateKey("codex-cli", "local")));
	// Gemini is disabled on the remote fixture computer; if the guard regressed, this would only record a failed state, never install.
	ProviderCliCompatibilityService().StartInstallProviderVersions(fixture.app, {{"gemini-cli", "latest", "lab"}}, false);
	UAM_ASSERT(!fixture.app.runtime_cli_versions_by_provider_id.contains(CliProviderVersionStateKey("gemini-cli", "lab")));
	// Re-enabling while another probe runs queues exactly that pair for the drain instead of dropping it.
	fixture.app.runtime_cli_version_check_task.running = true;
	ProviderCliCompatibilityService().CheckReenabledProvider(fixture.app, "codex", "local");
	UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.size() == 1);
	UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.front() == std::make_pair(std::string("codex-cli"), std::string("local")));
	UAM_ASSERT(fixture.app.runtime_cli_versions_by_provider_id.contains(CliProviderVersionStateKey("codex-cli", "local")));
	fixture.app.runtime_cli_version_check_task.running = false;
}
