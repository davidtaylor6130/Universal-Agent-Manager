#include "test_harness.h"
#include "common/memory/memory_policy.h"
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
