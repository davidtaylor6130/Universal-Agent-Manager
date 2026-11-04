#include "test_harness.h"
#include "common/memory/memory_policy.h"
#include "common/config/provider_chat_defaults.h"
#include "cef/uam_query_handler_internal.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

UAM_TEST(DefaultMemoryPolicyResolvesDynamicPrecedence)
{
	AppSettings settings;
	settings.memory_level_default = "open";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.memory_level = "default";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("open"));
	settings.provider_chat_defaults["codex-cli"].memory_level = "readOnly";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("readOnly"));
	settings.provider_chat_defaults["codex-cli"].memory_level = "off";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
	settings.provider_chat_defaults["codex-cli"].memory_level = "default";
	settings.provider_chat_defaults["codex-cli"].memory_enabled = false;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("open"));
	settings.memory_level_default = "balanced";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("balanced"));
	chat.memory_level = "strict";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("strict"));
	chat.memory_level = "off";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
	chat.memory_level = "";
	chat.memory_enabled = false;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
	chat.memory_enabled = true;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("strict"));
}

UAM_TEST(DefaultMemoryPolicyPersistsSelectionAcrossInheritedOff)
{
	TempDir temp("uam-memory-policy-default-persistence");
	AppSettings settings;
	settings.memory_level_default = "off";
	settings.provider_chat_defaults["codex-cli"] = uam::query_handler_internal::DefaultsFromPayload(
		nlohmann::json{{"memoryLevel", "default"}}, ProviderChatDefaults{}, "codex-cli");
	UAM_ASSERT_EQ(settings.provider_chat_defaults.at("codex-cli").memory_level, std::string("default"));
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT_EQ(loaded.provider_chat_defaults.at("codex-cli").memory_level, std::string("default"));
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.memory_level = "default";
	chat.memory_enabled = false;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root / "data", chat));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root / "data", chat.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->memory_level, std::string("default"));
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, *restored), std::string("off"));
	uam::AppState app;
	app.settings = loaded;
	app.chats.push_back(*restored);
	const nlohmann::json state = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(state["chats"][0].value("memoryLevel", ""), std::string("default"));
	UAM_ASSERT(!state["chats"][0].value("memoryEnabled", true));

	loaded.memory_level_default = "readOnly";
	loaded.memory_enabled_default = true;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, *restored), std::string("readOnly"));
	const nlohmann::json frontend = uam::settings_frontend_json::SerializeCommonSettingsFields(loaded);
	UAM_ASSERT_EQ(frontend["providerChatDefaults"]["codex-cli"].value("memoryLevel", ""), std::string("default"));
}

UAM_TEST(InheritedMemoryPolicyRejectsStaleCreationAndKeepsReadOnly)
{
	TempDir temp("uam-memory-policy-stale-output");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.memory_level_default = "strict";
	ChatSession chat = ChatDomainService().CreateNewChat("", "gemini-cli");
	chat.memory_level = "default";
	chat.workspace_directory = (temp.root / "workspace").string();
	fs::create_directories(chat.workspace_directory);
	chat.messages.push_back({MessageRole::User, "Remember that fixes need tests.", "now"});
	const std::string output = R"({"memories":[{"scope":"local","category":"Lessons/User_Lessons","title":"Test preference","memory":"Critical fixes require tests.","evidence":"User said: Remember that fixes need tests.","confidence":"high"}]})";
	std::string error;
	UAM_ASSERT(MemoryService::PrepareWorkerOutput(app.data_root, chat, chat.workspace_directory, output, -1, &error) == nullptr);
	ChatSession snapshot = chat;
	snapshot.memory_level = uam::memory_policy::Resolve(app.settings, chat);
	const std::shared_ptr<PreparedMemoryOutput> prepared = MemoryService::PrepareWorkerOutput(app.data_root, snapshot, chat.workspace_directory, output, -1, &error);
	UAM_ASSERT(prepared != nullptr);
	app.settings.memory_level_default = "readOnly";
	UAM_ASSERT(!MemoryService::PublishPreparedWorkerOutput(app, chat, chat.workspace_directory, prepared, &error));
	UAM_ASSERT(!MemoryService::ApplyWorkerOutput(app, chat, chat.workspace_directory, output, -1, &error));
	UAM_ASSERT_EQ(chat.memory_last_processed_message_count, 0);
	app.chats.push_back(chat);
	UAM_ASSERT(MemoryService::ListManualScanCandidates(app).empty());
	app.settings.memory_level_default = "strict";
	UAM_ASSERT(MemoryService::ApplyWorkerOutput(app, app.chats.front(), chat.workspace_directory, output, -1, &error));
	const std::string recall = MemoryService::BuildRecallPreface(app, app.chats.front(), "Continue");
	app.settings.memory_level_default = "readOnly";
	UAM_ASSERT_EQ(MemoryService::BuildRecallPreface(app, app.chats.front(), "Continue"), recall);
	app.settings.memory_level_default = "off";
	UAM_ASSERT(MemoryService::BuildRecallPreface(app, app.chats.front(), "Continue").empty());
}

UAM_TEST(InheritedMemoryPolicyCancelsPendingRecallBeforeStartupWait)
{
	TempDir temp("uam-memory-policy-pending-recall");
	for (const bool provider_override : {false, true})
	{
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.settings.memory_recall_budget_bytes = 2048;
		ChatSession chat;
		chat.id = "default-recall-owner";
		chat.provider_id = "gemini-cli";
		chat.workspace_directory = temp.root.string();
		chat.memory_level = "default";
		uam::AcpSessionState session;
		session.provider_id = chat.provider_id;
		session.session_id = "native-owner";
		session.queued_prompt = "accepted prompt";
		session.memory_recall_position = 0;
		session.memory_recall_query = session.queued_prompt;
		session.startup_model_request_id = 1;
		const std::shared_ptr<uam::MemoryRecallPreparation> state = std::make_shared<uam::MemoryRecallPreparation>();
		state->identity = {app.data_root, uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), chat.id,
			session.provider_id, session.session_id, session.queued_prompt, session.memory_recall_query, 0, session.turn_serial, 2048, "strict", false};
		session.memory_recall_preparation = std::make_unique<uam::MemoryRecallPreparationLease>(state);
		const std::string configuration = uam::acp_detail::PromptDeliveryConfiguration(app, session, chat);
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(!state->cancel.stop_requested());
		if (provider_override) app.settings.provider_chat_defaults[chat.provider_id].memory_level = "readOnly";
		else app.settings.memory_level_default = "readOnly";
		UAM_ASSERT(configuration != uam::acp_detail::PromptDeliveryConfiguration(app, session, chat));
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT(session.memory_recall_preparation == nullptr);
	}
}

UAM_TEST(InheritedMemoryPolicyRejectsRemoteCompletionFromOldDefault)
{
	for (bool provider_changed : {false, true})
	{
		TempDir temp("uam-remote-memory-completion");
		uam::AppState app;
		app.data_root = temp.root;
		ExecutionHost host;
		host.id = "owned-remote";
		host.platform = "linux";
		host.ssh_alias = "owned-no-network";
		app.settings.execution_hosts.push_back(host);
		ChatSession chat;
		chat.id = "owned-memory";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = host.id;
		chat.workspace_directory = "/owned-workspace";
		chat.memory_level = "default";
		chat.messages.push_back({MessageRole::User, "Fixture preference."});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		app.chats.push_back(chat);
		uam::AsyncMemoryExtractionTask task;
		task.data_root = app.data_root;
		task.chat_id = chat.id;
		task.owner_provider_id = chat.provider_id;
		task.memory_level = "strict";
		task.remote_host = host;
		task.remote_workspace = chat.workspace_directory;
		task.message_count = 1;
		app.memory_retry_not_before_by_chat_id[chat.id] = uam::GetAppTimeSeconds() + 60.0;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->result.ok = true;
		task.state->completed.store(true);
		task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
		task.extraction_state->remote_applied = true;
		app.memory_extraction_tasks.push_back(std::move(task));
		if (provider_changed) app.settings.provider_chat_defaults[chat.provider_id].memory_level = "balanced";
		else app.settings.memory_level_default = "balanced";
		(void)MemoryService::ProcessDueMemoryWork(app);
		UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, 0);
		UAM_ASSERT(app.memory_extraction_tasks.empty());
	}
}

UAM_TEST(InheritedMemoryPolicyCancelsRemoteContextAtSameRecallBudget)
{
	TempDir temp("uam-memory-policy-remote-recall");
	uam::AppState app;
	app.settings.memory_recall_budget_bytes = 2048;
	ExecutionHost host;
	host.id = "owned-remote";
	host.platform = "linux";
	host.ssh_alias = "owned-no-network";
	app.settings.execution_hosts.push_back(host);
	ChatSession chat;
	chat.id = "default-remote-recall";
	chat.provider_id = "gemini-cli";
	chat.execution_host_id = host.id;
	chat.workspace_directory = "/owned-workspace";
	chat.memory_level = "default";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.queued_prompt = "accepted prompt";
	session.startup_model_request_id = 1;
	const std::shared_ptr<uam::RemoteContextPreparation> preparation = std::make_shared<uam::RemoteContextPreparation>();
	preparation->host = host;
	preparation->workspace = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
	preparation->prompt = session.queued_prompt;
	preparation->memory_budget = 2048;
	preparation->memory_level = "strict";
	session.remote_context_preparation = preparation;
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(!preparation->cancel.stop_requested());
	app.settings.memory_level_default = "readOnly";
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(preparation->cancel.stop_requested());
	UAM_ASSERT(session.remote_context_preparation == nullptr);
}

UAM_TEST(InheritedMemoryPolicyCannotMarkOldColdLowSignalWorkProcessed)
{
	TempDir temp("uam-memory-policy-cold-scan");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.memory_idle_delay_seconds = -1;
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.memory_level = "default";
	chat.workspace_directory = temp.root.string();
	chat.messages.push_back({MessageRole::User, "Hello", "now"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats = ChatRepository::LoadLocalChatSummaries(app.data_root);
	uam::AsyncMemoryExtractionTask task;
	task.data_root = app.data_root;
	task.chat_id = chat.id;
	task.memory_level = "strict";
	task.prepared_from_cold = true;
	task.source_persisted_message_count = app.chats.front().persisted_message_count;
	task.source_persisted_messages_digest = app.chats.front().persisted_messages_digest;
	task.workspace_root = uam::paths::ResolveControllerWorkspaceRootPath(app, app.chats.front());
	task.state = std::make_shared<AsyncProcessTaskState>();
	task.state->completed.store(true);
	task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
	task.extraction_state->hydrated_message_count = 1;
	task.extraction_state->hydrated_messages_digest = task.source_persisted_messages_digest;
	task.extraction_state->low_signal_skip = true;
	app.memory_extraction_tasks.push_back(std::move(task));
	app.memory_retry_not_before_by_chat_id[chat.id] = uam::GetAppTimeSeconds() + 60.0;
	app.settings.memory_level_default = "open";
	UAM_ASSERT(MemoryService::ProcessDueMemoryWork(app));
	UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, 0);
	UAM_ASSERT(app.memory_extraction_tasks.empty());
}

UAM_TEST(MissingProviderMemoryDefaultsInheritAppPolicyAfterReload)
{
	TempDir temp("uam-memory-initial-defaults");
	AppSettings settings;
	settings.memory_level_default = "strict";
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	loaded.memory_level_default = "off";
	loaded.memory_enabled_default = false;
	for (const std::string& provider : {"gemini-cli", "codex-cli", "opencode-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
	{
		ChatSession chat;
		chat.provider_id = provider;
		chat.memory_level = "default";
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, chat), std::string("off"));
	}
}

UAM_TEST(NewChatMemorySelectionInheritsChangingProviderAndAppDefaults)
{
	AppSettings settings;
	settings.memory_level_default = "strict";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	uam::provider_chat_defaults::ApplyToChat(settings, chat);
	UAM_ASSERT_EQ(chat.memory_level, std::string("default"));
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("strict"));
	settings.memory_level_default = "off";
	settings.memory_enabled_default = false;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
	settings.provider_chat_defaults[chat.provider_id].memory_level = "strict";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("strict"));
	ChatSession provider_chat;
	provider_chat.provider_id = chat.provider_id;
	uam::provider_chat_defaults::ApplyToChat(settings, provider_chat);
	UAM_ASSERT_EQ(provider_chat.memory_level, std::string("default"));
	settings.provider_chat_defaults[chat.provider_id].memory_level = "off";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, provider_chat), std::string("off"));
}

UAM_TEST(ExplicitMemoryOverridesSurviveInheritedDefaultChangesAndReload)
{
	TempDir temp("uam-memory-explicit-defaults");
	AppSettings settings;
	settings.memory_level_default = "off";
	settings.memory_enabled_default = false;
	settings.provider_chat_defaults["codex-cli"].memory_level = "strict";
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	ChatSession inherited;
	inherited.provider_id = "codex-cli";
	inherited.memory_level = "default";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, inherited), std::string("strict"));
	ChatSession explicit_chat;
	explicit_chat.id = "explicit-memory-policy";
	explicit_chat.provider_id = "codex-cli";
	explicit_chat.memory_level = "strict";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root / "data", explicit_chat));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root / "data", explicit_chat.id);
	UAM_ASSERT(restored.has_value());
	loaded.provider_chat_defaults["codex-cli"].memory_level = "off";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, *restored), std::string("strict"));
}

UAM_TEST(ProviderSwitchPreservesExplicitAndInheritedMemorySelections)
{
	for (const std::string& level : {"off", "strict", "readOnly", "default"})
	{
		TempDir temp("uam-memory-provider-switch");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		UAM_ASSERT(app.provider_profiles.size() >= 2);
		const std::string source = app.provider_profiles[0].id;
		const std::string target = app.provider_profiles[1].id;
		app.settings.provider_chat_defaults[target].memory_level = "open";
		ChatSession chat;
		chat.id = "memory-switch";
		chat.provider_id = source;
		chat.memory_level = level;
		chat.memory_enabled = level != "off";
		app.chats.push_back(chat);
		UAM_ASSERT_EQ(uam::SwitchChatProvider(app, chat.id, target), uam::ChatProviderSwitchResult::Changed);
		const ChatSession& changed = app.chats.front();
		UAM_ASSERT_EQ(changed.memory_level, level);
		UAM_ASSERT_EQ(changed.memory_enabled, chat.memory_enabled);
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(app.settings, changed), level == "default" ? std::string("open") : level);
		const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(restored.has_value());
		UAM_ASSERT_EQ(restored->memory_level, level);
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(app.settings, *restored), level == "default" ? std::string("open") : level);
	}
}

UAM_TEST(HostScopedMemoryPolicyResolvesOpposingAppDefaults)
{
	for (const std::string& provider : {"gemini-cli", "codex-cli", "opencode-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
	{
		AppSettings settings;
		settings.memory_level_default = "strict";
		settings.host_scoped_settings["lab"].provider_chat_defaults[provider].memory_level = "off";
		ChatSession chat;
		chat.provider_id = provider;
		chat.execution_host_id = "lab";
		chat.memory_level = "default";
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
		settings.memory_level_default = "off";
		settings.memory_enabled_default = false;
		settings.host_scoped_settings["lab"].provider_chat_defaults[provider].memory_level = "strict";
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("strict"));
	}
}

UAM_TEST(HostScopedMemoryPolicyFallbackAndAliasesStayIsolated)
{
	AppSettings settings;
	settings.memory_level_default = "balanced";
	settings.provider_chat_defaults["codex-cli"].memory_level = "readOnly";
	settings.host_scoped_settings["lab"].provider_chat_defaults["codex-cli"].memory_level = "off";
	settings.host_scoped_settings["lab"].provider_chat_defaults["gemini-cli"].memory_level = "open";
	ChatSession chat;
	chat.provider_id = "codex";
	chat.memory_level = "default";
	chat.execution_host_id = "other";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("readOnly"));
	chat.execution_host_id = "local";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("readOnly"));
	chat.execution_host_id = "lab";
	chat.provider_id = "claude-cli";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("balanced"));
	chat.provider_id = "codex";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
	chat.provider_id = "gemini-cli";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("open"));
}

UAM_TEST(HostScopedMemoryPolicyDefaultRecordInheritsAppPolicy)
{
	AppSettings settings;
	settings.memory_level_default = "balanced";
	settings.provider_chat_defaults["codex-cli"].memory_level = "off";
	settings.host_scoped_settings["lab"].provider_chat_defaults["codex-cli"].memory_level = "default";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "lab";
	chat.memory_level = "default";
	UAM_ASSERT_EQ(uam::provider_chat_defaults::ForProvider(settings, chat.provider_id, chat.execution_host_id).memory_level, std::string("default"));
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("balanced"));
	settings.memory_level_default = "readOnly";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("readOnly"));
	chat.execution_host_id = "other";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
}

UAM_TEST(HostScopedMemoryPolicyPreservesExplicitChatSelections)
{
	AppSettings settings;
	settings.memory_level_default = "open";
	settings.provider_chat_defaults["codex-cli"].memory_level = "balanced";
	settings.host_scoped_settings["lab"].provider_chat_defaults["codex-cli"].memory_level = "off";
	ChatSession chat;
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "lab";
	for (const std::string& level : {"off", "strict", "readOnly", "balanced", "open"})
	{
		chat.memory_level = level;
		chat.memory_enabled = level != "off";
		UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), level);
	}
	chat.memory_level.clear();
	chat.memory_enabled = false;
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(settings, chat), std::string("off"));
}

UAM_TEST(HostScopedMemoryPolicyRoundTripPreservesInheritedSelection)
{
	TempDir temp("uam-host-memory-policy-settings");
	AppSettings settings;
	settings.memory_level_default = "strict";
	ExecutionHost host;
	host.id = "lab";
	host.transport = "ssh";
	host.ssh_alias = "private-fixture-never-connected";
	settings.execution_hosts.push_back(host);
	settings.host_scoped_settings[host.id].provider_chat_defaults["codex"].memory_level = "off";
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT_EQ(uam::provider_chat_defaults::ForProvider(loaded, "codex-cli", host.id).memory_level, std::string("off"));
	ChatSession chat;
	chat.id = "host-memory-inherited";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = host.id;
	uam::provider_chat_defaults::ApplyToChat(loaded, chat);
	UAM_ASSERT_EQ(chat.memory_level, std::string("default"));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root / "data", chat));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root / "data", chat.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->memory_level, std::string("default"));
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, *restored), std::string("off"));
	ProviderChatDefaults& changed_defaults = loaded.host_scoped_settings[host.id].provider_chat_defaults["codex-cli"];
	changed_defaults = uam::query_handler_internal::DefaultsFromPayload(nlohmann::json{{"memoryLevel", "readOnly"}, {"memoryEnabled", true}}, changed_defaults, "codex-cli");
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, *restored), std::string("readOnly"));
	ChatSession local = *restored;
	local.execution_host_id = "local";
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(loaded, local), std::string("strict"));
}

UAM_TEST(HostScopedMemoryPolicyRejectsStaleRemoteExtractionCompletion)
{
	TempDir temp("uam-host-memory-stale-extraction");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.memory_level_default = "strict";
	ExecutionHost host;
	host.id = "lab";
	host.platform = "linux";
	host.transport = "ssh";
	host.ssh_alias = "private-fixture-never-connected";
	app.settings.execution_hosts.push_back(host);
	ChatSession chat;
	chat.id = "host-memory-stale-extraction";
	chat.provider_id = "claude-cli";
	chat.execution_host_id = host.id;
	chat.workspace_directory = "/private-fixture-workspace";
	chat.memory_level = "default";
	chat.messages.push_back({MessageRole::User, "Fixture preference."});
	UAM_ASSERT_EQ(uam::memory_policy::Resolve(app.settings, chat), std::string("strict"));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	app.chats.push_back(chat);
	uam::AsyncMemoryExtractionTask task;
	task.data_root = app.data_root;
	task.chat_id = chat.id;
	task.owner_provider_id = chat.provider_id;
	task.memory_level = "strict";
	task.remote_host = host;
	task.remote_workspace = chat.workspace_directory;
	task.message_count = 1;
	task.state = std::make_shared<AsyncProcessTaskState>();
	task.state->result.ok = true;
	task.state->completed.store(true);
	task.extraction_state = std::make_shared<uam::AsyncMemoryExtractionState>();
	task.extraction_state->remote_applied = true;
	app.memory_extraction_tasks.push_back(std::move(task));
	app.memory_retry_not_before_by_chat_id[chat.id] = uam::GetAppTimeSeconds() + 60.0;
	app.settings.host_scoped_settings[host.id].provider_chat_defaults[chat.provider_id].memory_level = "off";
	(void)MemoryService::ProcessDueMemoryWork(app);
	UAM_ASSERT_EQ(app.chats.front().memory_last_processed_message_count, 0);
	UAM_ASSERT(app.memory_extraction_tasks.empty());
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->memory_last_processed_message_count, 0);
}

UAM_TEST(HostScopedMemoryPolicyCancelsPendingRemoteRecallBeforeStartupWait)
{
	TempDir temp("uam-host-memory-pending-recall");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.memory_level_default = "strict";
	app.settings.memory_recall_budget_bytes = 2048;
	ExecutionHost host;
	host.id = "lab";
	host.platform = "linux";
	host.transport = "ssh";
	host.ssh_alias = "private-fixture-never-connected";
	app.settings.execution_hosts.push_back(host);
	ChatSession chat;
	chat.id = "host-memory-pending-recall";
	chat.provider_id = "gemini-cli";
	chat.execution_host_id = host.id;
	chat.workspace_directory = "/private-fixture-workspace";
	chat.memory_level = "default";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.queued_prompt = "accepted fixture prompt";
	session.startup_model_request_id = 1;
	const std::shared_ptr<uam::RemoteContextPreparation> preparation = std::make_shared<uam::RemoteContextPreparation>();
	preparation->host = host;
	preparation->workspace = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
	preparation->prompt = session.queued_prompt;
	preparation->memory_budget = 2048;
	preparation->memory_level = "strict";
	session.remote_context_preparation = preparation;
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(!preparation->cancel.stop_requested());
	UAM_ASSERT(session.remote_context_preparation == preparation);
	app.settings.host_scoped_settings[host.id].provider_chat_defaults[chat.provider_id].memory_level = "off";
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(preparation->cancel.stop_requested());
	UAM_ASSERT(session.remote_context_preparation == nullptr);
	UAM_ASSERT_EQ(session.queued_prompt, std::string("accepted fixture prompt"));
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(app.remote_context_tasks.empty());
}
