#include "common/companion/protocol.h"
#include "core/chat_import_utils.h"
#include "cef/uam_bridge_request.h"
#include "common/platform/platform_speech_output.h"
#include "test_harness.h"
#include "durable_prompt_dispatch.h"
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

UAM_TEST(ChatRepositoryToleratesLegacyFieldsAndDropsThemOnWrite)
{
	TempDir temp("uam-chats");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-chat.json";

	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-chat",
  "provider_id": "codex-cli",
  "model_id": "flash",
  "reasoning_effort": "high",
  "service_tier": "fast",
  "native_session_id": "6a6f0f3b-1a0b-4a9c-8a01-111111111111",
  "folder_id": "folder-a",
  "branch_from_message_index": -9,
  "memory_last_processed_message_count": -2,
  "linked_files": [" src/main.cpp ", "   ", "README.md"],
  "template_override_id": "removed-template.md",
  "prompt_profile_bootstrapped": true,
  "rag_enabled": false,
  "rag_source_directories": ["/tmp/a"],
  "title": "Legacy",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": [
    {"role": "user", "content": "hello", "created_at": "2026-01-01 00:00:00", "tokens_input": 9999999999, "tokens_output": -1, "estimated_cost_usd": -2.5, "time_to_first_token_ms": -3, "processing_time_ms": 9999999999, "markdown_store_files": [" docs/goal.uam ", "   "]}
  ]
})"));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().id, std::string("legacy-chat"));
	UAM_ASSERT_EQ(loaded.front().native_session_id, std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT_EQ(loaded.front().model_id, std::string("flash"));
	UAM_ASSERT_EQ(loaded.front().reasoning_effort, std::string("high"));
	UAM_ASSERT_EQ(loaded.front().service_tier, std::string("fast"));
	UAM_ASSERT(loaded.front().service_tier_explicit);
	UAM_ASSERT_EQ(loaded.front().branch_from_message_index, -1);
	UAM_ASSERT_EQ(loaded.front().memory_last_processed_message_count, 0);
	UAM_ASSERT_EQ(loaded.front().linked_files.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded.front().linked_files[0], std::string("src/main.cpp"));
	UAM_ASSERT_EQ(loaded.front().linked_files[1], std::string("README.md"));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.front().markdown_store_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.front().markdown_store_files.front(), std::string("docs/goal.uam"));
	UAM_ASSERT_EQ(loaded.front().messages.front().tokens_input, std::numeric_limits<int>::max());
	UAM_ASSERT_EQ(loaded.front().messages.front().tokens_output, 0);
	UAM_ASSERT_EQ(loaded.front().messages.front().estimated_cost_usd, 0.0);
	UAM_ASSERT_EQ(loaded.front().messages.front().time_to_first_token_ms, 0);
	UAM_ASSERT_EQ(loaded.front().messages.front().processing_time_ms, std::numeric_limits<int>::max());
	UAM_ASSERT(!loaded.front().messages.front().continues_turn);

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	const std::string rewritten = ReadFile(legacy_file);
	const nlohmann::json rewritten_json = nlohmann::json::parse(rewritten);
	UAM_ASSERT_EQ(rewritten_json.value("model_id", ""), std::string("flash"));
	UAM_ASSERT_EQ(rewritten_json.value("reasoning_effort", ""), std::string("high"));
	UAM_ASSERT_EQ(rewritten_json.value("service_tier", ""), std::string("fast"));
	UAM_ASSERT(rewritten_json.value("service_tier_explicit", false));
	UAM_ASSERT_EQ(rewritten_json["linked_files"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(rewritten_json["messages"][0]["markdown_store_files"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT(rewritten.find("template_override_id") == std::string::npos);
	UAM_ASSERT(rewritten.find("prompt_profile_bootstrapped") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_enabled") == std::string::npos);
	UAM_ASSERT(rewritten.find("rag_source_directories") == std::string::npos);
	UAM_ASSERT(!rewritten_json["messages"][0].contains("continues_turn"));
	loaded.front().messages.front().priority_steer = true;
	loaded.front().messages.front().continues_turn = true;
	loaded.front().service_tier.clear();
	loaded.front().service_tier_explicit = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	std::optional<ChatSession> explicit_clear = ChatRepository::LoadLocalChat(temp.root, loaded.front().id);
	UAM_ASSERT(explicit_clear.has_value());
	UAM_ASSERT(explicit_clear->service_tier.empty());
	UAM_ASSERT(explicit_clear->service_tier_explicit);
	UAM_ASSERT(explicit_clear->messages.front().priority_steer);
	UAM_ASSERT(explicit_clear->messages.front().continues_turn);
	explicit_clear->service_tier_explicit = false;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *explicit_clear));
	std::optional<ChatSession> inherited = ChatRepository::LoadLocalChat(temp.root, loaded.front().id);
	UAM_ASSERT(inherited.has_value());
	UAM_ASSERT(inherited->service_tier.empty());
	UAM_ASSERT(!inherited->service_tier_explicit);
}

UAM_TEST(ChatRepositoryPersistsRemoteTurnReconnectMarker)
{
	TempDir temp("uam-chat-remote-turn-reconnect");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path chat_file = chats_dir / "remote-chat.json";
	UAM_ASSERT(uam::io::WriteTextFile(chat_file, R"({
  "id": "remote-chat",
  "provider_id": "codex-cli",
  "execution_host_id": "ssh-test",
  "title": "Remote",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": []
})"));

	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, "remote-chat");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(!loaded->remote_turn_reconnect_pending);

	loaded->remote_turn_reconnect_pending = true;
	loaded->remote_process_exists = true;
	loaded->remote_stop_cleanup_pending = true;
	loaded->remote_restart_pending = true;
	loaded->remote_process_control_token = "persisted-control-token";
	loaded->remote_delivered_stdout_cursor = 123;
	loaded->remote_delivered_stderr_cursor = 45;
	loaded->remote_source_exit_pending = true;
	loaded->remote_source_exit_code = 70;
	loaded->remote_uam_control_channel_id = "stable-control-channel";
	loaded->remote_interaction_responses.push_back(
	    {"request-7", R"({"jsonrpc":"2.0","id":7,"result":{}})"});
	loaded->remote_prompt_delivery_session_id = "acp-remote-chat";
	loaded->remote_prompt_delivery_id = "delivery-7";
	loaded->remote_prompt_delivery_payload = "{\"id\":7}\n";
	uam::AcpQueuedUserPromptState queued;
	queued.text = "persist this steer";
	queued.priority_steer = true;
	queued.goal_mode = true;
	queued.goal_id = "goal-1";
	loaded->acp_queued_prompts.push_back(queued);
	loaded->acp_dispatched_queued_prompt_count = 1;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
	loaded = ChatRepository::LoadLocalChat(temp.root, "remote-chat");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->remote_turn_reconnect_pending);
	UAM_ASSERT(loaded->remote_process_exists);
	UAM_ASSERT(loaded->remote_stop_cleanup_pending);
	UAM_ASSERT(loaded->remote_restart_pending);
	UAM_ASSERT_EQ(loaded->remote_process_control_token,
	              std::string("persisted-control-token"));
	UAM_ASSERT_EQ(loaded->remote_delivered_stdout_cursor, static_cast<std::uintmax_t>(123));
	UAM_ASSERT_EQ(loaded->remote_delivered_stderr_cursor, static_cast<std::uintmax_t>(45));
	UAM_ASSERT(loaded->remote_source_exit_pending);
	UAM_ASSERT_EQ(loaded->remote_source_exit_code, 70);
	UAM_ASSERT_EQ(loaded->remote_uam_control_channel_id,
	              std::string("stable-control-channel"));
	UAM_ASSERT_EQ(loaded->remote_interaction_responses.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded->remote_interaction_responses.front().request_id_json,
	              std::string("request-7"));
	UAM_ASSERT_EQ(loaded->remote_prompt_delivery_session_id,
	              std::string("acp-remote-chat"));
	UAM_ASSERT_EQ(loaded->remote_prompt_delivery_id, std::string("delivery-7"));
	UAM_ASSERT_EQ(loaded->remote_prompt_delivery_payload, std::string("{\"id\":7}\n"));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().text,
	              std::string("persist this steer"));
	UAM_ASSERT(loaded->acp_queued_prompts.front().priority_steer);
	UAM_ASSERT_EQ(loaded->acp_dispatched_queued_prompt_count,
	              static_cast<std::size_t>(1));

	loaded->execution_host_id = "local";
	loaded->imported_read_only = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
	loaded = ChatRepository::LoadLocalChat(temp.root, "remote-chat");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(!loaded->remote_turn_reconnect_pending);
	UAM_ASSERT(!loaded->remote_process_exists);
	UAM_ASSERT(!loaded->remote_stop_cleanup_pending);
	UAM_ASSERT(!loaded->remote_restart_pending);
	UAM_ASSERT(loaded->remote_process_control_token.empty());
	UAM_ASSERT_EQ(loaded->remote_delivered_stdout_cursor, static_cast<std::uintmax_t>(0));
	UAM_ASSERT_EQ(loaded->remote_delivered_stderr_cursor, static_cast<std::uintmax_t>(0));
	UAM_ASSERT(!loaded->remote_source_exit_pending);
	UAM_ASSERT_EQ(loaded->remote_source_exit_code, -1);
	UAM_ASSERT(loaded->remote_uam_control_channel_id.empty());
	UAM_ASSERT(loaded->remote_interaction_responses.empty());
	UAM_ASSERT(loaded->remote_prompt_delivery_session_id.empty());
	UAM_ASSERT(loaded->remote_prompt_delivery_id.empty());
	UAM_ASSERT(loaded->remote_prompt_delivery_payload.empty());
	UAM_ASSERT(loaded->acp_queued_prompts.empty());
	UAM_ASSERT_EQ(loaded->acp_dispatched_queued_prompt_count,
	              static_cast<std::size_t>(0));
}

UAM_TEST(ChatRepositoryPersistsRemotePendingRequests)
{
	TempDir temp("uam-remote-pending-requests");
	ChatSession chat;
	chat.id = "pending-chat";
	chat.execution_host_id = "ssh-test";
	chat.remote_next_request_id = 9;
	chat.remote_active_turn_id = "active-turn-3";
	chat.remote_turn_serial = 3;
	chat.remote_turn_user_message_index = 2;
	chat.remote_pending_requests = {
	    {7, "turn/steer", "delivery-7", "{\"id\":7}\n", 2, 3, "turn-3"},
	    {8, "turn/steer", "delivery-8", "", 4, 3, "turn-3", true}};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->remote_pending_requests == chat.remote_pending_requests);
	UAM_ASSERT_EQ(loaded->remote_next_request_id, 9);
	UAM_ASSERT_EQ(loaded->remote_active_turn_id, std::string("active-turn-3"));
	UAM_ASSERT_EQ(loaded->remote_turn_serial, 3);
	UAM_ASSERT_EQ(loaded->remote_turn_user_message_index, 2);
	for (bool imported : {false, true})
	{
		chat.execution_host_id = imported ? "ssh-test" : "local";
		chat.imported_read_only = imported;
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(loaded->remote_pending_requests.empty());
		UAM_ASSERT_EQ(loaded->remote_next_request_id, 1);
		UAM_ASSERT(loaded->remote_active_turn_id.empty());
		UAM_ASSERT_EQ(loaded->remote_turn_serial, 0);
		UAM_ASSERT_EQ(loaded->remote_turn_user_message_index, -1);
	}

	const fs::path chat_file = temp.root / "chats" / "pending-chat.json";
	UAM_ASSERT(uam::io::WriteTextFile(chat_file, R"({
	  "id":"pending-chat", "execution_host_id":"ssh-test", "remote_next_request_id":2147483647,
	  "remote_active_turn_id":false, "remote_turn_serial":-1,
	  "remote_turn_user_message_index":2147483648,
	  "remote_pending_requests":[
	    null, {"request_id":0,"method":"turn/steer"},
	    {"request_id":1.5,"method":"turn/steer"},
	    {"request_id":2147483648,"method":"turn/steer"},
	    {"request_id":2147483647,"method":"turn/steer"},
	    {"request_id":2,"method":""},
	    {"request_id":3,"method":"turn/steer","user_message_index":-2},
	    {"request_id":4,"method":"turn/steer","turn_serial":-1},
	    {"request_id":7,"method":"turn/steer"},
	    {"request_id":7,"method":"duplicate"}
	  ], "messages":[]
	})"));
	loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->remote_pending_requests.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded->remote_pending_requests.front().request_id, 7);
	UAM_ASSERT_EQ(loaded->remote_turn_user_message_index, -1);
	UAM_ASSERT_EQ(loaded->remote_next_request_id, 1);
	UAM_ASSERT(loaded->remote_active_turn_id.empty());
	UAM_ASSERT_EQ(loaded->remote_turn_serial, 0);
	UAM_ASSERT_EQ(loaded->remote_pending_requests.front().method, std::string("turn/steer"));

	chat.execution_host_id = "ssh-test";
	chat.imported_read_only = false;
	chat.remote_active_turn_id = std::string(257, 'x');
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	chat.remote_active_turn_id.clear();
	chat.remote_turn_serial = -1;
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	chat.remote_turn_serial = 0;
	chat.remote_turn_user_message_index = -2;
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	chat.remote_turn_user_message_index = -1;
	chat.remote_pending_requests.clear();
	for (int id = 1; id <= 65; ++id)
		chat.remote_pending_requests.push_back({id, "turn/steer", "", "", -1, 0, ""});
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	chat.remote_pending_requests.pop_back();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->remote_pending_requests.size(), static_cast<std::size_t>(64));
	chat.remote_pending_requests = {
	    {1, "turn/steer", "", std::string(2 * 1024 * 1024, 'x'), -1, 0, ""},
	    {2, "turn/steer", "", "x", -1, 0, ""}};
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	chat.remote_pending_requests.pop_back();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.remote_pending_requests.front().method = std::string(257, 'x');
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->remote_pending_requests.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded->remote_pending_requests.front().payload.size(),
	              static_cast<std::size_t>(2 * 1024 * 1024));
}

UAM_TEST(ChatRepositoryMigratesLegacyYoloModeToCommandSafetyTier)
{
	TempDir temp("uam-chat-yolo-migration");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-yolo.json";

	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-yolo",
  "provider_id": "codex-cli",
  "approval_mode": "yolo",
  "title": "Legacy Yolo",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": []
})"));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().approval_mode, std::string("default"));
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("yolo"));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	const nlohmann::json rewritten_json = nlohmann::json::parse(ReadFile(legacy_file));
	UAM_ASSERT_EQ(rewritten_json.value("approval_mode", ""), std::string("default"));
	UAM_ASSERT(!rewritten_json.contains("auto_approve_commands"));
	UAM_ASSERT_EQ(rewritten_json.value("commandSafetyTier", ""), std::string("yolo"));
}

UAM_TEST(ChatRepositoryMigratesLegacyAutoApproveBooleanToCommandSafetyTier)
{
	TempDir temp("uam-chat-auto-approve-migration");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-auto-approve.json";
	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-auto-approve",
  "provider_id": "codex-cli",
  "approval_mode": "default",
  "auto_approve_commands": true,
  "title": "Legacy Auto Approve",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": []
})"));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("yolo"));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded.front()));
	const nlohmann::json rewritten = nlohmann::json::parse(ReadFile(legacy_file));
	UAM_ASSERT(!rewritten.contains("auto_approve_commands"));
	UAM_ASSERT_EQ(rewritten.value("commandSafetyTier", ""), std::string("yolo"));
}

UAM_TEST(ChatRepositoryMigratesLegacyAcceptEditsAgentModeToPermissions)
{
	TempDir temp("uam-chat-accept-edits-migration");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	const fs::path legacy_file = chats_dir / "legacy-accept-edits.json";

	UAM_ASSERT(uam::io::WriteTextFile(legacy_file, R"({
  "id": "legacy-accept-edits",
  "provider_id": "claude-cli",
  "approval_mode": "acceptEdits",
  "title": "Legacy Accept Edits",
  "created_at": "2026-01-01 00:00:00",
  "updated_at": "2026-01-01 00:00:01",
  "messages": []
})"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().approval_mode, std::string("default"));
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("acceptEdits"));
}

UAM_TEST(NativeHistoryRefreshPreservesIndependentModelControls)
{
	TempDir temp("uam-native-refresh-model-controls");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession local;
	local.id = "chat-1";
	local.provider_id = "codex-cli";
	local.title = "Local";
	local.model_id = "gpt-5.4";
	local.reasoning_effort = "xhigh";
	local.service_tier = "fast";
	local.service_tier_explicit = true;
	local.provider_handoff_context = "User: preserve context";
	local.provider_handoff_session_id = "delivered-session";
	local.small_model_mode = true;
	local.command_safety_tier = "aiReview";
	local.workspace_isolation_kind = "gitWorktree";
	local.workspace_source_directory = "/source";
	local.workspace_base_ref = "abc123";
	local.workspace_branch_name = "uam/chat-1";
	local.workspace_worktree_directory = "/worktree";
	local.memory_enabled = false;
	local.memory_level = "off";
	local.memory_last_processed_message_count = 3;
	local.active_goal_id = "goal-1";
	local.goals.push_back(Goal{.id = "goal-1", .objective = "Finish the audit"});
	local.created_at = "2026-01-01T00:00:00.000Z";
	local.updated_at = "2026-01-01T00:00:01.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local));

	ChatSession native = local;
	native.reasoning_effort.clear();
	native.provider_handoff_context.clear();
	native.provider_handoff_session_id.clear();
	native.service_tier.clear();
	native.small_model_mode = false;
	native.command_safety_tier = "off";
	native.workspace_isolation_kind.clear();
	native.workspace_source_directory.clear();
	native.workspace_base_ref.clear();
	native.workspace_branch_name.clear();
	native.workspace_worktree_directory.clear();
	native.memory_enabled = true;
	native.memory_level = "strict";
	native.memory_last_processed_message_count = 0;
	native.goals.clear();
	native.active_goal_id.clear();
	std::vector<ChatSession> native_chats{native};
	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats, false);

	UAM_ASSERT_EQ(native_chats.front().reasoning_effort, std::string("xhigh"));
	UAM_ASSERT_EQ(native_chats.front().service_tier, std::string("fast"));
	UAM_ASSERT_EQ(native_chats.front().provider_handoff_context, local.provider_handoff_context);
	UAM_ASSERT_EQ(native_chats.front().provider_handoff_session_id, local.provider_handoff_session_id);
	UAM_ASSERT(native_chats.front().small_model_mode);
	UAM_ASSERT_EQ(native_chats.front().command_safety_tier, std::string("aiReview"));
	UAM_ASSERT_EQ(native_chats.front().workspace_isolation_kind, std::string("gitWorktree"));
	UAM_ASSERT_EQ(native_chats.front().workspace_worktree_directory, std::string("/worktree"));
	UAM_ASSERT(!native_chats.front().memory_enabled);
	UAM_ASSERT_EQ(native_chats.front().memory_level, std::string("off"));
	UAM_ASSERT_EQ(native_chats.front().memory_last_processed_message_count, 3);
	UAM_ASSERT_EQ(native_chats.front().goals.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(native_chats.front().active_goal_id, std::string("goal-1"));
}

UAM_TEST(NativeHistoryOverlayScopesMetadataToProviderAndWorkspace)
{
	TempDir temp("uam-overlay-identity");
	uam::AppState app;
	app.data_root = temp.root;
	std::vector<ChatSession> saved;
	for (int index = 0; index < 3; ++index)
	{
		ChatSession chat;
		chat.id = "saved-" + std::to_string(index);
		chat.native_session_id = "01234567-89ab-4cde-8f01-23456789abcd";
		chat.provider_id = index == 1 ? "opencode-cli" : "codex-cli";
		chat.workspace_directory = (temp.root / (index == 2 ? "two" : "one")).string();
		chat.title = "Saved title " + std::to_string(index);
		chat.messages = {Message{MessageRole::Assistant, "Saved answer " + std::to_string(index)}};
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		saved.push_back(chat);
	}
	for (bool collide_ids : {false, true})
	{
		std::vector<ChatSession> native = saved;
		for (std::size_t index = 0; index < native.size(); ++index)
		{
			native[index].id = collide_ids ? saved[(index + 1) % saved.size()].id : "native-" + std::to_string(index);
			native[index].title = "Native title";
			native[index].messages.clear();
		}
		ChatHistorySyncService().ApplyLocalOverrides(app, native, false);
		UAM_ASSERT_EQ(native.size(), saved.size());
		for (std::size_t index = 0; index < native.size(); ++index)
		{
			UAM_ASSERT_EQ(native[index].id, saved[index].id);
			UAM_ASSERT_EQ(native[index].title, saved[index].title);
			UAM_ASSERT_EQ(native[index].provider_id, saved[index].provider_id);
			UAM_ASSERT_EQ(native[index].workspace_directory, saved[index].workspace_directory);
			UAM_ASSERT_EQ(native[index].messages.front().content, saved[index].messages.front().content);
		}
	}
}

UAM_TEST(NativeHistoryRefreshHydratesLocalMessagesOnlyWhenTheyOverrideNative)
{
	TempDir temp("uam-native-refresh-lazy-local-messages");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession local;
	local.id = "chat-lazy-overlay";
	local.provider_id = "codex-cli";
	local.native_session_id = "native-lazy-overlay";
	local.title = "Local";
	local.created_at = "2026-01-01T00:00:00.000Z";
	local.updated_at = "2026-01-01T00:00:02.000Z";
	local.messages.push_back(Message{MessageRole::User, "Hello"});
	local.messages.push_back(Message{MessageRole::Assistant, "Complete local answer"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local));

	ChatSession native = local;
	native.updated_at = "2026-01-01T00:00:01.000Z";
	native.messages.resize(1);
	std::vector<ChatSession> native_chats{native};

	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats, false);

	UAM_ASSERT_EQ(native_chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(native_chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(native_chats.front().messages.back().content, std::string("Complete local answer"));
}

UAM_TEST(NativeHistoryRefreshPreservesLocalMessageAttachmentsAndSkillSnapshots)
{
	TempDir temp("uam-native-refresh-message-metadata");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession local;
	local.id = "chat-message-metadata";
	local.provider_id = "gemini-cli";
	local.native_session_id = "native-message-metadata";
	local.created_at = "2026-01-01T00:00:00.000Z";
	local.updated_at = "2026-01-01T00:00:01.000Z";
	Message local_message{MessageRole::User, "Review this"};
	local_message.created_at = local.created_at;
	local_message.markdown_store_files = {"/skills/review.uam"};
	local_message.markdown_store_prompt_blocks = {"Original immutable skill"};
	local_message.attachments.push_back(MessageAttachment{.id = "attachment-1", .name = "notes.txt", .path = "/workspace/notes.txt"});
	local.messages.push_back(local_message);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, local));

	ChatSession native = local;
	native.updated_at = "2026-01-01T00:00:02.000Z";
	native.messages.front().markdown_store_files.clear();
	native.messages.front().markdown_store_prompt_blocks.clear();
	native.messages.front().attachments.clear();
	std::vector<ChatSession> native_chats{native};

	ChatHistorySyncService().ApplyLocalOverrides(app, native_chats, false);

	UAM_ASSERT_EQ(native_chats.front().messages.front().markdown_store_files, local_message.markdown_store_files);
	UAM_ASSERT_EQ(native_chats.front().messages.front().markdown_store_prompt_blocks, local_message.markdown_store_prompt_blocks);
	UAM_ASSERT_EQ(native_chats.front().messages.front().attachments.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(native_chats.front().messages.front().attachments.front().id, std::string("attachment-1"));
}

UAM_TEST(ChatRepositoryMigratesLegacyDirectoryMessageRoles)
{
	TempDir temp("uam-chat-legacy-dir-roles");
	const fs::path legacy_chat_dir = temp.root / "chats" / "legacy-dir-roles";
	const fs::path messages_dir = legacy_chat_dir / "messages";
	fs::create_directories(messages_dir);

	UAM_ASSERT(uam::io::WriteTextFile(legacy_chat_dir / "meta.txt", " provider_id =codex-cli\n title =Legacy Directory\n created_at =2026-01-01T00:00:00.000Z\n updated_at =2026-01-01T00:00:01.000Z\n native_session_id =6a6f0f3b-1a0b-4a9c-8a01-111111111111\n branch_from_index = -9\n file = src/main.cpp \n file =   \n"));
	UAM_ASSERT(uam::io::WriteTextFile(messages_dir / "000_user.txt", "User message"));
	UAM_ASSERT(uam::io::WriteTextFile(messages_dir / "001_assistant.txt", "Assistant message"));
	UAM_ASSERT(uam::io::WriteTextFile(messages_dir / "002_system.txt", "System message"));
	UAM_ASSERT(uam::io::WriteTextFile(messages_dir / "003_unknown.txt", "Unknown role falls back"));
	UAM_ASSERT(uam::io::WriteTextFile(messages_dir / "004.txt", "Malformed role falls back"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(loaded.front().title, std::string("Legacy Directory"));
	UAM_ASSERT_EQ(loaded.front().native_session_id, std::string("6a6f0f3b-1a0b-4a9c-8a01-111111111111"));
	UAM_ASSERT_EQ(loaded.front().branch_from_message_index, -1);
	UAM_ASSERT_EQ(loaded.front().linked_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().linked_files.front(), std::string("src/main.cpp"));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(loaded.front().messages[0].role, MessageRole::User);
	UAM_ASSERT_EQ(loaded.front().messages[1].role, MessageRole::Assistant);
	UAM_ASSERT_EQ(loaded.front().messages[2].role, MessageRole::System);
	UAM_ASSERT_EQ(loaded.front().messages[3].role, MessageRole::User);
	UAM_ASSERT_EQ(loaded.front().messages[4].role, MessageRole::User);
	UAM_ASSERT_EQ(loaded.front().messages[1].content, std::string("Assistant message"));

	const nlohmann::json migrated_json = nlohmann::json::parse(ReadFile(temp.root / "chats" / "legacy-dir-roles.json"));
	UAM_ASSERT_EQ(migrated_json["messages"][1].value("role", ""), std::string("assistant"));
	UAM_ASSERT_EQ(migrated_json["messages"][2].value("role", ""), std::string("system"));
}

UAM_TEST(ChatRepositoryPersistsMemoryLevelAndMigratesLegacyBoolean)
{
	TempDir temp("uam-chat-memory-level");
	ChatSession chat;
	chat.id = "chat-memory-level";
	chat.provider_id = "gemini-cli";
	chat.title = "Memory Level";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.memory_level = "open";
	chat.memory_enabled = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().memory_level, std::string("open"));
	UAM_ASSERT(loaded.front().memory_enabled);

	const fs::path path = AppPaths::UamChatFilePath(temp.root, chat.id);
	nlohmann::json legacy = nlohmann::json::parse(ReadFile(path));
	legacy.erase("memory_level");
	legacy["memory_enabled"] = false;
	UAM_ASSERT(uam::io::WriteTextFile(path, legacy.dump(2)));
	loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().memory_level, std::string("off"));
	UAM_ASSERT(!loaded.front().memory_enabled);
}

UAM_TEST(ChatRepositoryValidPrimaryIgnoresOlderBackupMemoryMetadata)
{
	TempDir temp("uam-chat-memory-backup-mismatch");

	ChatSession chat;
	chat.id = "chat-memory-backup";
	chat.provider_id = "codex-cli";
	chat.title = "Memory Backup";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.memory_enabled = true;
	chat.memory_last_processed_message_count = 1;
	chat.memory_last_processed_at = "2026-01-01T00:00:02.000Z";

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path primary_path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path backup_path = primary_path.string() + ".bak";
	const std::string backup_text = ReadFile(primary_path);

	nlohmann::json primary_json = nlohmann::json::parse(ReadFile(primary_path));
	primary_json["memory_last_processed_message_count"] = 2;
	primary_json["memory_last_processed_at"] = "2026-01-01T00:00:03.000Z";
	UAM_ASSERT(uam::io::WriteTextFile(primary_path, primary_json.dump(2)));
	UAM_ASSERT(uam::io::WriteTextFile(backup_path, backup_text));

	std::string warning;
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root, &warning);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().memory_last_processed_message_count, 2);
	UAM_ASSERT(warning.empty());
}

UAM_TEST(ChatRepositoryValidPrimaryIgnoresOlderBackupMarkdownMetadata)
{
	TempDir temp("uam-chat-markdown-backup-mismatch");

	ChatSession chat;
	chat.id = "chat-markdown-backup";
	chat.provider_id = "codex-cli";
	chat.title = "Markdown Backup";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	Message message;
	message.role = MessageRole::Assistant;
	message.content = "Assistant";
	message.created_at = "2026-01-01T00:00:02.000Z";
	message.markdown_store_files.push_back("docs/original.uam");
	chat.messages.push_back(std::move(message));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path primary_path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path backup_path = primary_path.string() + ".bak";
	const std::string backup_text = ReadFile(primary_path);

	nlohmann::json primary_json = nlohmann::json::parse(ReadFile(primary_path));
	primary_json["messages"][0]["markdown_store_files"] = nlohmann::json::array({"docs/changed.uam"});
	UAM_ASSERT(uam::io::WriteTextFile(primary_path, primary_json.dump(2)));
	UAM_ASSERT(uam::io::WriteTextFile(backup_path, backup_text));

	std::string warning;
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root, &warning);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.front().markdown_store_files.front(), std::string("docs/changed.uam"));
	UAM_ASSERT(warning.empty());
}

UAM_TEST(ChatRepositoryRecoversValidatedBackupWhenPrimaryIsMissingOrCorrupt)
{
	for (const bool missing_primary : {false, true})
	{
		TempDir temp(std::string("uam-chat-backup-recovery-") + (missing_primary ? "missing" : "corrupt"));
		ChatSession chat;
		chat.id = "chat-backup-recovery";
		chat.provider_id = "codex-cli";
		chat.title = "Recovered";
		chat.created_at = "2026-01-01T00:00:00.000Z";
		chat.updated_at = "2026-01-01T00:00:01.000Z";
		chat.messages.push_back(Message{MessageRole::User, "durable message"});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		chat.title = "Replacement";
		chat.updated_at = "2026-01-01T00:00:02.000Z";
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

		const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		if (missing_primary)
		{
			std::error_code remove_error;
			UAM_ASSERT(fs::remove(primary, remove_error));
			UAM_ASSERT(!remove_error);
		}
		else
		{
			UAM_ASSERT(uam::io::WriteTextFile(primary, "{"));
		}

		const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
		UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(loaded.front().id, chat.id);
		UAM_ASSERT_EQ(loaded.front().title, std::string("Recovered"));
		UAM_ASSERT_EQ(loaded.front().messages.front().content, std::string("durable message"));
		UAM_ASSERT(fs::exists(primary));
		const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(restored.has_value());
		UAM_ASSERT_EQ(restored->title, std::string("Recovered"));
	}
}

UAM_TEST(ChatRepositoryRecoversBackupWhenPrimaryMessagesFieldIsNotAnArray)
{
	TempDir temp("uam-chat-backup-non-array-messages");
	ChatSession chat;
	chat.id = "chat-backup-non-array-messages";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.title = "Known good backup";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "durable transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	chat.title = "Malformed primary";
	chat.updated_at = "2026-01-01T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	nlohmann::json malformed = nlohmann::json::parse(ReadFile(primary));
	malformed["messages"] = nlohmann::json::object({{"unexpected", true}});
	UAM_ASSERT(uam::io::WriteTextFile(primary, malformed.dump(2)));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().title, std::string("Known good backup"));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.front().content, std::string("durable transcript"));
}

namespace
{
	ChatSession QueueJournalFixture(const fs::path& root)
	{
		ChatSession chat;
		chat.id = "queue-journal";
		chat.provider_id = "gemini-cli";
		chat.created_at = "2026-10-10T12:00:00Z";
		chat.updated_at = chat.created_at;
		chat.workspace_directory = root.string();
		chat.messages.push_back(Message{MessageRole::Assistant, std::string(1024 * 1024, 'x')});
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "future";
		prompt.text = "Original future request";
		prompt.uam_agent_definition_snapshot = "Pinned agent instructions";
		prompt.uam_agent_skills = {"skill.md"};
		prompt.markdown_store_files = {"notes.md"};
		prompt.markdown_store_prompt_blocks = {"Recorded markdown text"};
		prompt.goal_mode = true;
		prompt.goal_id = "goal-owner";
		prompt.computer_use_mode = true;
		prompt.attachments.push_back(MessageAttachment{.id = "image", .name = "image.png", .kind = "image", .mime_type = "image/png", .path = (root / "image.png").string(), .size_bytes = 123});
		chat.acp_queued_prompts.push_back(std::move(prompt));
		return chat;
	}
}

UAM_TEST(QueueJournalSameEditSnapshotCannotResurrectConsumedInput)
{
	TempDir temp("uam-queue-journal-consumed-snapshot");
	ChatSession chat;
	chat.id = "queue-consumed";
	chat.provider_id = "gemini-cli";
	chat.created_at = "2026-10-10T12:00:00Z";
	chat.updated_at = chat.created_at;
	chat.messages.push_back(Message{MessageRole::Assistant, "Previous answer"});
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "future";
	prompt.text = "Original";
	chat.acp_queued_prompts.push_back(prompt);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.acp_queued_prompts.front().text = "Edited";
	++chat.acp_queued_prompts.front().revision;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const ChatSession stale = chat;
	chat.acp_queued_prompts.clear();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, stale));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id)->acp_queued_prompts.empty());
}

UAM_TEST(QueueJournalAcknowledgedEditsPreserveTranscriptAndColdReload)
{
	TempDir temp("uam-queue-journal-roundtrip");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string original = ReadFile(primary);
	chat.acp_queued_prompts.front().text = "Revised future request";
	chat.acp_queued_prompts.front().revision = 2;
	chat.acp_queue_held = true;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(!chat.acp_queue_storage_revision.empty());
	UAM_ASSERT_EQ(ReadFile(primary), original);
	for (const bool include_messages : {false, true})
	{
		std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, include_messages);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(!loaded->acp_queue_storage_error);
		UAM_ASSERT_EQ(loaded->acp_queue_storage_revision, chat.acp_queue_storage_revision);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().text, "Revised future request");
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().revision, 2);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().attachments, chat.acp_queued_prompts.front().attachments);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().uam_agent_definition_snapshot, chat.acp_queued_prompts.front().uam_agent_definition_snapshot);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().markdown_store_prompt_blocks, chat.acp_queued_prompts.front().markdown_store_prompt_blocks);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().uam_agent_skills, chat.acp_queued_prompts.front().uam_agent_skills);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().goal_id, "goal-owner");
		UAM_ASSERT(loaded->acp_queued_prompts.front().computer_use_mode);
		UAM_ASSERT(loaded->acp_queue_held);
		UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, *loaded));
		UAM_ASSERT_EQ(loaded->messages, chat.messages);
	}
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChatSummariesReadOnly(temp.root).front().acp_queue_storage_revision, chat.acp_queue_storage_revision);
	UAM_ASSERT_EQ(ReadFile(primary), original);
}

UAM_TEST(QueueJournalRejectsStaleFullAndPreparedSavesWithoutRevivingConsumedInput)
{
	TempDir temp("uam-queue-journal-stale");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const ChatSession stale = chat;
	const std::shared_ptr<PreparedChatSave> old_prepared = ChatRepository::PrepareChatSave(temp.root, stale);
	UAM_ASSERT(old_prepared != nullptr);
	chat.acp_queued_prompts.front().text = "Edited";
	++chat.acp_queued_prompts.front().revision;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, stale));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(old_prepared));
	const std::shared_ptr<PreparedChatSave> before_edit = ChatRepository::PrepareChatSave(temp.root, chat);
	UAM_ASSERT(before_edit != nullptr);
	const std::optional<std::string> primary_revision = ChatRepository::ChatStorageRevision(temp.root, chat.id);
	chat.acp_queued_prompts.front().text = "Edited again";
	++chat.acp_queued_prompts.front().revision;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(temp.root, chat.id), primary_revision);
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(before_edit));
	const ChatSession before_dispatch = chat;
	const std::shared_ptr<PreparedChatSave> before_dispatch_prepared = ChatRepository::PrepareChatSave(temp.root, chat);
	UAM_ASSERT(before_dispatch_prepared != nullptr);
	chat.acp_dispatched_queued_prompt_count = 1;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(before_dispatch_prepared));
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(temp.root, chat.id)->acp_dispatched_queued_prompt_count, std::size_t(1));
	chat.acp_queued_prompts.clear();
	chat.acp_dispatched_queued_prompt_count = 0;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->acp_queued_prompts.empty());
	UAM_ASSERT(!loaded->acp_queue_storage_error);
	UAM_ASSERT(fs::exists(temp.root / "queue-outboxes" / (chat.id + ".json")));
	// The retained record is consumed by the marker in primary storage, not by deleting it.
	UAM_ASSERT_EQ(loaded->acp_queue_storage_revision, before_dispatch.acp_queue_storage_revision);
}

UAM_TEST(QueueJournalCorruptionHoldsRecoveryWithoutProviderDispatch)
{
	TempDir temp("uam-queue-journal-corrupt");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path journal = temp.root / "queue-outboxes" / (chat.id + ".json");
	UAM_ASSERT(uam::io::WriteTextFile(journal, "{broken"));
	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->acp_queue_storage_error);
	UAM_ASSERT(loaded->acp_queue_held);
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, *loaded));
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, *loaded));
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.chats.push_back(std::move(*loaded));
	UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(app, true), std::size_t(0));
	uam::AcpSessionState* holder = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(holder != nullptr);
	UAM_ASSERT(!holder->running);
	UAM_ASSERT(!holder->reconnect_pending);
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, *holder, app.chats.front()));
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(app, chat.id, "New user request", &error));
	UAM_ASSERT(error.find("journal") != std::string::npos);
	UAM_ASSERT_EQ(ReadFile(journal), "{broken");
}

UAM_TEST(QueueJournalFailedWritesAndStaleHydrationPreserveDurableState)
{
	TempDir temp("uam-queue-journal-failures");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::optional<ChatSession> cold = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(cold.has_value());
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string original = ReadFile(primary);
	const fs::path journal = temp.root / "queue-outboxes" / (chat.id + ".json");
	fs::create_directories(journal);
	chat.acp_queued_prompts.front().text = "Edited";
	++chat.acp_queued_prompts.front().revision;
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(chat.acp_queue_storage_revision.empty());
	UAM_ASSERT_EQ(ReadFile(primary), original);
	fs::remove(journal);
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(!ChatRepository::HydrateChatMessages(temp.root, *cold));
	UAM_ASSERT(!cold->messages_loaded);
	const fs::path preserved = temp.root / "preserved-primary.json";
	fs::rename(primary, preserved);
	fs::create_directory(primary);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, chat));
	fs::remove(primary);
	fs::rename(preserved, primary);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(temp.root, chat.id)->acp_queued_prompts.front().text, "Edited");
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, chat.id).Failed());
	UAM_ASSERT(!fs::exists(journal));
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(!fs::exists(primary));
}

UAM_TEST(QueueJournalDeletionRecoveryRetainsSurvivorAndRemovesOwnedJournal)
{
	TempDir temp("uam-queue-journal-delete");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession target = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, target));
	target.acp_queued_prompts.front().text = "Latest durable input";
	++target.acp_queued_prompts.front().revision;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, target));
	ChatSession survivor = QueueJournalFixture(temp.root);
	survivor.id = "survivor";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, survivor));
	const std::string survivor_bytes = ReadFile(AppPaths::UamChatFilePath(temp.root, survivor.id));
	app.chats = {target, survivor};
	uam::WorkspaceDeletionTask task;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {target.id}, task));
	UAM_ASSERT(uam::StageWorkspaceDeletion(task));
	const std::optional<ChatSession> staged = ChatRepository::LoadLocalChat(temp.root / ".deletion-transaction", target.id);
	UAM_ASSERT(staged.has_value());
	UAM_ASSERT_EQ(staged->acp_queued_prompts.front().text, "Latest durable input");
	// Interrupt after staging; real recovery completes the transaction without native provider work.
	UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
	UAM_ASSERT(!fs::exists(temp.root / "queue-outboxes" / (target.id + ".json")));
	UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, target.id).has_value());
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(temp.root, survivor.id)), survivor_bytes);
}

UAM_TEST(QueueJournalBoundsAndBaseRevisionMismatchNeverAcknowledgeLostInput)
{
	TempDir temp("uam-queue-journal-boundaries");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	ChatSession oversized = chat;
	oversized.acp_queued_prompts.front().text = std::string(2 * 1024 * 1024 + 1, 'q');
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, oversized));
	ChatSession duplicate = chat;
	duplicate.acp_queued_prompts.push_back(duplicate.acp_queued_prompts.front());
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, duplicate));
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	std::string changed = ReadFile(primary);
	changed += " ";
	UAM_ASSERT(uam::io::WriteTextFile(primary, changed));
	const std::optional<ChatSession> held = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(held.has_value());
	UAM_ASSERT(held->acp_queue_storage_error);
	UAM_ASSERT(held->acp_queue_held);
	UAM_ASSERT_EQ(held->acp_queued_prompts.front().text, chat.acp_queued_prompts.front().text);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, *held));
}

UAM_TEST(QueueJournalControlsKeepPendingTranscriptSaveAndDurableQueueOrder)
{
	TempDir temp("uam-queue-journal-controls");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = QueueJournalFixture(temp.root);
	uam::AcpQueuedUserPromptState next = chat.acp_queued_prompts.front();
	next.id = "next";
	next.text = "Second future request";
	chat.acp_queued_prompts.push_back(next);
	chat.acp_queue_held = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::string original = ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id));
	app.chats.push_back(std::move(chat));
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = "gemini-cli";
	session->processing = true;
	session->queue_held = true;
	session->queued_user_prompts.assign(app.chats.front().acp_queued_prompts.begin(), app.chats.front().acp_queued_prompts.end());
	app.acp_sessions.push_back(std::move(session));
	app.pending_chat_save_at_by_chat_id[app.chats.front().id] = 99;
	std::string error;
	UAM_ASSERT(uam::EditQueuedAcpPrompt(app, app.chats.front().id, "future", 1, "Edited first request", &error));
	UAM_ASSERT(uam::ReorderQueuedAcpPrompts(app, app.chats.front().id, {{"future", 2}, {"next", 1}}, {"next", "future"}, &error));
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, app.chats.front().id, 1, &error, std::pair<std::string, int>{"future", 2}));
	UAM_ASSERT(uam::ResumeQueuedAcpPrompts(app, app.chats.front().id, {{"next", 1}}, &error));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(temp.root, app.chats.front().id)), original);
	UAM_ASSERT_EQ(app.pending_chat_save_at_by_chat_id.at(app.chats.front().id), 99.0);
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->acp_queued_prompts.size(), std::size_t(1));
	UAM_ASSERT_EQ(restored->acp_queued_prompts.front().id, "next");
	UAM_ASSERT(!restored->acp_queue_held);
	UAM_ASSERT_EQ(restored->messages, app.chats.front().messages);
}

#if !defined(_WIN32)
UAM_TEST(QueueJournalRefusesLinkedStorageRootWithoutTouchingExternalData)
{
	TempDir temp("uam-queue-journal-link");
	ChatSession chat = QueueJournalFixture(temp.root);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::string original = ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id));
	const fs::path outside = temp.root / "unrelated";
	fs::create_directories(outside);
	const fs::path sentinel = outside / (chat.id + ".json");
	UAM_ASSERT(uam::io::WriteTextFile(sentinel, "Unrelated data"));
	fs::create_directory_symlink(outside, temp.root / "queue-outboxes");
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	UAM_ASSERT(ChatRepository::DeleteChatStorageFiles(temp.root, chat.id).Failed());
	UAM_ASSERT_EQ(ReadFile(sentinel), "Unrelated data");
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)), original);
}
#endif

UAM_TEST(ChatRepositorySavesOpenedTimeWithoutRewritingMessages)
{
	TempDir temp("uam-chat-opened-time");
	ChatSession chat;
	chat.id = "chat-opened-time";
	chat.provider_id = "opencode-cli";
	chat.title = "Large transcript";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.last_opened_at = chat.updated_at;
	chat.messages.push_back(Message{MessageRole::Assistant, std::string(1024 * 1024, 'x')});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string original_primary = ReadFile(primary);

	chat.last_opened_at = "2026-01-01T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(temp.root, chat));
	UAM_ASSERT_EQ(ReadFile(primary), original_primary);
	std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(summary.has_value());
	UAM_ASSERT_EQ(summary->last_opened_at, chat.last_opened_at);
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, *summary));
	UAM_ASSERT_EQ(summary->last_opened_at, chat.last_opened_at);
	UAM_ASSERT_EQ(summary->messages.front().content, chat.messages.front().content);
	const std::optional<ChatSession> full = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(full.has_value());
	UAM_ASSERT_EQ(full->last_opened_at, chat.last_opened_at);
	const std::vector<ChatSession> all_full = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(all_full.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(all_full.front().last_opened_at, chat.last_opened_at);

	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	UAM_ASSERT(uam::io::WriteTextFile(summary_path, "{"));
	chat.last_opened_at = "2026-01-01T00:00:03.000Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(temp.root, chat));
	UAM_ASSERT(ReadFile(primary) != original_primary);
	const std::optional<ChatSession> recovered = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(recovered.has_value());
	UAM_ASSERT_EQ(recovered->last_opened_at, chat.last_opened_at);
	UAM_ASSERT_EQ(recovered->messages.front().content, chat.messages.front().content);
}

UAM_TEST(ChatRepositoryUnloadedMetadataSavePreservesMessagesFromValidatedBackup)
{
	TempDir temp("uam-chat-unloaded-save-backup");
	ChatSession chat;
	chat.id = "chat-unloaded-save-backup";
	chat.provider_id = "codex-cli";
	chat.title = "First generation";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "durable transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.title = "Current metadata";
	chat.updated_at = "2026-01-01T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!summaries.front().messages_loaded);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	UAM_ASSERT(uam::io::WriteTextFile(primary, "{"));

	summaries.front().pinned = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, summaries.front()));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT(restored->pinned);
	UAM_ASSERT_EQ(restored->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(restored->messages.front().content, std::string("durable transcript"));
	const nlohmann::json retained_backup =
	    nlohmann::json::parse(ReadFile(uam::io::MakeBackupPath(primary)));
	UAM_ASSERT_EQ(retained_backup["messages"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(retained_backup["messages"][0].value("content", ""), std::string("durable transcript"));
}

UAM_TEST(ChatRepositoryUnloadedSaveRejectsMetadataOnlyPrimaryWhenBackupHasTranscript)
{
	TempDir temp("uam-chat-unloaded-save-metadata-only-primary");
	ChatSession chat;
	chat.id = "chat-unloaded-save-metadata-only-primary";
	chat.provider_id = "codex-cli";
	chat.title = "First generation";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "durable transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.title = "Current metadata";
	chat.updated_at = "2026-01-01T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), static_cast<std::size_t>(1));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	nlohmann::json metadata_only = nlohmann::json::parse(ReadFile(primary));
	metadata_only.erase("messages");
	metadata_only["persisted_message_count"] = 1;
	UAM_ASSERT(uam::io::WriteTextFile(primary, metadata_only.dump(2)));

	summaries.front().pinned = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, summaries.front()));
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(restored.has_value());
	UAM_ASSERT_EQ(restored->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(restored->messages.front().content, std::string("durable transcript"));
}

UAM_TEST(ChatRepositoryDoesNotWarnWhenValidPrimaryDiffersFromPreviousGenerationBackup)
{
	TempDir temp("uam-chat-normal-backup-generation");
	ChatSession chat;
	chat.id = "chat-normal-backup-generation";
	chat.provider_id = "codex-cli";
	chat.title = "First generation";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "durable transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.title = "Second generation";
	chat.updated_at = "2026-01-01T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	std::string warning;
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root, &warning);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().title, std::string("Second generation"));
	UAM_ASSERT(warning.empty());
}

UAM_TEST(LocalChatBundleExportPreservesEveryCanonicalChatFile)
{
	TempDir temp("uam-local-chat-bundle-export");
	const fs::path data_root = temp.root / "source";
	const fs::path bundle = temp.root / "bundle";
	ChatSession first;
	first.id = "chat-export-a";
	first.provider_id = "codex-cli";
	first.title = "First";
	first.created_at = "2026-01-01T00:00:00.000Z";
	first.updated_at = "2026-01-01T00:00:01.000Z";
	first.messages.push_back(Message{MessageRole::User, "First transcript"});
	ChatSession second = first;
	second.id = "chat-export-b";
	second.title = "Second";
	second.messages.front().content = "Second transcript";
	UAM_ASSERT(ChatRepository::SaveChat(data_root, first));
	UAM_ASSERT(ChatRepository::SaveChat(data_root, second));
	const std::string first_canonical = ReadFile(AppPaths::UamChatFilePath(data_root, first.id));
	const std::string second_canonical = ReadFile(AppPaths::UamChatFilePath(data_root, second.id));

	const uam::LocalChatBundleExportResult result = uam::LocalChatBundleService::Export(data_root, bundle);

	UAM_ASSERT(result.ok);
	UAM_ASSERT(!result.degraded);
	UAM_ASSERT_EQ(result.total_count, static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(result.exported_count, static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(ReadFile(bundle / "chats" / "chat-export-a.json"), first_canonical);
	UAM_ASSERT_EQ(ReadFile(bundle / "chats" / "chat-export-b.json"), second_canonical);
	const nlohmann::json manifest = nlohmann::json::parse(ReadFile(bundle / "manifest.json"));
	UAM_ASSERT_EQ(manifest.value("schema", ""), std::string(uam::LocalChatBundleService::kSchema));
	UAM_ASSERT_EQ(manifest.value("version", 0), uam::LocalChatBundleService::kVersion);
	UAM_ASSERT_EQ(manifest.value("chatCount", 0), 2);
	UAM_ASSERT_EQ(manifest["chats"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(manifest["chats"][0].value("file", ""), std::string("chats/chat-export-a.json"));
	UAM_ASSERT_EQ(manifest["chats"][1].value("file", ""), std::string("chats/chat-export-b.json"));
}

UAM_TEST(LocalChatBundleExportRejectsANonEmptyDestinationWithoutChangingIt)
{
	TempDir temp("uam-local-chat-bundle-nonempty");
	const fs::path data_root = temp.root / "source";
	const fs::path bundle = temp.root / "existing";
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(bundle));
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "keep.txt", "do not change"));

	const uam::LocalChatBundleExportResult result = uam::LocalChatBundleService::Export(data_root, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT(!result.degraded);
	UAM_ASSERT(!result.errors.empty());
	UAM_ASSERT_EQ(ReadFile(bundle / "keep.txt"), std::string("do not change"));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(bundle / "manifest.json"));
}

UAM_TEST(LocalChatBundleExportRejectsAChatThatImportWouldReject)
{
	TempDir temp("uam-local-chat-bundle-export-limit");
	const fs::path data_root = temp.root / "source";
	const fs::path bundle = temp.root / "bundle";
	const fs::path chat_path = AppPaths::UamChatFilePath(data_root, "chat-export-too-large");
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(chat_path.parent_path()));
	UAM_ASSERT(uam::io::WriteTextFile(chat_path, "{}"));
	std::error_code resize_error;
	fs::resize_file(chat_path, 64ULL * 1024ULL * 1024ULL + 1, resize_error);
	UAM_ASSERT(!resize_error);

	const uam::LocalChatBundleExportResult result = uam::LocalChatBundleService::Export(data_root, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT(!result.errors.empty());
	UAM_ASSERT(uam::strings::Contains(result.errors.front(), "safe bundle data limit"));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(bundle));
}

UAM_TEST(LocalChatBundleExportDoesNotHydrateLegacyStorage)
{
	TempDir temp("uam-local-chat-bundle-export-legacy");
	const fs::path data_root = temp.root / "source";
	const fs::path bundle = temp.root / "bundle";
	const fs::path legacy_message = AppPaths::ChatPath(data_root, "chat-legacy") / "messages" / "000-user.txt";
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(legacy_message.parent_path()));
	UAM_ASSERT(uam::io::WriteTextFile(legacy_message, "x"));
	std::error_code resize_error;
	fs::resize_file(legacy_message, 64ULL * 1024ULL * 1024ULL + 1, resize_error);
	UAM_ASSERT(!resize_error);

	const uam::LocalChatBundleExportResult result = uam::LocalChatBundleService::Export(data_root, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT(!result.errors.empty());
	UAM_ASSERT(uam::strings::Contains(result.errors.front(), "Legacy local chat storage"));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(AppPaths::UamChatFilePath(data_root, "chat-legacy")));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(bundle));
}

#if !defined(_WIN32)
UAM_TEST(LocalChatBundleExportRejectsAManifestThatImportWouldReject)
{
	TempDir temp("uam-local-chat-bundle-export-manifest-limit");
	const fs::path data_root = temp.root / "source";
	const fs::path bundle = temp.root / "bundle";
	const fs::path chats = AppPaths::UamChatsRootPath(data_root);
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(chats));
	for (std::size_t index = 0; index < 10000; ++index)
	{
		const std::string id = "chat-" + std::string(190, '"') + "-" + std::to_string(index);
		std::ofstream file(AppPaths::UamChatFilePath(data_root, id), std::ios::binary | std::ios::trunc);
		file << nlohmann::json({{"id", id}}).dump();
		UAM_ASSERT(file.good());
	}

	const uam::LocalChatBundleExportResult result = uam::LocalChatBundleService::Export(data_root, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT(!result.errors.empty());
	UAM_ASSERT(uam::strings::Contains(result.errors.front(), "safe bundle manifest limit"));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(bundle));
}
#endif

UAM_TEST(LocalChatBundleImportValidatesManifestAndEveryChatBeforeMutation)
{
	TempDir temp("uam-local-chat-bundle-validation");
	const fs::path source = temp.root / "source";
	const fs::path target = temp.root / "target";
	const fs::path bundle = temp.root / "bundle";
	for (const std::string& id : {"chat-valid-a", "chat-valid-b"})
	{
		ChatSession chat;
		chat.id = id;
		chat.title = id;
		chat.created_at = "2026-01-01T00:00:00.000Z";
		chat.updated_at = "2026-01-01T00:00:01.000Z";
		chat.messages.push_back(Message{MessageRole::User, id});
		UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	}
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "chats" / "chat-valid-b.json", "{"));

	const uam::LocalChatBundleImportResult malformed = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(!malformed.ok);
	UAM_ASSERT_EQ(malformed.imported_count, static_cast<std::size_t>(0));
	UAM_ASSERT(ChatRepository::LoadLocalChats(target).empty());

	nlohmann::json manifest = nlohmann::json::parse(ReadFile(bundle / "manifest.json"));
	manifest["version"] = 99;
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "manifest.json", manifest.dump(2)));
	const uam::LocalChatBundleImportResult wrong_schema = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(!wrong_schema.ok);
	UAM_ASSERT_EQ(wrong_schema.imported_count, static_cast<std::size_t>(0));
	UAM_ASSERT(ChatRepository::LoadLocalChats(target).empty());
}

UAM_TEST(LocalChatBundleImportRenamesConflictsWithoutOverwritingExistingChats)
{
	TempDir temp("uam-local-chat-bundle-conflict");
	const fs::path source = temp.root / "source";
	const fs::path target = temp.root / "target";
	const fs::path bundle = temp.root / "bundle";
	ChatSession exported;
	exported.id = "chat-same";
	exported.title = "Exported title";
	exported.native_session_id = "provider-session-shared";
	exported.created_at = "2026-01-01T00:00:00.000Z";
	exported.updated_at = "2026-01-01T00:00:01.000Z";
	exported.messages.push_back(Message{MessageRole::User, "exported transcript"});
	ChatSession existing = exported;
	existing.title = "Existing title";
	existing.messages.front().content = "existing transcript";
	UAM_ASSERT(ChatRepository::SaveChat(source, exported));
	UAM_ASSERT(ChatRepository::SaveChat(target, existing));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);

	const uam::LocalChatBundleImportResult result = uam::LocalChatBundleService::Import(target, bundle);

	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(result.imported_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(result.renamed_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(result.items.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(result.items.front().renamed);
	UAM_ASSERT(result.items.front().imported_id != exported.id);
	UAM_ASSERT_EQ(ChatRepository::LoadLocalChat(target, exported.id)->messages.front().content, std::string("existing transcript"));
	const std::optional<ChatSession> imported = ChatRepository::LoadLocalChat(target, result.items.front().imported_id);
	UAM_ASSERT(imported.has_value());
	UAM_ASSERT_EQ(imported->messages.front().content, std::string("exported transcript"));
	UAM_ASSERT(imported->title.find("Imported") != std::string::npos);
	UAM_ASSERT(imported->native_session_id.empty());
}

UAM_TEST(LocalChatBundleImportStripsLocalExecutionAuthority)
{
	TempDir temp("uam-local-chat-bundle-passive-import");
	const fs::path source = temp.root / "source";
	const fs::path target = temp.root / "target";
	const fs::path bundle = temp.root / "bundle";
	ChatSession exported;
	exported.id = "chat-passive";
	exported.title = "Portable transcript";
	exported.provider_id = "codex-cli";
	exported.native_session_id = "provider-session";
	exported.execution_host_id = "ssh-remote";
	exported.remote_turn_reconnect_pending = true;
	exported.remote_restart_pending = true;
	exported.remote_delivered_stdout_cursor = 123;
	exported.remote_delivered_stderr_cursor = 45;
	exported.folder_id = "local-folder";
	exported.workspace_directory = "/private/tmp/sensitive";
	exported.workspace_isolation_kind = "gitWorktree";
	exported.workspace_source_directory = "/private/tmp/source";
	exported.workspace_base_ref = "main";
	exported.workspace_branch_name = "dangerous";
	exported.workspace_worktree_directory = "/private/tmp/worktree";
	exported.approval_mode = "yolo";
	exported.command_safety_tier = "yolo";
	exported.uam_agent_id = "custom-agent";
	exported.agent_run_id = "active-run";
	exported.goal_owner_chat_id = "owner";
	exported.goal_iteration_goal_id = "goal";
	exported.goal_iteration_turn_kind = "worker";
	exported.goal_iteration_repair_attempts = 3;
	exported.uam_control_enabled = true;
	exported.extra_flags = "--dangerous-provider-flag";
	exported.active_goal_id = "goal";
	exported.linked_files = {"/private/tmp/secret.txt"};
	exported.created_at = "2026-01-01T00:00:00.000Z";
	exported.updated_at = "2026-01-01T00:00:01.000Z";
	exported.messages.push_back(Message{MessageRole::User, "portable transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(source, exported));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);

	const uam::LocalChatBundleImportResult result = uam::LocalChatBundleService::Import(target, bundle);

	UAM_ASSERT(result.ok);
	const std::optional<ChatSession> imported = ChatRepository::LoadLocalChat(target, exported.id);
	UAM_ASSERT(imported.has_value());
	UAM_ASSERT(imported->native_session_id.empty());
	UAM_ASSERT_EQ(imported->execution_host_id, std::string("local"));
	UAM_ASSERT(!imported->remote_turn_reconnect_pending);
	UAM_ASSERT(!imported->remote_restart_pending);
	UAM_ASSERT_EQ(imported->remote_delivered_stdout_cursor, static_cast<std::uintmax_t>(0));
	UAM_ASSERT_EQ(imported->remote_delivered_stderr_cursor, static_cast<std::uintmax_t>(0));
	UAM_ASSERT(imported->folder_id.empty());
	UAM_ASSERT(imported->workspace_directory.empty());
	UAM_ASSERT(imported->workspace_isolation_kind.empty());
	UAM_ASSERT(imported->workspace_source_directory.empty());
	UAM_ASSERT(imported->workspace_base_ref.empty());
	UAM_ASSERT(imported->workspace_branch_name.empty());
	UAM_ASSERT(imported->workspace_worktree_directory.empty());
	UAM_ASSERT(imported->imported_read_only);
	UAM_ASSERT_EQ(imported->approval_mode, std::string("default"));
	UAM_ASSERT_EQ(imported->command_safety_tier, std::string("off"));
	UAM_ASSERT_EQ(imported->uam_agent_id, std::string("build"));
	UAM_ASSERT(imported->agent_run_id.empty());
	UAM_ASSERT(imported->goal_owner_chat_id.empty());
	UAM_ASSERT(imported->goal_iteration_goal_id.empty());
	UAM_ASSERT(imported->goal_iteration_turn_kind.empty());
	UAM_ASSERT_EQ(imported->goal_iteration_repair_attempts, 0);
	UAM_ASSERT(!imported->uam_control_enabled);
	UAM_ASSERT(imported->extra_flags.empty());
	UAM_ASSERT(imported->active_goal_id.empty());
	UAM_ASSERT(imported->linked_files.empty());
	UAM_ASSERT(!imported->memory_enabled);
	UAM_ASSERT_EQ(imported->memory_level, std::string("off"));
	UAM_ASSERT_EQ(imported->memory_last_processed_message_count, 0);
	UAM_ASSERT(imported->memory_last_processed_at.empty());
	UAM_ASSERT(!imported->small_model_mode);
	uam::AppState imported_app;
	imported_app.chats.push_back(*imported);
	UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(imported_app, imported_app.chats.front()).empty());
	UAM_ASSERT_EQ(imported->messages.front().content, std::string("portable transcript"));
}

UAM_TEST(LocalChatBundleImportRejectsChatsAboveTheResourceLimit)
{
	TempDir temp("uam-local-chat-bundle-resource-limit");
	const fs::path source = temp.root / "source";
	const fs::path target = temp.root / "target";
	const fs::path bundle = temp.root / "bundle";
	ChatSession chat;
	chat.id = "chat-too-large";
	chat.title = "Too large";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "small fixture"});
	UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	nlohmann::json manifest = nlohmann::json::parse(ReadFile(bundle / "manifest.json"));
	manifest["chats"][0]["byteSize"] = 64ULL * 1024ULL * 1024ULL + 1ULL;
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "manifest.json", manifest.dump(2)));

	const uam::LocalChatBundleImportResult result = uam::LocalChatBundleService::Import(target, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT_EQ(result.imported_count, static_cast<std::size_t>(0));
	UAM_ASSERT(std::ranges::any_of(result.errors, [](const std::string& error) {
		return uam::strings::Contains(error, "safe import limit");
	}));
}

UAM_TEST(LocalChatBundleImportReportsDestinationWriteFailures)
{
	TempDir temp("uam-local-chat-bundle-write-failure");
	const fs::path source = temp.root / "source";
	const fs::path target = temp.root / "target";
	const fs::path bundle = temp.root / "bundle";
	ChatSession chat;
	chat.id = "chat-write-failure";
	chat.title = "Cannot write";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::User, "preserve me"});
	UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(target));
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatsRootPath(target), "block chats directory"));

	const uam::LocalChatBundleImportResult result = uam::LocalChatBundleService::Import(target, bundle);

	UAM_ASSERT(!result.ok);
	UAM_ASSERT(!result.degraded);
	UAM_ASSERT_EQ(result.total_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(result.imported_count, static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(result.failed_count, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(result.items.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!result.items.front().imported);
	UAM_ASSERT(!result.items.front().error.empty());
}

UAM_TEST(ChatRepositoryAccumulatesLoadWarnings)
{
	TempDir temp("uam-chat-load-warning-accumulate");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	UAM_ASSERT(uam::io::WriteTextFile(chats_dir / "bad-json.json", "{"));
	UAM_ASSERT(uam::io::WriteTextFile(chats_dir / "missing-id.json", R"({"title":"Missing id"})"));

	std::string warning;
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root, &warning);
	UAM_ASSERT(loaded.empty());
	UAM_ASSERT(warning.find("bad-json.json") != std::string::npos);
	UAM_ASSERT(warning.find("contains invalid JSON") != std::string::npos);
	UAM_ASSERT(warning.find("missing-id.json") != std::string::npos);
	UAM_ASSERT(warning.find("is missing a chat id") != std::string::npos);
	UAM_ASSERT(warning.find('\n') != std::string::npos);
}

UAM_TEST(ChatRepositoryDeletesStorageFilesWithSafeIdsOnly)
{
	TempDir temp("uam-chat-delete-storage");

	ChatSession chat;
	chat.id = "chat-delete-storage";
	chat.provider_id = "codex-cli";
	chat.title = "Delete Storage";
	chat.created_at = "2026-01-01 00:00:00";
	chat.updated_at = "2026-01-01 00:00:01";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const std::string padded_chat_id = "xx" + chat.id + "yy";
	const std::string_view sliced_chat_id = std::string_view(padded_chat_id).substr(2, chat.id.size());
	const fs::path legacy_dir = AppPaths::ChatPath(temp.root, sliced_chat_id);
	UAM_ASSERT(uam::io::WriteTextFile(legacy_dir / "message.txt", "legacy"));
	UAM_ASSERT(fs::exists(AppPaths::UamChatFilePath(temp.root, sliced_chat_id)));
	UAM_ASSERT(fs::exists(legacy_dir));

	const ChatStorageDeleteResult deleted = ChatRepository::DeleteChatStorageFiles(temp.root, sliced_chat_id);
	UAM_ASSERT(!deleted.Failed());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(temp.root, sliced_chat_id)));
	UAM_ASSERT(!fs::exists(legacy_dir));

	const fs::path escaped_file = temp.root / "escaped.json";
	UAM_ASSERT(uam::io::WriteTextFile(escaped_file, "outside"));
	const ChatStorageDeleteResult rejected = ChatRepository::DeleteChatStorageFiles(temp.root, "../escaped");
	UAM_ASSERT(rejected.unsafe_chat_id);
	UAM_ASSERT(rejected.Failed());
	UAM_ASSERT(fs::exists(escaped_file));
}

UAM_TEST(ChatRepositoryReportsSummaryDeleteFailures)
{
	TempDir temp("uam-chat-summary-delete-failure");
	const std::string chat_id = "chat-summary-delete-failure";
	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(temp.root, chat_id);
	fs::create_directories(summary_path);
	UAM_ASSERT(uam::io::WriteTextFile(summary_path / "keep.txt", "not removable as a file"));

	const ChatStorageDeleteResult result = ChatRepository::DeleteChatStorageFiles(temp.root, chat_id);
	UAM_ASSERT(result.Failed());
	UAM_ASSERT(result.summary_file_error);
}

UAM_TEST(ChatDomainServiceAutoTitlesOnlyPlaceholderNewSession)
{
	ChatSession placeholder_chat;
	placeholder_chat.title = "New Session";

	ChatDomainService().AddMessage(placeholder_chat, MessageRole::User, "   Draft the release notes for the beta build   ");
	UAM_ASSERT_EQ(placeholder_chat.title, std::string("Draft the release notes for the beta build"));

	ChatSession custom_title_chat;
	custom_title_chat.title = "Project kickoff";

	ChatDomainService().AddMessage(custom_title_chat, MessageRole::User, "Rename me");
	UAM_ASSERT_EQ(custom_title_chat.title, std::string("Project kickoff"));

	ChatSession generated_title_chat = ChatDomainService().CreateNewChat("folder-1", "gemini-cli");
	const std::string generated_title = generated_title_chat.title;
	ChatDomainService().AddMessage(generated_title_chat, MessageRole::User, "Keep generated title");
	UAM_ASSERT_EQ(generated_title_chat.title, generated_title);

	ChatSession assistant_first_chat;
	assistant_first_chat.title = "New Session";
	ChatDomainService().AddMessage(assistant_first_chat, MessageRole::Assistant, "Assistant first");
	UAM_ASSERT_EQ(assistant_first_chat.title, std::string("New Session"));
}

UAM_TEST(ChatDomainServiceCreateNewChatNormalizesBoundaryIds)
{
	const ChatSession chat = ChatDomainService().CreateNewChat(" folder-1 ", " codex-cli ");

	UAM_ASSERT(!chat.id.empty());
	UAM_ASSERT_EQ(chat.folder_id, std::string("folder-1"));
	UAM_ASSERT_EQ(chat.provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(chat.branch_root_chat_id, chat.id);
	UAM_ASSERT_EQ(chat.branch_from_message_index, -1);
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
}

UAM_TEST(ChatDomainServiceBranchesPastUserMessagesWithoutOverwritingHistory)
{
	TempDir temp("uam-message-branch");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "codex-cli");
	source.id = "chat-source";
	source.branch_root_chat_id = source.id;
	source.title = "Source";
	source.execution_host_id = "ssh-windows";
	source.extra_flags = "--include-directories ../shared";
	source.reviewer_model_id = "gpt-review";
	source.messages.push_back(Message{MessageRole::User, "Original prompt"});
	source.messages.push_back(Message{MessageRole::Assistant, "Original answer"});
	app.chats.push_back(source);
	ChatDomainService().SelectChatById(app, source.id);

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited prompt")));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	const ChatSession* edited = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(edited != nullptr);
	UAM_ASSERT(edited->id != source.id);
	UAM_ASSERT_EQ(edited->parent_chat_id, source.id);
	UAM_ASSERT_EQ(edited->branch_from_message_index, 0);
	UAM_ASSERT(edited->branch_message_edited);
	UAM_ASSERT_EQ(edited->execution_host_id, source.execution_host_id);
	UAM_ASSERT_EQ(edited->extra_flags, source.extra_flags);
	UAM_ASSERT_EQ(edited->reviewer_model_id, source.reviewer_model_id);
	UAM_ASSERT_EQ(edited->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(edited->messages.front().content, std::string("Edited prompt"));
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source.id)->messages[0].content, std::string("Original prompt"));
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source.id)->messages.size(), static_cast<std::size_t>(2));

	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	const auto saved_edited = std::ranges::find_if(saved, [edited_id = edited->id](const ChatSession& chat) { return chat.id == edited_id; });
	UAM_ASSERT(saved_edited != saved.end());
	UAM_ASSERT(saved_edited->branch_message_edited);
	UAM_ASSERT_EQ(saved_edited->execution_host_id, source.execution_host_id);
	UAM_ASSERT_EQ(saved_edited->reviewer_model_id, source.reviewer_model_id);
	UAM_ASSERT_EQ(saved_edited->messages.front().content, std::string("Edited prompt"));

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0));
	const ChatSession* reverted = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(reverted != nullptr);
	UAM_ASSERT(!reverted->branch_message_edited);
	UAM_ASSERT_EQ(reverted->execution_host_id, source.execution_host_id);
	UAM_ASSERT_EQ(reverted->messages.front().content, std::string("Original prompt"));
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source.id)->messages.size(), static_cast<std::size_t>(2));
}

UAM_TEST(ChatDomainServiceBranchOperationIdIsIdempotentAndRejectsCollisions)
{
	TempDir temp("uam-message-branch-operation-id");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "codex-cli");
	source.id = "chat-operation-source";
	source.messages.push_back(Message{MessageRole::User, "Original prompt"});
	app.chats.push_back(source);

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::nullopt, std::string("operation-1")));
	const std::string branch_id = ChatDomainService().SelectedChatId(app);
	UAM_ASSERT(!branch_id.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	const auto saved_branch = std::ranges::find_if(saved, [branch_id](const ChatSession& chat) { return chat.id == branch_id; });
	UAM_ASSERT(saved_branch != saved.end());
	UAM_ASSERT_EQ(saved_branch->branch_operation_id, std::string("operation-1"));
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::nullopt, std::string("operation-1")));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), branch_id);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("operation-1")));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("operation-2")));
	const std::string edited_branch_id = ChatDomainService().SelectedChatId(app);
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Different"), std::string("operation-2")));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), edited_branch_id);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(3));
}



UAM_TEST(ColdPromptCompletionParsingFailureRespondsOnceBeforeDeferral)
{
	using namespace uam::query_handler_async;
	struct Callback
	{
		void Success(const std::string&) { ++successes; }
		void Failure(int code, const std::string&) { ++failures; status = code; }
		int successes = 0;
		int failures = 0;
		int status = 0;
	};
	Callback captured;
	Callback* callback = &captured;
	AsyncCefResult response = AsyncSuccess({});
	CompleteAsyncCefQuery(callback, response, [&](AsyncCefResult& result)
	{
		const nlohmann::json malformed{{"goalMode", "invalid"}};
		(void)malformed.value("goalMode", false);
		result.callback_deferred = true;
	});
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->status, 400);
	UAM_ASSERT_EQ(callback->successes, 0);
	UAM_ASSERT(!response.callback_deferred);
	response = AsyncSuccess({});
	CompleteAsyncCefQuery(callback, response, [&](AsyncCefResult& result)
	{
		callback->Success("{}");
		result.callback_deferred = true;
	});
	UAM_ASSERT_EQ(callback->failures, 1);
	UAM_ASSERT_EQ(callback->successes, 1);
}

UAM_TEST(ColdPromptPreparationStopAndOldCompletionCannotReleaseNewAdmission)
{
	TempDir temp("uam-cold-prompt-stop");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatDomainService domain;
	ChatSession saved = domain.CreateNewChat("", "codex-cli");
	saved.id = "cold-prompt";
	saved.messages.push_back(Message{MessageRole::User, "Keep durable history"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, saved));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const auto first = domain.BeginColdHistoryPreparation(app, saved.id);
	UAM_ASSERT(first != nullptr);
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(app, saved.id, "must not overtake", &error));
	UAM_ASSERT(!uam::SteerAcpPrompt(app, saved.id, "must not steer", {}, {}, false, &error));
	UAM_ASSERT(domain.LoadColdHistoryPreparation(*first, error));
	UAM_ASSERT(uam::StopAcpSession(app, saved.id));
	UAM_ASSERT(first->ownership->cancellation.stop_requested());
	const auto second = domain.BeginColdHistoryPreparation(app, saved.id);
	UAM_ASSERT(second != nullptr);
	UAM_ASSERT(!domain.CompleteColdHistoryPreparation(app, *first));
	UAM_ASSERT(domain.BeginColdHistoryPreparation(app, saved.id) == nullptr);
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(app.acp_sessions.empty());
	UAM_ASSERT(uam::CancelAcpTurn(app, saved.id));
	UAM_ASSERT(!domain.LoadColdHistoryPreparation(*second, error));
	UAM_ASSERT(!domain.CompleteColdHistoryPreparation(app, *second));
	const auto persisted = ChatRepository::LoadLocalChat(temp.root, saved.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->messages, saved.messages);
}

UAM_TEST(ColdPromptPreparationRejectsStaleIdentityStorageAndWarmBody)
{
	TempDir temp("uam-cold-prompt-stale");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatDomainService domain;
	ChatSession saved = domain.CreateNewChat("", "codex-cli");
	saved.id = "cold-prompt-stale";
	saved.native_session_id = "123e4567-e89b-12d3-a456-426614174000";
	saved.messages.push_back(Message{MessageRole::User, "Original history"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, saved));
	for (int variation = 0; variation < 7; ++variation)
	{
		app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
		const auto prepared = domain.BeginColdHistoryPreparation(app, saved.id);
		UAM_ASSERT(prepared != nullptr);
		std::string error;
		UAM_ASSERT(domain.LoadColdHistoryPreparation(*prepared, error));
		switch (variation)
		{
		case 0: app.chats.front().native_session_id = "native-changed"; break;
		case 1: app.chats.front().provider_id = "claude-cli"; break;
		case 2: app.chats.front().execution_host_id = "changed-host"; break;
		case 3:
			UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, app.chats.front()));
			app.chats.front().messages.front().content = "Modified history";
			break;
		case 4: app.pending_chat_save_at_by_chat_id[saved.id] = 1; break;
		case 5: app.worktree_operation_chat_ids.insert(saved.id); break;
		case 6:
			saved.messages.front().content += " changed on disk";
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, saved));
			break;
		}
		UAM_ASSERT(!domain.CompleteColdHistoryPreparation(app, *prepared));
		UAM_ASSERT(app.acp_sessions.empty());
		app.pending_chat_save_at_by_chat_id.clear();
		app.worktree_operation_chat_ids.clear();
	}
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const auto prepared = domain.BeginColdHistoryPreparation(app, saved.id);
	UAM_ASSERT(prepared != nullptr);
	std::string error;
	UAM_ASSERT(domain.LoadColdHistoryPreparation(*prepared, error));
	app.chats.front().title = "Keep live title";
	UAM_ASSERT(domain.CompleteColdHistoryPreparation(app, *prepared));
	UAM_ASSERT_EQ(app.chats.front().messages, saved.messages);
	UAM_ASSERT_EQ(app.chats.front().title, std::string("Keep live title"));
	UAM_ASSERT_EQ(app.chats.front().native_session_id, saved.native_session_id);
	UAM_ASSERT(!domain.CompleteColdHistoryPreparation(app, *prepared));
}


UAM_TEST(ColdPromptPreparationContinuesThroughDurableFifoOutbox)
{
	TempDir temp("uam-cold-prompt-outbox");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const ChatDomainService domain;
	ChatSession saved = domain.CreateNewChat("", app.provider_profiles.front().id);
	saved.id = "cold-prompt-outbox";
	saved.workspace_directory = temp.root.string();
	saved.messages.push_back(Message{MessageRole::User, "Keep prior conversation"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, saved));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const auto prepared = domain.BeginColdHistoryPreparation(app, saved.id);
	UAM_ASSERT(prepared != nullptr);
	std::string error;
	UAM_ASSERT(domain.LoadColdHistoryPreparation(*prepared, error));
	UAM_ASSERT(domain.CompleteColdHistoryPreparation(app, *prepared));
	// A busy in-memory transport queues without launching or writing to any provider.
	auto owned_session = std::make_unique<uam::AcpSessionState>();
	owned_session->chat_id = saved.id;
	owned_session->provider_id = saved.provider_id;
	owned_session->running = true;
	owned_session->processing = true;
	app.acp_sessions.push_back(std::move(owned_session));
	UAM_ASSERT(uam::SendAcpPrompt(app, saved.id, "first accepted", &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, saved.id, "second accepted", &error));
	const auto persisted = ChatRepository::LoadLocalChat(temp.root, saved.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->messages.front().content, saved.messages.front().content);
	UAM_ASSERT_EQ(persisted->acp_queued_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(persisted->acp_queued_prompts[0].text, std::string("first accepted"));
	UAM_ASSERT_EQ(persisted->acp_queued_prompts[1].text, std::string("second accepted"));
}

UAM_TEST(ColdPromptPreparationUnreadableHistoryNeverPublishes)
{
	TempDir temp("uam-cold-prompt-unreadable");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatDomainService domain;
	ChatSession saved = domain.CreateNewChat("", "codex-cli");
	saved.id = "cold-prompt-unreadable";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, saved));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const auto prepared = domain.BeginColdHistoryPreparation(app, saved.id);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT(ChatRepository::DeleteChatStorageFiles(temp.root, saved.id).Failed() == false);
	std::string error;
	UAM_ASSERT(!domain.LoadColdHistoryPreparation(*prepared, error));
	UAM_ASSERT(!domain.CompleteColdHistoryPreparation(app, *prepared));
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(app.acp_sessions.empty());
}

UAM_TEST(ColdBranchPreparationStopOwnsSourceBeforeAnyBranchExists)
{
	uam::AppState app;
	const ChatDomainService domain;
	app.chats.push_back(domain.CreateNewChat("", "codex-cli"));
	const std::string source_id = app.chats.front().id;
	const std::shared_ptr<uam::BranchPreparationOwnership> stopped = domain.BeginBranchPreparation(app, source_id, "");
	UAM_ASSERT(stopped != nullptr);
	UAM_ASSERT(domain.BeginBranchPreparation(app, source_id, "") == nullptr);
	UAM_ASSERT(uam::CancelAcpTurn(app, source_id));
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source_id, stopped));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_sessions.empty());

	const std::shared_ptr<uam::BranchPreparationOwnership> retry = domain.BeginBranchPreparation(app, source_id, "");
	UAM_ASSERT(retry != nullptr);
	// An old completion must neither create a branch nor release a newer reservation.
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source_id, stopped));
	UAM_ASSERT(domain.BeginBranchPreparation(app, source_id, "") == nullptr);
	UAM_ASSERT(uam::StopAcpSession(app, source_id));
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source_id, retry));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));

	const std::shared_ptr<uam::BranchPreparationOwnership> abandoned = domain.BeginBranchPreparation(app, source_id, "");
	UAM_ASSERT(domain.ConsumeBranchPreparation(app, source_id, abandoned));
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source_id, abandoned));
	{ const std::shared_ptr<uam::BranchPreparationOwnership> unposted = domain.BeginBranchPreparation(app, source_id, ""); }
	UAM_ASSERT(domain.BeginBranchPreparation(app, source_id, "") != nullptr);
}

UAM_TEST(ColdBranchPreparationDeletionAndExistingBranchStopInvalidateOnlyOwners)
{
	TempDir temp("uam-cold-branch-deletion");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatDomainService domain;
	ChatSession source = domain.CreateNewChat("", "codex-cli");
	source.id = "cold-delete-source";
	source.messages.push_back(Message{MessageRole::User, "Keep source history"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, source));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	app.chats.push_back(domain.CreateNewChat("", "codex-cli"));
	const std::string unrelated_id = app.chats.back().id;
	const std::shared_ptr<uam::BranchPreparationOwnership> unrelated = domain.BeginBranchPreparation(app, unrelated_id, "");
	const std::shared_ptr<uam::BranchPreparationOwnership> replay = domain.BeginBranchPreparation(app, source.id, "existing-branch");
	UAM_ASSERT(uam::StopAcpSession(app, "existing-branch"));
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source.id, replay));
	const std::shared_ptr<uam::BranchPreparationOwnership> deleting = domain.BeginBranchPreparation(app, source.id, "");
	uam::WorkspaceDeletionTask deletion;
	UAM_ASSERT(uam::PrepareChatDeletion(app, {source.id}, deletion));
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, source.id, deleting));
	UAM_ASSERT(domain.ConsumeBranchPreparation(app, unrelated_id, unrelated));
	// Preparation preflight must not hydrate or remove the user's persisted transcript.
	UAM_ASSERT(!domain.FindChatById(app, source.id)->messages_loaded);
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(temp.root, source.id, true);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT(persisted->messages == source.messages);
}

UAM_TEST(ColdBranchPreparationRejectsChangedIdentityAndUnreadableHistory)
{
	TempDir temp("uam-cold-branch-identity");
	uam::AppState app;
	app.data_root = temp.root;
	const ChatDomainService domain;
	ChatSession source = domain.CreateNewChat("", "codex-cli");
	source.id = "cold-identity-source";
	source.messages.push_back(Message{MessageRole::User, "Original history"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, source));
	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const ChatSession summary = app.chats.front();
	const ChatDomainService::BranchHistoryIdentity identity = domain.CaptureBranchHistoryIdentity(summary);
	const std::optional<ChatSession> loaded = domain.LoadBranchHistoryDetached(temp.root, identity);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(domain.MatchesPreparedBranchHistory(summary, identity, false, &*loaded));
	for (const std::function<void(ChatSession&)>& change : std::vector<std::function<void(ChatSession&)>>{
	    [](ChatSession& chat) { chat.provider_id = "gemini-cli"; },
	    [](ChatSession& chat) { chat.execution_host_id = "other-host"; },
	    [](ChatSession& chat) { chat.native_session_id = "other-session"; },
	    [](ChatSession& chat) { chat.workspace_directory = "other-workspace"; },
	    [](ChatSession& chat) { chat.workspace_worktree_directory = "other-worktree"; },
	    [](ChatSession& chat) { chat.updated_at = "other-revision"; },
	    [](ChatSession& chat) { ++chat.persisted_message_count; },
	    [](ChatSession& chat) { chat.persisted_messages_digest = "other-digest"; }})
	{
		ChatSession changed = summary;
		change(changed);
		UAM_ASSERT(!domain.MatchesPreparedBranchHistory(changed, identity, false, &*loaded));
	}
	ChatSession warmed = *loaded;
	UAM_ASSERT(domain.MatchesPreparedBranchHistory(warmed, identity, false, &*loaded));
	warmed.messages.front().content = "Unsaved change";
	UAM_ASSERT(!domain.MatchesPreparedBranchHistory(warmed, identity, false, &*loaded));
	UAM_ASSERT(!domain.MatchesPreparedBranchHistory(summary, identity, true, nullptr));
	UAM_ASSERT(!domain.MatchesPreparedBranchHistory(summary, identity, false, nullptr));

	const std::shared_ptr<uam::BranchPreparationOwnership> failed = domain.BeginBranchPreparation(app, source.id, "");
	const fs::path history = temp.root / "chats" / (source.id + ".json");
	const std::string original_bytes = ReadFile(history);
	UAM_ASSERT(uam::io::WriteTextFile(history, "{broken history"));
	UAM_ASSERT(!domain.LoadBranchHistoryDetached(temp.root, identity).has_value());
	UAM_ASSERT(domain.ConsumeBranchPreparation(app, source.id, failed));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(uam::io::WriteTextFile(history, original_bytes));
	const std::shared_ptr<uam::BranchPreparationOwnership> retry = domain.BeginBranchPreparation(app, source.id, "");
	UAM_ASSERT(retry != nullptr);
	UAM_ASSERT(domain.LoadBranchHistoryDetached(temp.root, identity).has_value());
	UAM_ASSERT(domain.ConsumeBranchPreparation(app, source.id, retry));
	UAM_ASSERT_EQ(ReadFile(history), original_bytes);
}

UAM_TEST(ChatBranchIdempotentReplayUsesDetachedColdHistoriesWithoutWarmingChats)
{
	TempDir temp("uam-message-branch-cold-idempotent");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("", "codex-cli");
	source.id = "cold-operation-source";
	source.branch_root_chat_id = source.id;
	source.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	Message prompt{MessageRole::User, "Original prompt"};
	prompt.attachments.push_back(MessageAttachment{.id = "cold-image", .name = "cold.png", .kind = "image", .path = "cold.png"});
	source.messages.push_back(prompt);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, source));
	app.chats.push_back(source);
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::nullopt, std::string("cold-operation-1")));
	const std::string branch_id = ChatDomainService().SelectedChatId(app);
	UAM_ASSERT(!branch_id.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));

	std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), static_cast<std::size_t>(2));
	for (ChatSession& summary : summaries)
	{
		ChatSession* current = ChatDomainService().FindChatById(app, summary.id);
		UAM_ASSERT(current != nullptr);
		*current = std::move(summary);
	}
	ChatSession* cold_source = ChatDomainService().FindChatById(app, source.id);
	ChatSession* cold_branch = ChatDomainService().FindChatById(app, branch_id);
	UAM_ASSERT(cold_source != nullptr && !cold_source->messages_loaded);
	UAM_ASSERT(cold_branch != nullptr && !cold_branch->messages_loaded);

	const ChatDomainService domain_service;
	const ChatDomainService::BranchHistoryIdentity source_identity = domain_service.CaptureBranchHistoryIdentity(*cold_source);
	const ChatDomainService::BranchHistoryIdentity branch_identity = domain_service.CaptureBranchHistoryIdentity(*cold_branch);
	const std::optional<ChatSession> detached_source = domain_service.LoadBranchHistoryDetached(temp.root, source_identity);
	const std::optional<ChatSession> detached_branch = domain_service.LoadBranchHistoryDetached(temp.root, branch_identity);
	UAM_ASSERT(detached_source.has_value() && detached_source->messages_loaded);
	UAM_ASSERT(detached_branch.has_value() && detached_branch->messages_loaded);

	app.worktree_operation_chat_ids.insert(source.id);
	app.worktree_operation_chat_ids.insert(branch_id);
	ChatDomainService().SelectChatById(app, source.id);
	std::string reused_branch_id;
	std::string error;
	UAM_ASSERT(uam::BranchFromMessageAndRetry(app, source.id, 0, std::nullopt, &reused_branch_id, &error,
	                                         std::string("cold-operation-1"), nullptr, std::nullopt,
	                                         &*detached_source, &*detached_branch));
	UAM_ASSERT_EQ(reused_branch_id, branch_id);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), branch_id);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(!ChatDomainService().FindChatById(app, source.id)->messages_loaded);
	UAM_ASSERT(!ChatDomainService().FindChatById(app, branch_id)->messages_loaded);

	ChatDomainService::BranchHistoryIdentity stale_source_identity = source_identity;
	stale_source_identity.updated_at = "stale-snapshot";
	UAM_ASSERT(!domain_service.LoadBranchHistoryDetached(temp.root, stale_source_identity).has_value());
	const std::string selected_before_collision = ChatDomainService().SelectedChatId(app);
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(app, source.id, 0, std::string("Different prompt"), nullptr, &error,
	                                          std::string("cold-operation-1"), nullptr, std::nullopt,
	                                          &*detached_source, &*detached_branch));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), selected_before_collision);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
}

UAM_TEST(MessageBranchAttachmentSelectionPreservesSourceAndRejectsOperationCollisions)
{
	TempDir temp("uam-message-branch-attachments");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("", "codex-cli");
	source.id = "attachment-source";
	Message message{MessageRole::User, "Original prompt"};
	message.attachments = {MessageAttachment{.id = "first", .name = "first.png", .kind = "image", .path = "first.png"},
	    MessageAttachment{.id = "second", .name = "second.png", .kind = "image", .path = "second.png"}};
	source.messages.push_back(message);
	app.chats.push_back(source);
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::nullopt));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChat(app)->messages.back().attachments, message.attachments);
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string(), std::string("image-only"), nullptr, std::vector<std::string>{"first"}));
	UAM_ASSERT(ChatDomainService().SelectedChat(app)->messages.back().content.empty());
	UAM_ASSERT_EQ(ChatDomainService().SelectedChat(app)->messages.back().attachments.front().id, std::string("first"));
	const std::size_t before_empty = app.chats.size();
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string(), std::string("empty-edit"), nullptr, std::vector<std::string>{}));
	UAM_ASSERT_EQ(app.chats.size(), before_empty);
	const std::vector<std::string> retained{"second"};
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("keep-second"), nullptr, retained));
	const std::string branch_id = ChatDomainService().SelectedChatId(app);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChat(app)->messages.back().attachments.size(), std::size_t(1));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChat(app)->messages.back().attachments.front(), message.attachments.back());
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source.id)->messages.front().attachments, message.attachments);
	// Replays compare the persisted result, including the complete native attachment records.
	ChatDomainService().SelectedChat(app)->messages_loaded = false;
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("keep-second"), nullptr, retained));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), branch_id);
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("keep-second")));
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::string("keep-second"), nullptr, std::vector<std::string>{}));
	const std::size_t count = app.chats.size();
	for (const std::vector<std::string>& invalid : {std::vector<std::string>{""}, std::vector<std::string>{"unknown"}, std::vector<std::string>{"second", "second"}})
	{
		UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited"), std::nullopt, nullptr, invalid));
		UAM_ASSERT_EQ(app.chats.size(), count);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), branch_id);
	}
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Text only"), std::string("remove-all"), nullptr, std::vector<std::string>{}));
	UAM_ASSERT(ChatDomainService().SelectedChat(app)->messages.back().attachments.empty());
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source.id)->messages.front().attachments, message.attachments);
}

UAM_TEST(ChatDomainServiceBranchesWithIndependentPausedGoalSnapshots)
{
	TempDir temp("uam-message-branch-goals");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "codex-cli");
	source.id = "chat-goal-source";
	source.active_goal_id = "goal-active-source";
	source.goals.push_back(Goal{
	    .id = "goal-active-source", .objective = "Finish the source task.",
	    .status = GoalStatus::Active, .completed_items = {"Inspect source"},
	    .remaining_items = {"Implement change"}, .current_step = "Implement change",
	    .last_verification = "Source test passed", .loop_count = 2});
	source.goals.push_back(Goal{
	    .id = "goal-paused-source", .objective = "Keep this paused goal.",
	    .status = GoalStatus::Paused, .remaining_items = {"Review later"}});
	source.messages.push_back(Message{MessageRole::User, "Original prompt"});
	app.chats.push_back(source);
	ChatDomainService().SelectChatById(app, source.id);

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited prompt")));
	const ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch != nullptr);
	UAM_ASSERT(branch->active_goal_id.empty());
	UAM_ASSERT_EQ(branch->goals.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(branch->goal_owner_chat_id.empty());
	UAM_ASSERT(branch->goals[0].id != source.goals[0].id);
	UAM_ASSERT(branch->goals[1].id != source.goals[1].id);
	UAM_ASSERT(branch->goals[0].id != branch->goals[1].id);
	UAM_ASSERT_EQ(branch->goals[0].status, GoalStatus::Paused);
	UAM_ASSERT_EQ(branch->goals[1].status, GoalStatus::Paused);
	UAM_ASSERT_EQ(branch->goals[0].objective, source.goals[0].objective);
	UAM_ASSERT_EQ(branch->goals[0].completed_items, source.goals[0].completed_items);
	UAM_ASSERT_EQ(branch->goals[0].remaining_items, source.goals[0].remaining_items);
	UAM_ASSERT_EQ(branch->goals[0].current_step, source.goals[0].current_step);
	UAM_ASSERT_EQ(branch->goals[0].last_verification, source.goals[0].last_verification);
	UAM_ASSERT_EQ(branch->goals[0].loop_count, source.goals[0].loop_count);

	const ChatSession* unchanged_source = ChatDomainService().FindChatById(app, source.id);
	UAM_ASSERT(unchanged_source != nullptr);
	UAM_ASSERT_EQ(unchanged_source->active_goal_id, std::string("goal-active-source"));
	UAM_ASSERT_EQ(unchanged_source->goals[0].status, GoalStatus::Active);
	UAM_ASSERT_EQ(unchanged_source->goals[0].id, std::string("goal-active-source"));
	UAM_ASSERT_EQ(unchanged_source->goals[1].id, std::string("goal-paused-source"));

	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	const auto persisted_branch = std::ranges::find_if(saved, [&branch](const ChatSession& chat)
	{
		return chat.id == branch->id;
	});
	UAM_ASSERT(persisted_branch != saved.end());
	UAM_ASSERT(persisted_branch->active_goal_id.empty());
	UAM_ASSERT_EQ(persisted_branch->goals.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(persisted_branch->goals[0].status, GoalStatus::Paused);

	ChatSession worker = ChatDomainService().CreateNewChat("folder-1", "codex-cli");
	worker.id = "chat-internal-goal-worker";
	worker.goal_owner_chat_id = source.id;
	worker.goal_iteration_goal_id = source.goals[0].id;
	worker.goals = source.goals;
	worker.messages.push_back(Message{MessageRole::User, "Internal worker prompt"});
	app.chats.push_back(worker);
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, worker.id, 0));
	const ChatSession* worker_branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(worker_branch != nullptr);
	UAM_ASSERT(worker_branch->goals.empty());
	UAM_ASSERT(worker_branch->active_goal_id.empty());
}

UAM_TEST(ChatDomainServiceRejectsMessageBranchDuringActiveTurn)
{
	TempDir temp("uam-message-branch-active");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "codex-cli");
	source.messages.push_back(Message{MessageRole::User, "Original prompt"});
	app.chats.push_back(source);
	auto acp = std::make_unique<uam::AcpSessionState>();
	acp->chat_id = source.id;
	acp->running = true;
	acp->processing = true;
	app.acp_sessions.push_back(std::move(acp));

	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, source.id, 0, std::string("Edited prompt")));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.status_line, std::string("Wait for the active turn to finish before branching."));
}

UAM_TEST(MessageBranchRetryDispatchesRegenerationWithoutDuplicatingPrompt)
{
	TempDir temp("uam-message-branch-retry");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const fs::path workspace = temp.root / "workspace";
	const fs::path markdown_store = temp.root / "markdown-store";
	fs::create_directories(workspace);
	fs::create_directories(markdown_store);
	const fs::path skill = markdown_store / "retry.uam";
	const std::string retry_skill_sentinel = "UAM_RETRY_SKILL_SENTINEL";
	const std::string source_path_sentinel = "C:\\Users\\outside\\retry.md";
	UAM_ASSERT(uam::io::WriteTextFile(skill, "---\ntitle: Retry Skill\nsourcePath: " + source_path_sentinel + "\n---\n# Retry Skill\n\n" + retry_skill_sentinel + "\n"));
	app.settings.markdown_store_directory = markdown_store.string();

	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "gemini-cli");
	source.id = "chat-source";
	source.model_id = "gemini-2.5-pro";
	// This fixture represents an already-running source session in its existing workspace.
	source.workspace_isolation_kind.clear();
	source.small_model_mode = true;
	source.workspace_directory = workspace.string();
	app.chats.push_back(std::move(source));

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	auto source_session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState* raw_source_session = source_session.get();
	raw_source_session->chat_id = "chat-source";
	raw_source_session->provider_id = "gemini-cli";
	raw_source_session->protocol_kind = "gemini-acp";
	raw_source_session->running = true;
	raw_source_session->initialized = true;
	raw_source_session->session_ready = true;
	raw_source_session->session_id = "source-session";
	raw_source_session->current_model_id = "gemini-2.5-pro";
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_source_session, workspace, sink_argv, &launch_error));
	app.acp_sessions.push_back(std::move(source_session));

	const std::vector<MessageAttachment> prompt_attachments = {
	    MessageAttachment{"file-1", "notes.txt", "file", "text/plain", "notes.txt"}};
	std::string send_error;
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-source", "Retry this prompt", {skill.string()}, prompt_attachments, false, &send_error));
	UAM_ASSERT(uam::StopAcpSession(app, "chat-source"));
	app.acp_sessions.clear();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats.front()));
	app.chats = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().markdown_store_prompt_blocks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages.front().markdown_store_prompt_blocks.front().find(retry_skill_sentinel) != std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().goals.size(), static_cast<std::size_t>(1));
	const std::string source_goal_id = app.chats.front().goals.front().id;

	const std::string changed_skill_sentinel = "UAM_CHANGED_RETRY_SKILL";
	UAM_ASSERT(uam::io::WriteTextFile(skill, "---\ntitle: Retry Skill\n---\n# Retry Skill\n\n" + changed_skill_sentinel + "\n"));

	app.chats = ChatRepository::LoadLocalChatSummaries(temp.root);
	const ChatDomainService domain;
	const ChatDomainService::BranchHistoryIdentity identity = domain.CaptureBranchHistoryIdentity(app.chats.front());
	const std::shared_ptr<uam::BranchPreparationOwnership> ownership = domain.BeginBranchPreparation(app, "chat-source", "");
	const std::optional<ChatSession> prepared_source = domain.LoadBranchHistoryDetached(temp.root, identity);
	UAM_ASSERT(prepared_source.has_value());
	UAM_ASSERT(domain.ConsumeBranchPreparation(app, "chat-source", ownership));
	UAM_ASSERT(domain.MatchesPreparedBranchHistory(app.chats.front(), identity, false, &*prepared_source));
	UAM_ASSERT(domain.CreateBranchFromMessage(app, "chat-source", 0, std::nullopt, std::string("retry-delivery"), nullptr, std::nullopt, &*prepared_source));
	UAM_ASSERT(!domain.FindChatById(app, "chat-source")->messages_loaded);
	ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch != nullptr);
	const std::string branch_id = branch->id;
	UAM_ASSERT_EQ(branch->model_id, std::string("gemini-2.5-pro"));

	auto session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState* raw_session = session.get();
	raw_session->chat_id = branch_id;
	raw_session->provider_id = "gemini-cli";
	raw_session->protocol_kind = "gemini-acp";
	raw_session->running = true;
	raw_session->initialized = true;
	raw_session->session_ready = true;
	raw_session->session_id = "retry-session";
	launch_error.clear();
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, workspace, sink_argv, &launch_error));
	app.acp_sessions.push_back(std::move(session));

	std::string retry_error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(app, branch_id, &retry_error));
	UAM_ASSERT(retry_error.empty());
	UAM_ASSERT_EQ(branch->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(branch->goals.size(), static_cast<std::size_t>(2));
	const auto paused_snapshot = std::ranges::find(branch->goals, GoalStatus::Paused, &Goal::status);
	UAM_ASSERT(paused_snapshot != branch->goals.end());
	UAM_ASSERT(paused_snapshot->id != source_goal_id);
	UAM_ASSERT(!branch->active_goal_id.empty());
	UAM_ASSERT(branch->active_goal_id != paused_snapshot->id);
	const Goal* branch_goal = uam::GoalService::FindActiveGoal(app, branch_id);
	UAM_ASSERT(branch_goal != nullptr);
	UAM_ASSERT_EQ(branch_goal->objective, std::string("Retry this prompt"));
	UAM_ASSERT_EQ(raw_session->turn_user_message_index, 0);
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(raw_session->queued_prompt.find("Continue working toward the active thread goal.") != std::string::npos);
	const std::size_t objective_at = raw_session->queued_prompt.find("Retry this prompt");
	UAM_ASSERT(objective_at != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Retry this prompt", objective_at + std::string("Retry this prompt").size()) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(retry_skill_sentinel) != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(changed_skill_sentinel) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(skill))) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(source_path_sentinel) == std::string::npos);
	// Generic ACP starts controls only after the real admitted-history save settles.
	const auto model_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (raw_session->model_change_request_id == 0 && raw_session->processing &&
	       std::chrono::steady_clock::now() < model_deadline)
	{
		UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
		(void)uam::FlushPendingChatSaves(app);
		(void)uam::acp_detail::SendQueuedPromptIfReady(app, *raw_session, *branch);
		if (raw_session->mode_change_request_id != 0)
		{
			UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(branch_id));
			UAM_ASSERT_EQ(raw_session->model_change_request_id, 0);
			UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
			const int mode_change_request_id = raw_session->mode_change_request_id;
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, *branch,
			    nlohmann::json({{"jsonrpc", "2.0"}, {"id", mode_change_request_id}, {"result", nlohmann::json::object()}}).dump()));
		}
		UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
		std::this_thread::yield();
	}
	UAM_ASSERT(raw_session->model_change_request_id != 0);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	const int model_change_request_id = raw_session->model_change_request_id;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, *branch, nlohmann::json({{"jsonrpc", "2.0"}, {"id", model_change_request_id}, {"result", nlohmann::json::object()}}).dump()));
	AwaitDurablePromptDispatch(app, *raw_session, *branch);
	UAM_ASSERT(raw_session->prompt_request_id != 0);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("sent"));

	const int delivered_request_id = raw_session->prompt_request_id;
	const std::size_t diagnostics_count = raw_session->diagnostics.size();
	UAM_ASSERT(!domain.ConsumeBranchPreparation(app, "chat-source", ownership));
	std::string replay_id;
	UAM_ASSERT(uam::BranchFromMessageAndRetry(app, "chat-source", 0, std::nullopt, &replay_id, &retry_error,
	    std::string("retry-delivery"), nullptr, std::nullopt, &*prepared_source));
	UAM_ASSERT_EQ(replay_id, branch_id);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, delivered_request_id);
	UAM_ASSERT_EQ(raw_session->diagnostics.size(), diagnostics_count);
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpQueueRejectsMergedSkillSnapshotsAbovePromptLimit)
{
	TempDir temp("uam-acp-queued-skill-limit");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const fs::path markdown_store = temp.root / "markdown-store";
	fs::create_directories(markdown_store);
	const fs::path first_skill = markdown_store / "first.uam";
	const fs::path second_skill = markdown_store / "second.uam";
	UAM_ASSERT(uam::io::WriteTextFile(first_skill, "---\ntitle: First\n---\n" + std::string(1100000, 'a')));
	UAM_ASSERT(uam::io::WriteTextFile(second_skill, "---\ntitle: Second\n---\n" + std::string(1100000, 'b')));
	app.settings.markdown_store_directory = markdown_store.string();

	ChatSession chat;
	chat.id = "chat-queued-skill-limit";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	app.chats.push_back(std::move(chat));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->processing = true;
	session->lifecycle_state = "processing";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "first", {first_skill.string()}, {}, false, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(!uam::SendAcpPrompt(app, app.chats.front().id, "second", {second_skill.string()}, {}, false, &error));
	UAM_ASSERT_EQ(error, std::string("Queued attached skill content exceeds the 2 MiB prompt limit."));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("first"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("processing"));

	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, app.chats.front().id, 0, &error));
	error.clear();
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "second", {second_skill.string()}, {}, false, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("second"));
}

UAM_TEST(OpenCodeMessageBranchRetryCarriesConversationContextToFreshSession)
{
	TempDir temp("uam-opencode-message-branch-context");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "opencode-cli");
	source.id = "chat-source";
	// Seed a historical source chat, rather than a new chat awaiting its first launch.
	source.workspace_isolation_kind.clear();
	source.workspace_directory = temp.root.string();
	Message earlier_user{MessageRole::User, ""};
	earlier_user.markdown_store_files = {"skills/original.uam"};
	earlier_user.markdown_store_prompt_blocks = {"<skill>UAM_ORIGINAL_SKILL_SNAPSHOT</skill>"};
	earlier_user.attachments.push_back(MessageAttachment{
	    "attachment-1", "diagram.png", "image", "image/png", ".UAM/attachments/chat-source/diagram.png"});
	source.messages.push_back(std::move(earlier_user));
	source.messages.push_back(Message{MessageRole::Assistant, "Earlier assistant context"});
	source.messages.push_back(Message{MessageRole::User, "Retry this prompt"});
	source.messages.push_back(Message{MessageRole::Assistant, "Do not leak this later response"});
	app.chats.push_back(std::move(source));

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, "chat-source", 2));
	ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch != nullptr);
	const fs::path changed_skill = temp.root / "skills" / "original.uam";
	fs::create_directories(changed_skill.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(changed_skill, "UAM_CHANGED_SKILL_ON_DISK"));

	auto session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState* raw_session = session.get();
	raw_session->chat_id = branch->id;
	raw_session->provider_id = "opencode-cli";
	raw_session->protocol_kind = "opencode-acp";
	raw_session->running = true;
	raw_session->initialized = true;
	const uam::AgentDefinitionCatalog agents = uam::AgentDefinitionService::Load(app.data_root, temp.root);
	const auto build_agent = std::ranges::find(agents.definitions, std::string("build"), &uam::AgentDefinition::id);
	UAM_ASSERT(build_agent != agents.definitions.end());
	raw_session->active_uam_agent_id = build_agent->id;
	raw_session->active_uam_agent_definition_hash = build_agent->definition_hash;
	raw_session->active_uam_agent_execution_capability = "opencode-native-agent-config";
	app.acp_sessions.push_back(std::move(session));

	std::string retry_error;
	UAM_ASSERT(uam::RetryLastAcpPrompt(app, branch->id, &retry_error));
	UAM_ASSERT(retry_error.empty());
	UAM_ASSERT(raw_session->queued_prompt.find("UAM_ORIGINAL_SKILL_SNAPSHOT") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(".UAM/attachments/chat-source/diagram.png") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("UAM_CHANGED_SKILL_ON_DISK") == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Earlier assistant context") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Retry this prompt") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("Do not leak this later response") == std::string::npos);
	UAM_ASSERT_EQ(branch->messages.size(), static_cast<std::size_t>(3));
}

UAM_TEST(MessageBranchRetryFailureRollsBackLegacyAndPreservesIdempotentBranch)
{
	TempDir temp("uam-message-branch-retry-rollback");
	const fs::path invalid_workspace = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(invalid_workspace, "file"));

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder-1", "gemini-cli");
	source.id = "chat-source";
	source.created_at = "2020-01-01T00:00:00Z";
	source.updated_at = source.created_at;
	source.branch_root_chat_id = source.id;
	source.workspace_directory = invalid_workspace.string();
	source.messages.push_back(Message{MessageRole::User, "Retry this prompt"});
	source.active_goal_id = "goal-rollback-source";
	source.goals.push_back(Goal{.id = source.active_goal_id, .objective = "Preserve this source goal."});
	app.chats.push_back(std::move(source));
	ChatDomainService().SelectChatById(app, "chat-source");
	UAM_ASSERT(PersistenceCoordinator().SaveSettings(app));

	std::string branch_id;
	std::string error;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(app, "chat-source", 0, std::nullopt, &branch_id, &error));
	UAM_ASSERT(!branch_id.empty());
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string("chat-source"));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, std::string("goal-rollback-source"));
	UAM_ASSERT_EQ(app.chats.front().goals.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT(app.acp_sessions.empty());
	UAM_ASSERT(app.cli_terminals.empty());

	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT(std::ranges::none_of(saved, [](const ChatSession& chat) { return chat.parent_chat_id == "chat-source"; }));

	uam::AppState reloaded;
	reloaded.data_root = temp.root;
	PersistenceCoordinator().LoadSettings(reloaded);
	UAM_ASSERT_EQ(reloaded.settings.last_selected_chat_id, std::string("chat-source"));

	std::string warning;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(app, "chat-source", 0, std::nullopt, &branch_id,
		&error, std::string("branch-retry-1"), &warning));
	UAM_ASSERT(!warning.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	const std::string preserved_branch_id = branch_id;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(app, "chat-source", 0, std::nullopt, &branch_id,
		&error, std::string("branch-retry-1"), &warning));
	UAM_ASSERT_EQ(branch_id, preserved_branch_id);
	UAM_ASSERT(!warning.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	const std::vector<ChatSession> saved_after_failure = ChatRepository::LoadLocalChats(temp.root);
	const auto preserved = std::ranges::find_if(saved_after_failure, [&preserved_branch_id](const ChatSession& chat)
	{
		return chat.id == preserved_branch_id;
	});
	UAM_ASSERT(preserved != saved_after_failure.end());
	UAM_ASSERT(!preserved->branch_retry_error.empty());
}

UAM_TEST(WorktreeRetentionInspectionPreservesReferencesDirtyFilesAndBranches)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-worktree-retention");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "baseline\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / ".gitignore", "ignored.log\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt .gitignore"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-retention";
	chat.title = "Retention fixture";
	chat.workspace_directory = repo.string();
	const uam::GitWorktreeService service;
	UAM_ASSERT(service.CreateForChat(app, chat).ok);
	const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
	uam::GitWorktreeRetentionCandidate candidate;
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	// Age metadata only inside this temporary repository; source content is never removed by inspection.
	const fs::file_time_type old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 15);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(repo / ".git")) fs::last_write_time(entry.path(), old);
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(worktree)) fs::last_write_time(entry.path(), old);
	fs::last_write_time(worktree, old);
	UAM_ASSERT(service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	UAM_ASSERT(RunGitForTest(repo, "pack-refs --all"));
	UAM_ASSERT(service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	UAM_ASSERT_EQ(candidate.branch_name, chat.workspace_branch_name);
	UAM_ASSERT(!candidate.head_sha.empty());
	const fs::path admin = candidate.administrative_directory;
	const uam::GitWorktreeRetentionCandidate eligible = candidate;
	UAM_ASSERT(uam::io::WriteTextFile(admin / "locked", "keep locked worktree"));
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	fs::remove(admin / "locked");
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, temp.root, worktree, {}, candidate));
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, repo, {}, candidate));
	UAM_ASSERT(service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
#if !defined(_WIN32)
	const fs::path alias = temp.root / "referenced-alias";
	fs::create_directory_symlink(worktree, alias);
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {alias}, candidate));
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, alias, {}, candidate));
#endif
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {worktree / "subfolder"}, candidate));
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {worktree.parent_path()}, candidate));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "ignored.log", "keep ignored output"));
	fs::last_write_time(worktree, old);
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	fs::remove(worktree / "ignored.log");
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "app.txt", "keep changed content\n"));
	fs::last_write_time(worktree, old);
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(worktree / "app.txt"), "keep changed content\n");
	UAM_ASSERT(RunGitForTest(repo, "show-ref --verify " + ShellQuoteForTest("refs/heads/" + chat.workspace_branch_name)));
	std::stop_source canceled;
	canceled.request_stop();
	UAM_ASSERT(!service.InspectRetentionCandidate(app.data_root, repo, worktree, {}, candidate, canceled.get_token()));
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> mutation = app.worktree_retention_gate.TryAcquireMutation();
	UAM_ASSERT(!service.RemoveRetentionCandidate(app.data_root, repo, eligible, {}, *mutation));
	mutation.reset();
	std::shared_ptr<uam::WorkspaceLeaseGate::Lease> cleanup = app.worktree_retention_gate.TryAcquireCleanup();
	UAM_ASSERT(cleanup);
	UAM_ASSERT(!service.RemoveRetentionCandidate(app.data_root, repo, eligible, {}, *cleanup));
	UAM_ASSERT(RunGitForTest(worktree, "restore app.txt"));
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(repo / ".git")) fs::last_write_time(entry.path(), old);
	fs::last_write_time(worktree, old);
	UAM_ASSERT(!service.RemoveRetentionCandidate(app.data_root, repo, eligible, {worktree}, *cleanup));
	UAM_ASSERT(!service.RemoveRetentionCandidate(app.data_root, repo, eligible, {}, *cleanup, canceled.get_token()));
	UAM_ASSERT(service.RemoveRetentionCandidate(app.data_root, repo, eligible, {}, *cleanup));
	UAM_ASSERT(!fs::exists(worktree));
	UAM_ASSERT(RunGitForTest(repo, "show-ref --verify " + ShellQuoteForTest("refs/heads/" + chat.workspace_branch_name)));
}

UAM_TEST(MessageBranchFromGitWorktreeChecksOutHistoricalAssistantCheckpoint)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-message-branch-git-history");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config core.autocrlf false"));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "baseline\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = repo.string();
	app.folders.push_back(folder);
	ChatSession source = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	source.id = "chat-history-source";
	source.workspace_directory = repo.string();
	source.messages = {
	    Message{MessageRole::User, "Make the first change."},
	    Message{MessageRole::Assistant, "First change complete."},
	    Message{MessageRole::User, "Continue from the first change."},
	};
	app.chats.push_back(std::move(source));

	uam::GitWorktreeService worktrees;
	UAM_ASSERT(worktrees.CreateForChat(app, app.chats.front()).ok);
	ChatSession& isolated = app.chats.front();
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(isolated));
	const fs::path source_worktree = isolated.workspace_worktree_directory;
	UAM_ASSERT(uam::io::WriteTextFile(source_worktree / "app.txt", "first change\n"));
	const uam::GitTurnCheckpointResult checkpoint = worktrees.CreateTurnCheckpoint(app, isolated, 1);
	UAM_ASSERT(checkpoint.ok && checkpoint.changed);
	UAM_ASSERT_EQ(isolated.messages[1].checkpoint_sha, checkpoint.checkpoint_sha);
	isolated.messages.push_back(Message{MessageRole::Assistant, "Later change complete."});
	UAM_ASSERT(uam::io::WriteTextFile(source_worktree / "app.txt", "later change\n"));
	const uam::GitTurnCheckpointResult later_checkpoint = worktrees.CreateTurnCheckpoint(app, isolated, 3);
	UAM_ASSERT(later_checkpoint.ok && later_checkpoint.changed);
	UAM_ASSERT_EQ(isolated.messages[3].checkpoint_sha, later_checkpoint.checkpoint_sha);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, isolated));
	const std::string source_head = uam::strings::Trim(
	    PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	        "git -C " + ShellQuoteForTest(repo.string()) + " rev-parse HEAD", 120000).output);

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, isolated.id, 2));
	const ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch != nullptr);
	UAM_ASSERT(branch->workspace_isolation_kind == uam::paths::kGitWorktreeIsolationKind);
	UAM_ASSERT(uam::paths::HasGitWorktreeSource(*branch));
	UAM_ASSERT(uam::paths::HasGitWorktreeDirectory(*branch));
	const fs::path branch_worktree = branch->workspace_worktree_directory;
	UAM_ASSERT(branch_worktree != source_worktree);
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, *branch), uam::paths::AbsolutePathNoThrow(branch_worktree));
	UAM_ASSERT_EQ(ReadFile(branch_worktree / "app.txt"), std::string("first change\n"));
	UAM_ASSERT(ReadFile(branch_worktree / "app.txt").find("later change") == std::string::npos);
	const ProcessExecutionResult branch_head = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	    "git -C " + ShellQuoteForTest(branch_worktree.string()) + " rev-parse HEAD", 120000);
	UAM_ASSERT(branch_head.ok);
	UAM_ASSERT_EQ(uam::strings::Trim(branch_head.output), checkpoint.checkpoint_sha);
	UAM_ASSERT_EQ(ReadFile(repo / "app.txt"), std::string("baseline\n"));
	const ProcessExecutionResult unchanged_source_head = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	    "git -C " + ShellQuoteForTest(repo.string()) + " rev-parse HEAD", 120000);
	UAM_ASSERT(unchanged_source_head.ok);
	UAM_ASSERT_EQ(uam::strings::Trim(unchanged_source_head.output), source_head);
}

UAM_TEST(MessageBranchFromManagedWorkspaceCreatesIndependentManagedRepository)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-message-branch-managed");
	const fs::path source_root = temp.root / "workspace";
	fs::create_directories(source_root / ".UAM");
	UAM_ASSERT(uam::io::WriteTextFile(source_root / "app.txt", "source\n"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = source_root.string();
	app.folders.push_back(folder);
	ChatSession source = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	source.id = "chat-managed-source";
	source.workspace_directory = source_root.string();
	source.messages.push_back(Message{MessageRole::User, "Branch this managed workspace."});
	app.chats.push_back(std::move(source));

	uam::GitWorktreeService worktrees;
	UAM_ASSERT(worktrees.CreateForChat(app, app.chats.front()).ok);
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(app.chats.front()));
	const fs::path source_worktree = app.chats.front().workspace_worktree_directory;
	UAM_ASSERT(uam::io::WriteTextFile(source_worktree / "app.txt", "source branch baseline\n"));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, app.chats.front().id, 0));
	const ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch != nullptr);
	UAM_ASSERT(branch->workspace_isolation_kind == uam::paths::kGitWorktreeIsolationKind);
	UAM_ASSERT(uam::paths::HasGitWorktreeDirectory(*branch));
	const fs::path branch_worktree = branch->workspace_worktree_directory;
	UAM_ASSERT(branch_worktree != source_worktree);
	UAM_ASSERT_EQ(ReadFile(branch_worktree / "app.txt"), std::string("source\n"));
	UAM_ASSERT_EQ(ReadFile(source_worktree / "app.txt"), std::string("source branch baseline\n"));
	const fs::path branch_repository = branch_worktree.parent_path().parent_path() /
	    ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(branch->id)));
	UAM_ASSERT(fs::is_directory(branch_repository));
	UAM_ASSERT_EQ(ReadFile(source_root / "app.txt"), std::string("source\n"));
}

UAM_TEST(MessageBranchRejectsMalformedOrMissingGitCheckpoint)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-message-branch-invalid-checkpoint");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "baseline\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder", "codex-cli");
	source.id = "chat-invalid-checkpoint";
	source.workspace_directory = repo.string();
	source.messages = {
	    Message{MessageRole::User, "First"},
	    Message{MessageRole::Assistant, "Done"},
	    Message{MessageRole::User, "Continue"},
	};
	source.messages[1].checkpoint_sha = "not-a-commit";
	app.chats.push_back(std::move(source));
	uam::GitWorktreeService worktrees;
	UAM_ASSERT(worktrees.CreateForChat(app, app.chats.front()).ok);
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(app.chats.front()));
	app.chats.front().workspace_base_ref.clear();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(app, "chat-invalid-checkpoint", 2));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.status_line.find("checkpoint") != std::string::npos);
}

UAM_TEST(MessageBranchRetryFailureCleansUpNewIsolatedWorktree)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-message-branch-retry-worktree-cleanup");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "baseline\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession source = ChatDomainService().CreateNewChat("folder", "gemini-cli");
	source.id = "chat-retry-worktree-source";
	source.workspace_directory = repo.string();
	source.uam_agent_id = "missing-agent-for-retry";
	source.messages.push_back(Message{MessageRole::User, "Retry this request."});
	app.chats.push_back(std::move(source));
	uam::GitWorktreeService worktrees;
	UAM_ASSERT(worktrees.CreateForChat(app, app.chats.front()).ok);
	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(app.chats.front()));
	const fs::path source_worktree = app.chats.front().workspace_worktree_directory;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	std::string branch_id;
	std::string error;
	UAM_ASSERT(!uam::BranchFromMessageAndRetry(app, "chat-retry-worktree-source", 0, std::nullopt, &branch_id, &error));
	UAM_ASSERT(!branch_id.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(fs::exists(source_worktree));
	const fs::path worktree_parent = source_worktree.parent_path().parent_path();
	UAM_ASSERT(!fs::exists(worktree_parent / branch_id));
	UAM_ASSERT(!fs::exists(worktree_parent / ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(branch_id)))));
	std::size_t remaining_worktrees = 0;
	for (const fs::directory_entry& entry : fs::recursive_directory_iterator(worktree_parent))
	{
		if (entry.path().filename() == ".git" && entry.is_regular_file()) ++remaining_worktrees;
	}
	UAM_ASSERT_EQ(remaining_worktrees, static_cast<std::size_t>(1));
}
