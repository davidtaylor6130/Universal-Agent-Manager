#include "common/companion/protocol.h"
#include "common/provider/copilot_native_history.h"
#include "core/chat_import_utils.h"
#include "cef/uam_bridge_request.h"
#include "common/platform/platform_speech_output.h"
#include "test_harness.h"
#include "common/platform/file_explorer_application.h"
#include "common/platform/powershell_selection.h"
#include "remote/remote_workspace_terminal.h"
#include "common/config/custom_icon.h"
#include "common/config/companion_network.h"
#include "app/chat_lifecycle_service.h"
#include "app/chat_search_service.h"
#include "app/local_chat_bundle_service.h"
#include "app/persistence_coordinator.h"
#include "app/transcript_retention_service.h"
#include "app/startup_native_import_service.h"
#include "app/runtime_orchestration_internal.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_client.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "common/config/mcp_server_config.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"

#include <cstdio>
#include <fstream>
#include <future>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <versionhelpers.h>
#endif

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

UAM_TEST(StateFingerprintLargeCatalogPerformance)
{
	TempDir temp("uam-fingerprint-performance");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	nlohmann::json models = nlohmann::json::array();
	for (int index = 0; index < 256; ++index)
		models.push_back({{"id", "fixture/model-" + std::to_string(index)}, {"name", "Fixture model " + std::to_string(index)}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", models, temp.root.generic_string()));
	for (int index = 0; index < 643; ++index)
	{
		ChatSession chat;
		chat.id = "fixture-chat-" + std::to_string(index);
		chat.provider_id = "claude-cli";
		chat.workspace_directory = temp.root.generic_string();
		chat.messages_loaded = false;
		chat.persisted_message_count = 100;
		chat.persisted_messages_digest = "unchanged-transcript";
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	for (const bool compact : {false, true})
	{
		std::vector<double> durations;
		for (int iteration = 0; iteration < 21; ++iteration)
		{
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app, compact);
			const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			UAM_ASSERT_EQ(fingerprint["chats"].size(), static_cast<std::size_t>(643));
			if (!compact) UAM_ASSERT(fingerprint["chats"][0]["acpSession"]["availableModels"].size() == models.size());
			UAM_ASSERT_EQ(fingerprint["chats"][0]["acpSession"].contains("catalogDigest"), compact);
			UAM_ASSERT_EQ(fingerprint["chats"][1]["acpSession"].contains("catalogDigest"), compact);
			if (iteration > 0) durations.push_back(elapsed);
		}
		std::sort(durations.begin(), durations.end());
		std::cout << "[fingerprint fixture] 643 chats, 256 models, unloaded transcripts, compact=" << compact
		          << ": p50=" << (durations[9] + durations[10]) / 2.0 << "ms p95=" << durations[18] << "ms\n";
	}
	const nlohmann::json original = uam::StateSerializer::SerializeFingerprint(app, true);
	models.push_back({{"id", "fixture/added-model"}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels("claude-cli", models, temp.root.generic_string()));
	const nlohmann::json refreshed = uam::StateSerializer::SerializeFingerprint(app, true);
	UAM_ASSERT(original["chats"][1]["acpSession"]["catalogDigest"] != refreshed["chats"][1]["acpSession"]["catalogDigest"]);
	const nlohmann::json patch = nlohmann::json::parse(uam::StatePatchForTests(app, app));
	UAM_ASSERT(!patch["data"].contains("chats"));
	uam::AppState before;
	before.data_root = temp.root;
	before.chats = app.chats;
	before.selected_chat_index = app.selected_chat_index;
	before.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	before.provider_model_catalog->Initialize(before.data_root);
	app.chats[1].title = "changed inactive chat";
	const nlohmann::json changed_patch = nlohmann::json::parse(uam::StatePatchForTests(before, app));
	UAM_ASSERT_EQ(changed_patch["data"]["chats"].size(), static_cast<std::size_t>(1));
	const nlohmann::json& hydrated = changed_patch["data"]["chats"][0];
	UAM_ASSERT_EQ(hydrated["id"], app.chats[1].id);
	UAM_ASSERT(hydrated["acpSession"]["availableModels"].size() == models.size());
	UAM_ASSERT(!hydrated["acpSession"].contains("catalogDigest"));
	before.chats = app.chats;
	app.selected_chat_index = 1;
	const nlohmann::json selected_patch = nlohmann::json::parse(uam::StatePatchForTests(before, app));
	UAM_ASSERT_EQ(selected_patch["data"]["selectedChatId"], app.chats[1].id);
	UAM_ASSERT(!selected_patch["data"].contains("chats"));
	UAM_ASSERT(uam::StateSerializer::Serialize(app)["chats"][1]["acpSession"]["availableModels"].size() == models.size());
	app.chats[2].messages_loaded = true;
	app.chats[2].messages.push_back(Message{MessageRole::Assistant, "before"});
	before.chats = app.chats;
	before.selected_chat_index = app.selected_chat_index;
	app.chats[2].messages[0].content = "edited";
	const nlohmann::json edited_patch = nlohmann::json::parse(uam::StatePatchForTests(before, app));
	UAM_ASSERT_EQ(edited_patch["data"]["chats"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(edited_patch["data"]["chats"][0]["id"], app.chats[2].id);
	UAM_ASSERT(edited_patch["data"]["chats"][0]["messagesDigest"] != uam::StateSerializer::MessageDigest(before.chats[2]));

}

UAM_TEST(StateSerializerIncludesChatModelId)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.title = "Model chat";
	chat.provider_id = "gemini-cli";
	chat.model_id = "auto-gemini-3";
	chat.reviewer_model_id = "gemini-3-pro";
	chat.approval_mode = "plan";
	chat.pinned = true;
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;
	app.settings.markdown_store_directory = " /tmp/serializer-markdown ";

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["settings"].value("markdownStoreDirectory", ""), std::string("/tmp/serializer-markdown"));
	UAM_ASSERT(serialized["chats"][0].value("pinned", false));
	UAM_ASSERT_EQ(serialized["chats"][0].value("modelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(serialized["chats"][0].value("reviewerModelId", ""), std::string("gemini-3-pro"));
	UAM_ASSERT_EQ(serialized["chats"][0].value("approvalMode", ""), std::string("plan"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("currentModeId", ""), std::string("plan"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("currentModelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("lifecycleState", ""), std::string("stopped"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("waitSeconds", -1), 0);
	UAM_ASSERT(serialized["chats"][0]["acpSession"]["pendingPermission"].is_null());
	UAM_ASSERT(serialized["chats"][0]["acpSession"]["pendingUserInput"].is_null());

	const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(fingerprint["settings"].value("markdownStoreDirectory", ""), std::string("/tmp/serializer-markdown"));
	UAM_ASSERT(fingerprint["chats"][0].value("pinned", false));
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("modelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("reviewerModelId", ""), std::string("gemini-3-pro"));
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("approvalMode", ""), std::string("plan"));
}

UAM_TEST(StateSerializerSummaryOnlyOmitsSelectedMessagesPreservingRuntimeMetadata)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-summary-only";
	chat.title = "Summary chat";
	chat.provider_id = "opencode-cli";
	chat.messages_loaded = true;
	chat.messages.push_back(Message{MessageRole::User, "Keep this out of the startup payload."});
	chat.messages.push_back(Message{MessageRole::Assistant, "The transcript loads on demand."});
	chat.computer_use_enabled = true;
	chat.model_id = "opencode/normal";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	const nlohmann::json full = uam::StateSerializer::Serialize(app);
	const nlohmann::json summary = uam::StateSerializer::Serialize(app, true);
	const nlohmann::json& full_chat = full["chats"][0];
	const nlohmann::json& summary_chat = summary["chats"][0];

	UAM_ASSERT_EQ(full_chat["messages"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT(!summary_chat.contains("messages"));
	UAM_ASSERT_EQ(summary_chat["messageCount"].get<std::size_t>(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(summary_chat["messagesDigest"], full_chat["messagesDigest"]);
	UAM_ASSERT_EQ(summary_chat["modelId"], full_chat["modelId"]);
	UAM_ASSERT_EQ(summary_chat["computerUseEnabled"], full_chat["computerUseEnabled"]);
	UAM_ASSERT(summary_chat.contains("acpSession"));
	UAM_ASSERT(summary_chat.contains("cliTerminal"));
}

UAM_TEST(StateSerializerCachesCatalogsPerScopeAndRefreshesBetweenSerializations)
{
	TempDir temp("uam-serializer-catalog-cache");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	ExecutionHost host_a;
	host_a.id = "host-a";
	host_a.transport = "ssh";
	host_a.ssh_alias = "fixture-a";
	ExecutionHost host_b = host_a;
	host_b.id = "host-b";
	host_b.ssh_alias = "fixture-b";
	app.provider_model_catalog->SetExecutionHostContext({host_a, host_b});
	const std::string provider = uam::provider_ids::kOpenCodeCli;
	const std::string workspace = (temp.root / "workspace").generic_string();
	const nlohmann::json first_models = nlohmann::json::array({{{"id", "first/model"}}});
	const nlohmann::json second_models = nlohmann::json::array({{{"id", "second/model"}}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, first_models, workspace, {}, "host-a"));
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, second_models, workspace, {}, "host-b"));

	ChatSession first;
	first.id = "chat-host-a";
	first.provider_id = provider;
	first.workspace_directory = workspace;
	first.execution_host_id = "host-a";
	ChatSession second = first;
	second.id = "chat-host-b";
	second.execution_host_id = "host-b";
	app.chats = {first, second};

	const nlohmann::json initial = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(initial["chats"][0]["acpSession"]["availableModels"], first_models);
	UAM_ASSERT_EQ(initial["chats"][1]["acpSession"]["availableModels"], second_models);

	const nlohmann::json refreshed_models = nlohmann::json::array({{{"id", "refreshed/model"}}});
	UAM_ASSERT(app.provider_model_catalog->RememberSuccessfulModels(provider, refreshed_models, workspace, {}, "host-a"));
	const nlohmann::json refreshed = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(refreshed["chats"][0]["acpSession"]["availableModels"], refreshed_models);
	UAM_ASSERT_EQ(refreshed["chats"][1]["acpSession"]["availableModels"], second_models);
	app.chats.push_back(second);
	app.chats.back().id = "selected-other-chat";
	app.selected_chat_index = 2;
	const nlohmann::json compact = uam::StateSerializer::SerializeFingerprint(app, true);
	UAM_ASSERT(compact["chats"][0]["acpSession"]["catalogDigest"] != compact["chats"][1]["acpSession"]["catalogDigest"]);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeChatSummary(app, app.chats[0])["acpSession"]["availableModels"], refreshed_models);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeChatSummary(app, app.chats[1])["acpSession"]["availableModels"], second_models);
}

UAM_TEST(InternalAgentChatsStayInRuntimeStateButNeverEnterTheSidebarPayload)
{
	uam::AppState app;
	ChatSession managed;
	managed.id = "managed-child";
	managed.title = "Managed child";
	managed.updated_at = "2026-01-02T00:00:00.000Z";
	managed.agent_run_id = "11111111-1111-4111-8111-111111111111";
	ChatSession visible;
	visible.id = "visible-chat";
	visible.title = "Visible chat";
	visible.updated_at = "2026-01-01T00:00:00.000Z";
	ChatSession legacy_goal_iteration;
	legacy_goal_iteration.id = "legacy-goal-iteration";
	legacy_goal_iteration.title = "Goal 1 - Iteration 1";
	legacy_goal_iteration.goal_owner_chat_id = visible.id;
	legacy_goal_iteration.updated_at = "2026-01-03T00:00:00.000Z";

	uam::runtime_orch_impl::ReplaceAppChatsWithNormalized(app, {managed, visible, legacy_goal_iteration});
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(3));
	app.selected_chat_index = ChatDomainService().FindChatIndexById(app, legacy_goal_iteration.id);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(serialized["chats"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(serialized["chats"][0].value("id", ""), visible.id);
	UAM_ASSERT_EQ(serialized.value("selectedChatId", ""), visible.id);
	UAM_ASSERT_EQ(fingerprint["chats"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(fingerprint["chats"][0].value("id", ""), visible.id);
	UAM_ASSERT_EQ(fingerprint.value("selectedChatId", ""), visible.id);
}

UAM_TEST(StateSerializerMarksMissingWorkspaceFoldersWithoutChangingMetadata)
{
	TempDir temp("uam-missing-workspace");
	ChatFolder folder;
	folder.id = "project";
	folder.title = "Project";
	folder.directory = (temp.root / "deleted").string();

	const nlohmann::json missing = uam::StateSerializer::SerializeFolder(folder);
	UAM_ASSERT(missing["missing"].get<bool>());
	UAM_ASSERT_EQ(missing["directory"].get<std::string>(), folder.directory);

	std::error_code ec;
	fs::create_directories(folder.directory, ec);
	UAM_ASSERT(!ec);
	UAM_ASSERT(!uam::StateSerializer::SerializeFolder(folder)["missing"].get<bool>());
}

UAM_TEST(StateSerializerIncludesShellActionConfigurationAndNotification)
{
	uam::AppState app;
	ShellAction action;
	action.id = "review";
	action.label = "Review Selection";
	action.skill_path = "/tmp/review.uam";
	action.provider_id = uam::provider_ids::kCodexCli;
	action.model_id = "gpt-5.4";
	action.group_path = {"Coding", "Review"};
	action.accepts_files = true;
	action.accepts_folders = false;
	action.enabled = true;
	app.shell_actions.push_back(action);
	app.shell_action_notification = "Shell actions applied successfully.";
	app.status_line = "Failed to save settings.";
	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["shellActions"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(serialized["shellActions"][0].value("label", ""), action.label);
	UAM_ASSERT_EQ(serialized["shellActions"][0].value("providerId", ""), action.provider_id);
	UAM_ASSERT_EQ(serialized["shellActions"][0].value("modelId", ""), action.model_id);
	UAM_ASSERT_EQ(serialized["shellActions"][0]["groupPath"].get<std::vector<std::string>>(), action.group_path);
	UAM_ASSERT(!serialized["shellActions"][0].value("acceptsFolders", true));
	UAM_ASSERT_EQ(serialized.value("shellActionNotification", ""), app.shell_action_notification);
	UAM_ASSERT_EQ(serialized.value("statusLine", ""), app.status_line);
}

UAM_TEST(StateSerializerUsesResolvedOpenCodeSessionIdForStoppedAcpSummary)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.title = "OpenCode chat";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = "/tmp/opencode-workspace";
	chat.native_session_id = "stale-session";
	app.resolved_native_sessions_by_chat_id[chat.id] = "resolved-session";
	app.chats.push_back(std::move(chat));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp.value("sessionId", ""), std::string("resolved-session"));
	UAM_ASSERT_EQ(acp.value("threadId", ""), std::string("resolved-session"));
}

UAM_TEST(StateSerializerUsesResolvedOpenCodeSessionIdForCliDiagnostics)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.title = "OpenCode chat";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = "/tmp/opencode-workspace";
	chat.native_session_id = "stale-session";
	app.resolved_native_sessions_by_chat_id[chat.id] = "resolved-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "term-chat-opencode";
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "resolved-session";
	app.cli_terminals.push_back(std::move(terminal));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json cli_debug = serialized["cliDebug"];
	UAM_ASSERT_EQ(cli_debug.value("runningTerminalCount", -1), 0);
	UAM_ASSERT_EQ(cli_debug["terminals"][0].value("attachedSessionId", ""), std::string("resolved-session"));
	UAM_ASSERT_EQ(cli_debug["terminals"][0].value("nativeSessionId", ""), std::string("resolved-session"));
}

UAM_TEST(StateSerializerChatTerminalSummaryPrefersLiveOpenCodeTerminalOverStaleDuplicate)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.title = "OpenCode chat";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = "/tmp/opencode-workspace";
	chat.native_session_id = "resolved-session";
	app.resolved_native_sessions_by_chat_id[chat.id] = "resolved-session";
	app.chats.push_back(chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->terminal_id = "term-stale";
	stale_terminal->frontend_chat_id = chat.id;
	stale_terminal->attached_chat_id = chat.id;
	stale_terminal->attached_session_id = "resolved-session";
	stale_terminal->running = false;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->terminal_id = "term-live";
	live_terminal->frontend_chat_id = chat.id;
	live_terminal->attached_chat_id = chat.id;
	live_terminal->attached_session_id = "resolved-session";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	live_terminal->last_activity_time_s = 10.0;
	live_terminal->startup_time_s = 12.5;
	app.cli_terminals.push_back(std::move(live_terminal));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json terminal = serialized["chats"][0]["cliTerminal"];
	UAM_ASSERT_EQ(terminal.value("terminalId", ""), std::string("term-live"));
	UAM_ASSERT_EQ(terminal.value("startupTime", 0.0), 12.5);
	const nlohmann::json fingerprint = uam::StateSerializer::SerializeFingerprint(app);
	app.cli_terminals.back()->startup_time_s = 13.5;
	UAM_ASSERT(fingerprint != uam::StateSerializer::SerializeFingerprint(app));
	UAM_ASSERT(terminal.value("running", false));
	UAM_ASSERT(terminal.value("active", false));
}

UAM_TEST(StatePatchSettingsIncludeChatDefaults)
{
	uam::AppState app;
	app.settings.active_provider_id = " OpenCode ";
	app.settings.ui_theme = " LIGHT ";
	app.settings.memory_idle_delay_seconds = 5;
	app.settings.memory_recall_budget_bytes = 100000;
	app.settings.memory_level_default = "balanced";
	app.settings.goal_max_loop_iterations = -1;
	app.settings.update_checks_enabled = false;
	app.settings.update_last_checked_at = "2026-07-13T20:00:00.000Z";
	app.settings.dismissed_update_versions["uam"] = "4.2.0";
	app.settings.default_new_chat_provider_id = " CoDeX ";
	app.settings.provider_chat_defaults[" CoDeX "] = ProviderChatDefaults{" gpt-5.4 ", " plan ", "yolo", false, " HIGH ", " FAST ", "strict", true};
	app.settings.memory_worker_bindings[" CoDeX "] = MemoryWorkerBinding{" GEMINI ", " worker-model "};
	app.settings.markdown_store_directory = " /tmp/markdown ";
	app.settings.default_editor_preset_id = " CLION ";
	app.settings.editor_file_associations = {
	    EditorFileAssociation{" cpp ", " C++ ", {" CPP ", ".h", ".CPP"}, " CLION "},
	    EditorFileAssociation{"cpp", "Duplicate C++", {".cxx"}, "xcode"},
	    EditorFileAssociation{"empty", "Empty", {"   "}, "vscode"},
	};

	const nlohmann::json settings = nlohmann::json::parse(uam::SettingsPatchForTests(app));
	UAM_ASSERT_EQ(settings.value("activeProviderId", ""), std::string("opencode-cli"));
	UAM_ASSERT_EQ(settings.value("theme", ""), std::string("light"));
	UAM_ASSERT_EQ(settings.value("memoryIdleDelaySeconds", 0), uam::settings::kMinMemoryIdleDelaySeconds);
	UAM_ASSERT_EQ(settings.value("memoryRecallBudgetBytes", 0), uam::settings::kMaxMemoryRecallBudgetBytes);
	UAM_ASSERT_EQ(settings.value("memoryLevelDefault", ""), std::string("balanced"));
	UAM_ASSERT(settings.value("memoryEnabledDefault", false));
	UAM_ASSERT_EQ(settings.value("goalMaxLoopIterations", -1), 200);
	UAM_ASSERT(!settings.value("updateChecksEnabled", true));
	UAM_ASSERT_EQ(settings.value("updateLastCheckedAt", ""), std::string("2026-07-13T20:00:00.000Z"));
	UAM_ASSERT_EQ(settings["dismissedUpdateVersions"].value("uam", ""), std::string("4.2.0"));
	UAM_ASSERT_EQ(settings.value("defaultNewChatProviderId", ""), std::string("codex-cli"));
	UAM_ASSERT_EQ(settings.value("markdownStoreDirectory", ""), std::string("/tmp/markdown"));
	UAM_ASSERT_EQ(settings.value("defaultEditorPresetId", ""), std::string("clion"));
	UAM_ASSERT_EQ(settings["providerChatDefaults"]["codex-cli"].value("modelId", ""), std::string("gpt-5.4"));
	UAM_ASSERT_EQ(settings["providerChatDefaults"]["codex-cli"].value("approvalMode", ""), std::string("plan"));
	UAM_ASSERT_EQ(settings["providerChatDefaults"]["codex-cli"].value("reasoningEffort", ""), std::string("high"));
	UAM_ASSERT_EQ(settings["providerChatDefaults"]["codex-cli"].value("serviceTier", ""), std::string("fast"));
	UAM_ASSERT_EQ(settings["providerChatDefaults"]["codex-cli"].value("memoryLevel", ""), std::string("off"));
	UAM_ASSERT(!settings["providerChatDefaults"]["codex-cli"].value("memoryEnabled", true));
	UAM_ASSERT(settings["providerChatDefaults"]["codex-cli"].value("smallModelMode", false));
	UAM_ASSERT_EQ(settings["memoryWorkerBindings"]["codex-cli"].value("workerProviderId", ""), std::string("gemini-cli"));
	UAM_ASSERT_EQ(settings["memoryWorkerBindings"]["codex-cli"].value("workerModelId", ""), std::string("worker-model"));
	UAM_ASSERT_EQ(settings["editorFileAssociations"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(settings["editorFileAssociations"][0].value("id", ""), std::string("cpp"));
	UAM_ASSERT_EQ(settings["editorFileAssociations"][0].value("name", ""), std::string("C++"));
	UAM_ASSERT_EQ(settings["editorFileAssociations"][0]["extensions"][0].get<std::string>(), std::string(".cpp"));
	UAM_ASSERT_EQ(settings["editorFileAssociations"][0]["extensions"][1].get<std::string>(), std::string(".h"));
	UAM_ASSERT_EQ(settings["editorFileAssociations"][0].value("editorPresetId", ""), std::string("clion"));
}

UAM_TEST(StatePatchIncludesEveryChangedTopLevelStateSlice)
{
	uam::AppState before;
	before.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	uam::AppState after;
	after.provider_profiles = before.provider_profiles;
	after.resource_collections.push_back(ResourceCollection{"collection-1", "Docs", false, {}});
	after.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	after.runtime_cli_versions_by_provider_id["codex-cli"].installed_version = "999.0.0";
	after.status_line = "Failed to persist settings.";

	const nlohmann::json patch = nlohmann::json::parse(uam::StatePatchForTests(before, after))["data"];
	UAM_ASSERT(patch.contains("resourceCollections"));
	UAM_ASSERT(patch.contains("cliVersionManager"));
	UAM_ASSERT_EQ(patch.value("statusLine", ""), after.status_line);
}

UAM_TEST(StatePushReplacesMalformedUtf8InsteadOfCrashing)
{
	uam::AppState before;
	uam::AppState after;
	after.status_line = std::string("broken ") + static_cast<char>(0xC3);

	const nlohmann::json patch = nlohmann::json::parse(uam::StatePatchForTests(before, after))["data"];
	UAM_ASSERT_EQ(patch.value("statusLine", ""), std::string("broken \xEF\xBF\xBD"));
	uam::PushStateUpdate(nullptr, after);
}

UAM_TEST(StatePushTracksUnselectedMessageChangesWithoutTimestampChanges)
{
	uam::AppState app;
	for (int index = 0; index < 2; ++index)
	{
		ChatSession chat;
		chat.id = "background-change-" + std::to_string(index);
		chat.updated_at = "2026-08-30T12:00:00.000Z";
		chat.messages.push_back({MessageRole::Assistant, "first version"});
		chat.messages.back().checkpoint_sha = "checkpoint";
		chat.messages.back().checkpoint_parent_sha = "parent";
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	const nlohmann::json original = uam::StateSerializer::SerializeFingerprint(app);
	Message& background = app.chats[1].messages.front();
	// Async checkpoint completion and rollback can change an idle, unselected turn.
	background.checkpoint_sha.clear();
	background.checkpoint_parent_sha.clear();
	const nlohmann::json rolled_back = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT(original["chats"][1]["messagesDigest"] != rolled_back["chats"][1]["messagesDigest"]);
	background.content = "other version";
	const nlohmann::json changed = uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT(rolled_back["chats"][1]["messagesDigest"] != changed["chats"][1]["messagesDigest"]);
	UAM_ASSERT(original["chats"][0]["messagesDigest"] == changed["chats"][0]["messagesDigest"]);
	UAM_ASSERT(changed == uam::StateSerializer::SerializeFingerprint(app));
}

UAM_TEST(StatePatchUsesSummaryOnlyForMessageChanges)
{
	uam::AppState before;
	ChatSession chat;
	chat.id = "chat-metadata-only";
	chat.title = "Before";
	chat.updated_at = "2026-07-25T00:00:00.000Z";
	chat.messages.push_back(Message{MessageRole::Assistant, "Stable transcript"});
	before.chats.push_back(chat);
	before.selected_chat_index = 0;

	uam::AppState after;
	after.chats = before.chats;
	after.selected_chat_index = before.selected_chat_index;
	after.chats.front().title = "After";
	after.chats.front().updated_at = "2026-07-25T00:00:01.000Z";

	const nlohmann::json metadata_patch = nlohmann::json::parse(uam::StatePatchForTests(before, after))["data"];
	UAM_ASSERT(metadata_patch.contains("chats"));
	UAM_ASSERT(!metadata_patch.contains("messagesByChatId"));

	after.chats.front().messages.back().content = "Edited transcript";
	const nlohmann::json message_patch = nlohmann::json::parse(uam::StatePatchForTests(before, after))["data"];
	UAM_ASSERT(message_patch.contains("chats"));
	UAM_ASSERT(!message_patch.contains("messagesByChatId"));
}

UAM_TEST(StatePatchSendsChatOrderOnlyWhenOrderChanges)
{
	uam::AppState before;
	ChatSession first;
	first.id = "first";
	ChatSession second;
	second.id = "second";
	before.chats.push_back(first);
	before.chats.push_back(second);

	uam::AppState metadata_changed;
	metadata_changed.chats = before.chats;
	metadata_changed.chats.front().title = "Renamed";
	const nlohmann::json metadata_patch =
	    nlohmann::json::parse(uam::StatePatchForTests(before, metadata_changed))["data"];
	UAM_ASSERT(metadata_patch.contains("chats"));
	UAM_ASSERT(!metadata_patch.contains("chatOrder"));

	uam::AppState reordered;
	reordered.chats = before.chats;
	std::swap(reordered.chats[0], reordered.chats[1]);
	const nlohmann::json order_patch =
	    nlohmann::json::parse(uam::StatePatchForTests(before, reordered))["data"];
	UAM_ASSERT(!order_patch.contains("chats"));
	UAM_ASSERT_EQ(order_patch["chatOrder"][0].get<std::string>(), std::string("second"));
	UAM_ASSERT_EQ(order_patch["chatOrder"][1].get<std::string>(), std::string("first"));
}

UAM_TEST(StatePatchIncludesOnlyChangedChatAndOmitsUnchangedChatPatch)
{
	uam::AppState before;
	ChatSession first;
	first.id = "first";
	first.title = "Stable";
	ChatSession second;
	second.id = "second";
	second.title = "Before";
	before.chats = {first, second};

	uam::AppState changed;
	changed.chats = before.chats;
	changed.chats[1].title = "After";
	const nlohmann::json changed_patch = nlohmann::json::parse(uam::StatePatchForTests(before, changed))["data"];
	UAM_ASSERT_EQ(changed_patch["chats"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(changed_patch["chats"][0].value("id", ""), std::string("second"));

	const nlohmann::json unchanged_patch = nlohmann::json::parse(uam::StatePatchForTests(changed, changed))["data"];
	UAM_ASSERT(!unchanged_patch.contains("chats"));
}

UAM_TEST(StatePushRetainsUnchangedChatBaselinesAcrossUpdatesAndRemoval)
{
	uam::AppState app;
	for (int index = 0; index < 128; ++index)
	{
		ChatSession chat;
		chat.id = "chat-" + std::to_string(index);
		chat.title = "Original";
		app.chats.push_back(std::move(chat));
	}
	app.selected_chat_index = 0;
	uam::PushStateUpdate(nullptr, app);

	app.chats.back().title = "Updated";
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	ChatSession removed = app.chats[1];
	app.chats.erase(app.chats.begin() + 1);
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	app.chats.insert(app.chats.begin() + 1, std::move(removed));
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
}

UAM_TEST(StatePatchKeepsLargeLiveChatUpdatesBelowTheCefScriptLimit)
{
	uam::AppState before;
	ChatSession chat;
	chat.id = "chat-large-live";
	chat.messages_loaded = true;
	chat.messages.push_back(Message{MessageRole::User, "Start"});
	before.chats.push_back(chat);
	before.selected_chat_index = 0;

	uam::AppState after;
	after.chats = before.chats;
	after.selected_chat_index = before.selected_chat_index;
	after.chats.front().messages.push_back(
	    Message{MessageRole::Assistant, std::string((2U * 1024U * 1024U) + 1U, 'x')});

	const std::string message = uam::StatePatchForTests(before, after);
	const nlohmann::json patch = nlohmann::json::parse(message)["data"];
	UAM_ASSERT(message.size() < 2U * 1024U * 1024U);
	UAM_ASSERT(patch.contains("chats"));
	UAM_ASSERT(!patch.contains("messagesByChatId"));
}

UAM_TEST(StatePushSkipsRevisionOnlyPatchWhileSelectedSummaryIsDeferred)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-live";
	chat.messages.push_back({MessageRole::Assistant, "first"});
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;
	uam::PushStateUpdate(nullptr, app);

	app.chats.front().messages.front().content = "second";
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	app.chats.front().messages.front().content = "third";
	const auto revision_before_deferred = app.state_revision;
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT(uam::HasDeferredStatePush());
	UAM_ASSERT_EQ(app.state_revision, revision_before_deferred);
}

UAM_TEST(StatePushIgnoresVolatileAcpWaitSecondsButKeepsRealChanges)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-waiting";
	chat.messages.push_back({MessageRole::User, "first version"});
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-waiting";
	session->wait_started_time_s = uam::GetAppTimeSeconds() - 10.0;
	app.acp_sessions.push_back(std::move(session));
	const std::string original_digest = uam::StateSerializer::MessageDigest(app.chats.front());
	uam::PushStateUpdate(nullptr, app);
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	const auto revision_before_timer = app.state_revision;
	app.acp_sessions.front()->wait_started_time_s -= 20.0;
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(app.state_revision, revision_before_timer);

	app.chats.front().messages.front().content = "other version";
	UAM_ASSERT(uam::StateSerializer::MessageDigest(app.chats.front()) != original_digest);
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(app.state_revision, revision_before_timer + 1);
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));

	uam::PushStateUpdate(nullptr, app);
	const auto revision_before_rename = app.state_revision;
	UAM_ASSERT(!uam::PushStateUpdateIfChanged(nullptr, app));
	app.chats.front().title = "Waiting chat renamed";
	UAM_ASSERT(uam::PushStateUpdateIfChanged(nullptr, app));
	UAM_ASSERT_EQ(app.state_revision, revision_before_rename + 1);
}


UAM_TEST(SelectedHistoryFingerprintReusesOnlyExactlyEqualPrefixes)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "selected-prefix-fingerprint";
	for (int index = 0; index < 128; ++index)
	{
		Message message{MessageRole::Assistant, std::string(4096, 'a')};
		message.tool_calls.push_back(ToolCall{"tool-" + std::to_string(index), "read", "{}", std::string(65537, 'b'), "completed"});
		message.blocks.push_back(MessageBlock{"tool_call", "", message.tool_calls.front().id, "{\"id\":1}"});
		chat.messages.push_back(std::move(message));
	}
	app.chats.push_back(std::move(chat));
	ChatDomainService().SelectChatById(app, app.chats.front().id);
	const std::string initial = uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"];
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(128));
	UAM_ASSERT_EQ(initial, uam::StateSerializer::MessageDigest(app.chats.front()));
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"], initial);
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	const nlohmann::json unchanged_page = uam::StateSerializer::SerializeMessagePage(app.chats.front(), 20, std::nullopt, true, initial, true);
	UAM_ASSERT(unchanged_page.value("unchanged", false));
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	UAM_ASSERT(uam::ChatSearchTargetMatches(app.chats.front(), initial, 0, true));
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));

	for (int variation = 0; variation < 5; ++variation)
	{
		Message& tail = app.chats.front().messages.back();
		switch (variation)
		{
		case 0: tail.tool_calls.front().result_text[32000] = 'c'; break;
		case 1: tail.blocks.front().request_id_json = "{\"id\":2}"; break;
		case 2: tail.continues_turn = true; break;
		case 3: tail.content[2048] = 'c'; break;
		case 4: tail.tool_calls.front().args_json = "[]"; break;
		}
		const std::string previous = uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"];
		UAM_ASSERT(previous != initial);
		UAM_ASSERT_EQ(previous, uam::StateSerializer::MessageDigest(app.chats.front()));
		UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"], previous);
		UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	}
	app.chats.front().messages.front().thoughts = "changed earlier history";
	const std::string earlier = uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"];
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(128));
	UAM_ASSERT_EQ(earlier, uam::StateSerializer::MessageDigest(app.chats.front()));
	app.chats.front().messages.pop_back();
	const std::string shorter = uam::StateSerializer::SerializeFingerprint(app)["chats"][0]["messagesDigest"];
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(shorter, uam::StateSerializer::MessageDigest(app.chats.front()));
	app.chats.front().messages.push_back(Message{MessageRole::User, "new prompt"});
	app.chats.front().messages.push_back(Message{MessageRole::Assistant, "new answer"});
	const std::string appended = uam::StateSerializer::MessageDigest(app.chats.front(), true);
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), std::size_t{2});
	UAM_ASSERT_EQ(appended, uam::StateSerializer::MessageDigest(app.chats.front()));
	UAM_ASSERT(appended != shorter);
	app.chats.front().messages.clear();
	const std::string empty = uam::StateSerializer::MessageDigest(app.chats.front(), true);
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), std::size_t{0});
	UAM_ASSERT_EQ(empty, uam::StateSerializer::MessageDigest(app.chats.front()));

}

UAM_TEST(SelectedHistoryFingerprintSwitchColdReleaseAndReopenInvalidateCache)
{
	uam::AppState app;
	ChatSession first;
	first.id = "selected-cache-first";
	first.messages = {Message{MessageRole::Assistant, "first history"}};
	ChatSession second = first;
	second.id = "selected-cache-second";
	app.chats = {first, second};
	ChatDomainService().SelectChatById(app, first.id);
	(void)uam::StateSerializer::SerializeFingerprint(app);
	(void)uam::StateSerializer::SerializeFingerprint(app);
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	ChatDomainService().SelectChatById(app, second.id);
	(void)uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(1));
	ChatSession& current = app.chats[1];
	current.persisted_message_count = 1;
	current.persisted_messages_digest = "durable-cold-digest";
	current.messages.clear();
	current.messages_loaded = false;
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["chats"][1]["messagesDigest"], std::string("durable-cold-digest"));
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(0));
	current.messages_loaded = true;
	current.messages = first.messages;
	current.messages.front().content = "other history";
	const std::string reopened = uam::StateSerializer::SerializeFingerprint(app)["chats"][1]["messagesDigest"];
	UAM_ASSERT_EQ(uam::StateSerializer::SelectedHistoryRehashedMessageCount(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(reopened, uam::StateSerializer::MessageDigest(current));
}

UAM_TEST(NativeChildPreparedPublicationKeepsColdHistoryDetailsWithoutUiReads)
{
	TempDir temp("uam-native-child-prepared");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, "opencode-cli");
	UAM_ASSERT(profile != nullptr);
	ChatSession parent;
	parent.id = "parent-prepared";
	parent.provider_id = profile->id;
	parent.native_session_id = "ses_parent";
	parent.workspace_directory = temp.root.string();
	ChatSession local = parent;
	local.id = "uam-child-prepared";
	local.native_session_id = "ses_child";
	local.messages = {Message{MessageRole::Assistant, "native answer", "2026-10-10T01:00:00Z"}};
	local.updated_at = local.messages.front().created_at;
	local.messages.front().markdown_store_files = {"retained-context.uam"};
	local.messages.front().tool_calls = {ToolCall{"tool", "read", "{}", "local tool details", "completed"}};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, local));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT(!app.chats.front().messages_loaded);
	app.chats.push_back(parent);
	ChatSession native = local;
	native.id = "ses_child";
	native.messages.front().markdown_store_files.clear();
	native.messages.front().tool_calls.clear();
	const ChatHistorySyncService service;
	service.OverlayPreparedLocalHistory(local, native);
	UAM_ASSERT_EQ(native.messages, local.messages);
	// A prepared publication must not parse transcript files on its UI caller.
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(temp.root, local.id), "unreadable body"));
	ChatSession* published = service.FindOrImportNativeSessionChatForOpen(app, parent, *profile, "ses_child", false, &native, true);
	UAM_ASSERT(published != nullptr);
	UAM_ASSERT_EQ(published->id, local.id);
	UAM_ASSERT(published->messages_loaded);
	UAM_ASSERT_EQ(published->messages, local.messages);
	UAM_ASSERT_EQ(published->native_session_id, local.native_session_id);
	ChatSession wrong = native;
	wrong.workspace_directory = (temp.root / "another-workspace").string();
	UAM_ASSERT(service.FindOrImportNativeSessionChatForOpen(app, parent, *profile, "ses_child", false, &wrong, true) == nullptr);
	wrong = native;
	wrong.native_session_id = "ses_foreign";
	UAM_ASSERT(service.FindOrImportNativeSessionChatForOpen(app, parent, *profile, "ses_child", false, &wrong, true) == nullptr);
}

UAM_TEST(NativeChildReadOnlyPreparationNeverRepairsAndRejectsChangedOverlaySave)
{
	TempDir temp("uam-native-child-readonly");
	ChatSession local;
	local.id = "chat-readonly-child";
	local.provider_id = "opencode-cli";
	local.native_session_id = "ses_readonly_child";
	local.messages = {Message{MessageRole::Assistant, "saved annotation", "2026-10-10T00:00:00Z"}};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, local));
	const fs::path summary = AppPaths::UamChatSummaryFilePath(temp.root, local.id);
	UAM_ASSERT(fs::remove(summary));
	const std::string primary_revision = *ChatRepository::ChatStorageRevision(temp.root, local.id);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(temp.root).size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!fs::exists(summary));
	UAM_ASSERT_EQ(*ChatRepository::ChatStorageRevision(temp.root, local.id), primary_revision);
	UAM_ASSERT(uam::io::WriteTextFile(summary, "stale summary cache"));
	std::string unchanged_cache;
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(temp.root).size(), static_cast<std::size_t>(1));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, local.id, false, nullptr, false).has_value());
	UAM_ASSERT(uam::io::TryReadTextFile(summary, unchanged_cache));
	UAM_ASSERT_EQ(unchanged_cache, std::string("stale summary cache"));
	UAM_ASSERT_EQ(*ChatRepository::ChatStorageRevision(temp.root, local.id), primary_revision);
	const std::string revision = primary_revision;
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(temp.root, local, revision);
	UAM_ASSERT(prepared != nullptr);
	local.messages.front().content = "newer saved annotation";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, local));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(temp.root, local.id)->messages.front().content, local.messages.front().content);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, local.id);
	UAM_ASSERT(uam::io::WriteTextFile(primary, "broken primary"));
	const std::string broken_revision = *ChatRepository::ChatStorageRevision(temp.root, local.id);
	(void)ChatRepository::LoadLocalChatSummariesReadOnly(temp.root);
	UAM_ASSERT_EQ(*ChatRepository::ChatStorageRevision(temp.root, local.id), broken_revision);
	std::stop_source cancelled;
	cancelled.request_stop();
	UAM_ASSERT(ChatRepository::LoadLocalChatSummariesReadOnly(temp.root, cancelled.get_token()).empty());
}

UAM_TEST(NativeChildDetachedGeminiPreparationKeepsProviderResolution)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-native-child-gemini-detached");
	const fs::path workspace = temp.root / "workspace";
	const fs::path gemini_home = temp.root / "gemini";
	const fs::path project = gemini_home / ".gemini" / "tmp" / "project";
	fs::create_directories(workspace);
	fs::create_directories(project / "chats");
	UAM_ASSERT(uam::io::WriteTextFile(project / ".project_root", workspace.string()));
	ScopedEnvVar home("GEMINI_CLI_HOME", gemini_home.string());
	const ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	ChatSession source;
	source.id = "chat-detached-gemini-parent";
	source.provider_id = profile.id;
	source.workspace_directory = workspace.string();
	const std::string native_id = "11111111-1111-4111-8111-111111111111";
	const nlohmann::json history = {{"sessionId", native_id}, {"startTime", "2026-10-10T00:00:00Z"},
	    {"lastUpdated", "2026-10-10T00:00:01Z"}, {"messages", nlohmann::json::array({{{"type", "user"}, {"content", "Gemini child request"}}})}};
	UAM_ASSERT(uam::io::WriteTextFile(project / "chats" / "session.json", history.dump()));
	uam::AppState isolated;
	isolated.data_root = temp.root / "data";
	isolated.provider_profiles = {profile};
	isolated.chats.push_back(source);
	const std::vector<ChatSession> local_summaries = ChatRepository::LoadLocalChatSummariesReadOnly(isolated.data_root);
	ChatSession* prepared = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(
	    isolated, source, profile, native_id, false, nullptr, false, {}, &local_summaries);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT_EQ(prepared->native_session_id, native_id);
	UAM_ASSERT(fs::equivalent(uam::paths::PathFromUtf8(prepared->workspace_directory), workspace));
	UAM_ASSERT_EQ(prepared->messages.front().content, std::string("Gemini child request"));
	UAM_ASSERT(!fs::exists(AppPaths::UamChatsRootPath(isolated.data_root)));
	fs::remove_all(project / "chats");
	isolated.chats = {source};
	isolated.resolved_native_sessions_by_chat_id.clear();
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(
	    isolated, source, profile, native_id, false, nullptr, false, {}, &local_summaries) == nullptr);
	UAM_ASSERT(!fs::exists(project / "chats"));
	UAM_ASSERT(!fs::exists(AppPaths::UamChatsRootPath(isolated.data_root)));
#endif
}

UAM_TEST(NativeChildPreparationStopOwnsBothSourceAndTarget)
{
	uam::AppState app;
	const ChatDomainService domain;
	ChatSession parent;
	parent.id = "native-preparation-source";
	ChatSession child;
	child.id = "native-preparation-child";
	child.messages_loaded = false;
	app.chats = {parent, child};
	const std::shared_ptr<uam::BranchPreparationOwnership> first = domain.BeginBranchPreparation(app, parent.id, child.id);
	UAM_ASSERT(first != nullptr);
	std::stop_source worker_cancel(first->cancellation);
	UAM_ASSERT(uam::StopAcpSession(app, child.id));
	UAM_ASSERT(worker_cancel.stop_requested());
	const std::shared_ptr<uam::BranchPreparationOwnership> next = domain.BeginBranchPreparation(app, parent.id, child.id);
	UAM_ASSERT(next != nullptr);
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, parent.id, first));
	UAM_ASSERT(!app.chats[1].messages_loaded);
	UAM_ASSERT(uam::CancelAcpTurn(app, parent.id));
	UAM_ASSERT(next->cancellation.stop_requested());
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, parent.id, next));
	UAM_ASSERT(app.acp_sessions.empty());
}

UAM_TEST(StateSerializerMessageDigestTracksEarlierMessageChanges)
{
	ChatSession chat;
	chat.id = "chat-digest";
	chat.messages_loaded = true;
	chat.messages.push_back({MessageRole::User, "first version", "2026-01-01T00:00:00.000Z"});
	chat.messages.push_back({MessageRole::Assistant, "unchanged answer", "2026-01-01T00:00:01.000Z"});
	const nlohmann::json serialized_before = uam::StateSerializer::SerializeSession(chat);
	const std::string before = serialized_before.value("messagesDigest", "");
	UAM_ASSERT_EQ(uam::StateSerializer::MessageDigest(chat), before);

	chat.messages.front().content = "other version";
	const nlohmann::json serialized_after = uam::StateSerializer::SerializeSession(chat);
	const std::string after = serialized_after.value("messagesDigest", "");
	UAM_ASSERT_EQ(uam::StateSerializer::MessageDigest(chat), after);
	UAM_ASSERT(before != after);

	chat.messages.back().tool_calls.emplace_back();
	ToolCall& tool = chat.messages.back().tool_calls.back();
	tool.id = "digest-tool";
	tool.status = "completed";
	for (const std::size_t payload_size : {32U, (64U * 1024U) + 1U})
	{
		tool.args_json.assign(payload_size, 'a');
		tool.result_text.assign(payload_size, 'b');
		tool.args_json[payload_size / 4] = '\0';
		tool.result_text[payload_size / 4] = '\0';
		const std::string original = uam::StateSerializer::SerializeSession(chat).value("messagesDigest", "");
		tool.args_json[payload_size / 2] = 'c';
		const std::string changed_args = uam::StateSerializer::SerializeSession(chat).value("messagesDigest", "");
		UAM_ASSERT(original != changed_args);
		tool.result_text[payload_size / 2] = 'd';
		const std::string changed_result = uam::StateSerializer::SerializeSession(chat).value("messagesDigest", "");
		UAM_ASSERT(changed_args != changed_result);
	}
}

UAM_TEST(StateSerializerMessagePagesReturnLatestAndOlderSlices)
{
	ChatSession chat;
	chat.messages_loaded = true;
	for (int index = 0; index < 4; ++index)
	{
		chat.messages.push_back({MessageRole::User, "message-" + std::to_string(index),
		                         "2026-01-01T00:00:0" + std::to_string(index) + ".000Z"});
	}

	const nlohmann::json latest = uam::StateSerializer::SerializeMessagePage(chat, 2, std::nullopt);
	UAM_ASSERT_EQ(latest["startIndex"], 2);
	UAM_ASSERT_EQ(latest["totalCount"], 4);
	UAM_ASSERT_EQ(latest["messages"].size(), 2U);
	UAM_ASSERT_EQ(latest["messages"][0]["content"], std::string("message-2"));
	UAM_ASSERT_EQ(latest["messages"][1]["content"], std::string("message-3"));

	const nlohmann::json older = uam::StateSerializer::SerializeMessagePage(chat, 2, 2);
	UAM_ASSERT_EQ(older["startIndex"], 0);
	UAM_ASSERT_EQ(older["totalCount"], 4);
	UAM_ASSERT_EQ(older["messages"].size(), 2U);
	UAM_ASSERT_EQ(older["messages"][0]["content"], std::string("message-0"));
	UAM_ASSERT_EQ(older["messages"][1]["content"], std::string("message-1"));
	UAM_ASSERT_EQ(latest["messagesDigest"], older["messagesDigest"]);
	const std::string digest = latest["messagesDigest"].get<std::string>();
	const nlohmann::json unchanged = uam::StateSerializer::SerializeMessagePage(
	    chat, 2, std::nullopt, true, digest);
	UAM_ASSERT(unchanged.value("unchanged", false));
	UAM_ASSERT(!unchanged.contains("messages"));
	UAM_ASSERT_EQ(unchanged["startIndex"], 2);
	UAM_ASSERT_EQ(unchanged["totalCount"], 4);
	UAM_ASSERT_EQ(unchanged["messagesDigest"], latest["messagesDigest"]);
	const nlohmann::json older_with_digest = uam::StateSerializer::SerializeMessagePage(
	    chat, 2, 2, true, digest);
	UAM_ASSERT(older_with_digest.contains("messages"));

	chat.messages[0].content = "changed";
	const nlohmann::json changed = uam::StateSerializer::SerializeMessagePage(
	    chat, 2, std::nullopt, true, digest);
	UAM_ASSERT(changed.contains("messages"));
	UAM_ASSERT(latest["messagesDigest"] != changed["messagesDigest"]);
	chat.messages[1].role = MessageRole::Assistant;
	chat.messages[3].role = MessageRole::System;
	const nlohmann::json navigation = uam::StateSerializer::SerializeMessagePage(chat, 2, std::nullopt);
	UAM_ASSERT_EQ(navigation["userMessageIndices"], nlohmann::json::array({0, 2}));

}

UAM_TEST(ToolOutputIsDeferredOnlyAfterTheInlinePayloadLimit)
{
	ChatSession chat;
	chat.id = "chat-deferred-tool-output";
	Message assistant{MessageRole::Assistant, "Done"};
	ToolCall small_tool;
	small_tool.id = "tool-small";
	small_tool.name = "Read file";
	small_tool.result_text = "small result";
	small_tool.status = "completed";
	assistant.tool_calls.push_back(small_tool);
	ToolCall large_tool = small_tool;
	large_tool.id = "tool-large";
	large_tool.result_text.assign((64 * 1024) + 1, 'x');
	large_tool.is_sub_agent = true;
	large_tool.child_activity = "Earlier activity\n\nReading package.json";
	assistant.tool_calls.push_back(large_tool);
	chat.messages.push_back(std::move(assistant));

	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(chat);
	const nlohmann::json& serialized_small = serialized["messages"][0]["toolCalls"][0];
	const nlohmann::json& serialized_large = serialized["messages"][0]["toolCalls"][1];
	UAM_ASSERT(!serialized_small.value("contentDeferred", true));
	UAM_ASSERT_EQ(serialized_small.value("content", ""), std::string("small result"));
	UAM_ASSERT(serialized_large.value("contentDeferred", false));
	UAM_ASSERT(!serialized_large.contains("content"));
	UAM_ASSERT_EQ(serialized_large.value("currentActivity", ""), std::string("Reading package.json"));
	const std::string persisted_digest = serialized_large.value("contentDigest", "");
	UAM_ASSERT(!persisted_digest.empty());
	chat.messages[0].tool_calls[1].result_text.back() = 'z';
	UAM_ASSERT(persisted_digest != uam::StateSerializer::SerializeSession(chat)["messages"][0]["toolCalls"][1].value("contentDigest", ""));


	uam::AppState app;
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	auto live_session = std::make_unique<uam::AcpSessionState>();
	live_session->chat_id = chat.id;
	uam::AcpToolCallState live_tool;
	live_tool.id = "tool-live-large";
	live_tool.content.assign((64 * 1024) + 1, 'y');
	live_tool.is_sub_agent = true;
	live_tool.child_activity = std::string(1000, 'z');
	live_session->tool_calls.push_back(std::move(live_tool));
	app.acp_sessions.push_back(std::move(live_session));
	const nlohmann::json live_serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json& serialized_live =
	    live_serialized["chats"][0]["acpSession"]["toolCalls"][0];
	UAM_ASSERT(serialized_live.value("contentDeferred", false));
	UAM_ASSERT(!serialized_live.contains("content"));
	UAM_ASSERT_EQ(serialized_live.value("currentActivity", "").size(), std::size_t{240});
	const std::string live_digest = serialized_live.value("contentDigest", "");
	UAM_ASSERT(!live_digest.empty());
	app.acp_sessions[0]->tool_calls[0].content.back() = 'z';
	const std::string changed_digest = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["toolCalls"][0].value("contentDigest", "");
	UAM_ASSERT(live_digest != changed_digest);
	app.acp_sessions[0]->tool_calls[0].permission_review_reason = "Reviewed";
	UAM_ASSERT(changed_digest != uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["toolCalls"][0].value("contentDigest", ""));

}

UAM_TEST(CompanionMessagePagesDeferPersistedToolOutput)
{
	ChatSession chat;
	Message assistant{MessageRole::Assistant, "Done"};
	for (int index = 0; index < 200; ++index)
	{
		ToolCall tool;
		tool.id = "tool-" + std::to_string(index);
		tool.name = "Read file";
		tool.result_text.assign(32 * 1024, static_cast<char>('a' + index % 26));
		tool.status = "completed";
		assistant.tool_calls.push_back(std::move(tool));
	}
	chat.messages.push_back(std::move(assistant));

	const nlohmann::json page = uam::StateSerializer::SerializeMessagePage(chat, 200, std::nullopt, true);
	UAM_ASSERT(page.dump().size() < 200 * 1024U);
	for (const nlohmann::json& tool : page["messages"][0]["toolCalls"])
	{
		UAM_ASSERT(tool.value("contentDeferred", false));
		UAM_ASSERT(!tool.contains("content"));
	}
	const nlohmann::json content = uam::StateSerializer::ToolCallContentPageForFrontend(
		chat.messages[0].tool_calls[0].result_text, 0);
	UAM_ASSERT_EQ(content.value("content", "").size(), 32U * 1024U);
}

UAM_TEST(ToolOutputPagesAreBoundedNavigableAndUtf8Safe)
{
	constexpr std::size_t page_bytes = 128 * 1024;
	std::string content(page_bytes - 1, 'a');
	content += "\xE2\x82\xAC";
	content.append(page_bytes, 'b');

	const nlohmann::json first =
	    uam::StateSerializer::ToolCallContentPageForFrontend(content, 0);
	UAM_ASSERT_EQ(first.value("offset", 1), 0);
	UAM_ASSERT_EQ(first.value("nextOffset", 0), page_bytes - 1);
	UAM_ASSERT(first.value("hasMore", false));
	UAM_ASSERT(!first.value("hasPrevious", true));
	UAM_ASSERT(first.value("content", "").size() <= page_bytes);

	const nlohmann::json second = uam::StateSerializer::ToolCallContentPageForFrontend(
	    content, first.value("nextOffset", 0));
	UAM_ASSERT_EQ(second.value("offset", 0), page_bytes - 1);
	UAM_ASSERT(second.value("hasPrevious", false));
	UAM_ASSERT_EQ(second.value("previousOffset", 1), 0);
	UAM_ASSERT(second.value("content", "").starts_with("\xE2\x82\xAC"));
	UAM_ASSERT(second.value("content", "").size() <= page_bytes);

	const nlohmann::json latest = uam::StateSerializer::ToolCallContentPageForFrontend(
	    content, std::numeric_limits<std::size_t>::max());
	UAM_ASSERT_EQ(latest.value("offset", 0), latest.value("lastOffset", 1));
	UAM_ASSERT(!latest.value("hasMore", true));
	UAM_ASSERT_EQ(latest.value("nextOffset", 0), content.size());
	UAM_ASSERT_EQ(latest.value("totalBytes", 0), content.size());
}

UAM_TEST(StateSerializerIncludesAllCliVersionManagers)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ProviderProfile duplicate_codex_profile = ProviderProfileStore::DefaultCodexProfile();
	duplicate_codex_profile.id = " CoDeX ";
	app.provider_profiles.push_back(duplicate_codex_profile);
	ProviderProfile custom_cli_profile;
	custom_cli_profile.id = "custom-provider";
	custom_cli_profile.title = "Custom Provider";
	custom_cli_profile.supports_cli = true;
	app.provider_profiles.push_back(custom_cli_profile);
	ChatSession chat;
	chat.id = "chat-1";
	chat.title = "Codex version chat";
	chat.provider_id = "codex-cli";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;
	app.runtime_cli_version_provider_id = "codex-cli";
	app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
	app.runtime_cli_versions_by_provider_id["codex-cli"].installed_version = "0.148.0";
	app.runtime_cli_versions_by_provider_id["codex-cli"].supported = true;
	app.runtime_cli_versions_by_provider_id["codex-cli"].message = "Codex CLI version is supported.";

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json manager = serialized["cliVersionManager"];
	const nlohmann::json& providers = manager["providers"];
	UAM_ASSERT(providers.is_array());
	UAM_ASSERT(providers.size() >= 2);

	const auto codex_it = std::ranges::find_if(providers, [](const nlohmann::json& provider) { return provider.value("providerId", "") == "codex-cli"; });
	UAM_ASSERT(codex_it != providers.end());
	UAM_ASSERT_EQ(std::ranges::count_if(providers, [](const nlohmann::json& provider) { return provider.value("providerId", "") == "codex-cli"; }), 1);
	UAM_ASSERT(std::ranges::none_of(providers, [](const nlohmann::json& provider) { return provider.value("providerId", "") == " CoDeX " || provider.value("providerId", "") == "custom-provider"; }));
	UAM_ASSERT_EQ(codex_it->value("installedVersion", ""), std::string("0.148.0"));
	UAM_ASSERT_EQ(codex_it->value("preferredVersion", ""), std::string("latest"));
	UAM_ASSERT_EQ(codex_it->value("verifiedVersion", ""), std::string("0.148.0"));
	UAM_ASSERT_EQ(codex_it->value("verifiedAt", ""), std::string("2026-08-27"));
	UAM_ASSERT_EQ(codex_it->value("status", ""), std::string("verified"));
	UAM_ASSERT((*codex_it)["availableVersions"].is_array());
	UAM_ASSERT(!(*codex_it)["availableVersions"].empty());
	const auto claude_it = std::ranges::find_if(providers, [](const nlohmann::json& provider) { return provider.value("providerId", "") == "claude-cli"; });
	UAM_ASSERT(claude_it != providers.end());
	UAM_ASSERT_EQ(claude_it->value("preferredVersion", ""), std::string("latest"));
	UAM_ASSERT(!(*claude_it)["availableVersions"].empty());

	app.runtime_cli_install_tasks[uam::provider_ids::kCodexCli].running = true;

	app.runtime_cli_install_tasks[uam::provider_ids::kCodexCli].command_preview = "npm install -g @openai/codex@0.124.0";
	const nlohmann::json running_manager = uam::StateSerializer::Serialize(app)["cliVersionManager"];
	const nlohmann::json& running_providers = running_manager["providers"];
	const auto running_codex_it = std::ranges::find_if(running_providers, [](const nlohmann::json& provider) { return provider.value("providerId", "") == "codex-cli"; });
	UAM_ASSERT(running_codex_it != running_providers.end());
	UAM_ASSERT_EQ(running_codex_it->value("status", ""), std::string("installing"));
	UAM_ASSERT_EQ(running_codex_it->value("running", false), true);
	UAM_ASSERT_EQ(running_codex_it->value("lastCommand", ""), std::string("npm install -g @openai/codex@0.124.0"));
}

UAM_TEST(StateSerializerProviderJsonIncludesNpmPackageAndShortName)
{
	// FE-1: serialized provider JSON must carry shortName and npmPackageName so the frontend
	// can prefer live state over the static fallback table.
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json& providers = serialized["providers"];
	UAM_ASSERT(providers.is_array());
	UAM_ASSERT(!providers.empty());

	for (const nlohmann::json& provider : providers)
	{
		UAM_ASSERT(!provider.value("id", "").empty());
		UAM_ASSERT(!provider.value("shortName", "").empty());
		UAM_ASSERT_EQ(provider.value("npmPackageName", "").empty(), !uam::provider_ids::IsVersionManagedCliProviderId(provider.value("id", "")));
	}

	const auto gemini_it = std::ranges::find_if(providers, [](const nlohmann::json& p) { return p.value("id", "") == "gemini-cli"; });
	UAM_ASSERT(gemini_it != providers.end());
	UAM_ASSERT_EQ(gemini_it->value("npmPackageName", ""), std::string("@google/gemini-cli"));

	const auto claude_it = std::ranges::find_if(providers, [](const nlohmann::json& p) { return p.value("id", "") == "claude-cli"; });
	UAM_ASSERT(claude_it != providers.end());
	UAM_ASSERT_EQ(claude_it->value("npmPackageName", ""), std::string("@anthropic-ai/claude-code"));
}

UAM_TEST(RemoteCliElevationIsOptInAndOnlyElevatesLinuxNpmInstall)
{
	ExecutionHost host;
	host.id = "homelab";
	host.transport = "ssh";
	host.ssh_alias = "uam-homelab";
	host.platform = "linux";
	const std::string command = BuildBulkNpmInstallCommand({{"codex-cli", "latest"}, {"claude-cli", "latest"}});
	UAM_ASSERT_EQ(BuildRemoteCliCommandArgv(host, command, true, "npm"), (std::vector<std::string>{"sh", "-lc", command}));
	host.cli_updates_as_admin = true;
	const std::vector<std::string> elevated = BuildRemoteCliCommandArgv(host, command, true, "npm");
	UAM_ASSERT_EQ(elevated.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(elevated.back().find("npm prefix -g") != std::string::npos);
	UAM_ASSERT(elevated.back().ends_with("exec sudo -n -- npm --prefix \"$uam_update_prefix\" " + command.substr(4)));
	UAM_ASSERT_EQ(BuildRemoteCliCommandArgv(host, "codex --version", false, "npm"), (std::vector<std::string>{"sh", "-lc", "codex --version"}));
	UAM_ASSERT(BuildRemoteCliCommandArgv(host, "brew upgrade codex", true, "homebrew").empty());
	host.platform = "windows";
	UAM_ASSERT(BuildRemoteCliCommandArgv(host, command, true, "npm").empty());
	host.platform = "linux";
	host.transport = "local";
	UAM_ASSERT(BuildRemoteCliCommandArgv(host, command, true, "npm").empty());
}

#if !defined(_WIN32)
UAM_TEST(RemoteCliElevationRefusalNeverFallsBackAndKeepsNpmPrefix)
{
	TempDir fixture("uam-elevation");
	const fs::path npm = fixture.root / "npm";
	const fs::path sudo = fixture.root / "sudo";
	{
		std::ofstream script(npm);
		script << "#!/bin/sh\nif [ \"$1\" = prefix ]; then echo /usr; else echo UNELEVATED_INSTALL; fi\n";
	}
	{
		std::ofstream script(sudo);
		script << "#!/bin/sh\nprintf 'SUDO_ARG:%s\\n' \"$@\"\nexit 17\n";
	}
	fs::permissions(npm, fs::perms::owner_exec, fs::perm_options::add);
	fs::permissions(sudo, fs::perms::owner_exec, fs::perm_options::add);
	ExecutionHost host;
	host.id = "homelab";
	host.transport = "ssh";
	host.platform = "linux";
	host.cli_updates_as_admin = true;
	const std::string install = BuildBulkNpmInstallCommand({{"codex-cli", "latest"}});
	const std::string command = "PATH=" + ShellQuoteForTest(fixture.root.string()) + ":/usr/bin:/bin; export PATH; " + BuildRemoteCliCommandArgv(host, install, true, "npm").back();
	const ProcessExecutionResult refused = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 10000);
	UAM_ASSERT_EQ(refused.exit_code, 17);
	UAM_ASSERT(refused.output.find("SUDO_ARG:-n\nSUDO_ARG:--\nSUDO_ARG:npm\nSUDO_ARG:--prefix\nSUDO_ARG:/usr\n") != std::string::npos);
	UAM_ASSERT(refused.output.find("UNELEVATED_INSTALL") == std::string::npos);
	{
		std::ofstream script(npm);
		script << "#!/bin/sh\necho relative-prefix\n";
	}
	const ProcessExecutionResult invalid = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 10000);
	UAM_ASSERT_EQ(invalid.exit_code, 1);
	UAM_ASSERT(invalid.output.find("SUDO_ARG:") == std::string::npos);
}
#endif

UAM_TEST(RemoteDisabledStartupErrorDoesNotBlockUnrelatedHostOptions)
{
	for (const std::string status : {"disabled", "error", "pending", "enabled"})
	for (const bool previous_enabled : {false, true})
	for (const bool enabled : {false, true})
	{
		ExecutionHost previous;
		previous.startup_enabled = previous_enabled;
		previous.startup_status = status;
		ExecutionHost current = previous;
		current.startup_enabled = enabled;
		current.cli_updates_as_admin = true;
		current.instruction_file = "/home/david/AGENTS.md";
		const bool configure = uam::execution_hosts::NeedsStartupConfiguration(current, previous);
		UAM_ASSERT_EQ(configure, enabled != previous_enabled || enabled || status == "pending");
		// Disabled errors remain visible; unrelated choices require no SSH startup operation.
		current.startup_status = configure ? "pending" : previous.startup_status;
		UAM_ASSERT(current.cli_updates_as_admin);
		if (!configure) UAM_ASSERT_EQ(current.startup_status, status);
	}
}

UAM_TEST(RemoteStartupFailurePreservesSavedCliElevationAndInstructions)
{
	for (const bool previous_enabled : {false, true})
	for (const bool succeeded : {false, true})
	{
		ExecutionHost previous;
		previous.id = "ssh-options";
		previous.transport = "ssh";
		previous.platform = "linux";
		previous.ssh_alias = "homelab";
		previous.startup_enabled = previous_enabled;
		ExecutionHost current = previous;
		current.startup_enabled = !previous_enabled;
		current.cli_updates_as_admin = true;
		current.instruction_file = "/home/david/AGENTS.md";
		uam::execution_hosts::ApplyStartupConfigurationResult(current, previous, succeeded);
		UAM_ASSERT(current.cli_updates_as_admin);
		UAM_ASSERT_EQ(current.instruction_file, "/home/david/AGENTS.md");
		UAM_ASSERT_EQ(current.startup_enabled, succeeded ? !previous_enabled : previous_enabled);
		UAM_ASSERT_EQ(current.startup_status, succeeded ? (!previous_enabled ? "enabled" : "disabled") : "error");
		const std::vector<ExecutionHost> restored = uam::execution_hosts::Parse(uam::execution_hosts::Serialize({uam::execution_hosts::LocalHost(), current}));
		UAM_ASSERT_EQ(restored.size(), 2u);
		UAM_ASSERT(restored[1].cli_updates_as_admin);
		UAM_ASSERT_EQ(restored[1].instruction_file, current.instruction_file);
	}
}

UAM_TEST(RemoteCliElevationSettingSurvivesSerializationAndDefaultsOff)
{
	ExecutionHost host;
	host.id = "homelab";
	host.transport = "ssh";
	host.ssh_alias = "uam-homelab";
	host.platform = "linux";
	host.cli_updates_as_admin = true;
	nlohmann::json saved = uam::execution_hosts::Serialize({host});
	std::vector<ExecutionHost> loaded = uam::execution_hosts::Parse(saved);
	UAM_ASSERT(!loaded.front().cli_updates_as_admin);
	UAM_ASSERT(loaded.back().cli_updates_as_admin);
	saved.back().erase("cliUpdatesAsAdmin");
	loaded = uam::execution_hosts::Parse(saved);
	UAM_ASSERT(!loaded.back().cli_updates_as_admin);
}

UAM_TEST(ConfiguredRemoteCliRowsExposeUnknownScopedPoliciesBeforeAnyProbe)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ProviderProfile alias = app.provider_profiles.front();
	alias.id = " GEMINI ";
	app.provider_profiles.push_back(alias);
	ExecutionHost first;
	first.id = "remote-first";
	first.transport = "ssh";
	first.ssh_alias = "private-first";
	first.platform = "linux";
	ExecutionHost second = first;
	second.id = "remote-second";
	second.ssh_alias = "private-second";
	second.platform = "windows";
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), first, second};
	uam::CliProviderVersionState& local = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kGeminiCli];
	local.checked = true;
	local.installed_version = "private-local-only";
	const nlohmann::json state = uam::StateSerializer::Serialize(app);
	const nlohmann::json& manager = state["cliVersionManager"];
	std::set<std::string> expected;
	for (const nlohmann::json& provider : manager["providers"])
		expected.insert(provider["providerId"].get<std::string>());
	UAM_ASSERT_EQ(expected.size(), std::size_t{6});
	UAM_ASSERT_EQ(manager["remoteProviders"].size(), expected.size() * 2);
	std::map<std::string, std::set<std::string>> actual;
	for (const nlohmann::json& provider : manager["remoteProviders"])
	{
		const std::string host_id = provider["executionHostId"].get<std::string>();
		UAM_ASSERT(host_id == first.id || host_id == second.id);
		UAM_ASSERT(actual[host_id].insert(provider["providerId"].get<std::string>()).second);
		UAM_ASSERT_EQ(provider["status"].get<std::string>(), std::string("unknown"));
		UAM_ASSERT(provider["installedVersion"].get<std::string>().empty());
		UAM_ASSERT(!provider["running"].get<bool>());
		UAM_ASSERT(provider["canInstall"].is_boolean());
		UAM_ASSERT(provider["availableVersions"].is_array());
		UAM_ASSERT(!provider["routingConnectionIdentity"].get<std::string>().empty());
		UAM_ASSERT(provider["lastOutput"].get<std::string>().empty());
	}
	UAM_ASSERT_EQ(actual[first.id], expected);
	UAM_ASSERT_EQ(actual[second.id], expected);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeFingerprint(app)["cliVersionManager"], manager);
	UAM_ASSERT(!app.runtime_cli_version_check_task.running);
	UAM_ASSERT(app.runtime_cli_install_tasks.empty());
	UAM_ASSERT_EQ(app.runtime_cli_versions_by_provider_id.size(), std::size_t{1});
}

UAM_TEST(RemoteCliRowsRejectOldConnectionsAndForeignMaintenanceObservations)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ExecutionHost old;
	old.id = "remote-replaced";
	old.transport = "ssh";
	old.ssh_alias = "private-old";
	old.platform = "linux";
	ExecutionHost current = old;
	current.ssh_alias = "private-current";
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost(), current};
	const std::string key = CliProviderVersionStateKey(uam::provider_ids::kCodexCli, current.id);
	uam::CliProviderVersionState& cached = app.runtime_cli_versions_by_provider_id[key];
	cached.provider_id = uam::provider_ids::kCodexCli;
	cached.execution_host = old;
	cached.checked = true;
	cached.installed_version = "private-old-version";
	cached.raw_output = "private old output";
	app.runtime_cli_version_provider_id = uam::provider_ids::kCodexCli;
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_check_task.execution_host = old;
	app.runtime_cli_version_check_task.command_preview = "private old check";
	app.runtime_cli_install_tasks[key].running = true;
	app.runtime_cli_install_tasks[key].execution_host = old;
	app.runtime_cli_install_tasks[key].command_preview = "private old install";
	app.pending_cli_updates[key] = {uam::provider_ids::kCodexCli, "private old target", old};
	const auto codex_row = [&app]() -> nlohmann::json
	{
		const nlohmann::json rows = uam::StateSerializer::Serialize(app)["cliVersionManager"]["remoteProviders"];
		for (const nlohmann::json& row : rows)
			if (row["providerId"] == uam::provider_ids::kCodexCli) return row;
		return nlohmann::json{};
	};
	nlohmann::json row = codex_row();
	UAM_ASSERT(row.is_object());
	UAM_ASSERT_EQ(row["status"].get<std::string>(), std::string("unknown"));
	UAM_ASSERT(row["installedVersion"].get<std::string>().empty());
	UAM_ASSERT(row["lastOutput"].get<std::string>().empty());
	UAM_ASSERT(row["lastCommand"].get<std::string>().empty());
	UAM_ASSERT(!row["running"].get<bool>());
	cached.execution_host = current;
	cached.installed_version = "0.149.0";
	cached.raw_output = "private current output";
	row = codex_row();
	UAM_ASSERT_EQ(row["installedVersion"].get<std::string>(), std::string("0.149.0"));
	UAM_ASSERT_EQ(row["lastOutput"].get<std::string>(), std::string("private current output"));
	UAM_ASSERT(!row["running"].get<bool>());
	cached.provider_id = uam::provider_ids::kOpenCodeCli;
	row = codex_row();
	UAM_ASSERT(row["installedVersion"].get<std::string>().empty());
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	UAM_ASSERT(uam::StateSerializer::Serialize(app)["cliVersionManager"]["remoteProviders"].empty());
}

UAM_TEST(CliProviderVersionCommandsUseCuratedPackages)
{
	const ProviderCliCompatibilityService service;

	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("gemini-cli"), std::string("gemini --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("codex-cli"), std::string("codex --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests(" CoDeX "), std::string("codex --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests(std::string_view("xx opencode-cli yy").substr(2, 13)), std::string("opencode --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("claude-cli"), std::string("claude --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("opencode-cli"), std::string("opencode --version"));
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("copilot-cli"), std::string("copilot --version"));
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("antigravity-cli"), std::string("agy --version"));
#endif
	UAM_ASSERT_EQ(BuildCliProviderVersionProbeCommandForTests("unknown-provider"), std::string(""));
	UAM_ASSERT(service.PreferredVersionForProvider("unknown-provider").empty());
	UAM_ASSERT(!service.IsSupportedVersionForProvider("unknown-provider", "latest"));
	UAM_ASSERT(service.VerifiedAtForProvider("unknown-provider").empty());
	uam::AppState unknown_provider_app;
	service.StartProviderVersionCheck(unknown_provider_app, "unknown-provider", true);
	UAM_ASSERT(!unknown_provider_app.runtime_cli_version_check_task.running);
	UAM_ASSERT(!unknown_provider_app.runtime_cli_versions_by_provider_id.contains(uam::provider_ids::kGeminiCli));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("gemini-cli", "0.38.1"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("gemini-cli", "0.55.1"), std::string("npm install -g @google/gemini-cli@0.55.1"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("codex-cli", "0.124.0"), std::string("npm install -g @openai/codex@0.124.0"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests(std::string_view("xx codex-cli yy").substr(2, 11), std::string_view("xx0.123.0yy").substr(2, 7)), std::string("npm install -g @openai/codex@0.123.0"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests(std::string_view("xx codex yy").substr(2, 7), std::string_view("xx 0.124.0 yy").substr(2, 9)), std::string("npm install -g @openai/codex@0.124.0"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("claude-cli", "latest"), std::string("npm install -g @anthropic-ai/claude-code@latest"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("claude-cli", "release_2026-05.1"), std::string("npm install -g @anthropic-ai/claude-code@release_2026-05.1"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("opencode-cli", "1.15.12"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("opencode-cli", "1.15.13"), std::string("npm install -g opencode-ai@1.15.13"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("copilot-cli", "latest"), std::string("npm install -g @github/copilot@latest"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("copilot-cli", "1.0.60"), std::string("npm install -g @github/copilot@1.0.60"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("copilot-cli", "1.0.59"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForMethodForTests("opencode-cli", "1.18.0", "homebrew-formula"), std::string("brew upgrade opencode"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForMethodForTests("codex-cli", "0.124.0", "homebrew-cask"), std::string("brew upgrade --cask codex"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForMethodForTests("copilot-cli", "latest", "winget"), std::string("winget upgrade --id GitHub.Copilot --exact --source winget --accept-source-agreements --accept-package-agreements"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForMethodForTests("copilot-cli", "1.0.60", "winget"), std::string("winget upgrade --id GitHub.Copilot --exact --source winget --accept-source-agreements --accept-package-agreements --version 1.0.60"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForMethodForTests("claude-cli", "latest", "npm"), std::string("npm install -g @anthropic-ai/claude-code@latest"));
	UAM_ASSERT_EQ(ExtractCliProviderInstallMethodForTests("[UAM CLI PATH] /opt/homebrew/bin/opencode|../Cellar/opencode/1.17.15/bin/opencode\n1.17.15\n"), std::string("homebrew-formula"));
	UAM_ASSERT_EQ(ExtractCliProviderInstallMethodForTests("[UAM CLI PATH] /opt/homebrew/bin/codex|/opt/homebrew/Caskroom/codex/0.144.5/codex\ncodex-cli 0.144.5\n"), std::string("homebrew-cask"));
	UAM_ASSERT_EQ(ExtractCliProviderInstallMethodForTests("[UAM CLI PATH] /opt/homebrew/bin/claude|../lib/node_modules/@anthropic-ai/claude-code/bin/claude.exe\n2.1.207\n"), std::string("npm"));
	UAM_ASSERT_EQ(ExtractCliProviderInstallMethodForTests("[UAM CLI PATH] C:\\Users\\Jos\xc3\xa9\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe|\n1.0.75\n"), std::string("winget"));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("unknown-provider", "0.38.1"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("codex-cli", "--bad"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("claude-cli", "--bad"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("opencode-cli", "--bad"), std::string(""));
	UAM_ASSERT_EQ(BuildCliProviderInstallCommandForTests("copilot-cli", "--bad"), std::string(""));
	UAM_ASSERT_EQ(service.PreferredVersionForProvider("codex-cli"), std::string("latest"));
	UAM_ASSERT_EQ(service.PreferredVersionForProvider("claude-cli"), std::string("latest"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("codex-cli", "0.123.0"));
	UAM_ASSERT(service.IsSupportedVersionForProvider(std::string_view("xx CoDeX yy").substr(2, 7), std::string_view("xx 0.124.0 yy").substr(2, 9)));
	UAM_ASSERT(service.IsSupportedVersionForProvider("codex-cli", "0.125.0"));
	UAM_ASSERT(!service.IsSupportedVersionForProvider("opencode-cli", "1.15.12"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("opencode-cli", "1.15.13"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("opencode-cli", "1.17.20"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("claude-cli", "latest"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("copilot-cli", "latest"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("copilot-cli", "1.0.60"));
	UAM_ASSERT(service.IsSupportedVersionForProvider("copilot-cli", "1.1.0"));
	UAM_ASSERT(!service.IsSupportedVersionForProvider("copilot-cli", "1.0.59"));
	UAM_ASSERT(!service.IsSupportedVersionForProvider("claude-cli", "bad/version"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("codex-cli", "0.148.0"), std::string("verified"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("codex-cli", "0.149.0"), std::string("untested-newer"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("codex-cli", "0.147.0"), std::string("untested"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("gemini-cli", "0.55.0"), std::string("known-incompatible"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("claude-cli", "2.1.0"), std::string("provider-managed"));
	UAM_ASSERT_EQ(service.CompatibilityStatusForProvider("copilot-cli", ""), std::string("unavailable"));
}

UAM_TEST(ProviderRuntimeCompatibilityBlocksPendingAndKnownBadVersions)
{
	uam::AppState app;
	for (const char* provider : {"codex-cli", "gemini-cli", "claude-cli"})
	{
		app.runtime_cli_versions_by_provider_id[provider] = {};
		UAM_ASSERT(ProviderRuntimeRegistry::ResolveById(provider).LocalCliCompatibilityError(app).empty());
	}
	for (const char* provider : {"copilot-cli", "opencode-cli"})
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider);
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).empty());
		uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[provider];
		if (!runtime.IsEnabled())
		{
			UAM_ASSERT(runtime.LocalCliCompatibilityError(app).empty());
			continue;
		}
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).find("Checking") != std::string::npos);
		state.checked = true;
		const bool copilot = std::string_view(provider) == "copilot-cli";
		state.installed_version = copilot ? "1.0.59" : "1.15.12";
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).find(copilot ? "1.0.60 or newer" : "1.15.13 or newer") != std::string::npos);
		state.supported = true;
		UAM_ASSERT(runtime.LocalCliCompatibilityError(app).empty());
	}
}

UAM_TEST(RemoteCliUpdateReportsBlockingPidsWithoutTrustingRemoteText)
{
	const std::string probe = "[UAM CLI PATH] /usr/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 1\n";
	std::string error;
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(probe, probe + "[UAM CLI PIDS] 123,456\n", "linux", &error));
	UAM_ASSERT(error.find("Blocking PIDs: 123,456") != std::string::npos);
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(probe, probe + "[UAM CLI PIDS] untrusted text\n", "linux", &error));
	UAM_ASSERT(error.find("untrusted text") == std::string::npos);
	UAM_ASSERT(error.find("idle background session") != std::string::npos);
}

UAM_TEST(CliProviderVersionRemoteUpdateRequiresUnchangedIdentifiedInstallation)
{
	const std::string npm = "[UAM CLI PATH] /usr/local/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 0\ncodex-cli 0.148.0\n";
	std::string error;
	UAM_ASSERT(ValidateRemoteCliInstallProbeForTests(npm, npm, "linux", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(npm, "[UAM CLI PATH] /opt/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n0.148.0", "linux", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(npm, "[UAM CLI PATH] /usr/local/bin/codex|../Cellar/codex/0.148.0/bin/codex\n0.148.0", "linux", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(npm, "Failed to run command: SSH helper unavailable", "linux", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests("0.148.0", "0.148.0", "linux", &error));
	const std::string standalone = "[UAM CLI PATH] C:\\Tools\\codex.exe|\n0.148.0";
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(standalone, standalone, "windows", &error));
	const std::string shim = "[UAM CLI PATH] C:\\npm\\codex.cmd|C:\\npm\\node_modules\\@openai\\codex\n[UAM CLI RUNNING] 0\n0.148.0";
	UAM_ASSERT(ValidateRemoteCliInstallProbeForTests(shim, shim, "windows", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(shim, standalone, "windows", &error));
	UAM_ASSERT(ValidateRemoteCliInstallProbeForTests(npm, npm, "linux", &error));
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(npm, "[UAM CLI PATH] /usr/local/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n[UAM CLI RUNNING] 1\n0.148.0", "linux", &error));
	UAM_ASSERT(error.find("Stop it before updating") != std::string::npos);
	UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(npm, "[UAM CLI PATH] /usr/local/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n0.148.0", "linux", &error));
	const std::string windows_probe = BuildInstallAwareCliProbeForTests("codex-cli", "windows");
	std::string encoded_script;
	UAM_ASSERT(uam::base64::Decode(windows_probe.substr(windows_probe.find("-EncodedCommand ") + 16), encoded_script));
	std::string script;
	for (std::size_t index = 0; index < encoded_script.size(); index += 2) script.push_back(encoded_script[index]);
	UAM_ASSERT(script.find("Get-Process -Name 'codex'") != std::string::npos);
	UAM_ASSERT(script.find("[UAM CLI RUNNING]") != std::string::npos);
	UAM_ASSERT(script.find("npm.cmd prefix -g") != std::string::npos);
	UAM_ASSERT(script.find("package.json") != std::string::npos);
	UAM_ASSERT(script.find("node_modules\\@openai\\codex\\") != std::string::npos);
	const std::string opencode_probe = BuildInstallAwareCliProbeForTests("opencode-cli", "linux");
	UAM_ASSERT(opencode_probe.find("-v exe='opencode'") != std::string::npos);
	UAM_ASSERT(opencode_probe.find("node_modules/opencode-ai/") != std::string::npos);
	UAM_ASSERT(opencode_probe.find("uam_ps_status=$?") != std::string::npos);
	UAM_ASSERT(opencode_probe.find("sub(\".*/\",\"\",base)") != std::string::npos);
	const std::string opencode_windows_probe = BuildInstallAwareCliProbeForTests("opencode-cli", "windows");
	UAM_ASSERT(uam::base64::Decode(opencode_windows_probe.substr(opencode_windows_probe.find("-EncodedCommand ") + 16), encoded_script));
	script.clear();
	for (std::size_t index = 0; index < encoded_script.size(); index += 2) script.push_back(encoded_script[index]);
	UAM_ASSERT(script.find("Get-Process -Name 'opencode'") != std::string::npos);
	UAM_ASSERT(script.find("node_modules\\opencode-ai\\") != std::string::npos);
	const std::string copilot_probe = BuildInstallAwareCliProbeForTests("copilot-cli", "windows");
	UAM_ASSERT(uam::base64::Decode(copilot_probe.substr(copilot_probe.find("-EncodedCommand ") + 16), encoded_script));
	script.clear();
	for (std::size_t index = 0; index < encoded_script.size(); index += 2) script.push_back(encoded_script[index]);
	UAM_ASSERT(script.find("if(-not $p)") < script.find("Get-Command 'pwsh'"));
	UAM_ASSERT(script.find("Write-Output '[UAM CLI PATH] |'") != std::string::npos);

	for (const bool missing : {false, true})
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.runtime_cli_version_provider_id = uam::provider_ids::kCodexCli;
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
		app.runtime_cli_version_check_task.state->result.output = missing
		    ? "[UAM CLI PATH] |\ncodex: command not found\n[Provider CLI exited with code 127]"
		    : "Failed to run command: SSH helper unavailable";
		app.runtime_cli_version_check_task.state->completed.store(true);
		ProviderCliCompatibilityService().Poll(app);
		const uam::CliProviderVersionState& result = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCodexCli);
		UAM_ASSERT_EQ(result.check_error.empty(), missing);
	}
}

UAM_TEST(CliProviderRemoteInstallRechecksCachedBusyStateBeforeMutation)
{
	const std::string identity = "[UAM CLI PATH] /usr/local/bin/codex|../lib/node_modules/@openai/codex/bin/codex.js\n";
	for (const std::string marker : {std::string("[UAM CLI RUNNING] 1\n"), std::string("[UAM CLI RUNNING] unknown\n"), std::string{}})
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ExecutionHost host;
		host.id = "fixture-update";
		host.transport = "ssh";
		host.ssh_alias = "fixture-update";
		host.platform = "linux";
		host.runner_status = "ready";
		host.runner_version = "fixture";
		host.runner_protocol_version = 3;
		app.settings.execution_hosts.push_back(host);
		uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", host.id)];
		state.execution_host = host;
		state.checked = true;
		state.installed_version = "0.148.0";
		state.install_method = "npm";
		state.raw_output = identity + marker + "codex-cli 0.148.0\n";
		std::string error;
		UAM_ASSERT(ProviderCliCompatibilityService().StartInstallProviderVersion(app, "codex-cli", "latest", &error, host.id, false, true));
		UAM_ASSERT(app.runtime_cli_install_tasks.empty());
		UAM_ASSERT(ValidateRemoteCliInstallProbeForTests(state.raw_output, identity + "[UAM CLI RUNNING] 0\n0.148.0", "linux", &error));
		UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(state.raw_output, identity + "[UAM CLI RUNNING] 1\n0.148.0", "linux", &error));
		UAM_ASSERT(!ValidateRemoteCliInstallProbeForTests(state.raw_output, identity + "[UAM CLI RUNNING] unknown\n0.148.0", "linux", &error));
		state.raw_output = "[UAM CLI PATH] relative/codex|../lib/node_modules/@openai/codex/bin/codex.js\n0.148.0";
		UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, "codex-cli", "latest", &error, host.id, false, true));
	}
}

UAM_TEST(CliProviderHomebrewSelectionMatchesActualUpgradeWithoutWeakeningExactPins)
{
	for (const std::string method : {std::string("homebrew-cask"), std::string("npm")})
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id["codex-cli"];
		state.install_method = method;
		std::string error;
		UAM_ASSERT(ProviderCliCompatibilityService().StartInstallProviderVersion(app, "codex-cli", "0.148.0", &error, {}, false, true));
		UAM_ASSERT_EQ(state.selected_version, method == "homebrew-cask" ? std::string("latest") : std::string("0.148.0"));
		state.last_install_status = "succeeded";
		state.install_output = "Installer completed";
		app.runtime_cli_version_provider_id = "codex-cli";
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
		app.runtime_cli_version_check_task.state->result.output = "codex-cli 0.162.0\n";
		app.runtime_cli_version_check_task.state->completed.store(true);
		ProviderCliCompatibilityService().Poll(app);
		UAM_ASSERT_EQ(state.last_install_status, method == "homebrew-cask" ? std::string("succeeded") : std::string("failed"));
		UAM_ASSERT(app.runtime_cli_install_tasks.empty());
	}
}

// Process-free completed-update recheck: no installer task is consumed because that
// would enqueue a real probe. The only task below is an already-completed receipt.
static uam::CliProviderVersionState OpenCodeCompletedUpdateRecheckForTests(
    const std::string& method, const std::string& observed_version)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ProviderCliCompatibilityService service;
	auto& state = app.runtime_cli_versions_by_provider_id["opencode-cli"];
	const std::string executable = method == "homebrew-formula" ? "/opt/homebrew/bin/opencode" : "/fixture/npm/bin/opencode";
	const auto probe = [&](const std::string& version)
	{
		const std::string target = method == "homebrew-formula" ?
		    "../Cellar/opencode/" + version + "/bin/opencode" : "../lib/node_modules/opencode-ai/bin/opencode";
		return "[UAM CLI PATH] " + executable + "|" + target + "\n[UAM CLI RUNNING] 0\nopencode " + version + "\n";
	};
	state.checked = true;
	state.installed_version = "1.19.3";
	state.install_method = method;
	state.raw_output = probe(state.installed_version);
	const std::string original_probe = state.raw_output;
	const std::string offered_version = "2.1.2"; // Fixed observed offer; no current-latest lookup.
	std::string error;
	UAM_ASSERT(service.StartInstallProviderVersion(app, "opencode-cli", offered_version, &error, {}, false, true));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(state.selected_version, method == "homebrew-formula" ? std::string("latest") : offered_version);
	UAM_ASSERT_EQ(state.install_command, method == "homebrew-formula" ?
	    std::string("brew upgrade opencode") : std::string("npm install -g opencode-ai@2.1.2"));
	UAM_ASSERT(app.runtime_cli_install_tasks.empty());
	UAM_ASSERT(app.pending_cli_updates.empty());
	UAM_ASSERT(app.runtime_cli_version_check_queue.empty());
	UAM_ASSERT(!app.runtime_cli_version_check_task.running);
	UAM_ASSERT(app.runtime_cli_version_check_task.worker == nullptr);
	UAM_ASSERT(app.runtime_cli_version_check_task.state == nullptr);

	// Seed the existing command-success boundary, not a worker/native result claim.
	state.last_install_status = "succeeded";
	state.install_output = "Synthetic package-manager command completed with status zero.";
	app.runtime_cli_version_provider_id = "opencode-cli";
	auto& check = app.runtime_cli_version_check_task;
	check.execution_host = state.execution_host;
	check.running = true;
	check.state = std::make_shared<AsyncProcessTaskState>();
	check.state->result.ok = true;
	check.state->result.exit_code = 0;
	check.state->result.output = probe(observed_version);
	if (observed_version == "1.19.3") UAM_ASSERT_EQ(check.state->result.output, original_probe);
	UAM_ASSERT(check.worker == nullptr);
	check.state->completed.store(true, std::memory_order_release);
	service.Poll(app);

	UAM_ASSERT(!check.running && check.worker == nullptr && check.state == nullptr);
	UAM_ASSERT(app.runtime_cli_install_tasks.empty());
	UAM_ASSERT(app.pending_cli_updates.empty());
	UAM_ASSERT(app.runtime_cli_version_check_queue.empty());
	UAM_ASSERT(state.checked && state.supported && state.check_error.empty());
	UAM_ASSERT_EQ(state.install_method, method);
	UAM_ASSERT_EQ(state.installed_version, observed_version);
	return state;
}

UAM_TEST(OpenCodeHomebrewCompletedNoOpDoesNotClaimOfferedUpdateAchieved)
{
	const auto result = OpenCodeCompletedUpdateRecheckForTests("homebrew-formula", "1.19.3");
	UAM_ASSERT_EQ(result.selected_version, std::string("latest"));
	// The requested 2.1.2 update was not achieved; no particular failure wording is required.
	UAM_ASSERT(result.last_install_status != "succeeded");
}

UAM_TEST(OpenCodeNpmCompletedMismatchedTargetRemainsFailed)
{
	const auto result = OpenCodeCompletedUpdateRecheckForTests("npm", "1.19.3");
	UAM_ASSERT_EQ(result.selected_version, std::string("2.1.2"));
	UAM_ASSERT_EQ(result.last_install_status, std::string("failed"));
}

UAM_TEST(OpenCodeHomebrewCompletedBelowOfferedMinimumDoesNotClaimUpdateAchieved)
{
	const auto result = OpenCodeCompletedUpdateRecheckForTests("homebrew-formula", "1.20.0");
	UAM_ASSERT_EQ(result.selected_version, std::string("latest"));
	// A real increase is insufficient when the requested update was 2.1.2.
	UAM_ASSERT(result.last_install_status != "succeeded");
}

UAM_TEST(OpenCodeHomebrewCompletedNewerThanOfferRemainsSuccessful)
{
	const auto result = OpenCodeCompletedUpdateRecheckForTests("homebrew-formula", "2.1.3");
	UAM_ASSERT_EQ(result.selected_version, std::string("latest"));
	UAM_ASSERT_EQ(result.last_install_status, std::string("succeeded"));
}

UAM_TEST(CliProviderVersionCompletionsRejectReconfiguredOrRemovedHosts)
{
	for (const bool installing : {false, true})
	{
		for (const bool removed : {false, true})
		{
			uam::AppState app;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ExecutionHost original;
			original.id = "fixture-remote";
			original.transport = "ssh";
			original.ssh_alias = "old-fixture";
			original.platform = "linux";
			original.runner_version = "fixture";
			original.runner_protocol_version = 3;
			ExecutionHost replacement = original;
			replacement.ssh_alias = "replacement-fixture";
			if (!removed) app.settings.execution_hosts.push_back(replacement);
			const std::string key = CliProviderVersionStateKey(uam::provider_ids::kCodexCli, original.id);
			uam::CliProviderVersionState& cached = app.runtime_cli_versions_by_provider_id[key];
			cached.execution_host = removed ? original : replacement;
			cached.checked = true;
			cached.installed_version = "0.149.0";
			cached.raw_output = "replacement output";
			cached.last_install_status = "untouched";
			uam::AsyncCommandTask& task = installing ? app.runtime_cli_install_tasks[key] : app.runtime_cli_version_check_task;
			task.running = true;
			task.execution_host = original;
			task.state = std::make_shared<AsyncProcessTaskState>();
			task.state->result.output = "codex-cli 0.148.0";
			task.state->completed.store(true);

			app.runtime_cli_version_provider_id = uam::provider_ids::kCodexCli;
			if (!installing)
			{
				const std::string queued_key = CliProviderVersionStateKey(uam::provider_ids::kOpenCodeCli, original.id);
				app.runtime_cli_versions_by_provider_id[queued_key].execution_host = original;
				app.runtime_cli_version_check_queue.emplace_back(uam::provider_ids::kOpenCodeCli, original.id);
			}
			ProviderCliCompatibilityService service;
			service.Poll(app);
			UAM_ASSERT(std::ranges::none_of(app.runtime_cli_install_tasks, [](const auto& entry) { return entry.second.running; }));
			UAM_ASSERT(!app.runtime_cli_version_check_task.running);
			if (removed)
			{
				UAM_ASSERT(!app.runtime_cli_versions_by_provider_id.contains(key));
			}
			else
			{
				const uam::CliProviderVersionState& result = app.runtime_cli_versions_by_provider_id.at(key);
				UAM_ASSERT_EQ(result.installed_version, std::string("0.149.0"));
				UAM_ASSERT_EQ(result.raw_output, std::string("replacement output"));
				UAM_ASSERT_EQ(result.last_install_status, std::string("untouched"));
				// Cached results from the previous connection are invalidated even without a task.
				app.runtime_cli_versions_by_provider_id.at(key).execution_host = original;
				service.Poll(app);
				UAM_ASSERT(!app.runtime_cli_versions_by_provider_id.contains(key));
			}
		}
	}
}

UAM_TEST(CliProviderVersionProbeUpdatesRemoteHelperHealthWithoutConfusingMissingCli)
{
	for (const std::optional<bool> connected : {std::optional<bool>{false}, std::optional<bool>{true}, std::optional<bool>{}})
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ExecutionHost host;
		host.id = "fixture-remote";
		host.transport = "ssh";
		host.ssh_alias = "fixture-remote";
		host.platform = "linux";
		host.runner_status = "ready";
		host.runner_version = "fixture";
		host.runner_protocol_version = 3;
		host.last_seen_at = "before";
		app.settings.execution_hosts.push_back(host);
		app.runtime_cli_version_provider_id = uam::provider_ids::kOpenCodeCli;
		uam::AsyncCommandTask& task = app.runtime_cli_version_check_task;
		task.running = true;
		task.execution_host = host;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->remote_helper_connected = connected;
		task.state->result.output = connected == false
		    ? "Failed to run command: SSH helper unavailable"
		    : "[UAM CLI PATH] |\nopencode: command not found";
		task.state->completed.store(true);

		ProviderCliCompatibilityService().Poll(app);

		const ExecutionHost& observed = app.settings.execution_hosts.front();
		UAM_ASSERT_EQ(observed.runner_status, connected == false ? std::string("error") : std::string("ready"));
		UAM_ASSERT_EQ(app.remote_host_health_changed, connected.has_value());
		UAM_ASSERT_EQ(observed.last_seen_at != "before", connected == true);
	}
}

UAM_TEST(CliProviderVersionProbeRejectsSemverFromFailedCommand)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.runtime_cli_version_provider_id = uam::provider_ids::kCopilotCli;
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
	app.runtime_cli_version_check_task.state->result.output = "Failed to run command: copilot --version\n\n"
	                                                          "GitHub Copilot CLI requires Node.js 22.0.0; current version is 20.11.1.";
	app.runtime_cli_version_check_task.state->completed.store(true);

	ProviderCliCompatibilityService().Poll(app);

	const uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCopilotCli);
	UAM_ASSERT(state.checked);
	UAM_ASSERT(!state.supported);
	UAM_ASSERT(state.installed_version.empty());
	UAM_ASSERT(state.message.find("Could not check") != std::string::npos);
}

UAM_TEST(CopilotVersionProbeReportsMissingWindowsPowerShellPrerequisite)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.runtime_cli_version_provider_id = uam::provider_ids::kCopilotCli;
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
	app.runtime_cli_version_check_task.state->result.output = "GitHub Copilot CLI requires PowerShell 6 or newer; pwsh was not found.\n\n"
	                                                          "[Provider CLI exited with code 1]";
	app.runtime_cli_version_check_task.state->completed.store(true);

	ProviderCliCompatibilityService().Poll(app);

	const uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCopilotCli);
	UAM_ASSERT_EQ(state.message, std::string("GitHub Copilot CLI requires PowerShell 6 or newer (pwsh) on Windows."));
}

UAM_TEST(CopilotVersionProbeReportsMissingCliBeforePowerShellPrerequisite)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.runtime_cli_version_provider_id = uam::provider_ids::kCopilotCli;
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
	app.runtime_cli_version_check_task.state->result.output = "[UAM CLI PATH] |\ncopilot was not found on PATH\n";
	app.runtime_cli_version_check_task.state->completed.store(true);

	ProviderCliCompatibilityService().Poll(app);

	const uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCopilotCli);
	UAM_ASSERT_EQ(state.message, std::string("GitHub Copilot CLI is not installed or not on PATH."));
	UAM_ASSERT(state.check_error.empty());
}

UAM_TEST(CliProviderInstallTreatsLaunchAndTimeoutFailuresAsFailures)
{
	for (const std::string output : {
	         std::string("Failed to run command: npm install -g @github/copilot@latest"),
	         std::string("partial output\n\n[Provider CLI command timed out]"),
	         std::string("bounded prefix\n\n[Provider CLI command failed: Command output exceeded the 4 MiB safety limit.]"),
	     })
	{
		uam::AppState app;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

		app.runtime_cli_install_tasks[uam::provider_ids::kCopilotCli].running = true;
		app.runtime_cli_install_tasks[uam::provider_ids::kCopilotCli].state = std::make_shared<AsyncProcessTaskState>();
		app.runtime_cli_install_tasks[uam::provider_ids::kCopilotCli].state->result.output = output;
		app.runtime_cli_install_tasks[uam::provider_ids::kCopilotCli].state->completed.store(true);

		ProviderCliCompatibilityService().Poll(app);

		const uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCopilotCli);
		UAM_ASSERT_EQ(state.last_install_status, std::string("failed"));
		UAM_ASSERT_EQ(state.message, std::string("Update command failed."));
		UAM_ASSERT(!app.runtime_cli_version_check_task.running);
	}
}

UAM_TEST(CliProviderVersionInstallRejectsUnknownProvider)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	std::string error;

	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, "unknown-provider", "0.38.1", &error));
	UAM_ASSERT_EQ(error, std::string("Unsupported provider: unknown-provider"));
	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, " UNKNOWN-PROVIDER ", "0.38.1", &error));
	UAM_ASSERT_EQ(error, std::string("Unsupported provider: unknown-provider"));
	UAM_ASSERT(std::ranges::none_of(app.runtime_cli_install_tasks, [](const auto& entry) { return entry.second.running; }));
}

UAM_TEST(CliProviderVersionInstallBlocksActiveProviderAliases)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	std::string error;

#if UAM_ENABLE_RUNTIME_CODEX_CLI
	auto session = std::make_unique<uam::AcpSessionState>();
	session->provider_id = " CoDeX ";
	session->prompt_request_id = 42;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, uam::provider_ids::kCodexCli, "0.124.0", &error));
	UAM_ASSERT_EQ(error, std::string("Provider sessions are running. Stop the affected sessions and retry the update."));
#elif UAM_ENABLE_RUNTIME_GEMINI_CLI
	auto session = std::make_unique<uam::AcpSessionState>();
	session->provider_id = " GEMINI ";
	session->prompt_request_id = 42;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, uam::provider_ids::kGeminiCli, std::string(uam::PreferredGeminiCliVersion()), &error));
	UAM_ASSERT_EQ(error, std::string("Provider sessions are running. Stop the affected sessions and retry the update."));
#else
	(void)app;
	(void)error;
#endif
}

UAM_TEST(CliProviderVersionInstallAllowsCompatibilityBlockedCopilotDeferredQueue)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "local-copilot-update";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	app.chats.push_back(chat);
	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	version.checked = true;
	version.supported = false;
	version.installed_version = "1.0.68";

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->reconnect_pending = true;
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Retry after update"});
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(!ProviderCliInstallBlockedByActiveRuntimeForTests(app, uam::provider_ids::kCopilotCli));
	raw_session->processing = true;
	UAM_ASSERT(ProviderCliInstallBlockedByActiveRuntimeForTests(app, uam::provider_ids::kCopilotCli));
	raw_session->processing = false;
	version.supported = true;
	UAM_ASSERT(ProviderCliInstallBlockedByActiveRuntimeForTests(app, uam::provider_ids::kCopilotCli));
#endif
}

UAM_TEST(CliProviderVersionInstallBlocksTerminalsMatchedByNativeSession)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	std::string error;

#if UAM_ENABLE_RUNTIME_CODEX_CLI || UAM_ENABLE_RUNTIME_GEMINI_CLI
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const std::string provider_id = uam::provider_ids::kCodexCli;
#else
	const std::string provider_id = uam::provider_ids::kGeminiCli;
#endif
	ChatSession chat = ChatDomainService().CreateNewChat("folder-1", provider_id);
	chat.native_session_id = "native-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->attached_session_id = "native-session";
	terminal->running = true;
	terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(terminal));

	const std::string version = ProviderCliCompatibilityService().PreferredVersionForProvider(provider_id);
	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, provider_id, version, &error));
	UAM_ASSERT_EQ(error, std::string("Provider sessions are running. Stop the affected sessions and retry the update."));

	app.cli_terminals.clear();
	app.chats.clear();
	app.settings.active_provider_id = provider_id;
	ChatSession active_provider_chat = ChatDomainService().CreateNewChat("folder-1", "");
	active_provider_chat.native_session_id = "active-native-session";
	app.chats.push_back(active_provider_chat);

	auto active_provider_terminal = std::make_unique<uam::CliTerminalState>();
	active_provider_terminal->attached_session_id = "active-native-session";
	active_provider_terminal->running = true;
	active_provider_terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(active_provider_terminal));

	error.clear();
	UAM_ASSERT(!ProviderCliCompatibilityService().StartInstallProviderVersion(app, provider_id, version, &error));
	UAM_ASSERT_EQ(error, std::string("Provider sessions are running. Stop the affected sessions and retry the update."));
#else
	(void)app;
	(void)error;
#endif
}

UAM_TEST(CliProviderVersionParserExtractsSemverFromNoisyOutput)
{
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests("OpenAI Codex v0.124.0\n"), std::string("0.124.0"));
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests(std::string_view("xxOpenAI Codex v0.124.0yy").substr(2, 22)), std::string("0.124.0"));
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests("gemini 0.38.1\nextra 0.1.0"), std::string("0.38.1"));
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests("release 10.20.30-beta"), std::string("10.20.30"));
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests("Provider CLI returned no output."), std::string(""));
	UAM_ASSERT_EQ(ExtractCliProviderSemverVersionForTests("version 1.2"), std::string(""));
	UAM_ASSERT(CliProviderVersionOutputIndicatesMissingCommandForTests("zsh: command not found: codex"));
	UAM_ASSERT(CliProviderVersionOutputIndicatesMissingCommandForTests(std::string_view("xxzsh: command not found: codexyy").substr(2, 29)));
	UAM_ASSERT(CliProviderVersionOutputIndicatesMissingCommandForTests("'codex' is NOT RECOGNIZED as an internal or external command"));
	UAM_ASSERT(CliProviderVersionOutputIndicatesMissingCommandForTests("spawn opencode ENOENT: no such file or directory"));
	UAM_ASSERT(!CliProviderVersionOutputIndicatesMissingCommandForTests("codex version output was malformed"));
}

UAM_TEST(StateSerializerIncludesMessageToolCalls)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.title = "Tool chat";
	chat.provider_id = "gemini-cli";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "Done.";
	assistant.created_at = "2026-01-01T00:00:01.000Z";
	assistant.provider = "codex-cli";
	ToolCall tool_call;
	tool_call.id = "tool-1";
	tool_call.name = "Read file";
	tool_call.args_json = R"({"path":"file.txt"})";
	tool_call.result_text = "file contents";
	tool_call.status = "completed";
	assistant.tool_calls.push_back(std::move(tool_call));
	assistant.plan_summary = "Implement the focused fix.";
	assistant.markdown_store_files.push_back("docs/goal.uam");
	MessagePlanEntry plan_entry;
	plan_entry.content = "Update Codex app-server handling";
	plan_entry.priority = "1";
	plan_entry.status = "completed";
	assistant.plan_entries.push_back(std::move(plan_entry));
	MessageBlock tool_block;
	tool_block.type = "tool_call";
	tool_block.tool_call_id = "tool-1";
	assistant.blocks.push_back(std::move(tool_block));
	MessageBlock plan_block;
	plan_block.type = "plan";
	assistant.blocks.push_back(std::move(plan_block));
	MessageAttachment attachment;
	attachment.id = "attachment-1";
	attachment.name = "notes.txt";
	attachment.kind = "file";
	attachment.mime_type = "text/plain";
	attachment.path = ".UAM/attachments/chat-1/notes.txt";
	attachment.size_bytes = 12;
	attachment.copied = true;
	assistant.attachments.push_back(std::move(attachment));
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));
	app.selected_chat_index = 0;

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json tool_json = serialized["chats"][0]["messages"][0]["toolCalls"][0];
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0].value("providerId", ""), std::string("codex-cli"));
	UAM_ASSERT_EQ(tool_json.value("id", ""), std::string("tool-1"));
	UAM_ASSERT_EQ(tool_json.value("title", ""), std::string("Read file"));
	UAM_ASSERT_EQ(tool_json.value("status", ""), std::string("completed"));
	UAM_ASSERT(!tool_json.value("contentDeferred", true));
	UAM_ASSERT(tool_json.contains("content"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0].value("planSummary", ""), std::string("Implement the focused fix."));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["planEntries"][0].value("content", ""), std::string("Update Codex app-server handling"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["planEntries"][0].value("status", ""), std::string("completed"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][0].value("type", ""), std::string("tool_call"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][0].value("toolCallId", ""), std::string("tool-1"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][1].value("type", ""), std::string("plan"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["markdownStoreFiles"][0].get<std::string>(), std::string("docs/goal.uam"));
	const nlohmann::json attachment_json = serialized["chats"][0]["messages"][0]["attachments"][0];
	UAM_ASSERT_EQ(attachment_json.value("id", ""), std::string("attachment-1"));
	UAM_ASSERT_EQ(attachment_json.value("name", ""), std::string("notes.txt"));
	UAM_ASSERT_EQ(attachment_json.value("kind", ""), std::string("file"));
	UAM_ASSERT_EQ(attachment_json.value("type", ""), std::string("text/plain"));
	UAM_ASSERT_EQ(attachment_json.value("path", ""), std::string(".UAM/attachments/chat-1/notes.txt"));
	UAM_ASSERT_EQ(attachment_json.value("size", 0), 12);
	UAM_ASSERT(attachment_json.value("copied", false));
}

UAM_TEST(ProviderRegistryResolvesGeminiCodexClaudeOpenCodeCopilotAndUnknownExactly)
{
	const IProviderRuntime& gemini = ProviderRuntimeRegistry::ResolveById("gemini-cli");
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	UAM_ASSERT_EQ(std::string(gemini.RuntimeId()), std::string("gemini-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("gemini-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId(std::string_view("xxgeminiyy").substr(2, 6)));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId(" Gemini-Cli "));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById(" Gemini-Cli ").RuntimeId()), std::string("gemini-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId(" GEMINI "));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById(" GEMINI ").RuntimeId()), std::string("gemini-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("gemini-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled(std::string_view("xxgeminiyy").substr(2, 6)));
#else
	UAM_ASSERT_EQ(std::string(gemini.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("gemini-cli"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("gemini-cli"));
#endif

	const IProviderRuntime& codex = ProviderRuntimeRegistry::ResolveById("codex-cli");
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	UAM_ASSERT_EQ(std::string(codex.RuntimeId()), std::string("codex-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("codex-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId(" CoDeX "));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById(" CoDeX ").RuntimeId()), std::string("codex-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("codex-cli"));
#else
	UAM_ASSERT_EQ(std::string(codex.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("codex-cli"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("codex-cli"));
#endif

	const IProviderRuntime& unknown = ProviderRuntimeRegistry::ResolveById("unknown");
	UAM_ASSERT_EQ(std::string(unknown.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!unknown.SupportsTextWorkers());
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("unknown"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("unknown"));

	const IProviderRuntime& claude = ProviderRuntimeRegistry::ResolveById("claude-cli");
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	UAM_ASSERT_EQ(std::string(claude.RuntimeId()), std::string("claude-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("claude-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("claude"));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById("claude-code").RuntimeId()), std::string("claude-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("claude-cli"));
#else
	UAM_ASSERT_EQ(std::string(claude.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("claude-cli"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("claude-cli"));
#endif

	const IProviderRuntime& opencode = ProviderRuntimeRegistry::ResolveById("opencode-cli");
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	UAM_ASSERT_EQ(std::string(opencode.RuntimeId()), std::string("opencode-cli"));
	UAM_ASSERT_EQ(opencode.OnAcpMapApprovalModeId("default"), std::string("build"));
	UAM_ASSERT_EQ(opencode.OnAcpMapApprovalModeId("plan"), std::string("plan"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("opencode-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("open-code"));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById("opencode").RuntimeId()), std::string("opencode-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("opencode-cli"));
	const auto opencode_environment = opencode.BuildStructuredLaunchEnvironment({}, {});
	UAM_ASSERT_EQ(opencode_environment.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(opencode_environment.front().first, std::string("OPENCODE_PERMISSION"));
	const nlohmann::json opencode_permissions = nlohmann::json::parse(opencode_environment.front().second);
	UAM_ASSERT_EQ(opencode_permissions.value("*", ""), std::string("ask"));
	UAM_ASSERT_EQ(opencode_permissions.value("task", ""), std::string("deny"));
	UAM_ASSERT_EQ(opencode_permissions.value("uam-computer_computer_observe", ""), std::string("allow"));
	UAM_ASSERT_EQ(opencode_permissions.value("uam-computer_computer_action", ""), std::string("allow"));
#else
	UAM_ASSERT_EQ(std::string(opencode.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("opencode-cli"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("opencode-cli"));
#endif

	const IProviderRuntime& copilot = ProviderRuntimeRegistry::ResolveById("copilot-cli");
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	UAM_ASSERT_EQ(std::string(copilot.RuntimeId()), std::string("copilot-cli"));
	UAM_ASSERT_EQ(copilot.OnAcpMapApprovalModeId("default"), std::string(uam::approval_modes::kAcpAgentMode));
	UAM_ASSERT_EQ(copilot.OnAcpMapApprovalModeId("acceptEdits"), std::string(uam::approval_modes::kAcpAgentMode));
	UAM_ASSERT_EQ(copilot.OnAcpMapApprovalModeId("plan"), std::string(uam::approval_modes::kAcpPlanMode));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("copilot-cli"));
	UAM_ASSERT(ProviderRuntimeRegistry::IsEnabledRuntimeId("github-copilot"));
	UAM_ASSERT_EQ(std::string(ProviderRuntimeRegistry::ResolveById("copilot").RuntimeId()), std::string("copilot-cli"));
	UAM_ASSERT(ProviderRuntime::IsRuntimeEnabled("copilot-cli"));
#else
	UAM_ASSERT_EQ(std::string(copilot.RuntimeId()), std::string("unsupported"));
	UAM_ASSERT(!ProviderRuntimeRegistry::IsEnabledRuntimeId("copilot-cli"));
	UAM_ASSERT(!ProviderRuntime::IsRuntimeEnabled("copilot-cli"));
#endif
}

UAM_TEST(BuiltInProviderProfilesFollowEnabledRuntimeFlags)
{
	const std::vector<ProviderProfile> profiles = ProviderProfileStore::BuiltInProfiles();
	std::vector<std::string> ids;
	for (const ProviderProfile& profile : profiles)
	{
		ids.push_back(profile.id);
	}

	const auto find_profile = [&profiles](const std::string& id) -> const ProviderProfile*
	{
		const auto it = std::ranges::find_if(profiles, [&id](const ProviderProfile& profile) { return profile.id == id; });
		return it == profiles.end() ? nullptr : &*it;
	};

	std::vector<std::string> expected;
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	expected.push_back("gemini-cli");
	const ProviderProfile* gemini_profile = find_profile(uam::provider_ids::kGeminiCli);
	UAM_ASSERT(gemini_profile != nullptr);
	UAM_ASSERT_EQ(gemini_profile->structured_protocol, std::string(uam::provider_profile_constants::kProtocolGeminiAcp));
	UAM_ASSERT_EQ(gemini_profile->history_adapter, std::string(uam::provider_profile_constants::kHistoryAdapterGeminiCliJson));
	UAM_ASSERT_EQ(gemini_profile->prompt_bootstrap, std::string(uam::provider_profile_constants::kPromptBootstrapGeminiAtPath));
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	expected.push_back("codex-cli");
	const ProviderProfile* codex_profile = find_profile(uam::provider_ids::kCodexCli);
	UAM_ASSERT(codex_profile != nullptr);
	UAM_ASSERT_EQ(codex_profile->structured_protocol, std::string(uam::provider_profile_constants::kProtocolCodexAppServer));
	UAM_ASSERT_EQ(codex_profile->history_adapter, std::string(uam::provider_profile_constants::kHistoryAdapterLocalJson));
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	expected.push_back("claude-cli");
	const ProviderProfile* claude_profile = find_profile(uam::provider_ids::kClaudeCli);
	UAM_ASSERT(claude_profile != nullptr);
	UAM_ASSERT_EQ(claude_profile->structured_protocol, std::string(uam::provider_profile_constants::kProtocolClaudeCodeStreamJson));
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	expected.push_back("opencode-cli");
	const ProviderProfile* opencode_profile = find_profile(uam::provider_ids::kOpenCodeCli);
	UAM_ASSERT(opencode_profile != nullptr);
	UAM_ASSERT_EQ(opencode_profile->structured_protocol, std::string(uam::provider_profile_constants::kProtocolOpenCodeAcp));
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	expected.push_back("copilot-cli");
	const ProviderProfile* copilot_profile = find_profile(uam::provider_ids::kCopilotCli);
	UAM_ASSERT(copilot_profile != nullptr);
	UAM_ASSERT_EQ(copilot_profile->structured_protocol, std::string(uam::provider_profile_constants::kProtocolCopilotSdk));
#endif
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	expected.push_back("antigravity-cli");
	const ProviderProfile* antigravity_profile = find_profile(uam::provider_ids::kAntigravityCli);
	UAM_ASSERT(antigravity_profile != nullptr);
	UAM_ASSERT(antigravity_profile->supports_structured);
#endif
	UAM_ASSERT_EQ(ids, expected);
}

UAM_TEST(CanonicalUuidValidationRejectsMalformedRunIds)
{
	UAM_ASSERT(uam::uuid::IsCanonicalUuid("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT(uam::uuid::IsCanonicalUuid("6A6F0F3B-1A0B-4A9C-8A01-111111111111"));
	for (const char* id : {"", "6a6f0f3b1a0b4a9c8a01111111111111", "urn:uuid:6a6f0f3b-1a0b-4a9c-8a01-111111111111",
	    " 6a6f0f3b-1a0b-4a9c-8a01-111111111111", "6a6f0f3b_1a0b-4a9c-8a01-111111111111", "6a6f0f3b-1a0b-4a9c-8a01-zzzzzzzzzzzz"})
	{
		UAM_ASSERT(!uam::uuid::IsCanonicalUuid(id));
	}
}

UAM_TEST(CodexThreadIdValidatorAcceptsOnlyUuidThreadIds)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	for (const std::string& id : {std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"), std::string("urn:uuid:6a6f0f3b-1a0b-4a9c-8a01-111111111111")})
	{
		chat.native_session_id = id;
		UAM_ASSERT_EQ(runtime.OnAcpValidateResumeId(chat), id);
	}
	chat.native_session_id = " 6a6f0f3b-1a0b-4a9c-8a01-111111111111 ";
	UAM_ASSERT_EQ(runtime.OnAcpValidateResumeId(chat), std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	for (const char* id : {"", "chat-1", "native-abc", "thread-1", "6a6f0f3b-1a0b-4a9c-8a01-zzzzzzzzzzzz"})
	{
		chat.native_session_id = id;
		UAM_ASSERT(runtime.OnAcpValidateResumeId(chat).empty());
	}
#endif
}

#if defined(__APPLE__)
UAM_TEST(MacPlatformGeneratesCanonicalVersionFourUuids)
{
	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const std::string uuid = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		UAM_ASSERT_EQ(uuid.size(), static_cast<std::size_t>(36));
		for (std::size_t index = 0; index < uuid.size(); ++index)
		{
			UAM_ASSERT(index == 8 || index == 13 || index == 18 || index == 23
			               ? uuid[index] == '-'
			               : std::string_view("0123456789abcdef").find(uuid[index]) != std::string_view::npos);
		}
		UAM_ASSERT_EQ(uuid[14], '4');
		UAM_ASSERT(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
	}
}
#endif

UAM_TEST(EnabledProviderRuntimesSatisfyLaunchContract)
{
	ChatSession chat;
	chat.id = "contract-chat";
	AppSettings settings;
	for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
		chat.provider_id = profile.id;
		UAM_ASSERT(runtime.IsEnabled());
		UAM_ASSERT_EQ(std::string(runtime.RuntimeId()), profile.id);
		UAM_ASSERT(!runtime.BuildInteractiveArgv(profile, chat, settings).empty());
		UAM_ASSERT_EQ(runtime.BuildWorkerArgv(profile, settings, "contract prompt", "").empty(), !runtime.SupportsTextWorkers());
		UAM_ASSERT_EQ(runtime.BuildStructuredLaunchArgv(profile, chat).empty(), !profile.supports_structured);
	}
}

UAM_TEST(ProviderTextWorkersAreIsolatedAndIgnoreUnsafeExtraFlags)
{
	uam::AppState app;
	uam::CliProviderVersionState& copilot_version = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	copilot_version.checked = true;
	copilot_version.supported = true;
	uam::CliProviderVersionState& opencode_version = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kOpenCodeCli];
	opencode_version.execution_host = uam::execution_hosts::LocalHost();
	opencode_version.checked = true;
	opencode_version.supported = true;
	opencode_version.installed_version = "1.19.4";
	AppSettings settings;
	settings.provider_extra_flags = "--dangerously-skip-permissions --yolo --allow-all --ask-for-approval never --agent unsafe --plugin-dir injected";

	for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
	{
		if (!ProviderRuntimeRegistry::Resolve(profile).SupportsTextWorkers()) continue;
		std::filesystem::path isolation_directory;
		{
			std::string error;
			const uam::ProviderWorkerInvocation invocation = uam::BuildProviderWorkerInvocation(app, profile, settings, "text-only worker prompt", "", uam::ProviderWorkerPathMode::BasePath, &error);
			UAM_ASSERT(!invocation.Empty());
			UAM_ASSERT(error.empty());
			UAM_ASSERT(invocation.isolated_working_directory != nullptr);
			UAM_ASSERT(invocation.direct_process);
			UAM_ASSERT(invocation.shell_command.empty());
			UAM_ASSERT_EQ(invocation.standard_input, std::string("text-only worker prompt"));
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "text-only worker prompt"));
			UAM_ASSERT(invocation.command_preview.find("text-only worker prompt") == std::string::npos);
			isolation_directory = *invocation.isolated_working_directory;
			UAM_ASSERT(uam::paths::IsDirectoryNoThrow(isolation_directory));
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "--dangerously-skip-permissions"));
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "--yolo"));
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "--allow-all"));
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "unsafe"));
			UAM_ASSERT_EQ(uam::ranges::Contains(invocation.argv, "--agent"), profile.id == uam::provider_ids::kOpenCodeCli);
			UAM_ASSERT(!uam::ranges::Contains(invocation.argv, "--plugin-dir"));

			if (profile.id == uam::provider_ids::kCodexCli)
			{
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "read-only"));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "--ignore-user-config"));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "features.shell_tool=false"));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "web_search=\"disabled\""));
			}
			else if (profile.id == uam::provider_ids::kClaudeCli)
			{
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "--tools"));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "--no-session-persistence"));
			}
			else if (profile.id == uam::provider_ids::kOpenCodeCli)
			{
				const std::vector<std::string>::const_iterator agent = std::ranges::find(invocation.argv, "--agent");
				UAM_ASSERT(agent != invocation.argv.end() && std::next(agent) != invocation.argv.end());
				UAM_ASSERT_EQ(*std::next(agent), std::string("uam-text-worker"));
				const std::vector<std::string>::const_iterator title = std::ranges::find(invocation.argv, "--title");
				UAM_ASSERT(title != invocation.argv.end() && std::next(title) != invocation.argv.end());
				UAM_ASSERT_EQ(*std::next(title), std::string("UAM text worker"));
				const std::vector<std::string>::const_iterator directory = std::ranges::find(invocation.argv, "--dir");
				UAM_ASSERT(directory != invocation.argv.end() && std::next(directory) != invocation.argv.end());
				UAM_ASSERT_EQ(*std::next(directory), uam::paths::Utf8PathString(isolation_directory));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "--pure"));
				const std::string config = uam::io::ReadTextFile(isolation_directory / "opencode.json");
				UAM_ASSERT(uam::strings::Contains(config, R"("*":"deny")"));
				const nlohmann::json agent_config = nlohmann::json::parse(config)["agent"]["uam-text-worker"];
				UAM_ASSERT_EQ(agent_config["mode"].get<std::string>(), std::string("primary"));
				UAM_ASSERT_EQ(agent_config["permission"]["*"].get<std::string>(), std::string("deny"));
			}
			else if (profile.id == uam::provider_ids::kGeminiCli)
			{
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "--admin-policy"));
				UAM_ASSERT(uam::ranges::Contains(invocation.argv, "plan"));
				const std::string policy = uam::io::ReadTextFile(isolation_directory / "deny-all-tools.toml");
				UAM_ASSERT(uam::strings::Contains(policy, "decision = \"deny\""));
			}
		}
		UAM_ASSERT(!uam::paths::PathExistsNoThrow(isolation_directory));
	}
}

UAM_TEST(GeminiCliInteractiveArgvUsesResumeAndFlags)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultGeminiProfile();
	AppSettings settings;
	settings.provider_extra_flags = "--checkpointing";


	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-abc";

	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(argv[0], std::string("gemini"));
	UAM_ASSERT_EQ(argv[1], std::string("--checkpointing"));
	UAM_ASSERT_EQ(argv[2], std::string("-r"));
	UAM_ASSERT_EQ(argv[3], std::string("native-abc"));
	UAM_ASSERT(!uam::ranges::Contains(argv, "--yolo"));
	settings.provider_extra_flags = "--yolo --full-auto --checkpointing";
	UAM_ASSERT_EQ(ProviderRuntime::BuildInteractiveArgv(profile, chat, settings), argv);
	settings.provider_extra_flags = " --checkpointing ";
	profile.runtime_flags = {" --profile ", "   ", "--trace"};
	const std::vector<std::string> merged_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(merged_argv, (std::vector<std::string>{"gemini", "--profile", "--trace", "--checkpointing", "-r", "native-abc"}));
	profile.runtime_flags.clear();
	settings.provider_extra_flags = "--checkpointing";


	// OC-3: a per-chat model id must reach the interactive gemini argv as --model <id> (trimmed).
	chat.model_id = " gemini-2.5-pro ";
	const std::vector<std::string> argv_with_model = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv_with_model.size(), static_cast<std::size_t>(6));
	UAM_ASSERT_EQ(argv_with_model[4], std::string("--model"));
	UAM_ASSERT_EQ(argv_with_model[5], std::string("gemini-2.5-pro"));
	chat.model_id.clear();

	profile.interactive_command = "   ";
	const std::vector<std::string> defaulted_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(defaulted_argv[0], std::string("gemini-cli"));
	UAM_ASSERT(!uam::ranges::Contains(defaulted_argv, "   "));
	profile.interactive_command.clear();

	UAM_ASSERT_EQ(ProviderRuntime::RoleFromNativeType(profile, std::string_view("xxUseryy").substr(2, 4)), MessageRole::User);
#endif
}

UAM_TEST(CodexCliInteractiveArgvUsesResumeModelAndFlags)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultCodexProfile();
	AppSettings settings;
	settings.provider_extra_flags = "--sandbox danger-full-access --ask-for-approval never --quiet";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	chat.model_id = " gpt-5.4 ";
	chat.reasoning_effort = " high ";
	chat.service_tier = " fast ";
	chat.service_tier_explicit = true;

	const std::vector<std::string> fresh = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(fresh.size(), static_cast<std::size_t>(9));
	UAM_ASSERT_EQ(fresh[0], std::string("codex"));
	UAM_ASSERT_EQ(fresh[1], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(fresh[2], std::string("-m"));
	UAM_ASSERT_EQ(fresh[3], std::string("gpt-5.4"));
	UAM_ASSERT_EQ(fresh[4], std::string("-c"));
	UAM_ASSERT_EQ(fresh[5], std::string("model_reasoning_effort=\"high\""));
	UAM_ASSERT_EQ(fresh[6], std::string("-c"));
	UAM_ASSERT_EQ(fresh[7], std::string("service_tier=\"fast\""));
	UAM_ASSERT_EQ(fresh[8], std::string("--quiet"));
	UAM_ASSERT(!uam::ranges::Contains(fresh, "--full-auto"));
	UAM_ASSERT(!uam::ranges::Contains(fresh, "danger-full-access"));
	UAM_ASSERT(!uam::ranges::Contains(fresh, "never"));
	ChatSession cleared_speed = chat;
	cleared_speed.service_tier.clear();
	const std::vector<std::string> cleared_speed_argv = ProviderRuntime::BuildInteractiveArgv(profile, cleared_speed, AppSettings{});
	UAM_ASSERT(uam::ranges::Contains(cleared_speed_argv, "service_tier=null"));
	cleared_speed.service_tier_explicit = false;
	const std::vector<std::string> inherited_speed_argv = ProviderRuntime::BuildInteractiveArgv(profile, cleared_speed, AppSettings{});
	UAM_ASSERT(!uam::ranges::Contains(inherited_speed_argv, "service_tier=null"));

	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	const std::vector<std::string> resumed = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(resumed.size(), static_cast<std::size_t>(10));
	UAM_ASSERT_EQ(resumed[0], std::string("codex"));
	UAM_ASSERT_EQ(resumed[1], std::string("resume"));
	UAM_ASSERT_EQ(resumed[2], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(resumed[3], chat.native_session_id);
	UAM_ASSERT_EQ(resumed[4], std::string("-m"));
	UAM_ASSERT_EQ(resumed[5], std::string("gpt-5.4"));
	UAM_ASSERT_EQ(resumed[6], std::string("-c"));
	UAM_ASSERT_EQ(resumed[7], std::string("model_reasoning_effort=\"high\""));
	UAM_ASSERT_EQ(resumed[8], std::string("-c"));
	UAM_ASSERT_EQ(resumed[9], std::string("service_tier=\"fast\""));

	chat.native_session_id = "chat-1";
	const std::vector<std::string> invalid_resume = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(invalid_resume.size(), static_cast<std::size_t>(8));
	UAM_ASSERT_EQ(invalid_resume[0], std::string("codex"));
	UAM_ASSERT_EQ(invalid_resume[1], std::string("--no-alt-screen"));
	UAM_ASSERT_EQ(invalid_resume[2], std::string("-m"));
	UAM_ASSERT_EQ(invalid_resume[3], std::string("gpt-5.4"));
	UAM_ASSERT_EQ(invalid_resume[4], std::string("-c"));
	UAM_ASSERT_EQ(invalid_resume[5], std::string("model_reasoning_effort=\"high\""));
	UAM_ASSERT_EQ(invalid_resume[6], std::string("-c"));
	UAM_ASSERT_EQ(invalid_resume[7], std::string("service_tier=\"fast\""));

	ProviderProfile invalid_history_profile = profile;
	invalid_history_profile.id = " codex-cli ";
	invalid_history_profile.history_adapter = uam::provider_profile_constants::kHistoryAdapterGeminiCliJson;
	const std::string config_error = ProviderRuntime::DisabledReason(invalid_history_profile);
	UAM_ASSERT(config_error.find("Provider 'codex-cli'") != std::string::npos);
#endif
}

UAM_TEST(ClaudeCliInteractiveArgvUsesResumeModelModeAndFlags)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultClaudeProfile();
	UAM_ASSERT_EQ(profile.native_goal_command, std::string("/goal"));
	AppSettings settings;
	settings.provider_extra_flags = "--add-dir ../shared";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "claude-cli";
	chat.model_id = " sonnet ";
	chat.approval_mode = " plan ";

	const std::vector<std::string> fresh = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(fresh.size(), static_cast<std::size_t>(7));
	UAM_ASSERT_EQ(fresh[0], std::string("claude"));
	UAM_ASSERT_EQ(fresh[1], std::string("--model"));
	UAM_ASSERT_EQ(fresh[2], std::string("sonnet"));
	UAM_ASSERT_EQ(fresh[3], std::string("--permission-mode"));
	UAM_ASSERT_EQ(fresh[4], std::string("plan"));
	UAM_ASSERT_EQ(fresh[5], std::string("--add-dir"));
	UAM_ASSERT_EQ(fresh[6], std::string("../shared"));

	chat.native_session_id = " claude-session-1 ";
	const std::vector<std::string> resumed = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(resumed.size(), static_cast<std::size_t>(7));
	UAM_ASSERT_EQ(resumed[0], std::string("claude"));
	UAM_ASSERT_EQ(resumed[1], std::string("--resume"));
	UAM_ASSERT_EQ(resumed[2], std::string("claude-session-1"));
	UAM_ASSERT_EQ(resumed[3], std::string("--model"));
	UAM_ASSERT_EQ(resumed[4], std::string("sonnet"));
	UAM_ASSERT_EQ(resumed[5], std::string("--permission-mode"));
	UAM_ASSERT_EQ(resumed[6], std::string("plan"));

	chat.approval_mode = " acceptEdits ";
	const std::vector<std::string> accept_edits = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	const auto default_permission = std::ranges::find(accept_edits, "--permission-mode");
	UAM_ASSERT(default_permission != accept_edits.end());
	UAM_ASSERT_EQ(*(default_permission + 1), std::string("default"));

	profile.interactive_command = "claude --debug-shell";
	chat.native_session_id.clear();
	chat.approval_mode = "plan";
	const std::vector<std::string> custom_command = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(custom_command[0], std::string("claude"));
	UAM_ASSERT_EQ(custom_command[1], std::string("--debug-shell"));
	UAM_ASSERT(uam::ranges::Contains(custom_command, "--permission-mode"));
#endif
}

UAM_TEST(ClaudeStructuredArgvTranslatesUamEditTier)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ChatSession chat;
	chat.provider_id = "claude-cli";
	chat.approval_mode = "default";
	chat.command_safety_tier = "acceptEdits";

	const std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(11));
	UAM_ASSERT_EQ(argv[0], std::string("claude"));
	UAM_ASSERT_EQ(argv[9], std::string("--permission-mode"));
	UAM_ASSERT_EQ(argv[10], std::string("acceptEdits"));

	chat.approval_mode = "plan";
	const std::vector<std::string> plan_argv = uam::BuildAcpLaunchArgvForTests(chat);
	UAM_ASSERT_EQ(plan_argv[10], std::string("plan"));
#endif
}

UAM_TEST(ClaudeStructuredArgvMapsOnlySupportedPermissionModes)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ChatSession chat;
	chat.provider_id = "claude-cli";
	chat.approval_mode = "default";
	chat.command_safety_tier = "off";
	std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
	const auto default_mode = std::ranges::find(argv, "--permission-mode");
	UAM_ASSERT(default_mode != argv.end());
	UAM_ASSERT_EQ(*(default_mode + 1), std::string("default"));

	chat.command_safety_tier = "aiReview";
	argv = uam::BuildAcpLaunchArgvForTests(chat);
	const auto automatic = std::ranges::find(argv, "--permission-mode");
	UAM_ASSERT(automatic != argv.end());
	UAM_ASSERT_EQ(*(automatic + 1), std::string("auto"));

	chat.command_safety_tier = "yolo";
	argv = uam::BuildAcpLaunchArgvForTests(chat);
	const auto yolo = std::ranges::find(argv, "--permission-mode");
	UAM_ASSERT(yolo != argv.end());
	UAM_ASSERT_EQ(*(yolo + 1), std::string("default"));

	chat.approval_mode = "plan";
	argv = uam::BuildAcpLaunchArgvForTests(chat);
	const auto plan = std::ranges::find(argv, "--permission-mode");
	UAM_ASSERT(plan != argv.end());
	UAM_ASSERT_EQ(*(plan + 1), std::string("plan"));

	ChatSession previous = chat;
	previous.approval_mode = "default";
	previous.command_safety_tier = "yolo";
	ChatSession requested = previous;
	requested.command_safety_tier = "off";
	UAM_ASSERT(!uam::query_handler_internal::CommandSafetyTierNeedsLiveUpdate(previous, requested));
#endif
}

UAM_TEST(StructuredLaunchArgvIsProviderOwnedAndStable)
{
	// RT-3: each provider's structured-launch argv is owned by its runtime and resolved via
	// the registry from BuildAcpLaunchArgv. Behavior must stay byte-identical per provider.
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	{
		ChatSession chat;
		chat.provider_id = "codex-cli";
		const std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
		UAM_ASSERT_EQ(argv, (std::vector<std::string>{
		    "codex", "--disable", "computer_use", "app-server", "--listen", "stdio://"}));
	}
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	{
		ChatSession chat;
		chat.provider_id = "opencode-cli";
		const std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
		UAM_ASSERT_EQ(argv, (std::vector<std::string>{"opencode", "acp"}));
	}
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	{
		ChatSession chat;
		chat.provider_id = "copilot-cli";
		const std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
		UAM_ASSERT_EQ(argv, (std::vector<std::string>{"copilot", "--headless", "--stdio", "--no-auto-update"}));
	}
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	{
		ChatSession chat;
		chat.provider_id = "gemini-cli";
		chat.model_id = " gemini-2.5-pro ";
		chat.id = "gemini-chat";
		const std::vector<std::string> argv = uam::BuildAcpLaunchArgvForTests(chat);
		UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(6));
		UAM_ASSERT_EQ(argv[0], std::string("gemini"));
		UAM_ASSERT_EQ(argv[1], std::string("--acp"));
		UAM_ASSERT_EQ(argv[2], std::string("--model"));
		UAM_ASSERT_EQ(argv[3], std::string("gemini-2.5-pro"));
		UAM_ASSERT_EQ(argv[4], std::string("--policy"));
		UAM_ASSERT_EQ(std::filesystem::path(argv[5]).filename().string(),
		    std::string("gemini-policy.toml"));
	}
#endif
}

UAM_TEST(OpenCodeCliBuildsCommandsAndInteractiveArgv)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
	AppSettings settings;
	settings.provider_extra_flags = "--agent build";

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	chat.native_session_id = " session-abc ";
	chat.model_id = " anthropic/claude-sonnet-4 ";

	const std::vector<std::string> argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, settings);
	UAM_ASSERT_EQ(argv.size(), static_cast<std::size_t>(7));
	UAM_ASSERT_EQ(argv[0], std::string("opencode"));
	UAM_ASSERT_EQ(argv[1], std::string("--session"));
	UAM_ASSERT_EQ(argv[2], std::string("session-abc"));
	UAM_ASSERT_EQ(argv[3], std::string("--model"));
	UAM_ASSERT_EQ(argv[4], std::string("anthropic/claude-sonnet-4"));
	UAM_ASSERT_EQ(argv[5], std::string("--agent"));
	UAM_ASSERT_EQ(argv[6], std::string("build"));
	UAM_ASSERT(!uam::ranges::Contains(argv, "--dangerously-skip-permissions"));

	profile.interactive_command = "   ";
	const std::vector<std::string> defaulted_argv = ProviderRuntime::BuildInteractiveArgv(profile, chat, AppSettings{});
	UAM_ASSERT_EQ(defaulted_argv[0], std::string("opencode"));
	UAM_ASSERT(uam::ranges::Contains(defaulted_argv, "opencode"));
	UAM_ASSERT(!uam::ranges::Contains(defaulted_argv, "   "));

	TempDir temp("uam-opencode-runtime-load-normalizes-blank-provider");
	ChatSession legacy_chat;
	legacy_chat.id = "chat-opencode-runtime-load";
	legacy_chat.provider_id.clear();
	legacy_chat.native_session_id = "open-code-session-runtime-load";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy_chat));

	// OC-6: LoadHistory no longer stamps provider ids (identical to claude/codex/copilot);
	// legacy blank-provider chats are normalized at the sidebar/terminal layer instead
	// (see the OpenCodeLocalHistoryPolling* and sidebar-normalization tests).
	const std::vector<ChatSession> loaded = ProviderRuntime::LoadHistory(ProviderProfileStore::DefaultOpenCodeProfile(), temp.root, {});
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(loaded.front().provider_id.empty());
	UAM_ASSERT_EQ(loaded.front().native_session_id, std::string("open-code-session-runtime-load"));
#endif
}

UAM_TEST(OpenCodeAcpSubAgentToolCallsAreVisibleAndPersistent)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-subagent");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	Message user;
	user.role = MessageRole::User;
	user.content = "Use a sub-agent.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->session_id = "opencode-session-1";
	session->turn_user_message_index = 0;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"agent-tool-1","title":"Planner agent","kind":"sub-agent","status":"running","subAgentId":"agent-session-1","subAgentTitle":"Planner","content":{"type":"text","text":"Inspecting with a sub-agent"}}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(raw_session->tool_calls[0].is_sub_agent);
	UAM_ASSERT_EQ(raw_session->tool_calls[0].sub_agent_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].sub_agent_title, std::string("Planner"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	const ToolCall& persisted_tool = app.chats.front().messages[1].tool_calls[0];
	UAM_ASSERT(persisted_tool.is_sub_agent);
	UAM_ASSERT_EQ(persisted_tool.sub_agent_id, std::string("agent-session-1"));
	UAM_ASSERT_EQ(persisted_tool.sub_agent_title, std::string("Planner"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json frontend_tool = serialized["chats"][0]["messages"][1]["toolCalls"][0];
	UAM_ASSERT(frontend_tool.value("isSubAgent", false));
	UAM_ASSERT_EQ(frontend_tool.value("subAgentId", ""), std::string("agent-session-1"));
	UAM_ASSERT_EQ(frontend_tool.value("subAgentTitle", ""), std::string("Planner"));

	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const auto loaded = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded[0].messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded[0].messages[1].tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(loaded[0].messages[1].tool_calls[0].is_sub_agent);
	UAM_ASSERT_EQ(loaded[0].messages[1].tool_calls[0].sub_agent_id, std::string("agent-session-1"));
#endif
}

UAM_TEST(AllProvidersRecognizeSharedSubAgentTools)
{
	for (const ProviderProfile& profile : ProviderProfileStore::BuiltInProfiles())
	{
		UAM_ASSERT(ProviderRuntime::ProviderRecognizesSubagentTool(profile, "task"));
		UAM_ASSERT(ProviderRuntime::ProviderRecognizesSubagentTool(profile, "functions.spawn_agent"));
		UAM_ASSERT(ProviderRuntime::ProviderRecognizesSubagentTool(profile, "delegate_to_agent"));
		UAM_ASSERT(!ProviderRuntime::ProviderRecognizesSubagentTool(profile, "bash"));
		UAM_ASSERT(!ProviderRuntime::ProviderRecognizesSubagentTool(profile, "task_status"));
		UAM_ASSERT(!ProviderRuntime::ProviderRecognizesSubagentTool(profile, "delegated"));
	}
}

UAM_TEST(OpenCodeAcpTaskToolCallIsDetectedAsSubAgentOnPendingUpdate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-task-subagent");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-task";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	Message user;
	user.role = MessageRole::User;
	user.content = "Delegate to a sub-agent.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-task";
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->session_id = "opencode-session-task";
	session->turn_user_message_index = 0;
	session->prompt_request_id = 20;
	session->pending_request_methods[20] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"task-1","title":"task","kind":"other","status":"running"}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(raw_session->tool_calls[0].is_sub_agent);
	UAM_ASSERT_EQ(raw_session->tool_calls[0].title, std::string("task"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].status, std::string("running"));
	UAM_ASSERT(raw_session->tool_calls[0].sub_agent_id.empty());
	UAM_ASSERT(raw_session->tool_calls[0].sub_agent_title.empty());
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call_update","toolCallId":"task-1","title":"Review code","status":"completed","rawOutput":{"output":"Done","metadata":{"sessionId":"ses_child_task"}}}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].sub_agent_id, std::string("ses_child_task"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].sub_agent_title, std::string("Review code"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Review complete."}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.back().tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.back().tool_calls[0].sub_agent_id, std::string("ses_child_task"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"task-2","title":"Another review","kind":"think","status":"pending","rawInput":{"subagent_type":"general","description":"Another review"}}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(raw_session->tool_calls[1].is_sub_agent);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call_update","toolCallId":"task-2","title":"Another review","kind":"think","status":"failed","rawOutput":{"error":"Task failed","metadata":{"sessionId":"ses_failed_child"}}}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls[1].sub_agent_id, std::string("ses_failed_child"));
#endif
}

UAM_TEST(OpenCodeAcpSessionNewSyncsResolvedNativeSessionMapping)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-session-new-sync");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 1;
	session->pending_request_methods[1] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"sessionId":"opencode-session-1"}})"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, std::string("opencode-session-1"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[app.chats.front().id], std::string("opencode-session-1"));
#endif
}

UAM_TEST(OpenCodeAcpSessionNewRebindsAttachedCliTerminalToResolvedSession)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-session-new-terminal-rebind");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 1;
	session->pending_request_methods[1] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "chat-1";
	terminal->attached_chat_id = "chat-1";
	terminal->attached_session_id = "stale-opencode-session";
	app.cli_terminals.push_back(std::move(terminal));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"sessionId":"opencode-session-1"}})"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[app.chats.front().id], std::string("opencode-session-1"));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals.front()), std::string("opencode-session-1"));
#endif
}

UAM_TEST(OpenCodeAcpSessionNewRebindsSessionOnlyCliTerminalToResolvedSession)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-session-new-terminal-rebind-session-only");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "stale-opencode-session";
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->terminal_id = "term-stale-opencode";
	terminal->frontend_chat_id.clear();
	terminal->attached_chat_id.clear();
	terminal->attached_session_id = "stale-opencode-session";
	app.cli_terminals.push_back(std::move(terminal));

	auto raw_session = std::make_unique<uam::AcpSessionState>();
	raw_session->chat_id = chat.id;
	raw_session->provider_id = uam::provider_ids::kOpenCodeCli;
	raw_session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	raw_session->running = true;
	raw_session->initialized = true;
	raw_session->session_ready = true;
	raw_session->session_setup_request_id = 1;
	raw_session->pending_request_methods[1] = "session/new";
	raw_session->session_id = "stale-opencode-session";
	uam::AcpSessionState* acp_session = raw_session.get();
	app.acp_sessions.push_back(std::move(raw_session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *acp_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"sessionId":"opencode-session-1"}})"));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[app.chats.front().id], std::string("opencode-session-1"));
	UAM_ASSERT_EQ(uam::CliTerminalPrimaryChatId(*app.cli_terminals.front()), std::string(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(*app.cli_terminals.front()), std::string(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals.front()), std::string("opencode-session-1"));
#endif
}

UAM_TEST(CodexChildHistoryLoadsItsRolloutAndRefreshesWithoutCrossingWorkspaces)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-child-history");
	const fs::path workspace = temp.root / "workspace";
	const fs::path codex_home = temp.root / "codex";
	fs::create_directories(workspace);
	fs::create_directories(codex_home / "sessions");
	ScopedEnvVar codex_home_env("CODEX_HOME", codex_home.string());
	const std::string child_id = "33333333-3333-4333-8333-333333333333";
	const fs::path rollout = codex_home / "sessions" / ("rollout-" + child_id + ".jsonl");
	const nlohmann::json metadata = {{"type", "session_meta"}, {"payload", {
	    {"id", child_id}, {"cwd", workspace.string()}, {"thread_source", "subagent"},
	    {"timestamp", "2026-07-24T10:00:00.000Z"}}}};
	const nlohmann::json prompt = {{"type", "response_item"}, {"timestamp", "2026-07-24T10:00:01.000Z"}, {"payload", {
	    {"type", "message"}, {"role", "user"}, {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", "Review this code"}}})}}}};
	const std::string initial = metadata.dump() + "\n" + prompt.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(rollout, initial));
	uam::AppState app;
	app.data_root = temp.root / "data";
	const ProviderProfile provider = ProviderProfileStore::DefaultCodexProfile();
	app.provider_profiles.push_back(provider);
	ChatSession parent;
	parent.id = "parent";
	parent.provider_id = provider.id;
	parent.workspace_directory = workspace.string();
	app.chats.push_back(parent);
	ChatSession* child = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id);
	UAM_ASSERT(child != nullptr);
	UAM_ASSERT_EQ(child->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(child->native_session_id, child_id);
	const std::string imported_id = child->id;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *child));
	const nlohmann::json answer = {{"type", "response_item"}, {"timestamp", "2026-07-24T10:00:02.000Z"}, {"payload", {
	    {"type", "message"}, {"role", "assistant"}, {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", "Review finished"}}})}}}};
	UAM_ASSERT(uam::io::WriteTextFile(rollout, initial + answer.dump() + "\n{incomplete"));
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id) == nullptr);
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, imported_id)->messages.size(), static_cast<std::size_t>(1));
	const std::string tool_history = R"({"type":"response_item","payload":{"type":"function_call","call_id":"call-read","name":"read_file","arguments":"{\"path\":\"README.md\"}"}}
{"type":"response_item","payload":{"type":"custom_tool_call","call_id":"call-patch","name":"apply_patch","input":"patch text"}}
{"type":"response_item","payload":{"type":"custom_tool_call_output","call_id":"call-patch","output":[{"type":"input_text","text":"patched"}]}}
{"type":"response_item","payload":{"type":"function_call_output","call_id":null,"output":"unlinked output"}}
{"type":"response_item","payload":{"type":"function_call_output","call_id":"orphan","output":"must not replace another result"}}
{"type":"response_item","payload":{"type":"function_call_output","call_id":"call-read","output":"file contents"}}
{"type":"response_item","timestamp":"2026-07-24T10:00:01.500Z","payload":{"type":"reasoning","summary":[{"type":"summary_text","text":"Checking edge cases"},{"type":"summary_text","text":"Reviewing cleanup"}],"encrypted_content":"opaque"}}
{"type":"response_item","payload":{"type":"reasoning","summary":[],"encrypted_content":"opaque"}}
{"type":"response_item","payload":{"type":"local_shell_call","call_id":"call-shell","id":"legacy-ignored","status":"in_progress","action":{"type":"exec","command":["pwd"],"working_directory":"/workspace"}}}
{"type":"response_item","payload":{"type":"function_call_output","call_id":"call-shell","output":"/workspace"}}
{"type":"response_item","payload":{"type":"local_shell_call","call_id":null,"id":"legacy-shell","status":"incomplete","action":{"type":"exec","command":["false"]}}}
{"type":"response_item","payload":{"type":"web_search_call","id":"search-1","status":"completed","action":{"type":"search","query":"Codex protocol"}}}
{"type":"response_item","payload":{"type":"web_search_call","id":null,"status":null,"action":null}}
{"type":"response_item","payload":{"type":"function_call","call_id":"call-child","name":"spawn_agent","arguments":"{}"}}
{"type":"response_item","payload":{"type":"function_call_output","call_id":"call-child","output":"{\"agent_id\":\"44444444-4444-4444-8444-444444444444\",\"nickname\":\"Reviewer\"}"}}
)";
	UAM_ASSERT(uam::io::WriteTextFile(rollout, initial + tool_history + answer.dump() + "\n"));
	fs::create_directories(codex_home / "archived_sessions");
	fs::rename(rollout, codex_home / "archived_sessions" / rollout.filename());
	UAM_ASSERT(!fs::exists(rollout));
	ChatSession read_request;
	read_request.native_session_id = child_id;
	read_request.workspace_directory = workspace.string();
	std::string read_error;
	const std::optional<ChatSession> snapshot = ChatHistorySyncService().LoadLocalCodexChildChat(read_request, &read_error);
	UAM_ASSERT(snapshot.has_value());
	UAM_ASSERT(read_error.empty());
	child = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id, false, &*snapshot);
	UAM_ASSERT(child != nullptr);
	UAM_ASSERT_EQ(child->id, imported_id);
	UAM_ASSERT_EQ(child->messages.size(), static_cast<std::size_t>(10));
	UAM_ASSERT_EQ(child->messages[3].thoughts, std::string("Checking edge cases\nReviewing cleanup"));
	UAM_ASSERT(child->messages[3].content.empty());
	UAM_ASSERT_EQ(child->messages[3].blocks[0].type, std::string("thought"));
	UAM_ASSERT_EQ(child->messages[1].tool_calls[0].name, std::string("read_file"));
	UAM_ASSERT_EQ(child->messages[1].tool_calls[0].args_json, std::string(R"({"path":"README.md"})"));
	UAM_ASSERT_EQ(child->messages[1].tool_calls[0].result_text, std::string("file contents"));
	UAM_ASSERT_EQ(child->messages[1].tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(child->messages[1].blocks[0].tool_call_id, std::string("call-read"));
	UAM_ASSERT_EQ(child->messages[2].tool_calls[0].name, std::string("apply_patch"));
	UAM_ASSERT_EQ(nlohmann::json::parse(child->messages[2].tool_calls[0].args_json).at("input"), "patch text");
	UAM_ASSERT_EQ(nlohmann::json::parse(child->messages[2].tool_calls[0].result_text).at(0).at("text"), "patched");
	const ToolCall& shell = child->messages[4].tool_calls[0];
	UAM_ASSERT_EQ(shell.name, std::string("shell"));
	UAM_ASSERT_EQ(shell.id, std::string("call-shell"));
	UAM_ASSERT_EQ(nlohmann::json::parse(shell.args_json).at("command").at(0), "pwd");
	UAM_ASSERT_EQ(shell.result_text, std::string("/workspace"));
	UAM_ASSERT_EQ(shell.status, std::string("completed"));
	UAM_ASSERT_EQ(child->messages[5].tool_calls[0].id, std::string("legacy-shell"));
	UAM_ASSERT_EQ(child->messages[5].tool_calls[0].status, std::string("incomplete"));
	UAM_ASSERT_EQ(child->messages[6].tool_calls[0].name, std::string("web_search"));
	UAM_ASSERT_EQ(child->messages[6].tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(nlohmann::json::parse(child->messages[6].tool_calls[0].args_json).at("query"), "Codex protocol");
	UAM_ASSERT(!child->messages[7].tool_calls[0].id.empty());
	UAM_ASSERT_EQ(child->messages[7].blocks[0].tool_call_id, child->messages[7].tool_calls[0].id);
	UAM_ASSERT(child->messages[7].tool_calls[0].status.empty());
	UAM_ASSERT(child->messages[8].tool_calls[0].is_sub_agent);
	UAM_ASSERT_EQ(child->messages[8].tool_calls[0].sub_agent_id, std::string("44444444-4444-4444-8444-444444444444"));
	UAM_ASSERT_EQ(child->messages[8].tool_calls[0].sub_agent_title, std::string("Reviewer"));
	UAM_ASSERT_EQ(child->messages.back().content, std::string("Review finished"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	parent.workspace_directory = (temp.root / "other").string();
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id) == nullptr);
	parent.workspace_directory = workspace.string();
	std::vector<std::string> added_keys;
	const ChatSession child_snapshot = *ChatDomainService().FindChatById(app, imported_id);
	UAM_ASSERT(ChatHistorySyncService().AddNativeImportTombstones(app.data_root, {child_snapshot}, added_keys));
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id) == nullptr);
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "native-import-tombstones.json", "invalid"));
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, parent, provider, child_id) == nullptr);
#endif
}
