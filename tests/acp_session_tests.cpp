#include "test_harness.h"

using namespace uam_test;

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

	const nlohmann::json serialized = uam::StateSerializer::Serialize(app);
	const nlohmann::json acp = serialized["chats"][0]["acpSession"];
	UAM_ASSERT_EQ(acp["availableModes"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModeId", ""), std::string("plan"));
	UAM_ASSERT_EQ(acp["availableModels"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(acp.value("currentModelId", ""), std::string("auto-gemini-3"));
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

	UAM_ASSERT(uam::ProcessAcpLineForTests(app, *raw_session, app.chats.front(), R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"tool_call","toolCallId":"tool-1","title":"Read file","kind":"read","status":"completed","content":{"type":"text","text":"file contents"}}}})"));
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
