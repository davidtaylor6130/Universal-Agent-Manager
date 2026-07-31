#include "test_harness.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

UAM_TEST(CopilotAuthenticationFailureExplainsHowToRecover)
{
	uam::AcpSessionState session;
	session.provider_id = uam::provider_ids::kCopilotCli;
	uam::acp_detail::AcpFailureDetails failure;
	failure.method = "session/new";
	failure.has_code = true;
	failure.code = -32000;
	failure.message = "Authentication required";

	const std::string message = uam::acp_detail::FormatAcpFailureMessage(session, failure);

	UAM_ASSERT(message.find("copilot login") != std::string::npos);
	UAM_ASSERT(message.find("then retry") != std::string::npos);
}

UAM_TEST(AcpWorkingDirectoryUsesUtf8)
{
	const std::filesystem::path workspace = uam::paths::PathFromUtf8("C:/Users/Jos\xc3\xa9/workspace");
	UAM_ASSERT_EQ(uam::acp_detail::AcpWorkingDirectoryString(workspace), uam::paths::Utf8PathString(workspace));
}

UAM_TEST(CopilotAcpLaunchBlocksPendingAndUnsupportedVersions)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-copilot-version-gate";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	uam::AcpSessionState session;
	std::string error;

	app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli] = {};
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, chat, &error));
	UAM_ASSERT(error.find("Checking") != std::string::npos);
	UAM_ASSERT(!session.running);

	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	version.checked = true;
	version.installed_version = "1.0.68";
	error.clear();
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, chat, &error));
	UAM_ASSERT(error.find("1.0.69 or newer") != std::string::npos);
	UAM_ASSERT(!session.running);
#endif
}

UAM_TEST(CopilotAcpRetriesPendingCompatibilityAndPreservesUndeliveredPrompt)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	TempDir temp("uam-acp-restart-prompt");
#if defined(_WIN32)
	const fs::path shim = temp.root / "copilot.cmd";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "@echo off\r\nmore > NUL\r\n"));
	const char path_separator = ';';
#else
	const fs::path shim = temp.root / "copilot";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\ncat >/dev/null\n"));
	std::error_code permissions_error;
	fs::permissions(shim, fs::perms::owner_all, fs::perm_options::replace, permissions_error);
	UAM_ASSERT(!permissions_error);
	const char path_separator = ':';
#endif
	const char* existing_path = std::getenv("PATH");
	const std::string combined_path = temp.root.string() + (existing_path == nullptr ? "" : (std::string(1, path_separator) + existing_path));
	ScopedEnvVar scoped_path("PATH", combined_path);

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession pending_chat;
	pending_chat.id = "chat-pending-copilot-check";
	pending_chat.provider_id = uam::provider_ids::kCopilotCli;
	pending_chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
	app.chats.push_back(pending_chat);
	app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli] = {};

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = pending_chat.id;
	terminal->attached_chat_id = pending_chat.id;
	terminal->running = true;
	terminal->should_launch = true;
	terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(terminal));

	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(app, pending_chat.id, "Reject while terminal fallback is busy.", {}, {}, false, &error));
	UAM_ASSERT(error.find("terminal fallback is busy") != std::string::npos);
	uam::AcpSessionState* pending_session = uam::FindAcpSessionForChat(app, pending_chat.id);
	UAM_ASSERT(pending_session != nullptr);
	UAM_ASSERT(pending_session->queued_user_prompts.empty());
	UAM_ASSERT(!pending_session->reconnect_pending);

	app.cli_terminals.front()->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	app.cli_terminals.front()->turn_state = uam::CliTerminalTurnState::Idle;
	error.clear();
	UAM_ASSERT(uam::SendAcpPrompt(app, pending_chat.id, "Cancel before the check.", {}, {}, false, &error));
	UAM_ASSERT(!app.cli_terminals.front()->running);
	UAM_ASSERT(uam::CancelAcpTurn(app, pending_chat.id, &error));
	UAM_ASSERT(pending_session->queued_user_prompts.empty());
	UAM_ASSERT(!pending_session->reconnect_pending);

	UAM_ASSERT(uam::SendAcpPrompt(app, pending_chat.id, "Remove before the check.", {}, {}, false, &error));
	UAM_ASSERT(uam::SteerQueuedAcpPrompt(app, pending_chat.id, 0, &error));
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.front().text, std::string("Remove before the check."));
	UAM_ASSERT(pending_session->queued_user_prompts.front().priority_steer);
	UAM_ASSERT(pending_session->reconnect_pending);
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, pending_chat.id, 0, &error));
	UAM_ASSERT(pending_session->queued_user_prompts.empty());
	UAM_ASSERT(!pending_session->reconnect_pending);

	UAM_ASSERT(uam::SendAcpPrompt(app, pending_chat.id, "Queue before steering.", {}, {}, false, &error));
	UAM_ASSERT(uam::SteerAcpPrompt(app, pending_chat.id, "Steer before the check.", {}, {}, false, &error));
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.front().text, std::string("Steer before the check."));
	UAM_ASSERT(pending_session->reconnect_pending);
	UAM_ASSERT(uam::CancelAcpTurn(app, pending_chat.id, &error));

	UAM_ASSERT(uam::SendAcpPrompt(app, pending_chat.id, "Send after the check.", {}, {}, false, &error));
	UAM_ASSERT(!pending_session->running);
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages.empty());

	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli];
	version.checked = true;
	version.supported = true;
	version.installed_version = "1.0.69";
	app.cli_terminals.front()->running = true;
	app.cli_terminals.front()->should_launch = true;
	app.cli_terminals.front()->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	app.cli_terminals.front()->turn_state = uam::CliTerminalTurnState::Busy;
	UAM_ASSERT(uam::SteerQueuedAcpPrompt(app, pending_chat.id, 0, &error));
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.front().text, std::string("Send after the check."));
	UAM_ASSERT(pending_session->reconnect_pending);
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		pending_session->reconnect_not_before_time_s = 0.0;
		UAM_ASSERT(uam::PollAllAcpSessions(app));
		UAM_ASSERT(!pending_session->running);
		UAM_ASSERT(pending_session->reconnect_pending);
		UAM_ASSERT_EQ(pending_session->reconnect_attempts, 0);
		UAM_ASSERT_EQ(pending_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	}
	app.cli_terminals.front()->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	app.cli_terminals.front()->turn_state = uam::CliTerminalTurnState::Idle;
	pending_session->reconnect_not_before_time_s = 0.0;
	UAM_ASSERT(uam::PollAllAcpSessions(app));
	UAM_ASSERT(pending_session->running);
	UAM_ASSERT(!app.cli_terminals.front()->running);
	UAM_ASSERT(!pending_session->reconnect_pending);
	UAM_ASSERT_EQ(pending_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!pending_session->processing);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *pending_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"agentInfo":{"name":"copilot","title":"GitHub Copilot","version":"1.0.69"},"agentCapabilities":{"loadSession":true}}})"));
	UAM_ASSERT(uam::PollAllAcpSessions(app));
	const int setup_request_id = pending_session->session_setup_request_id;
	UAM_ASSERT(setup_request_id != 0);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *pending_session, app.chats.front(), nlohmann::json({{"jsonrpc", "2.0"}, {"id", setup_request_id}, {"result", {{"sessionId", "copilot-session-after-check"}}}}).dump()));
	UAM_ASSERT(pending_session->processing);
	UAM_ASSERT(pending_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("Send after the check."));
	UAM_ASSERT(uam::StopAcpSession(app, pending_chat.id));
	app.acp_sessions.clear();
	app.chats.clear();

	ChatSession chat;
	chat.id = "chat-restart-prompt";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.workspace_directory = uam::paths::Utf8PathString(temp.root);

	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.processing = true;
	session.queued_prompt = "Preserve me";
	session.turn_user_message_index = 3;
	session.turn_serial = 7;

	error.clear();
	UAM_ASSERT(uam::acp_detail::StartAcpProcessForChat(app, session, chat, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(session.running);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.queued_prompt, std::string("Preserve me"));
	UAM_ASSERT_EQ(session.turn_user_message_index, 3);
	UAM_ASSERT_EQ(session.turn_serial, 7);

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
#endif
}

UAM_TEST(AcpTurnTimelinePreservesStreamOrder)
{
	TempDir temp("uam-acp-turn-events");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please inspect this.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	session->turn_serial = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	app.chats_with_unseen_updates.insert("chat-1");

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Before "}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Need to inspect the file first."}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"in_progress","content":{"type":"text","text":"Reading"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"completed","content":{"type":"text","text":"Read complete"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":5,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-1","title":"Read file","kind":"read","status":"pending","content":{"type":"text","text":"Read /tmp/file.txt"}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"}]}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"After"}}}})"));

	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(raw_session->turn_events[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(raw_session->turn_events[0].text, std::string("Before "));
	UAM_ASSERT_EQ(raw_session->turn_events[1].type, std::string("thought"));
	UAM_ASSERT_EQ(raw_session->turn_events[1].text, std::string("Need to inspect the file first."));
	UAM_ASSERT_EQ(raw_session->turn_events[2].type, std::string("tool_call"));
	UAM_ASSERT_EQ(raw_session->turn_events[2].tool_call_id, std::string("tool-1"));
	UAM_ASSERT_EQ(raw_session->turn_events[3].type, std::string("permission_request"));
	UAM_ASSERT_EQ(raw_session->turn_events[3].request_id_json, std::string("5"));
	UAM_ASSERT_EQ(raw_session->turn_events[4].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(raw_session->turn_events[4].text, std::string("After"));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Before After"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["turnEvents"].size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(acp.value("turnUserMessageIndex", -2), 0);
	UAM_ASSERT_EQ(acp.value("turnAssistantMessageIndex", -2), 1);
	UAM_ASSERT_EQ(acp.value("turnSerial", -2), 4);
	UAM_ASSERT(acp.value("readySinceLastSelect", false));
	UAM_ASSERT_EQ(acp["turnEvents"][1].value("type", ""), std::string("thought"));
	UAM_ASSERT_EQ(acp["turnEvents"][2].value("toolCallId", ""), std::string("tool-1"));
	UAM_ASSERT_EQ(acp["turnEvents"][3].value("requestId", ""), std::string("5"));
}

UAM_TEST(AcpPromptCompletionClearsProcessingByMethodAndPromptId)
{
	TempDir temp("uam-acp-completion");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "gemini-cli";
	session->processing = true;
	session->waiting_for_permission = true;
	session->prompt_request_id = 42;
	session->queued_prompt = "hello";
	session->current_assistant_message_index = 0;
	session->pending_permission.request_id_json = "7";
	session->pending_request_methods[42] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":42,"result":{"stopReason":"end_turn"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
	UAM_ASSERT(app.chats_with_unseen_updates.contains("chat-1"));

	raw_session->processing = true;
	raw_session->waiting_for_permission = true;
	raw_session->prompt_request_id = 99;
	raw_session->queued_prompt = "again";
	raw_session->current_assistant_message_index = 0;
	raw_session->pending_permission.request_id_json = "8";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":99,"result":{"stopReason":"end_turn"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));

	raw_session->processing = true;
	raw_session->waiting_for_permission = true;
	raw_session->prompt_request_id = 100;
	raw_session->queued_prompt = "bad json";
	raw_session->pending_permission.request_id_json = "9";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":)"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string(""));
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Invalid JSON from Gemini ACP") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("invalid_json"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find(R"({"jsonrpc":)") != std::string::npos);
}

UAM_TEST(AcpJsonRpcErrorsIncludeRequestDiagnostics)
{
	TempDir temp("uam-acp-jsonrpc-diagnostics");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "gemini-cli";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->prompt_request_id = 42;
	session->queued_prompt = "hello";
	session->recent_stderr = "Gemini stderr stack trace";
	session->pending_request_methods[42] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":42,"error":{"code":-32603,"message":"Internal error","data":{"cause":"boom","trace":"hidden detail"}}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(raw_session->last_error.find("Gemini ACP session/prompt failed (id=42, code=-32603): Internal error") != std::string::npos);
	UAM_ASSERT(raw_session->last_error.find("See diagnostics/stderr details.") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());

	const uam::AcpDiagnosticEntryState& diagnostic = raw_session->diagnostics.back();
	UAM_ASSERT_EQ(diagnostic.event, std::string("response"));
	UAM_ASSERT_EQ(diagnostic.reason, std::string("jsonrpc_error"));
	UAM_ASSERT_EQ(diagnostic.method, std::string("session/prompt"));
	UAM_ASSERT_EQ(diagnostic.request_id, std::string("42"));
	UAM_ASSERT(diagnostic.has_code);
	UAM_ASSERT_EQ(diagnostic.code, -32603);
	UAM_ASSERT_EQ(diagnostic.message, std::string("Internal error"));
	UAM_ASSERT(diagnostic.detail.find("error.data=") != std::string::npos);
	UAM_ASSERT(diagnostic.detail.find("Gemini stderr stack trace") != std::string::npos);

	raw_session->has_last_exit_code = true;
	raw_session->last_exit_code = 137;
	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp.value("lastExitCode", 0), 137);
	UAM_ASSERT(!acp["diagnostics"].empty());
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("reason", ""), std::string("jsonrpc_error"));
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("method", ""), std::string("session/prompt"));
	UAM_ASSERT_EQ(acp["diagnostics"].back().value("code", 0), -32603);
}

UAM_TEST(CodexAppServerErrorsUseCodexRuntimeName)
{
	TempDir temp("uam-codex-app-server-error-name");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = " CoDeX ";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = " CoDeX ";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 3;
	session->recent_stderr = "Codex app-server stderr";
	session->pending_request_methods[3] = "thread/start";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":3,"error":{"code":-32600,"message":"thread/start.persistFullHistory requires experimentalApi capability"}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Codex app-server thread/start failed (id=3, code=-32600): thread/start.persistFullHistory requires experimentalApi capability") != std::string::npos);
	UAM_ASSERT(raw_session->last_error.find("Gemini") == std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().method, std::string("thread/start"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("Codex app-server stderr") != std::string::npos);
}

UAM_TEST(CodexAppServerErrorNotificationsExposeRealMessage)
{
	TempDir temp("uam-codex-app-server-error-notification");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 4;
	session->recent_stderr = "Codex warning detail";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"error","params":{"error":{"message":"temporary upstream issue","codexErrorInfo":{"type":"server_error"},"additionalDetails":"retry detail"},"willRetry":true,"threadId":"6a6f0f3b-1a0b-4a9c-8a01-111111111111","turnId":"turn-1"}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("processing"));
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT_EQ(raw_session->last_error, std::string(""));
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("codex_turn_error_retrying"));
	UAM_ASSERT_EQ(raw_session->diagnostics.back().message, std::string("temporary upstream issue"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("willRetry=true") != std::string::npos);
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("retry detail") != std::string::npos);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"error","params":{"error":{"message":"fatal app-server failure","codexErrorInfo":{"type":"bad_request"},"additionalDetails":"fatal detail"},"willRetry":false,"threadId":"6a6f0f3b-1a0b-4a9c-8a01-111111111111","turnId":"turn-1"}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(raw_session->last_error.find("Codex app-server turn failed: fatal app-server failure") != std::string::npos);
	UAM_ASSERT(raw_session->last_error.find("See diagnostics/stderr details.") != std::string::npos);
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("codex_turn_error"));
	UAM_ASSERT_EQ(raw_session->diagnostics.back().message, std::string("fatal app-server failure"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("fatal detail") != std::string::npos);
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("Codex warning detail") != std::string::npos);
}

UAM_TEST(CodexAppServerErrorNotificationsTolerateStructuredDetails)
{
	TempDir temp("uam-codex-app-server-structured-error-notification");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"error","params":{"error":{"message":{"text":"structured plan failure"},"codexErrorInfo":null,"additionalDetails":{"reason":"plan payload was structured"}},"willRetry":false,"threadId":null,"turnId":42}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(raw_session->last_error.find(R"({"text":"structured plan failure"})") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("codex_turn_error"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("turnId=42") != std::string::npos);
	UAM_ASSERT(raw_session->diagnostics.back().detail.find(R"("reason":"plan payload was structured")") != std::string::npos);
}

UAM_TEST(CodexFailedTurnCompletionIsFatal)
{
	TempDir temp("uam-codex-failed-turn-completion");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	session->prompt_request_id = 4;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/completed","params":{"threadId":"6a6f0f3b-1a0b-4a9c-8a01-111111111111","turn":{"id":"turn-1","items":[],"status":"failed","error":{"message":"turn failed after retries","additionalDetails":"completion detail","codexErrorInfo":null}}}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(raw_session->last_error.find("Codex app-server turn/completed failed: turn failed after retries") != std::string::npos);
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("codex_turn_completed_error"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("completion detail") != std::string::npos);
}

UAM_TEST(CodexAppServerItemsTolerateNullAndStructuredFields)
{
	TempDir temp("uam-codex-structured-items");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const auto process = [&](const nlohmann::json& message) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), message.dump())); };

	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"item", {{"id", "cmd-null"}, {"type", "commandExecution"}, {"command", "ls"}, {"status", nullptr}, {"aggregatedOutput", nullptr}}}}}});
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].id, std::string("cmd-null"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].status, std::string("pending"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].content, std::string(""));

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "cmd-object"}, {"type", "commandExecution"}, {"command", "node"}, {"status", "completed"}, {"aggregatedOutput", {{"output", "done"}, {"exitCode", 0}}}}}}}});
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(raw_session->tool_calls[1].content.find(R"("output":"done")") != std::string::npos);
	UAM_ASSERT(raw_session->tool_calls[1].content.find(R"("exitCode":0)") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "cmd-array"}, {"type", "commandExecution"}, {"command", "printf"}, {"status", "completed"}, {"aggregatedOutput", nlohmann::json::array({"line1", "line2"})}}}}}});
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->tool_calls[2].content.find("line1") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "msg-null"}, {"type", "agentMessage"}, {"text", nullptr}}}}}});
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(0));

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "msg-object"}, {"type", "agentMessage"}, {"text", {{"text", "hello"}}}}}}}});
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages[0].content.find(R"("text":"hello")") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "plan-object"}, {"type", "plan"}, {"text", {{"summary", "structured plan"}}}}}}}});
	UAM_ASSERT(raw_session->plan_summary.find("structured plan") != std::string::npos);
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages[0].plan_summary.find("structured plan") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"item", {{"id", "user-1"}, {"type", "userMessage"}, {"text", "ignored"}}}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"item", {{"id", "reasoning-1"}, {"type", "reasoning"}, {"text", "ignored"}}}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"item", {{"id", "raw-1"}, {"type", "rawResponseItem"}, {"text", "ignored"}}}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", {{"item", {{"id", "unknown-1"}, {"type", "futureItem"}, {"text", "ignored"}}}}}});
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->last_error, std::string(""));

	process({{"jsonrpc", "2.0"}, {"method", "item/started"}, {"params", "not-an-object"}});
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(raw_session->last_error, std::string(""));
}

UAM_TEST(CodexAppServerReasoningAndPlansPersistToAssistantMessage)
{
	TempDir temp("uam-codex-reasoning-plan");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const auto process = [&](const nlohmann::json& message) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), message.dump())); };

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "reasoning-empty"}, {"type", "reasoning"}, {"content", nlohmann::json::array()}, {"summary", nlohmann::json::array()}}}}}});
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(0));

	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/textDelta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "reasoning-1"}, {"contentIndex", 0}, {"delta", "Inspecting files."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/summaryPartAdded"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "reasoning-1"}, {"summaryIndex", 0}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/summaryTextDelta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "reasoning-1"}, {"summaryIndex", 0}, {"delta", "Need to inspect."}}}});

	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->turn_events[0].type, std::string("thought"));
	std::string thoughts = app.chats.front().messages[0].thoughts;
	UAM_ASSERT(thoughts.find("### Reasoning") != std::string::npos);
	UAM_ASSERT(thoughts.find("Inspecting files.") != std::string::npos);
	UAM_ASSERT(thoughts.find("### Summary") != std::string::npos);
	UAM_ASSERT(thoughts.find("Need to inspect.") != std::string::npos);
	UAM_ASSERT(thoughts.find("[]") == std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "reasoning-1"}, {"type", "reasoning"}, {"content", nlohmann::json::array({"Duplicate raw"})}, {"summary", nlohmann::json::array({"Duplicate summary"})}}}}}});
	thoughts = app.chats.front().messages[0].thoughts;
	UAM_ASSERT_EQ(CountSubstring(thoughts, "Inspecting files."), static_cast<std::size_t>(1));
	UAM_ASSERT(thoughts.find("Duplicate raw") == std::string::npos);
	UAM_ASSERT(thoughts.find("Duplicate summary") == std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/textDelta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "reasoning-3"}, {"contentIndex", 0}, {"delta", "Streaming raw."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "reasoning-3"}, {"type", "reasoning"}, {"content", nlohmann::json::array({"Streaming raw."})}, {"summary", nlohmann::json::array({"Late completed summary"})}}}}}});
	thoughts = app.chats.front().messages[0].thoughts;
	UAM_ASSERT_EQ(CountSubstring(thoughts, "Streaming raw."), static_cast<std::size_t>(1));
	UAM_ASSERT(thoughts.find("Late completed summary") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "reasoning-2"}, {"type", "reasoning"}, {"content", nlohmann::json::array({"Loaded raw reasoning"})}, {"summary", nlohmann::json::array({"Loaded summary"})}}}}}});
	thoughts = app.chats.front().messages[0].thoughts;
	UAM_ASSERT(thoughts.find("Loaded raw reasoning") != std::string::npos);
	UAM_ASSERT(thoughts.find("Loaded summary") != std::string::npos);

	process({{"jsonrpc", "2.0"}, {"method", "turn/plan/updated"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"explanation", "Plan summary"}, {"plan", nlohmann::json::array({{{"step", "Inspect files"}, {"status", "completed"}}, {{"step", "Patch code"}, {"status", "pending"}}})}}}});
	UAM_ASSERT_EQ(raw_session->plan_summary, std::string("Plan summary"));
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->plan_entries[0].status, std::string("completed"));
	UAM_ASSERT_EQ(raw_session->plan_entries[1].content, std::string("Patch code"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_summary, std::string("Plan summary"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_entries.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_entries[1].status, std::string("pending"));

	const bool has_plan_event = std::ranges::any_of(raw_session->turn_events, [](const uam::AcpTurnEventState& event) { return event.type == "plan"; });
	UAM_ASSERT(has_plan_event);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("planSummary", ""), std::string("Plan summary"));
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"]["planEntries"][0].value("content", ""), std::string("Inspect files"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0].value("planSummary", ""), std::string("Plan summary"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["planEntries"][1].value("content", ""), std::string("Patch code"));
}

UAM_TEST(CodexAppServerAgentMessagesDeduplicateAndSeparateItems)
{
	TempDir temp("uam-codex-agent-message-items");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const auto process = [&](const nlohmann::json& message) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), message.dump())); };

	process({{"jsonrpc", "2.0"}, {"method", "item/agentMessage/delta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "msg-1"}, {"delta", "First update."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "msg-1"}, {"type", "agentMessage"}, {"text", "First update."}}}}}});

	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("First update."));

	process({{"jsonrpc", "2.0"}, {"method", "item/agentMessage/delta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "msg-2"}, {"delta", "Second update"}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "msg-2"}, {"type", "agentMessage"}, {"text", "Second update with suffix."}}}}}});

	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("First update.\n\nSecond update with suffix."));
	UAM_ASSERT_EQ(CountSubstring(app.chats.front().messages[0].content, "First update."), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(CountSubstring(app.chats.front().messages[0].content, "Second update"), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].blocks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].blocks[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].blocks[0].text, std::string("First update.\n\nSecond update with suffix."));
}

UAM_TEST(CodexAppServerCompletedPlanClearsDuplicateDeltaEntry)
{
	TempDir temp("uam-codex-plan-dedupe");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const auto process = [&](const nlohmann::json& message) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), message.dump())); };

	const std::string markdown_plan = "# Fix Plan\n\n## Summary\nUse only the formatted plan.";
	process({{"jsonrpc", "2.0"}, {"method", "item/plan/delta"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"itemId", "plan-1"}, {"delta", markdown_plan}}}});
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(1));

	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", {{"id", "plan-1"}, {"type", "plan"}, {"text", markdown_plan}}}}}});

	UAM_ASSERT_EQ(raw_session->plan_summary, markdown_plan);
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_summary, markdown_plan);
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_entries.size(), static_cast<std::size_t>(0));
	UAM_ASSERT_EQ(app.chats.front().messages[0].blocks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].blocks[0].type, std::string("plan"));
}

UAM_TEST(CodexAppServerPersistsOrderedBlocksAcrossReload)
{
	TempDir temp("uam-codex-ordered-blocks");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const auto process = [&](const nlohmann::json& message) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), message.dump())); };

	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/textDelta"}, {"params", {{"itemId", "reasoning-1"}, {"contentIndex", 0}, {"delta", "First thought."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/agentMessage/delta"}, {"params", {{"itemId", "msg-1"}, {"delta", "First visible text."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/completed"}, {"params", {{"item", {{"id", "cmd-1"}, {"type", "commandExecution"}, {"status", "completed"}, {"command", "rg Foo"}, {"aggregatedOutput", "matches"}}}}}});
	process({{"jsonrpc", "2.0"}, {"method", "item/reasoning/textDelta"}, {"params", {{"itemId", "reasoning-2"}, {"contentIndex", 0}, {"delta", "Second thought."}}}});
	process({{"jsonrpc", "2.0"}, {"method", "turn/plan/updated"}, {"params", {{"explanation", "Ordered plan."}, {"plan", nlohmann::json::array({{{"step", "Ship ordered blocks"}, {"status", "pending"}}})}}}});

	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	const Message& assistant = app.chats.front().messages[0];
	UAM_ASSERT_EQ(assistant.blocks.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(assistant.blocks[0].type, std::string("thought"));
	UAM_ASSERT(assistant.blocks[0].text.find("First thought.") != std::string::npos);
	UAM_ASSERT_EQ(assistant.blocks[1].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(assistant.blocks[1].text, std::string("First visible text."));
	UAM_ASSERT_EQ(assistant.blocks[2].type, std::string("tool_call"));
	UAM_ASSERT_EQ(assistant.blocks[2].tool_call_id, std::string("cmd-1"));
	UAM_ASSERT_EQ(assistant.blocks[3].type, std::string("thought"));
	UAM_ASSERT(assistant.blocks[3].text.find("Second thought.") != std::string::npos);
	UAM_ASSERT_EQ(assistant.blocks[4].type, std::string("plan"));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks.size(), static_cast<std::size_t>(5));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks[2].tool_call_id, std::string("cmd-1"));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks[4].type, std::string("plan"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][0].value("type", ""), std::string("thought"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][1].value("text", ""), std::string("First visible text."));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][2].value("toolCallId", ""), std::string("cmd-1"));
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["blocks"][4].value("type", ""), std::string("plan"));
}

UAM_TEST(AcpMissingSessionIdRecordsDiagnostics)
{
	TempDir temp("uam-acp-missing-session-id");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "gemini-cli";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":8,"result":{}})"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("Gemini ACP session/new failed (id=8): Gemini ACP did not return a session id.") != std::string::npos);
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("missing_session_id"));
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("result={}") != std::string::npos);
}

UAM_TEST(AcpSessionNewParsesModesModelsAndModeUpdates)
{
	TempDir temp("uam-acp-modes-models");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json session_new = {
	    {"jsonrpc", "2.0"},
	    {"id", 8},
	    {"result",
	     {
	         {"sessionId", "sess-1"},
	         {"modes",
	          {
	              {"availableModes", nlohmann::json::array({
	                                     {{"id", " default "}, {"name", " Default "}, {"description", " Run normally "}},
	                                     {{"id", " auto_edit "}, {"name", " Accept Edits "}, {"description", " Auto edit files "}},
	                                     {{"id", " auto "}, {"name", "Suppressed"}},
	                                 })},
	              {"currentModeId", " default "},
	          }},
	         {"models",
	          {
	              {"availableModels", nlohmann::json::array({
	                                      {{"modelId", " auto-gemini-3 "}, {"name", " Auto 3 "}, {"description", " Gemini 3 routing "}},
	                                      {{"id", " gemini-3-flash-preview "}, {"displayName", " Gemini 3 Flash "}, {"description", " Preview model "}},
	                                  })},
	              {"currentModelId", " auto-gemini-3 "},
	          }},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), session_new.dump()));
	UAM_ASSERT_EQ(raw_session->session_id, std::string("sess-1"));
	UAM_ASSERT_EQ(raw_session->available_modes.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_modes[1].id, std::string("acceptEdits"));
	UAM_ASSERT_EQ(raw_session->available_modes[1].name, std::string("Accept Edits"));
	UAM_ASSERT_EQ(raw_session->available_modes[1].description, std::string("Auto edit files"));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("default"));
	UAM_ASSERT_EQ(raw_session->available_models.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_models[0].id, std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(raw_session->available_models[1].name, std::string("Gemini 3 Flash"));
	UAM_ASSERT_EQ(raw_session->available_models[1].description, std::string("Preview model"));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("auto-gemini-3"));

	const nlohmann::json mode_update = {
	    {"jsonrpc", "2.0"},
	    {"method", "session/update"},
	    {"params", {{"update", {{"sessionUpdate", "current_mode_update"}, {"currentModeId", " plan "}}}}},
	};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), mode_update.dump()));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	UAM_ASSERT(app.provider_model_catalog->BeginDiscoveryIfMissing(app.chats.front().provider_id));
	app.provider_model_catalog->RememberRefreshFailure(app.chats.front().provider_id, "Model discovery failed");

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModes"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModeId", ""), std::string("plan"));
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModelId", ""), std::string("auto-gemini-3"));
	UAM_ASSERT_EQ(acp.value("lastError", ""), std::string{});
}

UAM_TEST(CopilotAcpCanonicalModesNormalizeToAppModes)
{
	TempDir temp("uam-copilot-acp-modes");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-copilot-modes";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = "session/new";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json session_new = {
	    {"jsonrpc", "2.0"},
	    {"id", 8},
	    {"result",
	     {
	         {"sessionId", "copilot-session-1"},
	         {"modes",
	          {
	              {"availableModes", nlohmann::json::array({
	                                     {{"id", uam::approval_modes::kAcpAgentMode}, {"name", "Agent"}},
	                                     {{"id", uam::approval_modes::kAcpPlanMode}, {"name", "Plan"}},
	                                     {{"id", uam::approval_modes::kAcpAutopilotMode}, {"name", "Autopilot"}},
	                                 })},
	              {"currentModeId", uam::approval_modes::kAcpAgentMode},
	          }},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), session_new.dump()));
	UAM_ASSERT_EQ(raw_session->available_modes.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_modes[0].id, std::string(uam::approval_modes::kDefaultApprovalMode));
	UAM_ASSERT_EQ(raw_session->available_modes[1].id, std::string(uam::approval_modes::kPlanApprovalMode));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string(uam::approval_modes::kDefaultApprovalMode));

	const nlohmann::json mode_update = {
	    {"jsonrpc", "2.0"},
	    {"method", "session/update"},
	    {"params", {{"update", {{"sessionUpdate", "current_mode_update"}, {"currentModeId", uam::approval_modes::kAcpPlanMode}}}}},
	};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), mode_update.dump()));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string(uam::approval_modes::kPlanApprovalMode));

	const nlohmann::json autopilot_update = {
	    {"jsonrpc", "2.0"},
	    {"method", "session/update"},
	    {"params", {{"update", {{"sessionUpdate", "current_mode_update"}, {"currentModeId", uam::approval_modes::kAcpAutopilotMode}}}}},
	};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), autopilot_update.dump()));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string(uam::approval_modes::kAcpAutopilotMode));
	const nlohmann::json set_mode = uam::acp_detail::BuildSetModeRequest(9, raw_session->session_id, uam::acp_detail::ProviderApprovalModeId(*raw_session, uam::approval_modes::kDefaultApprovalMode));
	UAM_ASSERT_EQ(set_mode["params"].value("modeId", ""), std::string(uam::approval_modes::kAcpAgentMode));

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Return to safe agent mode.", {}, {}, false, &error));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(raw_session->mode_change_request_id), std::string(uam::acp_methods::kSessionSetMode));
	UAM_ASSERT_EQ(raw_session->mode_change_requested_id, std::string(uam::approval_modes::kDefaultApprovalMode));
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(CopilotAvailableCommandsUpdateIsStoredOutsideATurnAndSerialized)
{
	TempDir temp("uam-copilot-acp-commands");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-copilot-commands";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolCopilotAcp;
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->lifecycle_state = "ready";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json commands_update = {
	    {"jsonrpc", "2.0"},
	    {"method", "session/update"},
	    {"params",
	     {
	         {"sessionId", "copilot-session-1"},
	         {"update",
	          {
	              {"sessionUpdate", "available_commands_update"},
	              {"availableCommands", nlohmann::json::array({
	                                        {
	                                            {"name", "security-review"},
	                                            {"description", "Review the current changes for security issues"},
	                                            {"input", {{"hint", "[focus]"}}},
	                                        },
	                                        {
	                                            {"name", "context"},
	                                            {"description", "Show context window usage"},
	                                        },
	                                    })},
	          }},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), commands_update.dump()));
	UAM_ASSERT_EQ(raw_session->available_commands.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_commands[0].name, std::string("security-review"));
	UAM_ASSERT_EQ(raw_session->available_commands[0].description, std::string("Review the current changes for security issues"));
	UAM_ASSERT_EQ(raw_session->available_commands[0].input_hint, std::string("[focus]"));
	UAM_ASSERT_EQ(raw_session->available_commands[1].name, std::string("context"));
	UAM_ASSERT(raw_session->available_commands[1].input_hint.empty());

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json available_commands = serialized["chats"][0]["acpSession"]["availableCommands"];
	UAM_ASSERT_EQ(available_commands.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(available_commands[0].value("name", ""), std::string("security-review"));
	UAM_ASSERT_EQ(available_commands[0].value("inputHint", ""), std::string("[focus]"));
	UAM_ASSERT_EQ(available_commands[1].value("description", ""), std::string("Show context window usage"));
}

UAM_TEST(CopilotAcpUsesModelSpecificReasoningConfigOptions)
{
	TempDir temp("uam-copilot-acp-reasoning-options");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-copilot-reasoning";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.reasoning_effort = "max";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolCopilotAcp;
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->pending_request_methods[8] = uam::acp_methods::kSessionNew;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json reasoning_option = {
	    {"type", "select"},
	    {"id", "reasoning_effort"},
	    {"name", "Reasoning Effort"},
	    {"currentValue", "low"},
	    {"options", nlohmann::json::array({
	                    {{"value", "low"}, {"name", "Low"}},
	                    {{"value", "high"}, {"name", "High"}},
	                })},
	};
	const nlohmann::json session_new = {
	    {"jsonrpc", "2.0"},
	    {"id", 8},
	    {"result",
	     {
	         {"sessionId", "copilot-session-1"},
	         {"models",
	          {
	              {"availableModels", nlohmann::json::array({
	                                      {{"modelId", "gpt-5.1"}, {"name", "GPT-5.1"}},
	                                  })},
	              {"currentModelId", "gpt-5.1"},
	          }},
	         {"configOptions", nlohmann::json::array({reasoning_option})},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), session_new.dump()));
	UAM_ASSERT_EQ(raw_session->available_models[0].supported_reasoning_efforts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_models[0].default_reasoning_effort, std::string("low"));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("low"));

	const nlohmann::json request = uam::acp_detail::BuildSetConfigOptionRequest(9, raw_session->session_id, "reasoning_effort", "high");
	UAM_ASSERT_EQ(request.value("method", ""), std::string(uam::acp_methods::kSessionSetConfigOption));
	UAM_ASSERT_EQ(request["params"].value("configId", ""), std::string("reasoning_effort"));
	UAM_ASSERT_EQ(request["params"].value("value", ""), std::string("high"));

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	const int request_id = raw_session->next_request_id;
	app.chats.front().reasoning_effort = "high";
	UAM_ASSERT(uam::SetAcpSessionReasoningEffort(app, app.chats.front().id, "high", &error, std::string("low")));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(request_id), std::string(uam::acp_methods::kSessionSetConfigOption));

	nlohmann::json updated_reasoning_option = reasoning_option;
	updated_reasoning_option["currentValue"] = "high";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"id", request_id},
	                                                          {"result", {{"configOptions", nlohmann::json::array({updated_reasoning_option})}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->reasoning_change_request_id, 0);
	UAM_ASSERT_EQ(raw_session->available_models[0].default_reasoning_effort, std::string("high"));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("high"));

	updated_reasoning_option["currentValue"] = "low";
	updated_reasoning_option["options"] = nlohmann::json::array({{{"value", "low"}, {"name", "Low"}}});
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params", {{"update", {{"sessionUpdate", "config_option_update"}, {"configOptions", nlohmann::json::array({updated_reasoning_option})}}}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->available_models[0].supported_reasoning_efforts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("low"));
	UAM_ASSERT(!uam::SetAcpSessionReasoningEffort(app, app.chats.front().id, "high", &error));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(CopilotAcpRefreshesReasoningOptionsAcrossModelChanges)
{
	TempDir temp("uam-copilot-acp-model-reasoning-options");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-copilot-model-reasoning";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.model_id = "model-a";
	chat.reasoning_effort = "low";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolCopilotAcp;
	session->session_id = "copilot-session-models";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->current_model_id = "model-a";
	uam::AcpModelState model_a;
	model_a.id = "model-a";
	model_a.default_reasoning_effort = "low";
	model_a.supported_reasoning_efforts = {"low", "high"};
	session->available_models.push_back(std::move(model_a));
	uam::AcpModelState model_b;
	model_b.id = "model-b";
	session->available_models.push_back(std::move(model_b));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));

	const nlohmann::json model_b_reasoning = {
	    {"type", "select"},
	    {"id", "reasoning_effort"},
	    {"currentValue", "medium"},
	    {"options", nlohmann::json::array({
	                    {{"value", "medium"}, {"name", "Medium"}},
	                    {{"value", "xhigh"}, {"name", "Extra high"}},
	                })},
	};
	app.chats.front().model_id = "model-b";
	app.chats.front().reasoning_effort = "xhigh";
	const int model_b_request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionModel(app, app.chats.front().id, "model-b", &error, std::string("model-a")));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(model_b_request_id), std::string(uam::acp_methods::kSessionSetModel));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params", {{"update", {{"sessionUpdate", "config_option_update"}, {"configOptions", nlohmann::json::array({model_b_reasoning})}}}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->available_models[1].supported_reasoning_efforts, (std::vector<std::string>{"medium", "xhigh"}));
	UAM_ASSERT_EQ(raw_session->reasoning_change_request_id, 0);
	UAM_ASSERT(!uam::SetAcpSessionReasoningEffort(app, app.chats.front().id, "xhigh", &error));
	raw_session->processing = true;
	raw_session->queued_prompt = "First prompt must wait for the selected reasoning effort.";

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), nlohmann::json({{"jsonrpc", "2.0"}, {"id", model_b_request_id}, {"result", nlohmann::json::object()}}).dump()));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("model-b"));
	UAM_ASSERT_EQ(raw_session->model_change_request_id, 0);
	UAM_ASSERT_EQ(raw_session->available_models[1].default_reasoning_effort, std::string("xhigh"));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("xhigh"));
	UAM_ASSERT(raw_session->session_ready);
	const int model_b_reasoning_request_id = raw_session->reasoning_change_request_id;
	UAM_ASSERT(model_b_reasoning_request_id != 0);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(model_b_reasoning_request_id), std::string(uam::acp_methods::kSessionSetConfigOption));
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT(!raw_session->queued_prompt.empty());
	raw_session->processing = false;
	raw_session->queued_prompt.clear();

	nlohmann::json model_b_applied = model_b_reasoning;
	model_b_applied["currentValue"] = "xhigh";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"id", model_b_reasoning_request_id},
	                                                          {"result", {{"configOptions", nlohmann::json::array({model_b_applied})}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->reasoning_change_request_id, 0);
	UAM_ASSERT_EQ(raw_session->available_models[1].default_reasoning_effort, std::string("xhigh"));

	const nlohmann::json model_a_reasoning = {
	    {"type", "select"},
	    {"id", "reasoning_effort"},
	    {"currentValue", "low"},
	    {"options", nlohmann::json::array({
	                    {{"value", "low"}, {"name", "Low"}},
	                    {{"value", "high"}, {"name", "High"}},
	                })},
	};
	app.chats.front().model_id = "model-a";
	app.chats.front().reasoning_effort = "high";
	const int model_a_request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionModel(app, app.chats.front().id, "model-a", &error, std::string("model-b")));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"id", model_a_request_id},
	                                                          {"result", {{"configOptions", nlohmann::json::array({model_a_reasoning})}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->available_models[0].supported_reasoning_efforts, (std::vector<std::string>{"low", "high"}));
	const int model_a_reasoning_request_id = raw_session->reasoning_change_request_id;
	UAM_ASSERT(model_a_reasoning_request_id != 0);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(model_a_reasoning_request_id), std::string(uam::acp_methods::kSessionSetConfigOption));

	nlohmann::json model_a_applied = model_a_reasoning;
	model_a_applied["currentValue"] = "high";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"id", model_a_reasoning_request_id},
	                                                          {"result", {{"configOptions", nlohmann::json::array({model_a_applied})}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(raw_session->available_models[0].default_reasoning_effort, std::string("high"));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("high"));

	app.chats.front().model_id = "model-b";
	app.chats.front().reasoning_effort = "medium";
	const int response_first_model_request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionModel(app, app.chats.front().id, "model-b", &error, std::string("model-a")));
	raw_session->processing = true;
	raw_session->queued_prompt = "This prompt must wait for the model config update.";
	UAM_ASSERT(raw_session->awaiting_model_config_options);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), nlohmann::json({{"jsonrpc", "2.0"}, {"id", response_first_model_request_id}, {"result", nlohmann::json::object()}}).dump()));
	UAM_ASSERT(raw_session->awaiting_model_config_options);
	UAM_ASSERT_EQ(raw_session->reasoning_change_request_id, 0);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT(!raw_session->queued_prompt.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params", {{"update", {{"sessionUpdate", "config_option_update"}, {"configOptions", nlohmann::json::array({model_b_reasoning})}}}}},
	                                                      })
	                                           .dump()));
	UAM_ASSERT(!raw_session->awaiting_model_config_options);
	UAM_ASSERT_EQ(raw_session->reasoning_change_request_id, 0);
	UAM_ASSERT(raw_session->prompt_request_id != 0);
	UAM_ASSERT(raw_session->queued_prompt.empty());

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(CopilotFirstQueuedPromptWaitsForSafeModeAfterSessionSetup)
{
	TempDir temp("uam-copilot-first-prompt-mode");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-copilot-first-prompt";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = uam::provider_ids::kCopilotCli;
	session->running = true;
	session->initialized = true;
	session->processing = true;
	session->queued_prompt = "First queued prompt";
	session->session_setup_request_id = 8;
	session->next_request_id = 9;
	session->pending_request_methods[8] = uam::acp_methods::kSessionNew;
	uam::AcpSessionState* raw_session = session.get();
#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json session_new = {
	    {"jsonrpc", "2.0"},
	    {"id", 8},
	    {"result",
	     {
	         {"sessionId", "6a6f0f3b-1a0b-4a9c-8a01-111111111111"},
	         {"modes",
	          {
	              {"availableModes", nlohmann::json::array({
	                                     {{"id", uam::approval_modes::kAcpAgentMode}, {"name", "Agent"}},
	                                     {{"id", uam::approval_modes::kAcpPlanMode}, {"name", "Plan"}},
	                                     {{"id", uam::approval_modes::kAcpAutopilotMode}, {"name", "Autopilot"}},
	                                 })},
	              {"currentModeId", uam::approval_modes::kAcpAutopilotMode},
	          }},
	     }},
	};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), session_new.dump()));
	UAM_ASSERT_EQ(raw_session->mode_change_request_id, 9);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(9), std::string(uam::acp_methods::kSessionSetMode));
	UAM_ASSERT_EQ(raw_session->mode_change_requested_id, std::string(uam::approval_modes::kDefaultApprovalMode));
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":9,"result":{}})"));
	UAM_ASSERT_EQ(raw_session->mode_change_request_id, 0);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 10);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(10), std::string(uam::acp_methods::kSessionPrompt));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(CodexCachedModelsPopulateSelectorBeforeAppServerStarts)
{
	TempDir temp("uam-codex-model-cache");
	ScopedEnvVar codex_home("CODEX_HOME", temp.root.string());
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "models_cache.json", R"({
  "models": [
    {"slug": "gpt-5.4", "display_name": "gpt-5.4", "description": "Latest frontier agentic coding model.", "visibility": "list", "defaultReasoningEffort": "medium", "supportedReasoningEfforts": [{"reasoningEffort": "low"}, {"reasoningEffort": "high"}], "additionalSpeedTiers": ["fast"]},
    {"slug": "hidden-model", "display_name": "Hidden", "visibility": "hidden"},
    {"slug": "gpt-5.4-mini", "display_name": "GPT-5.4-Mini", "description": "Smaller frontier agentic coding model.", "visibility": "list"},
    {"slug": "gpt-5.4", "display_name": "Duplicate", "visibility": "list"}
  ]
})"));

	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = " CoDeX ";
	app.chats.push_back(std::move(chat));

	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("id", ""), std::string("gpt-5.4"));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("name", ""), std::string("gpt-5.4"));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("defaultReasoningEffort", ""), std::string("medium"));
	UAM_ASSERT_EQ(acp["availableModels"][0]["supportedReasoningEfforts"][0], std::string("low"));
	UAM_ASSERT_EQ(acp["availableModels"][0]["additionalSpeedTiers"][0], std::string("fast"));
	UAM_ASSERT_EQ(acp["availableModels"][1].value("id", ""), std::string("gpt-5.4-mini"));
	UAM_ASSERT_EQ(acp["availableModels"][1].value("name", ""), std::string("GPT-5.4-Mini"));
	UAM_ASSERT_EQ(acp.value("currentModelId", ""), std::string(""));
}

UAM_TEST(OpenCodeConfigModelsPopulateSelectorBeforeAcpStarts)
{
	TempDir temp("uam-opencode-model-config");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar disable_zen_refresh("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	const fs::path config_dir = temp.root / "opencode";
	fs::create_directories(config_dir);
	UAM_ASSERT(uam::io::WriteTextFile(config_dir / "opencode.json", R"({
  "provider": {
    "ollama-r9700": {
      "name": "Ollama on R9700",
      "models": {
        "qwen3.6:35b-a3b-q4_K_M": { "name": " Qwen3.6 35B A3B Q4 " },
        "qwen3-coder:30b": { "name": " Qwen3 Coder 30B ", "description": " Coding model " }
      }
    },
    "local-openai": {
      "models": {
        "gpt-oss:20b": { "name": " GPT-OSS 20B " }
      }
    }
  },
  "model": " ollama-r9700/qwen3.6:35b-a3b-q4_K_M "
})"));

	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = " open-code ";
	app.chats.push_back(std::move(chat));

	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	auto model_by_id = [&](const std::string& id) -> nlohmann::json
	{
		for (const nlohmann::json& model : acp["availableModels"])
		{
			if (model.value("id", "") == id)
			{
				return model;
			}
		}
		return nlohmann::json::object();
	};
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(9));
	UAM_ASSERT_EQ(model_by_id("ollama-r9700/qwen3.6:35b-a3b-q4_K_M").value("name", ""), std::string("Qwen3.6 35B A3B Q4"));
	UAM_ASSERT_EQ(model_by_id("ollama-r9700/qwen3-coder:30b").value("description", ""), std::string("Coding model"));
	UAM_ASSERT_EQ(model_by_id("local-openai/gpt-oss:20b").value("name", ""), std::string("GPT-OSS 20B"));
	UAM_ASSERT_EQ(model_by_id("opencode/deepseek-v4-flash-free").value("name", ""), std::string("DeepSeek V4 Flash Free"));
	UAM_ASSERT_EQ(model_by_id("opencode/big-pickle").value("description", ""), std::string("OpenCode Zen limited-time stealth free model."));
	UAM_ASSERT_EQ(acp.value("currentModelId", ""), std::string("ollama-r9700/qwen3.6:35b-a3b-q4_K_M"));
}

UAM_TEST(ProviderModelCatalogPersistsSuccessfulRefreshAndIsolatesConfigurations)
{
	TempDir temp("uam-provider-model-cache");
	ProviderProfile first = ProviderProfileStore::DefaultOpenCodeProfile();
	first.interactive_command = "opencode --endpoint account-a";
	const nlohmann::json models = nlohmann::json::array({
	    {{"id", "vendor/reasoner"}, {"name", "Reasoner"}, {"supportedReasoningEfforts", nlohmann::json::array({"low", "high"})}},
	});

	{
		uam::ProviderModelCatalogService catalog;
		catalog.Initialize(temp.root, {first});
		UAM_ASSERT(catalog.GetCachedProviderModels(first.id).empty());
		UAM_ASSERT(catalog.BeginDiscoveryIfMissing(first.id));
		UAM_ASSERT(catalog.IsDiscoveryPending(first.id));
		UAM_ASSERT(!catalog.BeginDiscoveryIfMissing(first.id));
		UAM_ASSERT(catalog.RememberSuccessfulModels(first.id, models));
		UAM_ASSERT(!catalog.IsDiscoveryPending(first.id));
		UAM_ASSERT(!catalog.BeginDiscoveryIfMissing(first.id));
		catalog.RememberRefreshFailure(first.id, "refresh failed");
		UAM_ASSERT_EQ(catalog.GetCachedProviderModels(first.id), models);
		UAM_ASSERT_EQ(catalog.GetProviderRefreshError(first.id), std::string("refresh failed"));
		UAM_ASSERT(!catalog.RememberSuccessfulModels(first.id, nlohmann::json::array()));
		UAM_ASSERT_EQ(catalog.GetCachedProviderModels(first.id), models);
	}

	{
		uam::ProviderModelCatalogService restarted;
		restarted.Initialize(temp.root, {first});
		UAM_ASSERT_EQ(restarted.GetCachedProviderModels(first.id), models);
		UAM_ASSERT(!restarted.BeginDiscoveryIfStale(first.id));
		UAM_ASSERT(restarted.BeginDiscovery(first.id));
		restarted.RememberRefreshFailure(first.id, "background refresh failed");
		UAM_ASSERT_EQ(restarted.GetCachedProviderModels(first.id), models);
	}

	ProviderProfile second = first;
	second.interactive_command = "opencode --endpoint account-b";
	uam::ProviderModelCatalogService isolated;
	isolated.Initialize(temp.root, {second});
	UAM_ASSERT(isolated.GetCachedProviderModels(second.id).empty());
	UAM_ASSERT(isolated.BeginDiscoveryIfMissing(second.id));
	isolated.RememberRefreshFailure(second.id, "isolated failure");
	UAM_ASSERT(!isolated.IsDiscoveryPending(second.id));
	UAM_ASSERT(!isolated.BeginDiscoveryIfMissing(second.id));

	// A cache without the epoch freshness marker predates the seven-day policy and refreshes once.
	const fs::path cache_path = temp.root / "provider_model_catalog_cache.json";
	nlohmann::json persisted = nlohmann::json::parse(uam::io::ReadTextFile(cache_path));
	for (auto& entry : persisted["catalogs"].items()) entry.value().erase("updatedAtSec");
	UAM_ASSERT(uam::io::WriteTextFile(cache_path, persisted.dump()));
	uam::ProviderModelCatalogService stale;
	stale.Initialize(temp.root, {first});
	UAM_ASSERT(stale.BeginDiscoveryIfStale(first.id));
}

UAM_TEST(ProviderModelCatalogDropsConfiguredModelsWhenConfigIsDeleted)
{
	TempDir temp("uam-opencode-config-delete");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar disable_zen_refresh("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	const fs::path config = temp.root / "opencode" / "opencode.json";
	UAM_ASSERT(uam::io::WriteTextFile(config, R"({
	  "model": "local/test-model",
	  "provider": {
	    "local": {
	      "models": {
	        "test-model": { "name": "Test Model" }
	      }
	    }
	  }
	})"));

	uam::ProviderModelCatalogService catalog;
	catalog.Initialize(temp.root / "data");
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeModels().size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(catalog.GetConfiguredOpenCodeDefaultModel(), std::string("local/test-model"));

	UAM_ASSERT(fs::remove(config));
	catalog.Poll();

	UAM_ASSERT(catalog.GetConfiguredOpenCodeModels().empty());
	UAM_ASSERT(catalog.GetConfiguredOpenCodeDefaultModel().empty());
}

UAM_TEST(OpenCodeZenFreeModelsParseAndFilterOfficialModelList)
{
	const nlohmann::json parsed = uam::ProviderModelCatalogService::ParseOpenCodeZenFreeModels(nlohmann::json::parse(R"({
  "object": "list",
  "data": [
    { "id": "deepseek-v4-flash-free", "object": "model", "owned_by": "opencode" },
    { "id": "gpt-5.4", "object": "model", "owned_by": "opencode" },
    { "id": "big-pickle", "object": "model", "owned_by": "opencode" },
    { "id": "nemotron-3-super-free", "object": "model", "owned_by": "opencode" },
    { "id": "vendor-free", "object": "model", "owned_by": "other" },
    { "id": "", "object": "model", "owned_by": "opencode" },
    "bad"
  ]
})"));

	UAM_ASSERT_EQ(parsed.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(parsed[0].value("id", ""), std::string("opencode/deepseek-v4-flash-free"));
	UAM_ASSERT_EQ(parsed[0].value("name", ""), std::string("DeepSeek V4 Flash Free"));
	UAM_ASSERT_EQ(parsed[1].value("id", ""), std::string("opencode/big-pickle"));
	UAM_ASSERT_EQ(parsed[1].value("description", ""), std::string("OpenCode Zen limited-time stealth free model."));
	UAM_ASSERT_EQ(parsed[2].value("id", ""), std::string("opencode/nemotron-3-super-free"));
}

UAM_TEST(OpenCodeZenFreeModelsRefreshFromFixtureAndCache)
{
	TempDir temp("uam-opencode-zen-models");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	const fs::path fixture = temp.root / "zen-models.json";
	UAM_ASSERT(uam::io::WriteTextFile(fixture, R"({
  "object": "list",
  "data": [
    { "id": "minimax-m3-free", "object": "model", "owned_by": "opencode" },
    { "id": "qwen3.6-plus-free", "object": "model", "owned_by": "opencode" },
    { "id": "gpt-5.4", "object": "model", "owned_by": "opencode" }
  ]
})"));
	ScopedEnvVar fixture_env("UAM_OPENCODE_ZEN_MODELS_PATH", fixture.string());

	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	app.chats.push_back(std::move(chat));

	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);
	UAM_ASSERT(app.provider_model_catalog->MaybeStartRefresh());
	bool refreshed = false;
	for (int attempt = 0; attempt < 2000 && !refreshed; ++attempt)
	{
		refreshed = app.provider_model_catalog->Poll();
		if (!refreshed)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	UAM_ASSERT(refreshed);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("id", ""), std::string("opencode/minimax-m3-free"));
	UAM_ASSERT_EQ(acp["availableModels"][1].value("id", ""), std::string("opencode/qwen3.6-plus-free"));
	UAM_ASSERT(fs::exists(temp.root / "opencode_zen_free_models_cache.json"));
}

UAM_TEST(OpenCodeRuntimeModelsMergeWithConfiguredModels)
{
	TempDir temp("uam-opencode-model-merge");
	ScopedEnvVar config_home("XDG_CONFIG_HOME", temp.root.string());
	ScopedEnvVar disable_zen_refresh("UAM_DISABLE_OPENCODE_ZEN_REFRESH", "1");
	const fs::path config_dir = temp.root / "opencode";
	fs::create_directories(config_dir);
	UAM_ASSERT(uam::io::WriteTextFile(config_dir / "opencode.json", R"({
  "provider": {
    "ollama-r9700": {
      "models": {
        "qwen3-coder:30b": { "name": "Qwen3 Coder 30B" }
      }
    }
  }
})"));

	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "opencode-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "opencode-cli";
	session->protocol_kind = "opencode-acp";
	session->running = true;
	session->session_ready = true;
	session->available_models.push_back(uam::AcpModelState{" ollama-r9700/qwen3-coder:30b ", "Runtime duplicate", ""});
	session->available_models.push_back(uam::AcpModelState{" ollama-r9700/mistral-small3.2:24b ", " Mistral Small 3.2 24B ", ""});
	app.acp_sessions.push_back(std::move(session));

	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root);

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(8));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("id", ""), std::string("ollama-r9700/qwen3-coder:30b"));
	UAM_ASSERT_EQ(acp["availableModels"][0].value("name", ""), std::string("Qwen3 Coder 30B"));
	UAM_ASSERT_EQ(acp["availableModels"][1].value("id", ""), std::string("opencode/big-pickle"));
	UAM_ASSERT_EQ(acp["availableModels"][6].value("id", ""), std::string("opencode/nemotron-3-super-free"));
	UAM_ASSERT_EQ(acp["availableModels"][7].value("id", ""), std::string("ollama-r9700/mistral-small3.2:24b"));
	UAM_ASSERT_EQ(acp["availableModels"][7].value("name", ""), std::string("Mistral Small 3.2 24B"));
}

UAM_TEST(BackgroundCodexModelDiscoveryStopsAfterCachingModels)
{
	TempDir temp("uam-background-model-discovery");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-discovery";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));
	app.provider_model_catalog = std::make_unique<uam::ProviderModelCatalogService>();
	app.provider_model_catalog->Initialize(app.data_root, app.provider_profiles);
	UAM_ASSERT(app.provider_model_catalog->BeginDiscoveryIfMissing("codex-cli"));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-discovery";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->model_discovery_only = true;
	session->pending_request_methods[2] = "model/list";
	uam::AcpSessionState* raw_session = session.get();
	const std::string model_list_response = R"({"jsonrpc":"2.0","id":2,"result":{"data":[{"id":"gpt-test","displayName":"GPT Test","visibility":"list"}]}})";
#if defined(__APPLE__)
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, {"/bin/sh", "-c", "printf '%s\\n' '" + model_list_response + "'; sleep 5"}, &launch_error));
#endif
	app.acp_sessions.push_back(std::move(session));

#if defined(__APPLE__)
	for (int attempt = 0; attempt < 100 && raw_session->running; ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
#else
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), model_list_response));
#endif
	UAM_ASSERT(!raw_session->running);
	UAM_ASSERT(!raw_session->model_discovery_only);
	UAM_ASSERT(!raw_session->reconnect_pending);
	UAM_ASSERT(raw_session->session_id.empty());
	UAM_ASSERT(raw_session->last_error.empty());
	UAM_ASSERT(!app.provider_model_catalog->IsDiscoveryPending("codex-cli"));
	UAM_ASSERT_EQ(app.provider_model_catalog->GetCachedProviderModels("codex-cli")[0].value("id", ""), std::string("gpt-test"));
}

UAM_TEST(CodexAppServerStateTransitionsMapModelsTurnsToolsAndApprovals)
{
	TempDir temp("uam-codex-app-server-state");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	chat.approval_mode = "plan";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->next_request_id = 100;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	raw_session->initialize_request_id = 1;
	raw_session->pending_request_methods[1] = "initialize";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{"userAgent":"codex-cli/1.2.3"}})"));
	UAM_ASSERT(raw_session->initialized);
	UAM_ASSERT_EQ(raw_session->agent_title, std::string("Codex"));
	UAM_ASSERT_EQ(raw_session->agent_version, std::string("codex-cli/1.2.3"));

	raw_session->pending_request_methods[2] = "model/list";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":2,"result":{"currentModelId":"gpt-5.4-mini","data":[{"slug":"gpt-5.4","display_name":"gpt-5.4","description":"Latest frontier agentic coding model.","visibility":"list","defaultReasoningEffort":"medium","supportedReasoningEfforts":[{"reasoningEffort":"low"},{"reasoningEffort":"high"}],"additionalSpeedTiers":["fast"]},{"id":"gpt-5.4-mini","displayName":"GPT-5.4-Mini","description":"Smaller model","isDefault":true},{"slug":"hidden-model","display_name":"Hidden","visibility":"hidden"},{"id":"hidden","displayName":"Hidden","hidden":true},{"id":"gpt-5.4","displayName":"Duplicate","description":"Duplicate entry","visibility":"list"}]}})"));
	UAM_ASSERT_EQ(raw_session->available_models.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->available_models[0].id, std::string("gpt-5.4"));
	UAM_ASSERT_EQ(raw_session->available_models[0].name, std::string("gpt-5.4"));
	UAM_ASSERT_EQ(raw_session->available_models[0].description, std::string("Latest frontier agentic coding model."));
	UAM_ASSERT_EQ(raw_session->available_models[0].default_reasoning_effort, std::string("medium"));
	UAM_ASSERT_EQ(raw_session->available_models[0].supported_reasoning_efforts[0], std::string("low"));
	UAM_ASSERT_EQ(raw_session->available_models[0].additional_speed_tiers[0], std::string("fast"));
	UAM_ASSERT_EQ(raw_session->available_models[1].id, std::string("gpt-5.4-mini"));
	UAM_ASSERT_EQ(raw_session->available_models[1].name, std::string("GPT-5.4-Mini"));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("gpt-5.4-mini"));

	raw_session->session_setup_request_id = 3;
	raw_session->pending_request_methods[3] = "thread/start";
	const std::string codex_thread_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), nlohmann::json({{"jsonrpc", "2.0"}, {"id", 3}, {"result", {{"thread", {{"id", codex_thread_id}}}, {"model", "gpt-5.4"}}}}).dump()));
	UAM_ASSERT_EQ(raw_session->session_id, codex_thread_id);
	UAM_ASSERT_EQ(raw_session->codex_thread_id, codex_thread_id);
	UAM_ASSERT_EQ(app.chats.front().native_session_id, codex_thread_id);
	UAM_ASSERT_EQ(raw_session->available_modes.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));

	raw_session->processing = true;
	raw_session->queued_prompt = "hello";
	raw_session->prompt_request_id = 4;
	raw_session->pending_request_methods[4] = "turn/start";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":4,"result":{"turn":{"id":"turn-1"}}})"));
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string("turn-1"));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("processing"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/agentMessage/delta","params":{"delta":"Hello from Codex."}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].provider, std::string("codex-cli"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("Hello from Codex."));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/plan/updated","params":{"explanation":"State transition plan","plan":[{"step":"Inspect files","status":"completed"},{"step":"Patch code","status":"pending"}]}})"));
	UAM_ASSERT_EQ(raw_session->plan_summary, std::string("State transition plan"));
	UAM_ASSERT_EQ(raw_session->plan_entries.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->plan_entries[1].content, std::string("Patch code"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_summary, std::string("State transition plan"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].plan_entries.size(), static_cast<std::size_t>(2));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/started","params":{"item":{"id":"cmd-1","type":"commandExecution","command":"ls","status":"running"}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/commandExecution/outputDelta","params":{"itemId":"cmd-1","delta":"file.txt\n"}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].id, std::string("cmd-1"));
	UAM_ASSERT(raw_session->tool_calls[0].content.find("file.txt") != std::string::npos);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":7,"method":"item/commandExecution/requestApproval","params":{"itemId":"cmd-1","command":"rm -rf build","availableDecisions":["accept","decline"]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string("7"));
	UAM_ASSERT_EQ(raw_session->pending_permission.provider_request_kind, std::string(uam::acp_permissions::kCodexCommandRequestKind));
	UAM_ASSERT_EQ(raw_session->pending_permission.options[0].id, std::string(uam::acp_permissions::kAcceptDecision));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("waitingPermission"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turnId":"turn-1"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
}

UAM_TEST(CodexCancelIgnoresLateApprovalAndClearsInterruptState)
{
	TempDir temp("uam-codex-cancel-approval");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState* raw_session = session.get();
	raw_session->chat_id = "chat-1";
	raw_session->provider_id = "codex-cli";
	raw_session->protocol_kind = "gemini-acp";
	raw_session->running = true;
	raw_session->initialized = true;
	raw_session->session_ready = true;
	raw_session->processing = true;
	raw_session->session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	raw_session->codex_thread_id = raw_session->session_id;
	raw_session->codex_turn_id = "turn-1";

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &launch_error));
	UAM_ASSERT(launch_error.empty());

	app.acp_sessions.push_back(std::move(session));

	std::string cancel_error;
	UAM_ASSERT(uam::CancelAcpTurn(app, "chat-1", &cancel_error));
	UAM_ASSERT(cancel_error.empty());
	UAM_ASSERT(raw_session->cancel_requested);
	UAM_ASSERT_EQ(raw_session->cancel_request_id, 1);
	UAM_ASSERT_EQ(raw_session->pending_request_methods[1], std::string("turn/interrupt"));
	UAM_ASSERT(!raw_session->waiting_for_permission);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":7,"method":"item/commandExecution/requestApproval","params":{"itemId":"cmd-1","command":"rm -rf build","availableDecisions":["accept","decline"]}})"));
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("ignored_permission_during_cancel"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":1,"result":{}})"));
	UAM_ASSERT(!raw_session->cancel_requested);
	UAM_ASSERT_EQ(raw_session->cancel_request_id, 0);
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string(""));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpCancelIgnoresLateGenericPermissionRequest)
{
	TempDir temp("uam-acp-cancel-generic-permission");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	uam::AcpSessionState* raw_session = session.get();
	raw_session->chat_id = "chat-1";
	raw_session->provider_id = "gemini-cli";
	raw_session->protocol_kind = "gemini-acp";
	raw_session->running = true;
	raw_session->initialized = true;
	raw_session->session_ready = true;
	raw_session->processing = true;
	raw_session->session_id = "gemini-session-1";

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &launch_error));
	UAM_ASSERT(launch_error.empty());

	app.acp_sessions.push_back(std::move(session));

	std::string cancel_error;
	UAM_ASSERT(uam::CancelAcpTurn(app, "chat-1", &cancel_error));
	UAM_ASSERT(cancel_error.empty());
	UAM_ASSERT(raw_session->cancel_requested);
	UAM_ASSERT(!raw_session->waiting_for_permission);

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":5,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-1","title":"Read file","kind":"read","status":"pending","content":{"type":"text","text":"Read /tmp/file.txt"}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"}]}})"));
	UAM_ASSERT(!raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.request_id_json, std::string(""));
	UAM_ASSERT(!raw_session->diagnostics.empty());
	UAM_ASSERT_EQ(raw_session->diagnostics.back().reason, std::string("ignored_permission_during_cancel"));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(CodexAppServerUserInputRequestsSurfaceAndSerialize)
{
	TempDir temp("uam-codex-user-input");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->session_ready = true;
	session->processing = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const nlohmann::json request = {
	    {"jsonrpc", "2.0"},
	    {"id", 11},
	    {"method", "item/tool/requestUserInput"},
	    {"params",
	     {
	         {"threadId", "thread-1"},
	         {"turnId", "turn-1"},
	         {"itemId", "input-1"},
	         {"questions", nlohmann::json::array({
	                           {
	                               {"id", "scope"},
	                               {"header", "Scope"},
	                               {"question", "Which scope?"},
	                               {"isOther", false},
	                               {"isSecret", false},
	                               {"options", nlohmann::json::array({{{"label", "Focused"}, {"description", "Only the bug"}}})},
	                           },
	                           {
	                               {"id", "note"},
	                               {"header", "Note"},
	                               {"question", "Any extra detail?"},
	                               {"isOther", true},
	                               {"isSecret", false},
	                               {"options", nullptr},
	                           },
	                       })},
	     }},
	};

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), request.dump()));
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(raw_session->waiting_for_user_input);
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("waitingUserInput"));
	UAM_ASSERT_EQ(raw_session->pending_user_input.request_id_json, std::string("11"));
	UAM_ASSERT_EQ(raw_session->pending_user_input.item_id, std::string("input-1"));
	UAM_ASSERT_EQ(raw_session->pending_user_input.questions.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->pending_user_input.questions[0].id, std::string("scope"));
	UAM_ASSERT_EQ(raw_session->pending_user_input.questions[0].options.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->pending_user_input.questions[0].options[0].label, std::string("Focused"));
	UAM_ASSERT(raw_session->pending_user_input.questions[1].is_other);
	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->turn_events[0].type, std::string("user_input_request"));
	UAM_ASSERT_EQ(raw_session->turn_events[0].request_id_json, std::string("11"));
	UAM_ASSERT_EQ(raw_session->turn_events[0].tool_call_id, std::string("input-1"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json pending = serialized["chats"][0]["acpSession"]["pendingUserInput"];
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("lifecycleState", ""), std::string("waitingUserInput"));
	UAM_ASSERT_EQ(pending.value("requestId", ""), std::string("11"));
	UAM_ASSERT_EQ(pending.value("itemId", ""), std::string("input-1"));
	UAM_ASSERT_EQ(pending["questions"][0].value("id", ""), std::string("scope"));
	UAM_ASSERT_EQ(pending["questions"][0]["options"][0].value("label", ""), std::string("Focused"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turnId":"turn-1"}})"));
	UAM_ASSERT(!raw_session->processing);
	UAM_ASSERT(!raw_session->waiting_for_user_input);
	UAM_ASSERT_EQ(raw_session->pending_user_input.request_id_json, std::string(""));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
}

UAM_TEST(CodexUserInputResponseBuilderMatchesProtocol)
{
	const std::map<std::string, std::vector<std::string>> answers = {
	    {"scope", {"Focused"}},
	    {"note", {"Extra context"}},
	};
	const nlohmann::json response = nlohmann::json::parse(uam::BuildCodexUserInputResponseForTests("11", answers));

	UAM_ASSERT_EQ(response.value("jsonrpc", ""), std::string("2.0"));
	UAM_ASSERT_EQ(response.value("id", 0), 11);
	UAM_ASSERT(response.contains("result"));
	UAM_ASSERT(response["result"].contains("answers"));
	UAM_ASSERT_EQ(response["result"]["answers"]["scope"]["answers"][0].get<std::string>(), std::string("Focused"));
	UAM_ASSERT_EQ(response["result"]["answers"]["note"]["answers"][0].get<std::string>(), std::string("Extra context"));
}

UAM_TEST(AcpDiagnosticRingCapsEntriesAndLongDetails)
{
	TempDir temp("uam-acp-diagnostic-ring");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const std::string long_invalid_line = std::string("{\"jsonrpc\":") + std::string(10000, 'x');
	for (int i = 0; i < 90; ++i)
	{
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), long_invalid_line));
	}

	UAM_ASSERT_EQ(raw_session->diagnostics.size(), static_cast<std::size_t>(80));
	UAM_ASSERT(raw_session->diagnostics.back().detail.size() < long_invalid_line.size());
	UAM_ASSERT(raw_session->diagnostics.back().detail.find("[truncated ") != std::string::npos);
}

UAM_TEST(AcpAssistantReplayIsStrippedFromNewTurn)
{
	TempDir temp("uam-acp-replay-strip");
	uam::AppState app;
	app.data_root = temp.root;

	const std::string previous_response = "Previous Gemini response with enough content to identify a replayed assistant message.";
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = previous_response;
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	session->assistant_replay_prefixes.push_back(previous_response);
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response + "\n\nSecond answer"}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(),
	                                       nlohmann::json({
	                                                          {"jsonrpc", "2.0"},
	                                                          {"method", "session/update"},
	                                                          {"params",
	                                                           {
	                                                               {"update",
	                                                                {
	                                                                    {"sessionUpdate", "agent_message_chunk"},
	                                                                    {"content", {{"type", "text"}, {"text", previous_response + "\n\nSecond answer with suffix"}}},
	                                                                }},
	                                                           }},
	                                                      })
	                                           .dump()));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer with suffix"));
	UAM_ASSERT_EQ(raw_session->turn_events.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->turn_events[0].text, std::string("Second answer with suffix"));
}

UAM_TEST(AcpLoadHistoryReplaySuppressesShortAssistantResponse)
{
	TempDir temp("uam-acp-short-replay");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = "OK";
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState user_replay;
	user_replay.session_update = "user_message_chunk";
	user_replay.text = "First prompt";
	session->load_history_replay_updates.push_back(std::move(user_replay));
	uam::AcpReplayUpdateState assistant_replay;
	assistant_replay.session_update = "agent_message_chunk";
	assistant_replay.text = "OK";
	session->load_history_replay_updates.push_back(std::move(assistant_replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"user_message_chunk","content":{"type":"text","text":"First prompt"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"OK"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"New answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("New answer"));
}

UAM_TEST(AcpLoadHistoryReplayStripsPrefixAndKeepsNewSuffix)
{
	TempDir temp("uam-acp-replay-suffix");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "First prompt";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "OK";
	assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(assistant));
	Message next_user;
	next_user.role = MessageRole::User;
	next_user.content = "Second prompt";
	next_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(next_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState replay;
	replay.session_update = "agent_message_chunk";
	replay.text = "OK";
	session->load_history_replay_updates.push_back(std::move(replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"OK\n\nSecond answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("Second answer"));
}

UAM_TEST(AcpThoughtsPersistOnAssistantMessage)
{
	TempDir temp("uam-acp-thought-persist");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please inspect this.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Need to inspect the file first."}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Done."}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages[1].thoughts, std::string("Need to inspect the file first."));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Done."));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][1].value("thoughts", ""), std::string("Need to inspect the file first."));
}

UAM_TEST(AcpStreamingPreservesIdenticalConsecutiveTextChunks)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-repeat-chunk";
	chat.provider_id = "gemini-cli";
	chat.messages.push_back(Message{MessageRole::User, "Laugh"});
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-repeat-chunk";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	const std::string chunk = R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"ha"}}}})";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), chunk));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), chunk));

	UAM_ASSERT_EQ(app.chats.front().messages.back().content, std::string("haha"));
}

UAM_TEST(AcpDeferredChatSaveRetriesAfterWriteFailure)
{
	TempDir temp("uam-acp-save-retry");
	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));

	uam::AppState app;
	app.data_root = blocked_root;
	ChatSession chat;
	chat.id = "chat-save-retry";
	chat.messages.push_back(Message{MessageRole::Assistant, "Keep this"});
	app.chats.push_back(std::move(chat));
	app.pending_chat_save_at_by_chat_id["chat-save-retry"] = 0.0;

	uam::FlushPendingChatSaves(app);

	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains("chat-save-retry"));
}

UAM_TEST(AcpDeferredChatSaveUsesTrailingDebounce)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-save-debounce";

	uam::acp_detail::ScheduleChatSave(app, chat, 1.0);
	const double first_due_at = app.pending_chat_save_at_by_chat_id.at(chat.id);
	uam::acp_detail::ScheduleChatSave(app, chat, 10.0);

	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.at(chat.id) > first_due_at + 8.0);
}

UAM_TEST(AcpToolCallsPersistOnAssistantMessage)
{
	TempDir temp("uam-acp-tool-persist");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message user;
	user.role = MessageRole::User;
	user.content = "Please read this file.";
	user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 0;
	session->prompt_request_id = 10;
	session->pending_request_methods[10] = "session/prompt";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"pending","rawInput":{"path":"README.md"},"locations":[{"path":"README.md"}]}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].content, std::string("README.md"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call_update","toolCallId":"tool-1","status":"completed","content":{"type":"text","text":"file contents"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":10,"result":{}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	const Message& assistant = app.chats.front().messages[1];
	UAM_ASSERT_EQ(assistant.role, MessageRole::Assistant);
	UAM_ASSERT_EQ(assistant.content, std::string(""));
	UAM_ASSERT_EQ(assistant.tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(assistant.tool_calls[0].id, std::string("tool-1"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].name, std::string("Read file"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].status, std::string("completed"));
	UAM_ASSERT_EQ(assistant.tool_calls[0].result_text, std::string("file contents"));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][1]["toolCalls"][0].value("title", ""), std::string("Read file"));
}

UAM_TEST(AcpCancellationFinalizesActiveToolCallsAndIgnoresLateUpdates)
{
	TempDir temp("uam-acp-cancel-tools");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message assistant;
	assistant.role = MessageRole::Assistant;
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "gemini-cli";
	session->processing = true;
	session->current_assistant_message_index = 0;
	session->turn_assistant_message_index = 0;
	session->tool_calls = {
		uam::AcpToolCallState{"pending", "Pending", "read", "pending", ""},
		uam::AcpToolCallState{"running", "Running", "execute", "running", ""},
		uam::AcpToolCallState{"progress", "Progress", "write", "in_progress", ""},
		uam::AcpToolCallState{"done", "Done", "read", "completed", ""},
	};
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::CancelAcpTurn(app, "chat-1", &error));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].status, std::string("cancelled"));
	UAM_ASSERT_EQ(raw_session->tool_calls[1].status, std::string("cancelled"));
	UAM_ASSERT_EQ(raw_session->tool_calls[2].status, std::string("cancelled"));
	UAM_ASSERT_EQ(raw_session->tool_calls[3].status, std::string("completed"));
	UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls[0].status, std::string("cancelled"));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"running","status":"in_progress"}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls[1].status, std::string("cancelled"));

	raw_session->provider_id = "codex-cli";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"item/started","params":{"item":{"id":"late","type":"commandExecution","status":"in_progress"}}})"));
	UAM_ASSERT_EQ(raw_session->tool_calls.size(), static_cast<std::size_t>(4));

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(serialized["chats"][0]["messages"][0]["toolCalls"][0].value("status", ""), std::string("cancelled"));
}

UAM_TEST(AcpLoadHistoryReplaySuppressesHistoricalThoughts)
{
	TempDir temp("uam-acp-thought-replay");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	Message first_user;
	first_user.role = MessageRole::User;
	first_user.content = "First prompt";
	first_user.created_at = "2026-01-01T00:00:00.000Z";
	chat.messages.push_back(std::move(first_user));
	Message first_assistant;
	first_assistant.role = MessageRole::Assistant;
	first_assistant.content = "Previous answer";
	first_assistant.thoughts = "Old thought";
	first_assistant.created_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(std::move(first_assistant));
	Message second_user;
	second_user.role = MessageRole::User;
	second_user.content = "Second prompt";
	second_user.created_at = "2026-01-01T00:00:02.000Z";
	chat.messages.push_back(std::move(second_user));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	session->turn_user_message_index = 2;
	uam::AcpReplayUpdateState user_replay;
	user_replay.session_update = "user_message_chunk";
	user_replay.text = "First prompt";
	session->load_history_replay_updates.push_back(std::move(user_replay));
	uam::AcpReplayUpdateState thought_replay;
	thought_replay.session_update = "agent_thought_chunk";
	thought_replay.text = "Old thought";
	session->load_history_replay_updates.push_back(std::move(thought_replay));
	uam::AcpReplayUpdateState assistant_replay;
	assistant_replay.session_update = "agent_message_chunk";
	assistant_replay.text = "Previous answer";
	session->load_history_replay_updates.push_back(std::move(assistant_replay));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"user_message_chunk","content":{"type":"text","text":"First prompt"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"Old thought"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Previous answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(raw_session->turn_events.empty());

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_thought_chunk","content":{"type":"text","text":"New thought"}}}})"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"New answer"}}}})"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(4));
	UAM_ASSERT_EQ(app.chats.front().messages[3].thoughts, std::string("New thought"));
	UAM_ASSERT_EQ(app.chats.front().messages[3].content, std::string("New answer"));
}


UAM_TEST(AutoApproveOptionIdMatchesAcpOptionKindsAndCommonLabels)
{
	// OpenCode-style ACP options classified by option kind, not just id/name text.
	uam::AcpPendingPermissionState pending;
	uam::AcpPermissionOptionState reject;
	reject.id = "no";
	reject.name = "No";
	reject.kind = "reject_once";
	uam::AcpPermissionOptionState proceed;
	proceed.id = "proceed";
	proceed.name = "Proceed";
	proceed.kind = "allow_once";
	pending.options.push_back(reject);
	pending.options.push_back(proceed);
	UAM_ASSERT_EQ(uam::AutoApproveOptionIdForTests(pending), std::string("proceed"));

	// Reject-only requests are never auto-approved.
	uam::AcpPendingPermissionState reject_only;
	reject_only.options.push_back(reject);
	UAM_ASSERT_EQ(uam::AutoApproveOptionIdForTests(reject_only), std::string(""));

	// Name-based fallback still works when kinds are absent.
	uam::AcpPendingPermissionState named;
	uam::AcpPermissionOptionState allow_named;
	allow_named.id = "opt-1";
	allow_named.name = "Allow once";
	named.options.push_back(allow_named);
	UAM_ASSERT_EQ(uam::AutoApproveOptionIdForTests(named), std::string("opt-1"));

	// "reject_always" kinds are skipped even when the name sounds positive.
	uam::AcpPendingPermissionState tricky;
	uam::AcpPermissionOptionState tricky_reject;
	tricky_reject.id = "always";
	tricky_reject.name = "Always";
	tricky_reject.kind = "reject_always";
	tricky.options.push_back(tricky_reject);
	UAM_ASSERT_EQ(uam::AutoApproveOptionIdForTests(tricky), std::string(""));
}

UAM_TEST(AcpPermissionResolutionRejectsUnofferedOptionAndAllowsSyntheticCancel)
{
	TempDir temp("uam-acp-permission-option-validation");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-permission-validation";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-permission-validation";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->running = true;
	session->processing = true;
	session->waiting_for_permission = true;
	session->pending_permission.request_id_json = "7";
	session->pending_permission.options.push_back({"allow-once", "Allow once", "allow_once"});
	session->pending_permission.options.push_back({"deny", "Deny", "reject_once"});
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	const bool forged_resolved = uam::ResolveAcpPermission(app, "chat-permission-validation", "7", "forged-allow", false, &error);
	const bool forged_rejected_as_unoffered = error.find("not offered") != std::string::npos;
	const bool forged_left_wait_pending = raw_session->waiting_for_permission &&
	                                      raw_session->pending_permission.request_id_json == "7";

	const bool offered_resolved = uam::ResolveAcpPermission(app, "chat-permission-validation", "7", "allow-once", false, &error);
	const bool offered_cleared_wait = !raw_session->waiting_for_permission &&
	                                  raw_session->pending_permission.request_id_json.empty();

	raw_session->processing = true;
	raw_session->waiting_for_permission = true;
	raw_session->pending_permission.request_id_json = "8";
	raw_session->pending_permission.options.push_back({"allow-once", "Allow once", "allow_once"});
	const bool cancelled_resolved = uam::ResolveAcpPermission(app, "chat-permission-validation", "8", "cancelled", true, &error);
	const bool cancelled_cleared_wait = !raw_session->waiting_for_permission &&
	                                    raw_session->pending_permission.request_id_json.empty();

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);

	UAM_ASSERT(!forged_resolved);
	UAM_ASSERT(forged_rejected_as_unoffered);
	UAM_ASSERT(forged_left_wait_pending);
	UAM_ASSERT(offered_resolved);
	UAM_ASSERT(offered_cleared_wait);
	UAM_ASSERT(cancelled_resolved);
	UAM_ASSERT(cancelled_cleared_wait);
}

UAM_TEST(CommandSafetyAppliesToStandardAcpExecutePermissions)
{
	TempDir temp("uam-acp-command-safety");
	uam::AppState app;
	app.data_root = temp.root;

	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	chat.command_safety_tier = "low";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-1";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":5,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-1","title":"Execute shell command","kind":"execute","status":"pending","rawInput":{"command":"rm -rf build","commands":["rm"]}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"},{"optionId":"deny","name":"Deny","kind":"reject_once"}]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.content, std::string("rm -rf build"));
	UAM_ASSERT_EQ(raw_session->pending_permission.safety_risk, std::string("warn_high"));
	UAM_ASSERT_EQ(raw_session->pending_permission.safety_tier, std::string("low"));
	UAM_ASSERT(raw_session->pending_permission.safety_requires_approval);

	raw_session->pending_permission = uam::AcpPendingPermissionState{};
	raw_session->waiting_for_permission = false;
	app.chats.front().command_safety_tier = "medium";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":6,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-2","title":"Execute shell command","kind":"execute","status":"pending","content":{"type":"text","text":"Read files"},"rawInput":{"command":"git reset --hard"}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"},{"optionId":"deny","name":"Deny","kind":"reject_once"}]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.content, std::string("git reset --hard"));
	UAM_ASSERT_EQ(raw_session->pending_permission.safety_risk, std::string("warn_high"));
	UAM_ASSERT(raw_session->pending_permission.safety_requires_approval);

	raw_session->pending_permission = uam::AcpPendingPermissionState{};
	raw_session->waiting_for_permission = false;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":7,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-3","title":"Execute shell commands","kind":"execute","status":"pending","content":{"type":"text","text":"Read files"},"rawInput":{"commands":["git reset --hard"]}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"},{"optionId":"deny","name":"Deny","kind":"reject_once"}]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT_EQ(raw_session->pending_permission.content, std::string(R"({"commands":["git reset --hard"]})"));
	UAM_ASSERT_EQ(raw_session->pending_permission.safety_risk, std::string("warn_high"));
	UAM_ASSERT(raw_session->pending_permission.safety_requires_approval);

	raw_session->pending_permission = uam::AcpPendingPermissionState{};
	raw_session->waiting_for_permission = false;
	app.chats.front().command_safety_tier = "off";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","id":8,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"tool-4","title":"Write file","kind":"execute","status":"pending","content":{"type":"text","text":"echo hello > output.txt"}},"options":[{"optionId":"allow-once","name":"Allow once","kind":"allow_once"},{"optionId":"deny","name":"Deny","kind":"reject_once"}]}})"));
	UAM_ASSERT(raw_session->waiting_for_permission);
	UAM_ASSERT(raw_session->pending_permission.safety_risk.empty());
	UAM_ASSERT(raw_session->pending_permission.safety_tier.empty());
	UAM_ASSERT(!raw_session->pending_permission.safety_requires_approval);
}

UAM_TEST(AcpQueuedUserPromptsPreserveFifoPayloadAndBeatGoalReview)
{
	TempDir temp("uam-acp-user-prompt-queue");
	const fs::path workspace = temp.root / "workspace";
	const fs::path markdown_store = temp.root / "markdown-store";
	fs::create_directories(workspace);
	fs::create_directories(markdown_store);
	const fs::path skill = markdown_store / "review.uam";
	const std::string skill_body_sentinel = "UAM_SKILL_BODY_SENTINEL";
	const std::string source_path_sentinel = "C:\\Users\\outside\\review.md";
	UAM_ASSERT(uam::io::WriteTextFile(skill, "---\ntitle: Review\nsourcePath: " + source_path_sentinel + "\n---\n# Review\n\n" + skill_body_sentinel + "\n"));
	const std::string normalized_skill = uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(skill));

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(app.provider_profiles.size() >= 2);
	app.settings.active_provider_id = app.provider_profiles.front().id;
	app.settings.markdown_store_directory = markdown_store.string();

	ChatSession chat;
	chat.id = "chat-queue";
	chat.provider_id = app.provider_profiles.front().id;
	chat.workspace_directory = workspace.string();
	Goal goal;
	goal.id = "goal-queue";
	goal.objective = "Finish queued work.";
	chat.active_goal_id = goal.id;
	chat.goals.push_back(goal);
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-queue";
	session->provider_id = app.provider_profiles.front().id;
	session->running = true;
	session->processing = true;
	session->session_ready = false;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	MessageAttachment attachment;
	attachment.id = "attachment-1";
	attachment.name = "diagram.png";
	attachment.kind = "image";
	attachment.mime_type = "image/png";
	attachment.path = "attachments/diagram.png";
	attachment.size_bytes = 42;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue", "First queued prompt", {skill.string()}, {attachment}, true, &error, goal.id));
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue", "Second queued prompt", {}, {}, true, &error, goal.id));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("First queued prompt\n\nSecond queued prompt"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().attachments.front().id, std::string("attachment-1"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().markdown_store_files.front(), normalized_skill);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().goal_id, goal.id);
	raw_session->processing = false;
	UAM_ASSERT_EQ(uam::SwitchChatProvider(app, "chat-queue", app.provider_profiles[1].id), uam::ChatProviderSwitchResult::ActiveRuntime);
	raw_session->processing = true;

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json queued = serialized["chats"][0]["acpSession"]["queuedPrompts"];
	UAM_ASSERT_EQ(queued.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(queued[0].value("text", ""), std::string("First queued prompt\n\nSecond queued prompt"));
	UAM_ASSERT_EQ(queued[0].value("goalId", ""), goal.id);
	UAM_ASSERT_EQ(queued[0]["attachments"][0].value("id", ""), std::string("attachment-1"));
	UAM_ASSERT(serialized.dump().find(skill_body_sentinel) == std::string::npos);

	const std::string changed_skill_sentinel = "UAM_CHANGED_SKILL_SENTINEL";
	UAM_ASSERT(uam::io::WriteTextFile(skill, "# Changed\n\n" + changed_skill_sentinel + "\n"));

	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw_session, app.chats.front(), "ready", nullptr);
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("First queued prompt\n\nSecond queued prompt"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].attachments.front().id, std::string("attachment-1"));
	UAM_ASSERT_EQ(app.chats.front().messages[0].markdown_store_files.front(), normalized_skill);
	UAM_ASSERT(raw_session->queued_prompt.find("First queued prompt\n\nSecond queued prompt") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(skill_body_sentinel) != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(changed_skill_sentinel) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(normalized_skill) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find(source_path_sentinel) == std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("attachments/diagram.png") != std::string::npos);
	UAM_ASSERT(!raw_session->goal_review_scheduled);

	raw_session->processing = true;
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue", "Cancel me", {}, {}, false, &error));
	raw_session->running = false;
	UAM_ASSERT(uam::CancelAcpTurn(app, "chat-queue", &error));
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	raw_session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Stop me"});
	UAM_ASSERT(uam::StopAcpSession(app, "chat-queue"));
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
}

UAM_TEST(AcpPromptRejectsOversizedCombinedSkillContent)
{
	TempDir temp("uam-acp-skill-prompt-limit");
	const fs::path markdown_store = temp.root / "markdown-store";
	fs::create_directories(markdown_store);
	const std::string body(1024U * 1024U + 1U, 'x');
	const fs::path first_skill = markdown_store / "first.uam";
	const fs::path second_skill = markdown_store / "second.uam";
	UAM_ASSERT(uam::io::WriteTextFile(first_skill, "# First\n" + body));
	UAM_ASSERT(uam::io::WriteTextFile(second_skill, "# Second\n" + body));

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = app.provider_profiles.front().id;
	app.settings.markdown_store_directory = markdown_store.string();
	ChatSession chat;
	chat.id = "chat-skill-limit";
	chat.provider_id = app.provider_profiles.front().id;
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(app, "chat-skill-limit", "Use both skills.", {first_skill.string(), second_skill.string()}, {}, false, &error));
	UAM_ASSERT(error.find("2 MiB prompt limit") != std::string::npos);
	UAM_ASSERT(app.chats.front().messages.empty());
}

UAM_TEST(AcpSmallModelModeCreatesGoalAndKeepsQueuedPromptsAtomic)
{
	TempDir temp("uam-acp-small-model-queue");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = app.provider_profiles.front().id;

	ChatSession chat;
	chat.id = "chat-small-model";
	chat.provider_id = app.provider_profiles.front().id;
	chat.workspace_directory = temp.root.string();
	chat.small_model_mode = true;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-small-model";
	session->provider_id = app.provider_profiles.front().id;
	session->running = true;
	session->session_ready = false;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-small-model", "Implement the requested feature.", {}, {}, false, &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().active_goal_id, app.chats.front().goals.front().id);
	UAM_ASSERT_EQ(app.chats.front().goals.front().objective, std::string("Implement the requested feature."));
	UAM_ASSERT(raw_session->queued_prompt.find("This is the planning turn") != std::string::npos);

	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-small-model", "Second instruction.", {}, {}, false, &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-small-model", "Third instruction.", {}, {}, false, &error));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("Second instruction."));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[1].text, std::string("Third instruction."));
}

UAM_TEST(AcpQueuedUserPromptFailureKeepsLaterPromptsInOrder)
{
	TempDir temp("uam-acp-user-prompt-failure");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = app.provider_profiles.front().id;
	ChatSession chat;
	chat.id = "chat-queue-failure";
	chat.provider_id = app.provider_profiles.front().id;
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-queue-failure";
	session->provider_id = app.provider_profiles.front().id;
	session->session_id = "session-1";
	session->running = true;
	session->processing = true;
	session->session_ready = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue-failure", "First", {}, {}, false, &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue-failure", "Second", {}, {}, false, &error));

	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw_session, app.chats.front(), "ready", nullptr);
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("First\n\nSecond"));
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT(uam::SendAcpPrompt(app, "chat-queue-failure", "Third", {}, {}, false, &error));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("First\n\nSecond\n\nThird"));
}

UAM_TEST(AcpSessionSetupResumesPreservedQueuedPromptsInOrder)
{
	TempDir temp("uam-acp-setup-resumes-queue");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-setup-resumes-queue";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-setup-resumes-queue";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->running = true;
	session->initialized = true;
	session->session_setup_request_id = 8;
	session->next_request_id = 9;
	session->pending_request_methods[8] = "session/new";
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"First preserved prompt"});
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Second preserved prompt"});
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::ProcessAcpLineForTests(
	    app,
	    *raw_session,
	    app.chats.front(),
	    R"({"jsonrpc":"2.0","id":8,"result":{"sessionId":"session-after-reconnect"}})"));

	UAM_ASSERT(raw_session->session_ready);
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 9);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(9), std::string("session/prompt"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages[0].content, std::string("First preserved prompt"));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Second preserved prompt"));
	UAM_ASSERT(raw_session->queued_prompt.empty());
	UAM_ASSERT(raw_session->last_error.empty());

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpSetupInactivityTimeoutStopsAndReconnectsWithoutDroppingQueuedWork)
{
	TempDir temp("uam-acp-setup-timeout");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-setup-timeout";
	chat.provider_id = "gemini-cli";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-setup-timeout";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->running = true;
	session->lifecycle_state = "starting";
	session->initialize_request_id = 1;
	session->pending_request_methods[1] = "initialize";
	session->last_runtime_activity_time_s = uam::GetAppTimeSeconds() - 3600.0;
	session->processing = true;
	session->queued_prompt = "Active undelivered prompt";
	session->turn_user_message_index = 0;
	session->turn_serial = 7;
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"First queued prompt"});
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Second queued prompt"});
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::PollAllAcpSessions(app));
	UAM_ASSERT(!raw_session->running);
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(raw_session->reconnect_pending);
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("error"));
	UAM_ASSERT(raw_session->last_error.find("setup timed out") != std::string::npos);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Active undelivered prompt"));
	UAM_ASSERT_EQ(raw_session->turn_user_message_index, 0);
	UAM_ASSERT_EQ(raw_session->turn_serial, 7);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("First queued prompt"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[1].text, std::string("Second queued prompt"));
	UAM_ASSERT(std::ranges::any_of(raw_session->diagnostics, [](const uam::AcpDiagnosticEntryState& diagnostic) {
		return diagnostic.reason == "setup_timeout";
	}));
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, raw_session->chat_id, 1, &error));
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, raw_session->chat_id, 0, &error));
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT(raw_session->reconnect_pending);
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT_EQ(raw_session->queued_prompt, std::string("Active undelivered prompt"));
	app.acp_sessions.clear();
	auto deferred_session = std::make_unique<uam::AcpSessionState>();
	deferred_session->chat_id = "chat-setup-timeout";
	deferred_session->provider_id = "gemini-cli";
	deferred_session->protocol_kind = "gemini-acp";
	raw_session = deferred_session.get();
	raw_session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Deferred Gemini prompt"});
	raw_session->reconnect_pending = true;
	app.acp_sessions.push_back(std::move(deferred_session));
	UAM_ASSERT(uam::AcpSessionHasDeferredUserQueueOnly(*raw_session));
	UAM_ASSERT(uam::SteerQueuedAcpPrompt(app, raw_session->chat_id, 0, &error));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("Deferred Gemini prompt"));
	UAM_ASSERT(raw_session->reconnect_pending);
	UAM_ASSERT(uam::SteerAcpPrompt(app, raw_session->chat_id, "New Gemini steering prompt", {}, {}, false, &error));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("New Gemini steering prompt"));
	UAM_ASSERT(raw_session->reconnect_pending);
}

UAM_TEST(AcpQueuedTurnAppliesDeferredCodexModeAndModelBeforeNextPrompt)
{
	TempDir temp("uam-acp-deferred-controls");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.active_provider_id = "codex-cli";
	ChatSession chat;
	chat.id = "chat-deferred-controls";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	chat.approval_mode = "plan";
	chat.model_id = "gpt-5.4-mini";
	app.chats.push_back(std::move(chat));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-deferred-controls";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->session_id = "thread-1";
	session->running = true;
	session->processing = false;
	session->session_ready = true;
	session->current_mode_id = "default";
	session->current_model_id = "gpt-5.4";
	uam::AcpModelState model;
	model.id = "gpt-5.4-mini";
	model.default_reasoning_effort = "high";
	model.supported_reasoning_efforts = {"low", "high", "xhigh"};
	session->available_models.push_back(std::move(model));
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	(void)uam::SendAcpPrompt(app, "chat-deferred-controls", "Use the new controls", {}, {}, false, &error);
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("gpt-5.4-mini"));
	UAM_ASSERT_EQ(app.chats.front().reasoning_effort, std::string("high"));
}

UAM_TEST(CodexIdleModelChangeUpdatesTheLiveSelectorState)
{
	TempDir temp("uam-codex-idle-model");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-codex-idle-model";
	chat.provider_id = "codex-cli";
	app.chats.push_back(std::move(chat));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-codex-idle-model";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->session_id = "thread-1";
	session->running = true;
	session->session_ready = true;
	session->current_model_id = "gpt-5.4";
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(uam::SetAcpSessionModel(app, "chat-codex-idle-model", "gpt-5.4-mini", &error));
	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("gpt-5.4-mini"));
}

UAM_TEST(AcpRejectedModelChangeRestoresPersistedAndRuntimeModel)
{
	TempDir temp("uam-acp-rejected-model-change");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-rejected-model-change";
	chat.provider_id = "gemini-cli";
	chat.model_id = "model-new";
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-rejected-model-change";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->session_id = "session-1";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->current_model_id = "model-old";
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif

	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	const int request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionModel(app, "chat-rejected-model-change", "model-new", &error, std::string{}));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(request_id), std::string("session/set_model"));

	const std::string rejection =
	    "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(request_id) +
	    ",\"error\":{\"code\":-32602,\"message\":\"Unsupported model\"}}";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), rejection));

	UAM_ASSERT_EQ(raw_session->current_model_id, std::string("model-old"));
	UAM_ASSERT_EQ(raw_session->model_change_request_id, 0);
	UAM_ASSERT(app.chats.front().model_id.empty());

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	UAM_ASSERT(serialized["chats"][0].value("modelId", "missing").empty());
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"].value("currentModelId", ""), std::string("model-old"));

	app.chats.front().model_id = "model-new";
	const int retry_request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionModel(app, "chat-rejected-model-change", "model-new", &error, std::string{}));
	UAM_ASSERT(uam::ProcessAcpLineForTests(
	    app,
	    *raw_session,
	    app.chats.front(),
	    nlohmann::json({{"jsonrpc", "2.0"}, {"id", retry_request_id}, {"result", nlohmann::json::object()}}).dump()));
	UAM_ASSERT_EQ(raw_session->lifecycle_state, std::string("ready"));
	UAM_ASSERT(raw_session->last_error.empty());

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpModeChangeFailuresRestorePersistedAndRuntimeMode)
{
	TempDir temp("uam-acp-mode-change-rollback");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-mode-change-rollback";
	chat.provider_id = "gemini-cli";
	chat.approval_mode = "plan";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-mode-change-rollback";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->session_id = "session-1";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->current_mode_id = "default";
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	const int rejected_request_id = raw_session->next_request_id;
	UAM_ASSERT(uam::SetAcpSessionMode(app, app.chats.front().id, "plan", &error, std::string("default")));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(rejected_request_id), std::string("session/set_mode"));

	const std::string rejection =
	    "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(rejected_request_id) +
	    ",\"error\":{\"code\":-32602,\"message\":\"Unsupported mode\"}}";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), rejection));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("default"));
	UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("default"));

	const nlohmann::json rejected_state = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(rejected_state["chats"][0].value("approvalMode", ""), std::string("default"));
	UAM_ASSERT_EQ(rejected_state["chats"][0]["acpSession"].value("currentModeId", ""), std::string("default"));

	app.chats.front().approval_mode = "plan";
	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
	error.clear();
	UAM_ASSERT(!uam::SetAcpSessionMode(app, app.chats.front().id, "plan", &error, std::string("default")));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("default"));
	UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("default"));
}

UAM_TEST(AcpQueuedPromptWaitsForModeChangeAcknowledgement)
{
	TempDir temp("uam-acp-mode-change-queued-prompt");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-mode-change-queued-prompt";
	chat.provider_id = "gemini-cli";
	chat.approval_mode = "plan";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-mode-change-queued-prompt";
	session->provider_id = "gemini-cli";
	session->protocol_kind = "gemini-acp";
	session->session_id = "session-1";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->current_mode_id = "default";
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Wait for the mode change.", {}, {}, false, &error));
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(1), std::string("session/set_mode"));
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 0);
	UAM_ASSERT(!raw_session->queued_prompt.empty());
	UAM_ASSERT(raw_session->processing);

	UAM_ASSERT(uam::ProcessAcpLineForTests(
	    app,
	    *raw_session,
	    app.chats.front(),
	    R"({"jsonrpc":"2.0","id":1,"result":{}})"));
	UAM_ASSERT_EQ(raw_session->current_mode_id, std::string("plan"));
	UAM_ASSERT_EQ(raw_session->prompt_request_id, 2);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(2), std::string("session/prompt"));
	UAM_ASSERT(raw_session->queued_prompt.empty());

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpSteerPrioritizesPromptPreservesQueueAndStartsAfterInterrupt)
{
	TempDir temp("uam-acp-steer");
	const fs::path store = temp.root / "store";
	fs::create_directories(store);
	const fs::path skill = store / "steer.uam";
	UAM_ASSERT(uam::io::WriteTextFile(skill, "# Steer\n"));

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.markdown_store_directory = store.string();
	ChatSession chat;
	chat.id = "chat-steer";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	Message assistant;
	assistant.role = MessageRole::Assistant;
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-steer";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->processing = true;
	session->session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	session->codex_turn_id = "turn-1";
	session->current_assistant_message_index = 0;
	session->tool_calls.push_back(uam::AcpToolCallState{"tool-1", "Write file", "write", "running", ""});
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Older queued"});
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string launch_error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &launch_error));
	app.acp_sessions.push_back(std::move(session));

	MessageAttachment attachment;
	attachment.id = "attachment-1";
	attachment.name = "notes.txt";
	attachment.kind = "file";
	attachment.path = "notes.txt";
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, "chat-steer", "Steer immediately", {skill.string()}, {attachment}, false, &error));
	UAM_ASSERT(raw_session->cancel_requested);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("Steer immediately"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[1].text, std::string("Older queued"));
	UAM_ASSERT_EQ(raw_session->tool_calls[0].status, std::string("cancelled"));
	UAM_ASSERT_EQ(raw_session->codex_turn_id, std::string("turn-1"));
	UAM_ASSERT(raw_session->cancel_request_id != 0);
	UAM_ASSERT_EQ(raw_session->pending_request_methods.at(raw_session->cancel_request_id), std::string("turn/interrupt"));

	const int interrupt_request_id = raw_session->cancel_request_id;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"turn/completed","params":{"turn":{"id":"turn-1","status":"interrupted"}}})"));
	UAM_ASSERT(!raw_session->cancel_requested);
	UAM_ASSERT_EQ(raw_session->cancel_request_id, 0);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("Older queued"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages[1].content, std::string("Steer immediately"));
	UAM_ASSERT_EQ(app.chats.front().messages[1].attachments.front().id, std::string("attachment-1"));

	const std::string late_interrupt_response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(interrupt_request_id) + ",\"result\":{}}";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), late_interrupt_response));
	UAM_ASSERT(raw_session->processing);

	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, *raw_session, app.chats.front(), "ready", nullptr);
	UAM_ASSERT(raw_session->queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(app.chats.front().messages[2].content, std::string("Older queued"));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpImmediateSendAfterSteerPreservesQueuedSkillSnapshotsInOrder)
{
	TempDir temp("uam-acp-steer-immediate-send");
	const fs::path store = temp.root / "store";
	fs::create_directories(store);
	const fs::path older_skill = store / "older.uam";
	const fs::path steer_skill = store / "steer.uam";
	const fs::path newest_skill = store / "newest.uam";
	UAM_ASSERT(uam::io::WriteTextFile(older_skill, "# Older\n\nOLDER_SKILL_SNAPSHOT\n"));
	UAM_ASSERT(uam::io::WriteTextFile(steer_skill, "# Steer\n\nSTEER_SKILL_SNAPSHOT\n"));
	UAM_ASSERT(uam::io::WriteTextFile(newest_skill, "# Newest\n\nNEWEST_SKILL_SNAPSHOT\n"));

#if defined(_WIN32)
	const fs::path shim = temp.root / "codex.cmd";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "@echo off\r\nmore > NUL\r\n"));
	const char path_separator = ';';
#else
	const fs::path shim = temp.root / "codex";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\ncat >/dev/null\n"));
	std::error_code permissions_error;
	fs::permissions(shim, fs::perms::owner_all, fs::perm_options::replace, permissions_error);
	UAM_ASSERT(!permissions_error);
	const char path_separator = ':';
#endif
	const char* existing_path = std::getenv("PATH");
	const std::string combined_path = temp.root.string() + (existing_path == nullptr ? "" : (std::string(1, path_separator) + existing_path));
	ScopedEnvVar scoped_path("PATH", combined_path);

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.markdown_store_directory = store.string();
	ChatSession chat;
	chat.id = "chat-steer-immediate-send";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	Message assistant;
	assistant.role = MessageRole::Assistant;
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = app.chats.front().id;
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->processing = true;
	session->session_id = "6a6f0f3b-1a0b-4a9c-8a01-333333333333";
	session->codex_turn_id = "turn-immediate-send";
	session->current_assistant_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Older queued", {older_skill.string()}, {}, false, &error));
	UAM_ASSERT(uam::SteerAcpPrompt(app, app.chats.front().id, "Steer immediately", {steer_skill.string()}, {}, false, &error));
	UAM_ASSERT(raw_session->cancel_requested);
	UAM_ASSERT(uam::io::WriteTextFile(older_skill, "# Changed\n\nCHANGED_OLDER_SKILL\n"));
	UAM_ASSERT(uam::io::WriteTextFile(steer_skill, "# Changed\n\nCHANGED_STEER_SKILL\n"));
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Newest queued", {newest_skill.string()}, {}, false, &error));
	UAM_ASSERT(uam::io::WriteTextFile(newest_skill, "# Changed\n\nCHANGED_NEWEST_SKILL\n"));

	UAM_ASSERT(raw_session->running);
	UAM_ASSERT(raw_session->processing);
	UAM_ASSERT(!raw_session->cancel_requested);
	UAM_ASSERT(raw_session->queued_prompt.find("Steer immediately") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("STEER_SKILL_SNAPSHOT") != std::string::npos);
	UAM_ASSERT(raw_session->queued_prompt.find("CHANGED_STEER_SKILL") == std::string::npos);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	const uam::AcpQueuedUserPromptState& remaining = raw_session->queued_user_prompts.front();
	UAM_ASSERT_EQ(remaining.text, std::string("Older queued\n\nNewest queued"));
	UAM_ASSERT_EQ(remaining.markdown_store_prompt_blocks.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(remaining.markdown_store_prompt_blocks[0].find("OLDER_SKILL_SNAPSHOT") != std::string::npos);
	UAM_ASSERT(remaining.markdown_store_prompt_blocks[1].find("NEWEST_SKILL_SNAPSHOT") != std::string::npos);
	UAM_ASSERT(remaining.markdown_store_prompt_blocks[0].find("CHANGED_OLDER_SKILL") == std::string::npos);
	UAM_ASSERT(remaining.markdown_store_prompt_blocks[1].find("CHANGED_NEWEST_SKILL") == std::string::npos);

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpQueuedPromptManagementRemovesAndPrioritizesSelectedPrompt)
{
	TempDir temp("uam-acp-queued-management");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-queued-management";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	Message assistant;
	assistant.role = MessageRole::Assistant;
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = "chat-queued-management";
	session->provider_id = "codex-cli";
	session->protocol_kind = "codex-app-server";
	session->running = true;
	session->initialized = true;
	session->session_ready = true;
	session->processing = true;
	session->session_id = "6a6f0f3b-1a0b-4a9c-8a01-222222222222";
	session->codex_turn_id = "turn-queued";
	session->current_assistant_message_index = 0;
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"First"});
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Second"});
	session->queued_user_prompts.push_back(uam::AcpQueuedUserPromptState{"Remove me"});
	uam::AcpSessionState* raw_session = session.get();

#if defined(_WIN32)
	const std::vector<std::string> sink_argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> sink_argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*raw_session, temp.root, sink_argv, &error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, "chat-queued-management", 2, &error));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(uam::SteerQueuedAcpPrompt(app, "chat-queued-management", 1, &error));
	UAM_ASSERT(raw_session->cancel_requested);
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[0].text, std::string("Second"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts[1].text, std::string("First"));

	const std::string interrupt_response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(raw_session->cancel_request_id) + ",\"result\":{}}";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), interrupt_response));
	UAM_ASSERT_EQ(app.chats.front().messages.back().content, std::string("Second"));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(raw_session->queued_user_prompts.front().text, std::string("First"));

	PlatformServicesFactory::Instance().process_service.StopStdioProcess(*raw_session, true);
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*raw_session);
}

UAM_TEST(AcpCompletedTurnStoresAssistantProcessingTime)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-duration";
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "Done";
	chat.messages.push_back(std::move(assistant));
	app.chats.push_back(std::move(chat));

	uam::AcpSessionState session;
	session.chat_id = "chat-duration";
	session.processing = true;
	session.current_assistant_message_index = 0;
	session.turn_assistant_message_index = 0;
	session.turn_started_time_s = uam::GetAppTimeSeconds() - 2.0;

	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, session, app.chats.front(), "ready", nullptr);

	UAM_ASSERT(app.chats.front().messages.front().processing_time_ms >= 1900);
	UAM_ASSERT_EQ(session.turn_started_time_s, 0.0);
}
