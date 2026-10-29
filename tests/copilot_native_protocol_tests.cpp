#include "test_harness.h"
#include "cef/state_serializer.h"
#include "app/copilot_agent_history_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/provider/copilot_native_history.h"
#include "common/provider/copilot_protocol.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/structured_framing.h"
#include "remote/runner_proxy.h"

#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#endif

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

struct CopilotPrivateWriter
{
	uam::AcpSessionState& session;
	std::FILE* file = std::tmpfile();
	explicit CopilotPrivateWriter(uam::AcpSessionState& target) : session(target)
	{
		UAM_ASSERT(file != nullptr);
#if defined(_WIN32)
		session.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
#else
		session.stdin_write_fd = fileno(file);
#endif
		session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
		    [](const char*, std::size_t count, std::string&) -> std::ptrdiff_t { return static_cast<std::ptrdiff_t>(count); }, [] {});
	}
	~CopilotPrivateWriter()
	{
		session.stdin_writer.reset();
#if defined(_WIN32)
		session.stdin_write = INVALID_HANDLE_VALUE;
#else
		session.stdin_write_fd = -1;
#endif
		std::fclose(file);
	}
};

nlohmann::json CopilotEvent(const std::string& type, const nlohmann::json& data,
    const std::string& id = {}, const std::string& session_id = "native-session")
{
	return {{"jsonrpc", "2.0"}, {"method", "session.event"}, {"params", {
	    {"sessionId", session_id}, {"event", {{"type", type}, {"id", id}, {"data", data}}}}}};
}

}

UAM_TEST(CopilotNativeRequestsUseSdkSessionProtocol)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AcpSessionState session;
	ChatSession chat;
	chat.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
	chat.model_id = "claude-sonnet";
	chat.reasoning_effort = "high";
	chat.approval_mode = "plan";
	session.session_id = chat.native_session_id;
	std::string method;
	const nlohmann::json resume = runtime.OnAcpBuildSetupRequest(4, chat, "/workspace", true, method);
	UAM_ASSERT_EQ(method, std::string("session.resume"));
	UAM_ASSERT_EQ(resume["method"], "session.resume");
	UAM_ASSERT_EQ(resume["params"]["sessionId"], chat.native_session_id);
	UAM_ASSERT_EQ(resume["params"]["workingDirectory"], "/workspace");
	UAM_ASSERT_EQ(resume["params"]["requestPermission"], true);
	UAM_ASSERT_EQ(resume["params"]["streaming"], true);
	UAM_ASSERT_EQ(resume["params"]["reasoningEffort"], "high");
	const nlohmann::json prompt = runtime.OnAcpBuildPrompt(session, 5, "Review this", chat, method);
	UAM_ASSERT_EQ(prompt["method"], "session.send");
	UAM_ASSERT_EQ(prompt["params"]["agentMode"], "plan");
	UAM_ASSERT_EQ(runtime.OnAcpBuildSteer(session, 6, "Also check", method)["params"]["mode"], "immediate");
	UAM_ASSERT_EQ(runtime.OnAcpBuildCompact(session, 7, method)["method"], "session.history.compact");
	UAM_ASSERT_EQ(runtime.OnAcpBuildCancel(session, 8, method)["method"], "session.abort");
	chat.native_session_id.clear();
	UAM_ASSERT_EQ(runtime.OnAcpBuildSetupRequest(9, chat, "/workspace", true, method)["method"], "session.create");
}

UAM_TEST(CopilotNativeMcpConversionPreservesEnvironmentAndIsIdempotent)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	nlohmann::json message = {{"method", "session.create"}, {"params", {{"mcpServers", nlohmann::json::array({
	    {{"name", "test"}, {"command", "/test/server"}, {"args", {"--stdio"}},
	     {"env", nlohmann::json::array({{{"name", "TEST_VALUE"}, {"value", "retained"}}})}}})}}}};
	runtime.PrepareStructuredMessage(message);
	UAM_ASSERT(message["params"]["mcpServers"].is_object());
	UAM_ASSERT_EQ(message["params"]["mcpServers"]["test"]["env"]["TEST_VALUE"], "retained");
	UAM_ASSERT_EQ(message["params"]["mcpServers"]["test"]["tools"], nlohmann::json::array({"*"}));
	const nlohmann::json normalized = message;
	runtime.PrepareStructuredMessage(message);
	UAM_ASSERT_EQ(message, normalized);
}

UAM_TEST(CopilotNativeSendAcknowledgmentDoesNotCompleteTurn)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	session.processing = true;
	session.turn_serial = 1;
	session.session_id = "native-session";
	std::string method;
	(void)runtime.OnAcpBuildPrompt(session, 4, "test", chat, method);
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.send", "4", {{"messageId", "message-1"}}));
	UAM_ASSERT(session.processing);
}

UAM_TEST(CopilotNativeCorrelatedSendAcknowledgmentDoesNotUseAcpCompletion)
{
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	chat.id = "native-correlated";
	chat.provider_id = "copilot-cli";
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.processing = true;
	session.prompt_request_id = 4;
	session.pending_request_methods[4] = "session.send";
	UAM_ASSERT(uam::acp_detail::ProcessAcpLine(app, session, chat,
	    R"({"jsonrpc":"2.0","id":4,"result":{"messageId":"accepted"}})", nullptr));
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
}

UAM_TEST(CopilotNativeStreamFinalAndReplayDoNotDuplicateText)
{
	TempDir root("uam-copilot-native");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	uam::AcpSessionState session;
	ChatSession chat;
	chat.id = "copilot-native-stream";
	chat.provider_id = "copilot-cli";
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.session_id = "native-session";
	session.processing = true;
	session.turn_serial = 1;
	std::string method;
	(void)runtime.OnAcpBuildPrompt(session, 1, "test", chat, method);
	const nlohmann::json delta = CopilotEvent("assistant.message_delta", {{"messageId", "message-1"}, {"deltaContent", "Hello"}}, "event-1");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, delta, nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, delta, nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("assistant.message", {{"messageId", "message-1"}, {"content", "Hello world"}}, "event-2"), nullptr));
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Hello world"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("assistant.message_delta", {{"messageId", "other"}, {"deltaContent", "wrong"}}, "foreign", "foreign-session"), nullptr));
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Hello world"));
}

UAM_TEST(CopilotNativePermissionResponsesRespectTransportAndDenial)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AcpSessionState session;
	session.session_id = "native-session";
	session.pending_permission.request_id_json = "\"permission-1\"";
	session.pending_permission.provider_request_method = "permission.request";
	const nlohmann::json legacy = runtime.OnAcpBuildPermissionResponse(session, "allow_once", false);
	UAM_ASSERT_EQ(legacy["id"], "permission-1");
	UAM_ASSERT_EQ(legacy["result"]["result"]["kind"], "approved");
	session.pending_permission.provider_request_method = "permission.requested";
	const nlohmann::json event = runtime.OnAcpBuildPermissionResponse(session, "reject_once", false);
	UAM_ASSERT_EQ(event["method"], "session.permissions.handlePendingPermissionRequest");
	UAM_ASSERT_EQ(event["params"]["requestId"], "permission-1");
	UAM_ASSERT_EQ(event["params"]["result"]["kind"], "reject");
	UAM_ASSERT_EQ(runtime.OnAcpBuildPermissionResponse(session, "allow_once", true)["params"]["result"]["kind"], "user-not-available");
}

UAM_TEST(CopilotNativeUnsupportedProtocolAndFailedCompactionRemainExplicit)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	runtime.OnAcpInitializeResult(session, {{"protocolVersion", 99}});
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("error"));
	UAM_ASSERT(!session.last_error.empty());
	session.manual_compaction_pending = true;
	session.processing = true;
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.history.compact", "5", {{"success", false}}));
	UAM_ASSERT(!session.manual_compaction_pending);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(!session.last_error.empty());
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeFramedUnicodeAndRemoteMarkersReachAdapter)
{
	TempDir root("uam-copilot-frames");
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "copilot-native-frames";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.remote_output_delivery_token = "private-token";
	session.running = true;
	session.processing = true;
	const std::string first = uam::structured_framing::Encode(CopilotEvent("assistant.message_delta", {{"messageId", "message"}, {"deltaContent", "Hello \u00e9"}}, "first").dump());
	const std::size_t split = first.find("Hello") + 7; // Split inside the two-byte UTF-8 character.
	UAM_ASSERT(uam::acp_detail::AppendAcpStdoutChunk(session, std::string_view(first).substr(0, split)));
	UAM_ASSERT_EQ(uam::acp_detail::ProcessBufferedAcpStdoutForTests(app, session, chat, nullptr, 16), std::size_t(0));
	const std::string marker = std::string(uam::remote::kRemoteOutputMarkerPrefix) + "private-token 120 0\n";
	const std::string second = uam::structured_framing::Encode(CopilotEvent("assistant.message", {{"messageId", "message"}, {"content", "Hello \u00e9 world"}}, "second").dump());
	UAM_ASSERT(uam::acp_detail::AppendAcpStdoutChunk(session, first.substr(split) + marker + second));
	UAM_ASSERT_EQ(uam::acp_detail::ProcessBufferedAcpStdoutForTests(app, session, chat, nullptr, 16), std::size_t(3));
	UAM_ASSERT(session.stdout_buffer.empty());
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Hello \u00e9 world"));
	UAM_ASSERT_EQ(session.pending_remote_stdout_cursor, std::uintmax_t(120));
}

UAM_TEST(CopilotNativeQuestionResponsesAndManagedApprovalsRemainExplicit)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	session.session_id = "native-session";
	const nlohmann::json question = {{"jsonrpc", "2.0"}, {"id", "question-1"}, {"method", "userInput.request"},
	    {"params", {{"sessionId", "native-session"}, {"question", "Which option?"}, {"choices", {"One", "Two"}}, {"allowFreeform", false}}}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, question, nullptr));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT(!session.pending_user_input.questions.front().is_other);
	UAM_ASSERT_EQ(runtime.OnAcpBuildUserInputResponse(session, {{"answer", {"One"}}})["result"], (nlohmann::json{{"answer", "One"}, {"wasFreeform", false}}));
	UAM_ASSERT(runtime.OnAcpBuildUserInputResponse(session, {}).contains("error"));
	uam::AcpPendingPermissionState pending;
	pending.provider_request_params_json = R"({"kind":"shell","managedApprovalRequired":true})";
	UAM_ASSERT(runtime.AcpPermissionRequiresUserDecision(pending));
	pending.provider_request_params_json = R"({"kind":"shell","requestSandboxBypass":true})";
	UAM_ASSERT(runtime.AcpPermissionRequiresUserDecision(pending));
}

UAM_TEST(CopilotNativeIdleSettlesOnlyAnOwnedTurn)
{
	TempDir root("uam-copilot-idle");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "copilot-native-idle";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.session_id = "native-session";
	session.processing = true;
	session.turn_serial = 3;
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("session.idle", nlohmann::json::object(), "early"), nullptr));
	UAM_ASSERT(session.processing);
	std::string method;
	(void)runtime.OnAcpBuildPrompt(session, 8, "Review", chat, method);
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.send", "8", {{"messageId", "message"}}));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("session.idle", nlohmann::json::object(), "complete"), nullptr));
	UAM_ASSERT(!session.processing);
}

UAM_TEST(CopilotNativeResumeFailureRetainsSavedIdentity)
{
	TempDir root("uam-copilot-resume");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "copilot-native-resume";
	chat.provider_id = "copilot-cli";
	chat.native_session_id = "saved-native";
	uam::AcpSessionState session;
	session.session_id = chat.native_session_id;
	session.session_setup_request_id = 3;
	uam::acp_detail::AcpResponseFailureDetails failure;
	failure.failure.method = "session.resume";
	failure.formatted_error = "Native session unavailable";
	UAM_ASSERT(runtime.OnAcpHandleError(app, session, chat, failure));
	UAM_ASSERT_EQ(chat.native_session_id, std::string("saved-native"));
	UAM_ASSERT_EQ(session.session_setup_request_id, 0);
	UAM_ASSERT(!session.session_ready);
	UAM_ASSERT_EQ(session.last_error, failure.formatted_error);
}

UAM_TEST(CopilotNativeDurableReceiptsPreventReplayAfterControllerRestart)
{
	TempDir root("uam-copilot-receipts");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "copilot-native-receipts";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState first;
	first.chat_id = chat.id;
	first.provider_id = chat.provider_id;
	first.session_id = "native-session";
	first.turn_serial = 1;
	first.processing = true;
	std::string method;
	(void)runtime.OnAcpBuildPrompt(first, 1, "test", chat, method);
	const nlohmann::json delta = CopilotEvent("assistant.message_delta", {{"messageId", "message"}, {"deltaContent", "Saved text"}}, "saved-event");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, first, chat, delta, nullptr));
	UAM_ASSERT_EQ(chat.messages.back().provider_event_ids, (std::vector<std::string>{"native-session:saved-event"}));
	ProviderProfile profile;
	UAM_ASSERT(runtime.SaveHistory(profile, root.root, chat));
	std::vector<ChatSession> loaded = runtime.LoadHistory(profile, root.root, {}, {});
	UAM_ASSERT_EQ(loaded.size(), std::size_t(1));
	chat = std::move(loaded.front());
	uam::AcpSessionState recovered;
	recovered.chat_id = chat.id;
	recovered.provider_id = chat.provider_id;
	recovered.session_id = "native-session";
	recovered.turn_serial = 1;
	recovered.processing = true;
	recovered.current_assistant_message_index = 0;
	uam::acp_detail::RestoreTurnEventsFromMessageBlocks(recovered, chat.messages.front());
	runtime.RestoreAcpTurnIdentity(recovered, "native-session:1");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, recovered, chat, delta, nullptr));
	UAM_ASSERT_EQ(chat.messages.front().content, std::string("Saved text"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, recovered, chat,
	    CopilotEvent("assistant.message", {{"messageId", "message"}, {"content", "Saved text continued"}}, "final-event"), nullptr));
	UAM_ASSERT_EQ(chat.messages.front().content, std::string("Saved text continued"));
}

UAM_TEST(CopilotNativeCompactionAcknowledgmentAndEventShareOneSummary)
{
	TempDir root("uam-copilot-compact");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "copilot-native-compact";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.session_id = "native-session";
	session.processing = true;
	session.manual_compaction_pending = true;
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("session.compaction_start", {{"compactionAttemptId", "attempt"}}, "start"), nullptr));
	const nlohmann::json result = {{"success", true}, {"summaryContent", "Retained context"}, {"compactionAttemptId", "attempt"}};
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.history.compact", "10", result));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("session.compaction_complete", result, "complete"), nullptr));
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(chat.messages.front().blocks.size(), std::size_t(1));
	UAM_ASSERT_EQ(chat.messages.front().blocks.front().text, std::string("Retained context"));
	UAM_ASSERT(!session.processing);
}

UAM_TEST(CopilotNativeHistoryRetainsToolsReasoningCompactionAndExactChildren)
{
	TempDir root("uam-copilot-history");
	const auto directory = root.root / "11111111-1111-4111-8111-111111111111";
	std::filesystem::create_directories(directory);
	UAM_ASSERT(uam::io::WriteTextFile(directory / "workspace.yaml", "cwd: \"/workspace\"\n"));
	const auto row = [](const char* type, const nlohmann::json& data)
	{
		return nlohmann::json{{"type", type}, {"data", data}, {"timestamp", "2026-10-16T12:00:00Z"}}.dump() + "\n";
	};
	const std::string transcript = row("session.start", {{"sessionId", directory.filename().string()}, {"detachedFromSpawningParentSessionId", "parent-native"}}) +
	    row("user.message", {{"content", "Inspect source"}}) +
	    row("assistant.reasoning", {{"reasoningId", "reason"}, {"content", "Inspect the file"}}) +
	    row("tool.execution_start", {{"toolCallId", "read-1"}, {"toolName", "view"}, {"arguments", {{"path", "/workspace/file"}}}}) +
	    row("tool.execution_complete", {{"toolCallId", "read-1"}, {"success", true}, {"result", {{"content", "File contents"}}}}) +
	    row("assistant.message", {{"messageId", "message"}, {"content", "Found the issue"}}) +
	    row("session.compaction_complete", {{"success", true}, {"summaryContent", "Retained summary"}, {"compactionAttemptId", "compact"}});
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", transcript + "{\"type\":"));
	std::string error;
	auto child = LoadCopilotSessionStateChat(directory, "/workspace", {}, &error);
	UAM_ASSERT(child.has_value());
	UAM_ASSERT_EQ(child->parent_chat_id, std::string("parent-native"));
	UAM_ASSERT_EQ(child->messages.front().content, std::string("Inspect source"));
	UAM_ASSERT_EQ(child->messages.back().thoughts, std::string("Inspect the file"));
	UAM_ASSERT_EQ(child->messages.back().tool_calls.size(), std::size_t(1));
	UAM_ASSERT_EQ(child->messages.back().tool_calls.front().result_text, std::string("File contents"));
	UAM_ASSERT_EQ(child->messages.back().tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(std::ranges::any_of(child->messages.back().blocks, [](const MessageBlock& block) { return block.type == "context_compaction" && block.text == "Retained summary"; }));
	UAM_ASSERT(LoadCopilotSessionStateChats(root.root, "/workspace", {}, &error).empty());
	UAM_ASSERT(!LoadCopilotSessionStateChat(directory, "/other", {}, &error));
}

UAM_TEST(CopilotNativeAbortAcknowledgmentRetainsMaintenanceUntilCompactSettles)
{
	TempDir root("uam-copilot-cancel-compact");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "cancel-compact";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState session;
	session.session_id = "native-session";
	session.manual_compaction_pending = true;
	session.processing = true;
	session.cancel_requested = true;
	session.cancel_request_id = 12;
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.abort", "12", nlohmann::json::object()));
	UAM_ASSERT(session.manual_compaction_pending);
	UAM_ASSERT(session.processing);
	std::string method;
	UAM_ASSERT(runtime.OnAcpBuildPrompt(session, 13, "New prompt", chat, method).is_null());
	UAM_ASSERT(runtime.OnAcpBuildSteer(session, 13, "New steer", method).is_null());
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.history.compact", "11", {{"success", false}}));
	UAM_ASSERT(!session.manual_compaction_pending);
	session.processing = true;
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.history.compact", "11", {{"success", true}, {"summaryContent", "Late"}}));
	UAM_ASSERT(session.processing);
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeRemoteCompactionResultReusesDurableCompletedDivider)
{
	TempDir root("uam-copilot-remote-compact");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "remote-compact";
	chat.provider_id = "copilot-cli";
	chat.execution_host_id = "remote";
	chat.remote_manual_compaction_pending = true;
	uam::AcpRemotePendingRequestState pending;
	pending.request_id = 7;
	pending.method = "session.history.compact";
	chat.remote_pending_requests.push_back(pending);
	uam::AcpSessionState session;
	session.session_id = "native-session";
	session.manual_compaction_pending = true;
	session.processing = true;
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("session.compaction_complete", {{"success", true}, {"summaryContent", "Durable summary"}, {"compactionAttemptId", "attempt"}}, "native-completed"), nullptr));
	UAM_ASSERT(!chat.remote_manual_compaction_boundary_json.empty());
	uam::AcpSessionState recovered;
	recovered.session_id = session.session_id;
	recovered.manual_compaction_pending = true;
	recovered.processing = true;
	UAM_ASSERT(runtime.OnAcpHandleResult(app, recovered, chat, "session.history.compact", "7", {{"success", true}, {"summaryContent", "Durable summary"}}));
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(chat.messages.front().blocks.size(), std::size_t(1));
	UAM_ASSERT(!recovered.processing);
}

UAM_TEST(CopilotNativeUnansweredPermissionsSurviveResetAndSavedAnswersReplay)
{
	TempDir root("uam-copilot-permission-replay");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "permission-replay";
	chat.provider_id = "copilot-cli";
	chat.execution_host_id = "remote";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.processing = true;
	const nlohmann::json event = CopilotEvent("permission.requested", {{"requestId", "permission"}, {"permissionRequest", {{"kind", "shell"}, {"fullCommandText", "echo private-fixture"}, {"managedApprovalRequired", true}}}}, "permission-event");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, event, nullptr));
	UAM_ASSERT(session.waiting_for_permission);
	session.recovering_remote_process = true;
	uam::acp_detail::ResetAcpRuntimeState(app, session, chat);
	session.session_id = "native-session";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, event, nullptr));
	UAM_ASSERT(session.waiting_for_permission);
	const nlohmann::json saved = runtime.OnAcpBuildPermissionResponse(session, "allow_once", false);
	chat.remote_interaction_responses.push_back({"\"permission\"", saved.dump()});
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	CopilotPrivateWriter writer(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, event, nullptr));
	UAM_ASSERT(!session.waiting_for_permission);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat,
	    {{"jsonrpc", "2.0"}, {"id", saved["id"]}, {"error", {{"code", -32603}, {"message", "Permission request not found"}}}}, nullptr));
	UAM_ASSERT(session.last_error.empty());
}

UAM_TEST(CopilotNativeSetupSaveFailureRetainsUnsentInputAndNativeIdentity)
{
	TempDir root("uam-copilot-setup-save-failure");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root / "blocked-data-root";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "blocked"));
	ChatSession chat;
	chat.id = "setup-save-failure";
	chat.provider_id = "copilot-cli";
	chat.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = chat.native_session_id;
	session.processing = true;
	session.queued_prompt = "Keep this unsent instruction";
	session.queued_user_prompts.push_back({.text = "Keep this followup"});
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.resume", "7", {{"sessionId", chat.native_session_id}}));
	UAM_ASSERT_EQ(chat.native_session_id, std::string("550e8400-e29b-41d4-a716-446655440000"));
	UAM_ASSERT_EQ(session.queued_prompt, std::string("Keep this unsent instruction"));
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t(1));
	UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Keep this followup"));
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(session.queue_held);
	UAM_ASSERT(!session.reconnect_pending);
	UAM_ASSERT(session.last_error.find("saved") != std::string::npos);
}

UAM_TEST(CopilotNativeWireMigrationRequiresAnOldOwnedRemoteProcess)
{
	ChatSession chat;
	chat.execution_host_id = "remote";
	chat.remote_process_exists = true;
	UAM_ASSERT(uam::copilot::NeedsNativeWireMigration(chat, "copilot-cli"));
	chat.remote_structured_protocol = "copilot-acp";
	UAM_ASSERT(uam::copilot::NeedsNativeWireMigration(chat, "copilot-cli"));
	chat.remote_structured_protocol = "copilot-sdk";
	UAM_ASSERT(!uam::copilot::NeedsNativeWireMigration(chat, "copilot-cli"));
	chat.remote_structured_protocol.clear();
	chat.execution_host_id = "local";
	UAM_ASSERT(!uam::copilot::NeedsNativeWireMigration(chat, "copilot-cli"));
	chat.execution_host_id = "remote";
	chat.remote_process_exists = false;
	UAM_ASSERT(!uam::copilot::NeedsNativeWireMigration(chat, "copilot-cli"));
	chat.remote_process_exists = true;
	UAM_ASSERT(!uam::copilot::NeedsNativeWireMigration(chat, "gemini-cli"));
}

UAM_TEST(CopilotAgentHistoryUsesActualParentEventsAndReadOnlyScopedReferences)
{
	TempDir root("uam-copilot-agent-history");
	const std::string parent = "550e8400-e29b-41d4-a716-446655440000";
	const std::string agent = "native-agent-1";
	const auto workspace = root.root / "workspace";
	fs::create_directories(workspace);
	const auto directory = root.root / "home" / "session-state" / parent;
	ScopedEnvVar home("COPILOT_HOME", (root.root / "home").string());
	const nlohmann::json records = nlohmann::json::array({
	    {{"id", "start"}, {"type", "session.start"}, {"data", {{"sessionId", parent}, {"context", {{"cwd", workspace.string()}}}}}},
	    {{"id", "root-user"}, {"type", "user.message"}, {"data", {{"content", "Root instruction"}}}},
	    {{"id", "root-tool"}, {"type", "tool.execution_start"}, {"data", {{"toolCallId", "spawn"}, {"toolName", "task"}}}},
	    {{"id", "agent-start"}, {"type", "subagent.started"}, {"agentId", agent}, {"data", {{"toolCallId", "spawn"}, {"agentDisplayName", "Reviewer"}, {"resumable", false}}}},
	    {{"id", "other-agent"}, {"type", "assistant.message"}, {"agentId", "different-agent"}, {"data", {{"messageId", "other"}, {"content", "Never mix another agent"}}}},
	    {{"id", "agent-reason"}, {"type", "assistant.reasoning"}, {"agentId", agent}, {"data", {{"reasoningId", "reason"}, {"content", "Actual reasoning"}}}},
	    {{"id", "agent-tool"}, {"type", "tool.execution_start"}, {"agentId", agent}, {"data", {{"toolCallId", "read"}, {"parentToolCallId", "spawn"}, {"toolName", "view"}, {"arguments", {{"path", "actual.txt"}}}}}},
	    {{"id", "agent-result"}, {"type", "tool.execution_complete"}, {"agentId", agent}, {"data", {{"toolCallId", "read"}, {"success", true}, {"result", {{"content", "Actual file text"}}}}}},
	    {{"id", "agent-text"}, {"type", "assistant.message"}, {"agentId", agent}, {"data", {{"messageId", "answer"}, {"parentToolCallId", "spawn"}, {"content", "Actual reply https://private.invalid/image?X-Amz-Signature=private-secret&expires=1"}}}},
	    {{"id", "root-text"}, {"type", "assistant.message"}, {"data", {{"messageId", "root-answer"}, {"content", "Root reply"}}}},
	});
	std::string text;
	for (const auto& record : records) text += record.dump() + "\n";
	// The journal contains real records, not a summary manufactured from the task result.
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", text));
	ChatSession source;
	source.id = "parent-chat";
	source.provider_id = "copilot-cli";
	source.native_session_id = parent;
	source.execution_host_id = "local";
	source.workspace_directory = workspace.string();
	const std::string reference = BuildCopilotAgentHistoryId(parent, agent, "local");
	const auto decoded = ParseCopilotAgentHistoryId(reference);
	UAM_ASSERT(decoded && decoded->parent_session_id == parent && decoded->agent_id == agent && decoded->execution_host_id == "local");
	UAM_ASSERT(reference.size() <= 240);
	UAM_ASSERT(BuildCopilotAgentHistoryId(parent, std::string(128, 'a'), "local").empty());
	UAM_ASSERT(!ParseCopilotAgentHistoryId(reference + "-00"));
	std::string error;
	auto child = uam::copilot::LoadAgentHistory(source, reference, std::nullopt, &error);
	UAM_ASSERT(child.has_value());
	UAM_ASSERT(error.empty());
	UAM_ASSERT(child->imported_read_only);
	UAM_ASSERT(child->native_session_id.empty());
	UAM_ASSERT_EQ(child->parent_chat_id, parent);
	UAM_ASSERT_EQ(child->execution_host_id, std::string("local"));
	UAM_ASSERT_EQ(child->messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(child->messages.front().thoughts, std::string("Actual reasoning"));
	UAM_ASSERT_EQ(child->messages.front().tool_calls.size(), std::size_t(1));
	UAM_ASSERT_EQ(child->messages.front().tool_calls.front().result_text, std::string("Actual file text"));
	UAM_ASSERT(child->messages.front().content.starts_with("Actual reply "));
	UAM_ASSERT_EQ(child->messages.front().content, std::string("Actual reply https://private.invalid/image?X-Amz-Signature=private-secret&expires=1"));
	const auto parent_chat = LoadCopilotSessionStateChat(directory, workspace);
	UAM_ASSERT(parent_chat.has_value());
	UAM_ASSERT_EQ(parent_chat->messages.size(), std::size_t(2));
	UAM_ASSERT_EQ(parent_chat->messages.back().content, std::string("Root reply"));
	UAM_ASSERT_EQ(parent_chat->messages.back().tool_calls.front().sub_agent_id, reference);
	ChatSession foreign = source;
	foreign.execution_host_id = "another-host";
	UAM_ASSERT(!uam::copilot::LoadAgentHistory(foreign, reference, std::nullopt, &error));
	foreign = source;
	foreign.native_session_id = "different-parent";
	UAM_ASSERT(!uam::copilot::LoadAgentHistory(foreign, reference, std::nullopt, &error));

	uam::AppState app;
	app.data_root = root.root / "data";
	app.chats.push_back(source);
	const ProviderProfile profile = ProviderProfileStore::DefaultCopilotProfile();
	ChatSession* imported = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &*child, true);
	UAM_ASSERT(imported && imported->native_session_id.empty() && imported->imported_read_only);
	UAM_ASSERT(!app.resolved_native_sessions_by_chat_id.contains(reference));
	child->messages.front().content = "Actual refreshed reply";
	UAM_ASSERT(ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &*child, true));
	UAM_ASSERT_EQ(app.chats.back().messages.front().content, std::string("Actual refreshed reply"));
	ChatSession forged = *child;
	forged.parent_chat_id = "wrong-parent";
	UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &forged, true));
	forged = *child;
	forged.execution_host_id = "another-host";
	UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &forged, true));
	app.chats.back().imported_read_only = false;
	UAM_ASSERT(!ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(app, source, profile, reference, false, &*child, true));

	ExecutionHost host;
	host.id = "ssh-host";
	host.transport = "ssh";
	// The snapshot reuses this controller's real workspace path, so the host platform must match its syntax.
#if defined(_WIN32)
	host.platform = "windows";
#else
	host.platform = "linux";
#endif
	ChatSession remote = source;
	remote.execution_host_id = host.id;
	const std::string remote_reference = BuildCopilotAgentHistoryId(parent, agent, host.id);
	const nlohmann::json snapshot = {{"providerId", "copilot-cli"}, {"sessionId", parent}, {"workspace", workspace.string()}, {"text", text}};
	auto remote_child = uam::copilot::ParseAgentHistorySnapshot(host, remote, remote_reference, snapshot, &error);
	UAM_ASSERT(remote_child && remote_child->native_session_id.empty() && remote_child->execution_host_id == host.id);
	UAM_ASSERT_EQ(remote_child->messages.front().thoughts, std::string("Actual reasoning"));
	UAM_ASSERT(!uam::copilot::ParseAgentHistorySnapshot(host, remote, reference, snapshot, &error));
}


UAM_TEST(CopilotNativePlanReviewRequiresExplicitSupportedApproval)
{
	TempDir root("uam-copilot-plan-review");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "plan-review";
	chat.provider_id = "copilot-cli";
	chat.approval_mode = "plan";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.processing = true;
	session.current_mode_id = "plan";
	const nlohmann::json params = {{"sessionId", session.session_id}, {"summary", "Review this plan"}, {"planContent", "Actual full plan content"}, {"actions", {"exit_only", "interactive", "autopilot", "autopilot_fleet"}}, {"recommendedAction", "autopilot"}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", "plan-request"}, {"method", "exitPlanMode.request"}, {"params", params}}, nullptr));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT_EQ(session.pending_user_input.questions.front().options.size(), std::size_t(3));
	UAM_ASSERT(session.pending_user_input.questions.front().question.find("Actual full plan content") != std::string::npos);
	UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
	const auto cancelled = runtime.OnAcpBuildUserInputResponse(session, {});
	UAM_ASSERT_EQ(cancelled["result"]["approved"], false);
	const auto feedback = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Please change the plan"}}});
	UAM_ASSERT_EQ(feedback["result"]["approved"], false);
	UAM_ASSERT_EQ(feedback["result"]["feedback"], "Please change the plan");
	const auto finish = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Finish planning without implementation"}}});
	UAM_ASSERT_EQ(finish["result"]["approved"], true);
	UAM_ASSERT_EQ(finish["result"]["selectedAction"], "exit_only");
	session.active_uam_agent_workspace_access = "read";
	const auto forbidden = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Approve plan and implement interactively"}}});
	UAM_ASSERT_EQ(forbidden["result"]["approved"], false);
	const auto autopilot = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"autopilot"}}});
	UAM_ASSERT_EQ(autopilot["result"]["approved"], false);
	std::string method;
	UAM_ASSERT_EQ(runtime.OnAcpBuildSetupRequest(1, chat, root.root.string(), false, method)["params"]["requestExitPlanMode"], true);
}

UAM_TEST(CopilotNativePlanApprovalPersistsPlanUntilNativeConfirmation)
{
	TempDir root("uam-copilot-plan-policy-transaction");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "not a directory"));
	ChatSession chat;
	chat.id = "plan-transaction";
	chat.provider_id = "copilot-cli";
	chat.approval_mode = "plan";
	chat.command_safety_tier = "ask";
	app.chats.push_back(chat);
	auto owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = chat.provider_id;
	owned->protocol_kind = "copilot-sdk";
	owned->session_id = "native-session";
	owned->running = true;
	owned->processing = true;
	owned->current_mode_id = "plan";
	auto& session = *owned;
	app.acp_sessions.push_back(std::move(owned));
	CopilotPrivateWriter writer(session);
	const auto request = nlohmann::json{{"jsonrpc", "2.0"}, {"id", "approve-plan"}, {"method", "exitPlanMode.request"}, {"params", {{"sessionId", session.session_id}, {"summary", "Proposed implementation"}, {"actions", {"interactive"}}, {"recommendedAction", "interactive"}}}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), request, nullptr));
	const std::map<std::string, std::vector<std::string>> answers{{"plan-review", {"Approve plan and implement interactively"}}};
	std::string error;
	UAM_ASSERT(!uam::ResolveAcpUserInput(app, chat.id, "\"approve-plan\"", answers, &error));
	UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(app.chats.front().command_safety_tier, std::string("ask"));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT(error.find("saved") != std::string::npos);
	app.data_root = root.root / "data";
	UAM_ASSERT(uam::ResolveAcpUserInput(app, chat.id, "\"approve-plan\"", answers, &error));
	UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(app.chats.front().command_safety_tier, std::string("ask"));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
	UAM_ASSERT(!session.waiting_for_user_input);
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved && saved->approval_mode == "plan");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), CopilotEvent("exit_plan_mode.completed", {{"requestId", "native-plan-uuid-unrelated-to-rpc-id"}, {"approved", true}, {"selectedAction", "interactive"}}), nullptr));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), CopilotEvent("session.mode_changed", {{"previousMode", "plan"}, {"newMode", "interactive"}}), nullptr));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("default"));
	UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("default"));
	const auto confirmed = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(confirmed && confirmed->approval_mode == "default");
}

UAM_TEST(CopilotNativeUnconfirmedPlanApprovalNeverLowersTheSavedCeiling)
{
	TempDir root("uam-copilot-plan-failures");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	for (int failure = 0; failure < 7; ++failure)
	{
		uam::AppState app;
		app.data_root = root.root / std::to_string(failure);
		const auto saved_root = app.data_root;
		ChatSession chat;
		chat.id = "unconfirmed-plan";
		chat.provider_id = "copilot-cli";
		chat.approval_mode = "plan";
		app.chats.push_back(chat);
		auto owned = std::make_unique<uam::AcpSessionState>();
		owned->chat_id = chat.id;
		owned->provider_id = chat.provider_id;
		owned->protocol_kind = "copilot-sdk";
		owned->session_id = "native-session";
		owned->running = owned->processing = true;
		owned->turn_serial = 1;
		owned->current_mode_id = "plan";
		auto& session = *owned;
		app.acp_sessions.push_back(std::move(owned));
		std::unique_ptr<CopilotPrivateWriter> writer;
		if (failure != 0) writer = std::make_unique<CopilotPrivateWriter>(session);
		UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), CopilotEvent("exit_plan_mode.requested", {{"requestId", "plan"}, {"summary", "Actual plan"}, {"actions", {"interactive"}}}), nullptr));
		const auto response = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Approve plan and implement interactively"}}});
		std::string error;
		UAM_ASSERT_EQ(uam::ResolveAcpUserInput(app, chat.id, "\"plan\"", {{"plan-review", {"Approve plan and implement interactively"}}}, &error), failure != 0);
		UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("plan"));
		if (failure == 1 || failure == 2)
		{
			const nlohmann::json ack = failure == 1 ? nlohmann::json{{"result", {{"success", false}}}} :
			    nlohmann::json{{"error", {{"code", -32603}, {"message", "Native decision rejected"}}}};
			nlohmann::json reply = ack;
			reply["jsonrpc"] = "2.0";
			reply["id"] = response["id"];
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), reply, nullptr));
		}
		if (failure == 3)
		{
			app.data_root = root.root / "unsaveable";
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "not a directory"));
		}
		if (failure == 4) session.active_uam_agent_workspace_access = "read";
		if (failure == 5) session.last_settled_turn_serial = session.turn_serial;
		if (failure == 6) ++session.turn_serial;
		UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), CopilotEvent("exit_plan_mode.completed", {{"requestId", "plan"}, {"approved", true}, {"selectedAction", "interactive"}}), nullptr));
		UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, app.chats.front(), CopilotEvent("session.mode_changed", {{"previousMode", "plan"}, {"newMode", "interactive"}}), nullptr));
		UAM_ASSERT_EQ(app.chats.front().approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
		const auto saved = ChatRepository::LoadLocalChat(saved_root, chat.id);
		UAM_ASSERT(saved && saved->approval_mode == "plan");
		std::string method;
		UAM_ASSERT_EQ(runtime.OnAcpBuildPrompt(session, 100, "Later turn", app.chats.front(), method)["params"]["agentMode"], "plan");
		if (failure == 3) UAM_ASSERT(!session.processing && session.last_error.find("saved") != std::string::npos);
	}
}

UAM_TEST(CopilotNativePlanEventDecisionReplaysWithoutOpeningAnotherReview)
{
	TempDir root("uam-copilot-plan-replay");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "plan-replay";
	chat.provider_id = "copilot-cli";
	chat.execution_host_id = "remote";
	chat.approval_mode = "plan";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	const auto event = CopilotEvent("exit_plan_mode.requested", {{"requestId", "native-plan-id"}, {"summary", "Actual plan"}, {"actions", {"interactive", "exit_only"}}, {"recommendedAction", "interactive"}}, "ephemeral-plan-event");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, event, nullptr));
	UAM_ASSERT(session.waiting_for_user_input);
	const auto decision = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Finish planning without implementation"}}});
	UAM_ASSERT_EQ(decision["method"], "session.ui.handlePendingExitPlanMode");
	UAM_ASSERT_EQ(decision["params"]["requestId"], "native-plan-id");
	UAM_ASSERT_EQ(decision["params"]["response"]["selectedAction"], "exit_only");
	chat.remote_interaction_responses.push_back({"\"native-plan-id\"", decision.dump()});
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	CopilotPrivateWriter writer(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, event, nullptr));
	UAM_ASSERT(!session.waiting_for_user_input);
	UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", decision["id"]}, {"result", {{"success", false}}}}, nullptr));
	UAM_ASSERT(session.last_error.empty());
	const auto approval_event = CopilotEvent("exit_plan_mode.requested", {{"requestId", "approved-native-id"}, {"summary", "A separately approved plan"}, {"actions", {"interactive"}}}, "approved-plan-event");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, approval_event, nullptr));
	const auto approval = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Approve plan and implement interactively"}}});
	chat.remote_interaction_responses.push_back({"\"approved-native-id\"", approval.dump()});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, approval_event, nullptr));
	UAM_ASSERT(!session.waiting_for_user_input && chat.approval_mode == "plan");
	const auto saved_plan = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved_plan && saved_plan->approval_mode == "plan");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, CopilotEvent("exit_plan_mode.completed", {{"requestId", "approved-native-id"}, {"approved", true}, {"selectedAction", "interactive"}}), nullptr));
	UAM_ASSERT_EQ(chat.approval_mode, std::string("default"));
	const auto saved_approval = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved_approval && saved_approval->approval_mode == "default");
}


UAM_TEST(CopilotNativePlanRepliesAndCompletionsRequireCurrentRequestOwnership)
{
	TempDir root("uam-copilot-plan-ownership");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "plan-ownership";
	chat.provider_id = "copilot-cli";
	chat.approval_mode = "plan";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.processing = true;
	session.current_mode_id = "plan";
	session.turn_serial = 1;
	const auto review = [](const std::string& id) { return CopilotEvent("exit_plan_mode.requested", {{"requestId", id}, {"summary", "Review actual plan"}, {"actions", {"interactive", "exit_only"}}, {"recommendedAction", "interactive"}}, "event-" + id); };
	const auto completed = [](const std::string& id, bool approved, const std::string& action) { return CopilotEvent("exit_plan_mode.completed", {{"requestId", id}, {"approved", approved}, {"selectedAction", action}}, "complete-" + id); };
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("first"), nullptr));
	const std::map<std::string, std::vector<std::string>> valid{{"plan-review", {"Approve plan and implement interactively"}}};
	const std::map<std::string, std::vector<std::string>> stale{{"stale-question", {"Approve plan and implement interactively"}}};
	auto forged = runtime.OnAcpBuildUserInputResponse(session, stale);
	std::string error;
	UAM_ASSERT(!runtime.PrepareAcpUserInputResponse(app, session, chat, stale, forged, &error));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT_EQ(chat.approval_mode, std::string("plan"));
	auto first = runtime.OnAcpBuildUserInputResponse(session, valid);
	UAM_ASSERT(runtime.PrepareAcpUserInputResponse(app, session, chat, valid, first, &error));
	runtime.NoteAcpUserInputResponse(session, first);
	runtime.NoteAcpPermissionResponse(session, first); // The shared writer invokes this hook only after successful delivery.
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("foreign", true, "interactive"), nullptr));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("first", true, "interactive"), nullptr));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("default"));
	session.turn_serial = 2;
	session.current_mode_id = "plan";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("first", true, "interactive"), nullptr));
	UAM_ASSERT_EQ(session.current_mode_id, std::string("plan"));
	const auto old_error = nlohmann::json{{"jsonrpc", "2.0"}, {"id", first["id"]}, {"error", {{"code", -32603}, {"message", "Stale response failure"}}}};
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, old_error, nullptr));
	UAM_ASSERT(session.processing && session.last_error.empty());
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("second"), nullptr));
	auto second = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Keep planning"}}});
	runtime.NoteAcpUserInputResponse(session, second);
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", second["id"]}, {"result", {{"success", true}}}}, nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", second["id"]}, {"error", {{"code", -32603}, {"message", "Duplicate failure"}}}}, nullptr));
	UAM_ASSERT(session.processing && session.last_error.empty());
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("resolved-elsewhere"), nullptr));
	auto resolved = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Keep planning"}}});
	runtime.NoteAcpUserInputResponse(session, resolved);
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", resolved["id"]}, {"result", {{"success", false}}}}, nullptr));
	UAM_ASSERT(session.processing && session.last_error.empty());
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("settled-turn"), nullptr));
	auto settled_plan = runtime.OnAcpBuildUserInputResponse(session, {{"plan-review", {"Keep planning"}}});
	runtime.NoteAcpUserInputResponse(session, settled_plan);
	uam::acp_detail::ResetAcpPendingInteractionState(session);
	session.processing = false;
	session.last_settled_turn_serial = session.turn_serial;
	session.lifecycle_state = "ready";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", settled_plan["id"]}, {"error", {{"code", -32603}, {"message", "Late settled-turn failure"}}}}, nullptr));
	UAM_ASSERT(session.last_error.empty());
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("ready"));
	session.processing = true;
	++session.turn_serial;
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("cancelled"), nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("cancelled", false, ""), nullptr));
	UAM_ASSERT(!session.waiting_for_user_input);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, review("exit-only"), nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("foreign", false, ""), nullptr));
	UAM_ASSERT(session.waiting_for_user_input);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("exit-only", true, "exit_only"), nullptr));
	UAM_ASSERT(!session.waiting_for_user_input);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", "ordinary"}, {"method", "userInput.request"}, {"params", {{"sessionId", "native-session"}, {"question", "Ordinary question"}}}}, nullptr));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, completed("ordinary", false, ""), nullptr));
	UAM_ASSERT(session.waiting_for_user_input);
	const std::map<std::string, std::vector<std::string>> wrong_question{{"stale-question", {"A reply"}}};
	auto wrong_response = runtime.OnAcpBuildUserInputResponse(session, wrong_question);
	UAM_ASSERT(!runtime.PrepareAcpUserInputResponse(app, session, chat, wrong_question, wrong_response, &error));
	UAM_ASSERT(session.waiting_for_user_input);
}


UAM_TEST(CopilotNativePermissionRepliesCannotFailNewOrAlreadySettledTurns)
{
	TempDir root("uam-copilot-permission-ack-ownership");
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	app.data_root = root.root;
	ChatSession chat;
	chat.id = "permission-ack";
	chat.provider_id = "copilot-cli";
	uam::AcpSessionState session;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "copilot-sdk";
	session.session_id = "native-session";
	session.running = true;
	session.processing = true;
	session.turn_serial = 1;
	CopilotPrivateWriter writer(session);
	const auto send = [&](const std::string& request)
	{
		session.pending_permission.request_id_json = nlohmann::json(request).dump();
		session.pending_permission.provider_request_method = "permission.requested";
		const auto response = runtime.OnAcpBuildPermissionResponse(session, "allow_once", false);
		UAM_ASSERT(uam::acp_detail::WriteAcpMessage(session, response));
		return response["id"];
	};
	const auto error = [&](const nlohmann::json& id)
	{
		return runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32603}, {"message", "Permission reply failed"}}}}, nullptr);
	};
	UAM_ASSERT(error("uam-permission:unknown"));
	UAM_ASSERT(session.processing && session.last_error.empty());
	const auto acknowledged = send("acknowledged");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", acknowledged}, {"result", {{"success", true}}}}, nullptr));
	UAM_ASSERT(error(acknowledged));
	UAM_ASSERT(session.processing && session.last_error.empty());
	const auto old_turn = send("old-turn");
	session.turn_serial = 2;
	UAM_ASSERT(error(old_turn));
	UAM_ASSERT(session.processing && session.last_error.empty());
	const auto settled = send("settled");
	session.last_settled_turn_serial = 2;
	UAM_ASSERT(error(settled));
	UAM_ASSERT(session.processing && session.last_error.empty());
	session.turn_serial = 3;
	const auto already_resolved = send("already-resolved");
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"id", already_resolved}, {"result", {{"success", false}}}}, nullptr));
	UAM_ASSERT(session.processing && session.last_error.empty());
	const auto failed = send("current-failure");
	UAM_ASSERT(error(failed));
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.last_error.find("Permission reply failed") != std::string::npos);
}


UAM_TEST(CopilotNativeHistorySeparatesCompletedResponsesAndDeduplicatesFinals)
{
	TempDir root("uam-copilot-history-response-boundaries");
	const auto directory = root.root / "550e8400-e29b-41d4-a716-446655440000";
	const auto events = nlohmann::json::array({
	    {{"id", "start"}, {"type", "session.start"}, {"data", {{"sessionId", directory.filename().string()}, {"context", {{"cwd", root.root.string()}}}}}},
	    {{"id", "user"}, {"type", "user.message"}, {"data", {{"content", "Keep each response"}}}},
	    {{"id", "delta-first"}, {"type", "assistant.message_delta"}, {"data", {{"messageId", "first"}, {"deltaContent", "First"}}}},
	    {{"id", "final-first"}, {"type", "assistant.message"}, {"data", {{"messageId", "first"}, {"content", "First"}}}},
	    {{"id", "repeat-final-first"}, {"type", "assistant.message"}, {"data", {{"messageId", "first"}, {"content", "First"}}}},
	    {{"id", "second"}, {"type", "assistant.message"}, {"data", {{"content", "Second"}}}},
	    {{"id", "third"}, {"type", "assistant.message"}, {"data", {{"content", "Third"}}}},
	    {{"id", "reason-delta"}, {"type", "assistant.reasoning_delta"}, {"data", {{"reasoningId", "reason"}, {"deltaContent", "Consider"}}}},
	    {{"id", "reason-final"}, {"type", "assistant.reasoning"}, {"data", {{"reasoningId", "reason"}, {"content", "Consider"}}}},
	    {{"id", "read-start"}, {"type", "tool.execution_start"}, {"data", {{"toolCallId", "read"}, {"toolName", "view"}, {"arguments", {{"path", "actual.txt"}}}}}},
	    {{"id", "fourth-delta"}, {"type", "assistant.message_delta"}, {"data", {{"messageId", "fourth"}, {"deltaContent", "Fourth"}}}},
	    {{"id", "fourth-final"}, {"type", "assistant.message"}, {"data", {{"messageId", "fourth"}, {"content", "Fourth"}}}},
	    {{"id", "fifth-final"}, {"type", "assistant.message"}, {"data", {{"messageId", "fifth"}, {"content", "Fifth"}}}},
	});
	std::string journal;
	for (const auto& event : events) journal += event.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", journal));
	std::string error;
	const auto loaded = LoadCopilotSessionStateChat(directory, root.root, {}, &error);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(loaded->messages.size(), std::size_t(6));
	for (std::size_t index = 0; const std::string content : {"Keep each response", "First", "Second", "Third", "Fourth", "Fifth"})
		UAM_ASSERT_EQ(loaded->messages[index++].content, content);
	UAM_ASSERT_EQ(loaded->messages[4].thoughts, std::string("Consider"));
	UAM_ASSERT_EQ(loaded->messages[4].tool_calls.size(), std::size_t(1));
	UAM_ASSERT_EQ(loaded->messages[4].tool_calls.front().id, std::string("read"));
}


UAM_TEST(CopilotNativeHistoryLateDuplicateFinalDoesNotSplitCurrentResponse)
{
	TempDir root("uam-copilot-history-late-final");
	const auto directory = root.root / "550e8400-e29b-41d4-a716-446655440000";
	const auto events = nlohmann::json::array({
	    {{"id", "start"}, {"type", "session.start"}, {"data", {{"sessionId", directory.filename().string()}, {"context", {{"cwd", root.root.string()}}}}}},
	    {{"id", "user"}, {"type", "user.message"}, {"data", {{"content", "Keep response ownership"}}}},
	    {{"id", "first-final"}, {"type", "assistant.message"}, {"data", {{"messageId", "first"}, {"content", "First response"}}}},
	    {{"id", "second-reason"}, {"type", "assistant.reasoning"}, {"data", {{"reasoningId", "second-reason"}, {"content", "Second reasoning"}}}},
	    {{"id", "duplicate-first-final"}, {"type", "assistant.message"}, {"data", {{"messageId", "first"}, {"content", "First response"}}}},
	    {{"id", "second-delta"}, {"type", "assistant.message_delta"}, {"data", {{"messageId", "second"}, {"deltaContent", "Second response"}}}},
	    {{"id", "second-tool"}, {"type", "tool.execution_start"}, {"data", {{"toolCallId", "second-read"}, {"toolName", "view"}}}},
	    {{"id", "second-final"}, {"type", "assistant.message"}, {"data", {{"messageId", "second"}, {"content", "Second response"}}}},
	});
	std::string journal;
	for (const auto& event : events) journal += event.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", journal));
	const auto loaded = LoadCopilotSessionStateChat(directory, root.root);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->messages.size(), std::size_t(3));
	UAM_ASSERT_EQ(loaded->messages[1].content, std::string("First response"));
	UAM_ASSERT(loaded->messages[1].thoughts.empty());
	UAM_ASSERT_EQ(loaded->messages[2].thoughts, std::string("Second reasoning"));
	UAM_ASSERT_EQ(loaded->messages[2].content, std::string("Second response"));
	UAM_ASSERT_EQ(loaded->messages[2].tool_calls.size(), std::size_t(1));
	UAM_ASSERT_EQ(loaded->messages[2].tool_calls.front().id, std::string("second-read"));
}

#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

void CopilotAutoRoutingSeedFixture(uam::AcpSessionState& session, ChatSession& chat)
{
	chat.id = "copilot-auto-routing-fixture";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
	chat.execution_host_id = "local";
	chat.model_id = "auto";
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.session_id = chat.native_session_id;
	session.process_execution_host_id = chat.execution_host_id;
	session.protocol_kind = "copilot-sdk";
	session.running = true;
	session.initialized = true;
	session.session_ready = true;
	session.lifecycle_state = "ready";
	session.current_model_id = "auto";
	session.copilot_native_state["autoRouting"] = {
	    {"owner", {{"chatId", chat.id}, {"sessionId", session.session_id}, {"executionHostId", "local"}, {"lifetime", "fixture-lifetime-1"}}},
	    {"revision", 0}, {"catalogStatus", "unknown"}, {"stateStatus", "ready"},
	    {"eligibleTiers", {{"present", false}}}, {"providerDefault", {{"present", false}}},
	    {"currentModel", {{"present", true}, {"value", "auto"}}},
	    {"effective", {{"present", true}, {"value", "balance"}}},
	    {"pending", {{"present", false}}}, {"activating", {{"present", false}}},
	    {"requests", nlohmann::json::object()}, {"lastError", ""}};
}

std::string CopilotAutoRoutingOwnFixtureRequest(uam::AcpSessionState& session, int id, const std::string& method)
{
	const std::string key = std::to_string(id);
	auto& routing = session.copilot_native_state["autoRouting"];
	routing["requests"][key] = {{"method", method}, {"owner", routing.at("owner")}, {"revision", routing.at("revision")}};
	session.pending_request_methods[id] = method;
	return key;
}

nlohmann::json CopilotAutoRoutingFixtureCatalog()
{
	return {{"list", nlohmann::json::array({{{"id", "auto"}}})},
	    {"auto", {{"defaultTier", "balance"}, {"tiers", nlohmann::json::array({
	        {{"id", "balance"}, {"displayName", "Balanced"}, {"description", "Provider default preference"}, {"type", "auto"}, {"status", {{"enabled", true}}}},
	        {{"id", "vendor/Latency:Fast+V2"}, {"displayName", "Low latency"}, {"description", "Provider-advertised latency preference"}, {"type", "auto"}, {"status", {{"enabled", true}}}},
	        {{"id", "efficiency"}, {"displayName", "Efficiency"}, {"description", "Unavailable preference"}, {"type", "auto"}, {"status", {{"enabled", false}, {"reason", "policy_rejected"}, {"message", "Disabled by provider policy"}}}}})}}}};
}

}

UAM_TEST(CopilotNativeAutoRoutingRetainsAdvertisedEligibility)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotAutoRoutingSeedFixture(session, chat);
	CopilotPrivateWriter writer(session);
	const auto catalog = CopilotAutoRoutingFixtureCatalog();
	const auto request = CopilotAutoRoutingOwnFixtureRequest(session, 201, "session.model.list");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.list", request, catalog));
	const auto& routing = session.copilot_native_state.at("autoRouting");
	UAM_ASSERT_EQ(routing.at("catalogStatus"), "ready");
	UAM_ASSERT_EQ(routing.at("eligibleTiers"), (nlohmann::json{{"present", true}, {"value", catalog.at("auto").at("tiers")}}));
	UAM_ASSERT_EQ(routing.at("providerDefault"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
	UAM_ASSERT_EQ(routing.at("eligibleTiers").at("value").size(), std::size_t(3));
	UAM_ASSERT_EQ(routing.at("eligibleTiers").at("value").at(1).at("id"), "vendor/Latency:Fast+V2");
	UAM_ASSERT_EQ(routing.at("eligibleTiers").at("value").at(2).at("status").at("enabled"), false);
	UAM_ASSERT_EQ(routing.at("eligibleTiers").at("value").at(2).at("status").at("reason"), "policy_rejected");
	UAM_ASSERT_EQ(routing.at("effective"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
	UAM_ASSERT_EQ(chat.model_id, std::string("auto"));
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeAutoRoutingKeepsPendingResetDistinctFromEffective)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotAutoRoutingSeedFixture(session, chat);
	CopilotPrivateWriter writer(session);
	const auto request = CopilotAutoRoutingOwnFixtureRequest(session, 202, "session.model.getCurrent");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", request,
	    {{"modelId", "auto"}, {"autoTier", "balance"}, {"pendingAutoTier", nullptr}, {"activatingAutoTier", "intelligence"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("stateStatus"), "ready");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("currentModel"), (nlohmann::json{{"present", true}, {"value", "auto"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("pending"), (nlohmann::json{{"present", true}, {"value", nullptr}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("activating"), (nlohmann::json{{"present", true}, {"value", "intelligence"}}));
	const auto refresh = CopilotAutoRoutingOwnFixtureRequest(session, 203, "session.model.getCurrent");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", refresh, {{"modelId", "auto"}, {"autoTier", "balance"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("pending"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("activating"), (nlohmann::json{{"present", false}}));
}

UAM_TEST(CopilotNativeAutoRoutingRejectsMalformedAndUnsupportedReplies)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotAutoRoutingSeedFixture(session, chat);
	CopilotPrivateWriter writer(session);
	const auto effective = session.copilot_native_state.at("autoRouting").at("effective");
	auto malformed = CopilotAutoRoutingFixtureCatalog();
	malformed["auto"]["tiers"][2]["status"]["enabled"] = "true";
	const auto bad_catalog = CopilotAutoRoutingOwnFixtureRequest(session, 204, "session.model.list");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.list", bad_catalog, malformed));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("catalogStatus"), "invalid");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("eligibleTiers"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), effective);
	const auto bad_state = CopilotAutoRoutingOwnFixtureRequest(session, 205, "session.model.getCurrent");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", bad_state,
	    {{"modelId", "concrete-other"}, {"autoTier", "fast"}, {"pendingAutoTier", 7}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("stateStatus"), "invalid");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("currentModel"), (nlohmann::json{{"present", true}, {"value", "auto"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), effective);
	const auto missing_metadata = CopilotAutoRoutingOwnFixtureRequest(session, 206, "session.model.list");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.list", missing_metadata, {{"list", nlohmann::json::array()}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("catalogStatus"), "unknown");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("eligibleTiers"), (nlohmann::json{{"present", false}}));
	uam::acp_detail::AcpResponseFailureDetails unsupported;
	unsupported.failure.method = "session.model.list";
	unsupported.failure.request_id = CopilotAutoRoutingOwnFixtureRequest(session, 207, unsupported.failure.method);
	unsupported.failure.has_code = true;
	unsupported.failure.code = -32601;
	unsupported.failure.message = "Method not found";
	unsupported.formatted_error = "Copilot Auto routing discovery is unsupported: Method not found";
	UAM_ASSERT(runtime.OnAcpHandleError(app, session, chat, unsupported));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("catalogStatus"), "unsupported");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("eligibleTiers"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), effective);
	UAM_ASSERT(session.running && session.session_ready);
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeAutoRoutingIgnoresStaleSessionReplies)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotAutoRoutingSeedFixture(session, chat);
	CopilotPrivateWriter writer(session);
	const auto before = session.copilot_native_state.at("autoRouting");
	const auto catalog_request = CopilotAutoRoutingOwnFixtureRequest(session, 208, "session.model.list");
	session.copilot_native_state["autoRouting"]["requests"][catalog_request]["owner"]["sessionId"] = "old-native-session";
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.list", catalog_request, CopilotAutoRoutingFixtureCatalog()));
	const auto state_request = CopilotAutoRoutingOwnFixtureRequest(session, 209, "session.model.getCurrent");
	session.copilot_native_state["autoRouting"]["requests"][state_request]["owner"]["sessionId"] = "old-native-session";
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", state_request, {{"modelId", "auto"}, {"autoTier", "fast"}}));
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", "999999", {{"modelId", "auto"}, {"autoTier", "intelligence"}}));
	for (const char* field : {"catalogStatus", "stateStatus", "eligibleTiers", "providerDefault", "currentModel", "effective", "pending", "activating"})
		UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at(field), before.at(field));
	UAM_ASSERT_EQ(session.session_id, chat.native_session_id);
	UAM_ASSERT_EQ(chat.model_id, std::string("auto"));
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeAutoRoutingSetupOmitsCapiOnCreateAndResume)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	ChatSession chat;
	chat.id = "copilot-auto-routing-setup";
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.model_id = "auto";
	for (const bool explicit_tier : {false, true})
	{
		chat.service_tier_explicit = explicit_tier;
		for (const std::string tier : {std::string{}, std::string("fast")})
		{
			chat.service_tier = tier;
			chat.native_session_id.clear();
			std::string method;
			const auto create = runtime.OnAcpBuildSetupRequest(210, chat, "/workspace", false, method);
			UAM_ASSERT_EQ(method, std::string("session.create"));
			UAM_ASSERT_EQ(create.at("params").at("model"), "auto");
			UAM_ASSERT(!create.at("params").contains("capi"));
			UAM_ASSERT(!create.at("params").contains("autoTier"));
			chat.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
			const auto resume = runtime.OnAcpBuildSetupRequest(211, chat, "/workspace", true, method);
			UAM_ASSERT_EQ(method, std::string("session.resume"));
			UAM_ASSERT_EQ(resume.at("params").at("sessionId"), chat.native_session_id);
			UAM_ASSERT(!resume.at("params").contains("capi"));
			UAM_ASSERT(!resume.at("params").contains("autoTier"));
		}
	}
}

#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

nlohmann::json CopilotAutoRoutingReadyFixture(const IProviderRuntime& runtime, uam::AppState& app,
    uam::AcpSessionState& session, ChatSession& chat)
{
	CopilotAutoRoutingSeedFixture(session, chat);
	session.copilot_native_state["autoRoutingPublicLifetime"] = "fixture-public-token";
	const auto id = CopilotAutoRoutingOwnFixtureRequest(session, 401, "session.model.list");
	(void)runtime.OnAcpHandleResult(app, session, chat, "session.model.list", id, CopilotAutoRoutingFixtureCatalog());
	return {{"chatId", chat.id}, {"nativeSessionId", session.session_id}, {"executionHostId", "local"}, {"lifetime", "fixture-public-token"}};
}

}

UAM_TEST(CopilotNativeAutoRoutingMutationWritesExactOpaqueTierAndDefaultNull)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotPrivateWriter writer(session);
	const auto owner = CopilotAutoRoutingReadyFixture(runtime, app, session, chat);
	auto bytes = std::make_shared<std::string>();
	session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
	    [bytes](const char* data, std::size_t count, std::string&) -> std::ptrdiff_t { bytes->append(data, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
	std::string error;
	UAM_ASSERT(runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", "vendor/Latency:Fast+V2", owner, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(2)));
	const auto boundary = bytes->find("\r\n\r\n");
	UAM_ASSERT(boundary != std::string::npos);
	const auto wire = nlohmann::json::parse(bytes->substr(boundary + 4));
	UAM_ASSERT_EQ(wire.at("method"), "session.model.switchAutoTier");
	UAM_ASSERT_EQ(wire.at("params"), (nlohmann::json{{"sessionId", chat.native_session_id}, {"autoTier", "vendor/Latency:Fast+V2"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective").at("value"), "balance");
	// Settle this owned request before exercising the distinct Default wire value.
	const auto id = std::to_string(wire.at("id").get<int>());
	UAM_ASSERT_EQ(session.config_option_change_request_id, wire.at("id").get<int>());
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.switchAutoTier", id,
	    {{"status", "pending"}, {"effectiveAutoTier", "balance"}, {"pendingAutoTier", "vendor/Latency:Fast+V2"}}));
	UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
	const auto reads = session.copilot_native_state.at("autoRouting").at("requests");
	for (auto it = reads.begin(); it != reads.end(); ++it)
		UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", it.key(),
		    {{"modelId", "auto"}, {"autoTier", "balance"}, {"pendingAutoTier", "vendor/Latency:Fast+V2"}}));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(2)));
	bytes->clear();
	UAM_ASSERT(runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", nullptr, owner, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(2)));
	const auto reset = nlohmann::json::parse(bytes->substr(bytes->find("\r\n\r\n") + 4));
	UAM_ASSERT(reset.at("params").at("autoTier").is_null());
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective").at("value"), "balance");
}

UAM_TEST(CopilotNativeAutoRoutingAckAndEventsKeepCommitmentHonest)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotPrivateWriter writer(session);
	const auto owner = CopilotAutoRoutingReadyFixture(runtime, app, session, chat);
	std::string error;
	UAM_ASSERT(runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", nullptr, owner, &error));
	const auto requests = session.copilot_native_state.at("autoRouting").at("requests");
	UAM_ASSERT_EQ(requests.size(), std::size_t(1));
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.switchAutoTier", requests.begin().key(),
	    {{"status", "pending"}, {"effectiveAutoTier", "balance"}, {"pendingAutoTier", nullptr}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective").at("value"), "balance");
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("pending"), (nlohmann::json{{"present", true}, {"value", nullptr}}));
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat,
	    CopilotEvent("session.auto_tier_switch_failed", {{"requestedAutoTier", nullptr}, {"effectiveAutoTier", "balance"}, {"reason", "policy_rejected"}}, "routing-failed", chat.native_session_id), nullptr));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective").at("value"), "balance");
	UAM_ASSERT(session.copilot_native_state.at("autoRouting").at("lastError").get<std::string>().find("policy_rejected") != std::string::npos);
	const auto before_child = session.copilot_native_state.at("autoRouting");
	auto child = CopilotEvent("session.model_change", {{"autoTier", "intelligence"}}, "routing-child", chat.native_session_id);
	child["params"]["event"]["agentId"] = "child-only";
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, child, nullptr));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting"), before_child);
	UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat,
	    CopilotEvent("session.model_change", {{"autoTier", nullptr}}, "routing-committed", chat.native_session_id), nullptr));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), (nlohmann::json{{"present", false}}));
	const auto committed = session.copilot_native_state.at("autoRouting").at("effective");
	// The earlier read was issued before commitment; it cannot resurrect Balance.
	const auto old_reads = session.copilot_native_state.at("autoRouting").at("requests");
	for (auto it = old_reads.begin(); it != old_reads.end(); ++it)
		UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", it.key(), {{"modelId", "auto"}, {"autoTier", "balance"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), committed);
	UAM_ASSERT(chat.messages.empty());
}

UAM_TEST(CopilotNativeAutoRoutingMutationRejectsWrongOwnerBusyAndDisabledTier)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotPrivateWriter writer(session);
	const auto owner = CopilotAutoRoutingReadyFixture(runtime, app, session, chat);
	const auto before = session.copilot_native_state.at("autoRouting").at("effective");
	std::string error;
	for (const char* field : {"chatId", "nativeSessionId", "executionHostId", "lifetime"})
	{
		auto wrong = owner;
		wrong[field] = "foreign-owner";
		UAM_ASSERT(!runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", nullptr, wrong, &error));
	}
	session.processing = true;
	UAM_ASSERT(!runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", nullptr, owner, &error));
	session.processing = false;
	UAM_ASSERT(!runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", "efficiency", owner, &error));
	UAM_ASSERT_EQ(error, std::string("Disabled by provider policy"));
	UAM_ASSERT(!runtime.OnAcpRequestNativeModelOption(app, session, chat, "autoRouting", "fast", owner, &error));
	UAM_ASSERT(session.copilot_native_state.at("autoRouting").at("requests").empty());
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), before);
	const auto projected = runtime.OnAcpNativeModelOptionsForFrontend(session).at("autoRouting");
	UAM_ASSERT_EQ(projected.at("ownership"), owner);
	UAM_ASSERT(!projected.contains("requests"));
	UAM_ASSERT(!projected.contains("owner"));
	UAM_ASSERT(projected.dump().find("fixture-lifetime-1") == std::string::npos);
	UAM_ASSERT_EQ(projected.at("available"), true);
}

UAM_TEST(CopilotNativeAutoRoutingBoundsAndReceiptOwnershipFailClosed)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AppState app;
	uam::AcpSessionState session;
	ChatSession chat;
	CopilotPrivateWriter writer(session);
	(void)CopilotAutoRoutingReadyFixture(runtime, app, session, chat);
	const auto effective = session.copilot_native_state.at("autoRouting").at("effective");
	auto oversized = CopilotAutoRoutingFixtureCatalog();
	oversized["auto"]["tiers"][1]["id"] = std::string(513, 'x');
	const auto oversized_id = CopilotAutoRoutingOwnFixtureRequest(session, 402, "session.model.list");
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.list", oversized_id, oversized));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("catalogStatus"), "invalid");
	for (const char* field : {"executionHostId", "lifetime"})
	{
		const auto id = CopilotAutoRoutingOwnFixtureRequest(session, field == std::string_view("lifetime") ? 404 : 403, "session.model.getCurrent");
		session.copilot_native_state["autoRouting"]["requests"][id]["owner"][field] = "foreign-owner";
		UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", id, {{"modelId", "auto"}, {"autoTier", "fast"}}));
		UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), effective);
	}
	const auto id = CopilotAutoRoutingOwnFixtureRequest(session, 405, "session.model.getCurrent");
	session.copilot_native_state["autoRouting"]["requests"][id]["deadline"] = uam::GetAppTimeSeconds() - 1.0;
	UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.model.getCurrent", id, {{"modelId", "auto"}, {"autoTier", "fast"}}));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("effective"), effective);
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("stateStatus"), "unknown");
}

#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

// Self-contained OS sink: no provider executable, RPC server, credentials or network.
// Seed only native ownership/readiness; discover selectable options through the public hook.
struct CopilotAutoDeadlineSink
{
	TempDir temp{"uam-copilot-auto-deadline"};
	uam::AppState app;
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
	} cleanup{app};
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
	uam::AcpSessionState* session = nullptr;
	std::shared_ptr<std::string> wire = std::make_shared<std::string>();
	std::shared_ptr<uam::platform::AsyncByteWriter> transport_writer;
	std::shared_ptr<uam::platform::AsyncByteWriter> observed_writer;
	uam::platform::StdioProcessPlatformFields original_process;
	std::string process_id;
	nlohmann::json owner;

	CopilotAutoDeadlineSink()
	{
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		app.settings.central_provider_configuration.enabled = false;
		app.settings.git_enabled = app.settings.git_auto_worktree = false;
		app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
		ChatSession chat;
		auto owned = std::make_unique<uam::AcpSessionState>();
		CopilotAutoRoutingSeedFixture(*owned, chat);
		chat.created_at = "2026-10-17T00:00:00Z";
		chat.workspace_directory = temp.root.string();
		chat.memory_enabled = false;
		chat.uam_control_enabled = false;
		owned->current_mode_id = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
		owned->launch_model_options_id = chat.model_id;
		owned->next_request_id = 1000;
		owned->last_runtime_activity_time_s = uam::GetAppTimeSeconds();
		owned->idle_interaction_started_time_s = uam::GetAppTimeSeconds();
		owned->copilot_native_state["autoRoutingLifetime"] = "fixture-lifetime-1";
		owned->copilot_native_state["autoRoutingPublicLifetime"] = "deadline-public-token";
		app.chats.push_back(std::move(chat));
		session = owned.get();
		app.acp_sessions.push_back(std::move(owned));
#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", "more > NUL"};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", "exec /bin/cat >/dev/null"};
#endif
		std::string error;
		auto& process = PlatformServicesFactory::Instance().process_service;
		UAM_ASSERT(process.StartStdioProcess(*session, temp.root, argv, &error));
		session->running = true;
		session->last_process_id = uam::acp_detail::AcpProcessHandleLabel(*session);
		// Initialize the platform's lazy writer before intercepting protocol frames.
		UAM_ASSERT(process.WriteToStdioProcess(*session, "\n", 1, &error));
		transport_writer = session->stdin_writer;
		UAM_ASSERT(transport_writer && transport_writer->Flush(std::chrono::seconds(2)));
		observed_writer = std::make_shared<uam::platform::AsyncByteWriter>(
		    [capture = wire, writer = transport_writer](const char* bytes, std::size_t count, std::string& failure) -> std::ptrdiff_t
		    {
			    capture->append(bytes, count);
			    return writer->Enqueue(bytes, count, &failure) ? static_cast<std::ptrdiff_t>(count) : -1;
		    }, [] {});
		session->stdin_writer = observed_writer;
		original_process = static_cast<const uam::platform::StdioProcessPlatformFields&>(*session);
		process_id = session->last_process_id;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Chat()));
		UAM_ASSERT(uam::acp_detail::ValidateAcpRestartOwnership(app, *session, Chat(), &error));
		owner = Projection().at("ownership");
		UAM_ASSERT_EQ(owner, (nlohmann::json{{"chatId", Chat().id}, {"nativeSessionId", Chat().native_session_id},
		    {"executionHostId", "local"}, {"lifetime", "deadline-public-token"}}));
		AssertSameProcess();
	}

	ChatSession& Chat() { return app.chats.front(); }
	nlohmann::json Projection() const { return runtime.OnAcpNativeModelOptionsForFrontend(*session).at("autoRouting"); }
	nlohmann::json Requests() const { return session->copilot_native_state.at("autoRouting").at("requests"); }

	std::vector<nlohmann::json> Frames()
	{
		UAM_ASSERT(observed_writer->Flush(std::chrono::seconds(2)));
		UAM_ASSERT(transport_writer->Flush(std::chrono::seconds(2)));
		std::vector<nlohmann::json> frames;
		std::string_view remaining(*wire);
		while (!remaining.empty())
		{
			const auto frame = uam::structured_framing::Read(remaining);
			UAM_ASSERT(frame.status == uam::structured_framing::Status::Complete);
			frames.push_back(nlohmann::json::parse(remaining.substr(frame.body_offset, frame.body_bytes)));
			remaining.remove_prefix(frame.consumed_bytes);
		}
		return frames;
	}

	void Result(const std::string& key, const nlohmann::json& result)
	{
		UAM_ASSERT(session->pending_request_methods.contains(std::stoi(key)));
		UAM_ASSERT(uam::acp_detail::ProcessAcpLine(app, *session, Chat(),
		    nlohmann::json{{"jsonrpc", "2.0"}, {"id", std::stoi(key)}, {"result", result}}.dump(), nullptr));
		UAM_ASSERT(!session->pending_request_methods.contains(std::stoi(key)));
		UAM_ASSERT(!Requests().contains(key));
	}

	void ReplyReads()
	{
		const auto requests = Requests();
		UAM_ASSERT(!requests.empty());
		for (auto it = requests.begin(); it != requests.end(); ++it)
		{
			const std::string method = it->at("method").get<std::string>();
			UAM_ASSERT(method == "session.model.list" || method == "session.model.getCurrent");
			UAM_ASSERT_EQ(it->at("owner"), session->copilot_native_state.at("autoRouting").at("owner"));
			UAM_ASSERT_EQ(session->pending_request_methods.at(std::stoi(it.key())), method);
			Result(it.key(), method == "session.model.list" ? CopilotAutoRoutingFixtureCatalog() :
			    nlohmann::json{{"modelId", "auto"}, {"autoTier", "balance"}, {"pendingAutoTier", nullptr}});
		}
		UAM_ASSERT(Requests().empty());
	}

	void Discover()
	{
		const auto before = Frames().size();
		std::string error;
		UAM_ASSERT(runtime.OnAcpRefreshNativeModelOptions(app, *session, Chat(), &error));
		const auto frames = Frames();
		UAM_ASSERT_EQ(frames.size(), before + 2);
		UAM_ASSERT_EQ(frames[before].at("method"), "session.model.list");
		UAM_ASSERT_EQ(frames[before + 1].at("method"), "session.model.getCurrent");
		for (std::size_t index = before; index < frames.size(); ++index)
			UAM_ASSERT_EQ(frames[index].at("params"), (nlohmann::json{{"sessionId", Chat().native_session_id}}));
		UAM_ASSERT_EQ(Requests().size(), std::size_t{2});
		UAM_ASSERT_EQ(Projection().at("requestPending"), true);
		UAM_ASSERT_EQ(session->config_option_change_request_id, 0);
		ReplyReads();
		const auto projection = Projection();
		UAM_ASSERT_EQ(projection.at("ownership"), owner);
		UAM_ASSERT_EQ(projection.at("catalogStatus"), "ready");
		UAM_ASSERT_EQ(projection.at("stateStatus"), "ready");
		UAM_ASSERT_EQ(projection.at("effective"), (nlohmann::json{{"present", true}, {"value", "balance"}}));
		UAM_ASSERT_EQ(projection.at("pending"), (nlohmann::json{{"present", true}, {"value", nullptr}}));
		UAM_ASSERT_EQ(projection.at("available"), true);
		UAM_ASSERT(session->pending_request_methods.empty());
	}

	std::string Switch()
	{
		const auto before = Frames().size();
		std::string error;
		UAM_ASSERT(runtime.OnAcpRequestNativeModelOption(app, *session, Chat(), "autoRouting", "vendor/Latency:Fast+V2", owner, &error));
		const auto frames = Frames();
		UAM_ASSERT_EQ(frames.size(), before + 1);
		const auto& frame = frames.back();
		UAM_ASSERT_EQ(frame.at("method"), "session.model.switchAutoTier");
		UAM_ASSERT_EQ(frame.at("params"), (nlohmann::json{{"sessionId", Chat().native_session_id}, {"autoTier", "vendor/Latency:Fast+V2"}}));
		const int id = frame.at("id").get<int>();
		const std::string key = std::to_string(id);
		UAM_ASSERT_EQ(Requests().size(), std::size_t{1});
		UAM_ASSERT_EQ(Requests().at(key).at("method"), "session.model.switchAutoTier");
		UAM_ASSERT_EQ(Requests().at(key).at("owner"), session->copilot_native_state.at("autoRouting").at("owner"));
		UAM_ASSERT(Requests().at(key).at("deadline").get<double>() > uam::GetAppTimeSeconds());
		UAM_ASSERT_EQ(session->pending_request_methods.at(id), "session.model.switchAutoTier");
		UAM_ASSERT_EQ(session->config_option_change_request_id, id);
		UAM_ASSERT_EQ(session->config_option_change_id, std::string("autoRouting"));
		UAM_ASSERT_EQ(Projection().at("requestPending"), true);
		return key;
	}

	void AssertSameProcess()
	{
		UAM_ASSERT(session->running && session->initialized && session->session_ready);
		UAM_ASSERT_EQ(session->last_process_id, process_id);
		UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*session), process_id);
		UAM_ASSERT_EQ(session->chat_id, Chat().id);
		UAM_ASSERT_EQ(session->session_id, Chat().native_session_id);
		UAM_ASSERT_EQ(session->process_execution_host_id, std::string("local"));
		UAM_ASSERT(session->stdin_writer == observed_writer);
#if defined(_WIN32)
		UAM_ASSERT(session->stdin_write == original_process.stdin_write && session->stdout_read == original_process.stdout_read && session->stderr_read == original_process.stderr_read);
		UAM_ASSERT(session->process_info.hProcess == original_process.process_info.hProcess && session->job_object == original_process.job_object);
#else
		UAM_ASSERT(session->stdin_write_fd == original_process.stdin_write_fd && session->stdout_read_fd == original_process.stdout_read_fd && session->stderr_read_fd == original_process.stderr_read_fd);
		UAM_ASSERT_EQ(session->child_pid, original_process.child_pid);
#endif
		UAM_ASSERT(!PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(*session));
		UAM_ASSERT(app.acp_process_stop_tasks.empty() && app.pending_acp_remote_stops.empty());
		UAM_ASSERT(!session->local_stop_pending && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->reconnect_pending);
		UAM_ASSERT(!session->recovering_remote_turn && !session->recovering_remote_process);
		UAM_ASSERT(!Chat().remote_restart_pending && !Chat().remote_stop_cleanup_pending);
		UAM_ASSERT_EQ(Projection().at("ownership"), owner);
		std::string error;
		UAM_ASSERT(uam::acp_detail::ValidateAcpRestartOwnership(app, *session, Chat(), &error));
	}
};

}

UAM_TEST(CopilotNativeAutoRoutingPollExpiresSilentOwnedRequests)
{
	// One regression for deadline service, covering both read-only and settings-gated requests.
	for (const bool switching : {false, true})
	{
		CopilotAutoDeadlineSink fixture;
		fixture.Discover();
		auto& session = *fixture.session;
		auto& chat = fixture.Chat();
		const auto observed = fixture.Projection();
		if (switching) (void)fixture.Switch();
		else
		{
			std::string error;
			UAM_ASSERT(fixture.runtime.OnAcpRefreshNativeModelOptions(fixture.app, session, chat, &error));
		}
		const auto requests = fixture.Requests();
		UAM_ASSERT_EQ(requests.size(), switching ? std::size_t{1} : std::size_t{2});
		const auto before_frames = fixture.Frames();
		// This unrelated transport correlation must survive exact owned retirement.
		session.pending_request_methods[9901] = "fixture/unrelated-read";
		if (switching)
		{
			Message authored;
			authored.role = MessageRole::User;
			authored.content = "Send this authored prompt after the option deadline";
			authored.created_at = "2026-10-17T00:00:01Z";
			authored.acp_prompt_not_sent = true;
			chat.messages.push_back(authored);
			session.processing = true;
			session.turn_serial = 7;
			session.turn_first_user_message_index = session.turn_user_message_index = 0;
			session.queued_prompt = authored.content;
			uam::AcpQueuedUserPromptState later;
			later.id = "after-deadline-followup";
			later.revision = 3;
			later.text = "Keep this later FIFO entry";
			session.queued_user_prompts.push_back(later);
			chat.acp_queued_prompts.push_back(later);
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		}
		const auto messages = chat.messages;
		const auto queue = session.queued_user_prompts;
		const auto durable_queue = chat.acp_queued_prompts;
		// Positive control: normal Poll must leave an unexpired owned operation alone.
		(void)uam::PollAllAcpSessions(fixture.app);
		fixture.AssertSameProcess();
		UAM_ASSERT_EQ(fixture.Requests(), requests);
		UAM_ASSERT_EQ(fixture.Frames(), before_frames);
		UAM_ASSERT_EQ(chat.messages, messages);
		for (auto it = requests.begin(); it != requests.end(); ++it)
		{
			UAM_ASSERT(it->at("deadline").get<double>() > uam::GetAppTimeSeconds());
			UAM_ASSERT_EQ(it->at("owner"), session.copilot_native_state.at("autoRouting").at("owner"));
			UAM_ASSERT_EQ(session.pending_request_methods.at(std::stoi(it.key())), it->at("method").get<std::string>());
			// Past private-fixture deadline is a deterministic timeout witness, not a product timeout change.
			session.copilot_native_state["autoRouting"]["requests"][it.key()]["deadline"] = uam::GetAppTimeSeconds() - 1.0;
		}
		const bool changed = uam::PollAllAcpSessions(fixture.app);
		fixture.AssertSameProcess();
		UAM_ASSERT(fixture.Requests().empty()); // Intended r1 adapter RED: Poll never services these deadlines.
		UAM_ASSERT(changed); // Option-only expiry must publish a state change even without a provider receipt.
		for (auto it = requests.begin(); it != requests.end(); ++it)
			UAM_ASSERT(!session.pending_request_methods.contains(std::stoi(it.key())));
		UAM_ASSERT_EQ(session.pending_request_methods.at(9901), "fixture/unrelated-read");
		UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
		UAM_ASSERT(session.config_option_change_id.empty());
		const auto projection = fixture.Projection();
		UAM_ASSERT_EQ(projection.at("requestPending"), false);
		UAM_ASSERT_EQ(projection.at("stateStatus"), "unknown");
		UAM_ASSERT_EQ(projection.at("available"), false);
		if (!switching) UAM_ASSERT_EQ(projection.at("catalogStatus"), "unknown");
		UAM_ASSERT_EQ(projection.at("effective"), observed.at("effective"));
		UAM_ASSERT_EQ(projection.at("pending"), observed.at("pending"));
		UAM_ASSERT_EQ(projection.at("activating"), observed.at("activating"));
		const auto error = projection.at("lastError").get<std::string>();
		UAM_ASSERT(!error.empty() && error.size() <= 2048 && error.find("timed out") != std::string::npos);
		UAM_ASSERT(session.last_error.empty() && session.last_turn_error.empty());
		UAM_ASSERT_EQ(session.queued_user_prompts, queue);
		UAM_ASSERT_EQ(chat.acp_queued_prompts, durable_queue);
		const auto frames = fixture.Frames();
		if (switching)
		{
			UAM_ASSERT_EQ(frames.size(), before_frames.size() + 1);
			UAM_ASSERT_EQ(frames.back().at("method"), "session.send");
			UAM_ASSERT_EQ(frames.back().at("params").at("prompt"), messages.front().content);
			UAM_ASSERT_EQ(frames.back().at("params").at("sessionId"), chat.native_session_id);
			UAM_ASSERT_EQ(session.prompt_request_id, frames.back().at("id").get<int>());
			UAM_ASSERT_EQ(session.pending_request_methods.at(session.prompt_request_id), "session.send");
			UAM_ASSERT(session.processing && session.queued_prompt.empty());
			UAM_ASSERT_EQ(session.turn_serial, 7);
			UAM_ASSERT_EQ(chat.messages.size(), messages.size());
			UAM_ASSERT_EQ(chat.messages.front().content, messages.front().content);
			UAM_ASSERT_EQ(chat.messages.front().created_at, messages.front().created_at);
			UAM_ASSERT(!chat.messages.front().acp_prompt_not_sent && !chat.messages.front().interrupted);
		}
		else
		{
			UAM_ASSERT_EQ(frames, before_frames);
			UAM_ASSERT_EQ(chat.messages, messages);
			UAM_ASSERT(!session.processing && session.prompt_request_id == 0);
		}
	}
}

UAM_TEST(CopilotNativeAutoRoutingNewSwitchResetsOldInactivityBaseline)
{
	CopilotAutoDeadlineSink fixture;
	fixture.Discover();
	auto& session = *fixture.session;
	const auto observed = fixture.Projection();
	// Keep the configured generic timeout intact; simulate a previously idle native lifetime.
	const int setup_timeout = fixture.app.settings.acp_setup_inactivity_timeout_seconds;
	const double admitted_at = uam::GetAppTimeSeconds();
	session.last_runtime_activity_time_s = admitted_at - static_cast<double>(setup_timeout) - 10.0;
	session.idle_interaction_started_time_s = admitted_at;
	UAM_ASSERT(admitted_at - session.last_runtime_activity_time_s > setup_timeout);
	const auto key = fixture.Switch();
	const auto requests = fixture.Requests();
	const auto frames = fixture.Frames();
	// No receipt, explicit refresh or input has serviced the freshly admitted switch.
	(void)uam::PollAllAcpSessions(fixture.app);
	UAM_ASSERT(session.running); // Intended r1 adapter RED: old generic baseline immediately stops the sink.
	fixture.AssertSameProcess();
	UAM_ASSERT_EQ(fixture.app.settings.acp_setup_inactivity_timeout_seconds, setup_timeout);
	UAM_ASSERT(session.last_runtime_activity_time_s >= admitted_at);
	UAM_ASSERT_EQ(fixture.Requests(), requests);
	UAM_ASSERT(fixture.Requests().at(key).at("deadline").get<double>() > uam::GetAppTimeSeconds());
	UAM_ASSERT_EQ(session.config_option_change_request_id, std::stoi(key));
	UAM_ASSERT_EQ(session.config_option_change_id, std::string("autoRouting"));
	UAM_ASSERT_EQ(session.pending_request_methods.at(std::stoi(key)), "session.model.switchAutoTier");
	UAM_ASSERT_EQ(fixture.Frames(), frames);
	const auto projection = fixture.Projection();
	UAM_ASSERT_EQ(projection.at("requestPending"), true);
	UAM_ASSERT_EQ(projection.at("effective"), observed.at("effective"));
	UAM_ASSERT_EQ(projection.at("pending"), observed.at("pending"));
	UAM_ASSERT_EQ(projection.at("activating"), observed.at("activating"));
	UAM_ASSERT(session.last_error.empty() && session.last_turn_error.empty());
	UAM_ASSERT(!session.processing && session.prompt_request_id == 0 && session.queued_prompt.empty() && session.queued_user_prompts.empty());
	UAM_ASSERT(fixture.Chat().messages.empty());
}

UAM_TEST(CopilotNativeAutoRoutingUnsupportedSwitchSurvivesReadsAndEvents)
{
	CopilotAutoDeadlineSink fixture;
	fixture.Discover();
	auto& session = *fixture.session;
	auto& chat = fixture.Chat();
	const auto observed = fixture.Projection();
	const auto key = fixture.Switch();
	const int serial = session.turn_serial;
	UAM_ASSERT(uam::acp_detail::ProcessAcpLine(fixture.app, session, chat,
	    nlohmann::json{{"jsonrpc", "2.0"}, {"id", std::stoi(key)},
	        {"error", {{"code", -32601}, {"message", "switchAutoTier method unavailable in fixture"}}}}.dump(), nullptr));
	UAM_ASSERT(fixture.Requests().empty());
	UAM_ASSERT(!session.pending_request_methods.contains(std::stoi(key)));
	UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
	UAM_ASSERT(session.config_option_change_id.empty());
	const auto unsupported = fixture.Projection();
	UAM_ASSERT_EQ(unsupported.at("available"), false);
	UAM_ASSERT(unsupported.at("disabledReason").get<std::string>().find("switchAutoTier") != std::string::npos);
	UAM_ASSERT_EQ(unsupported.at("effective"), observed.at("effective"));
	UAM_ASSERT_EQ(unsupported.at("pending"), observed.at("pending"));
	UAM_ASSERT_EQ(unsupported.at("activating"), observed.at("activating"));
	// Successful read APIs do not establish support for a rejected mutation method.
	const auto before_refresh = fixture.Frames().size();
	std::string error;
	UAM_ASSERT(fixture.runtime.OnAcpRefreshNativeModelOptions(fixture.app, session, chat, &error));
	UAM_ASSERT_EQ(fixture.Frames().size(), before_refresh + 2);
	UAM_ASSERT_EQ(fixture.Requests().size(), std::size_t{2});
	fixture.ReplyReads();
	const auto after_refresh = fixture.Projection();
	// Exercise a later event/read too, in this same native process/session/public lifetime.
	const auto before_event = fixture.Frames().size();
	UAM_ASSERT(fixture.runtime.OnAcpHandleMessage(fixture.app, session, chat,
	    CopilotEvent("session.model_change", {{"autoTier", "balance"}}, "deadline-after-unsupported", chat.native_session_id), nullptr));
	const auto event_frames = fixture.Frames();
	UAM_ASSERT_EQ(event_frames.size(), before_event + 1);
	UAM_ASSERT_EQ(event_frames.back().at("method"), "session.model.getCurrent");
	UAM_ASSERT_EQ(fixture.Requests().size(), std::size_t{1});
	fixture.ReplyReads();
	const auto after_event = fixture.Projection();
	const auto before_retry = fixture.Frames();
	const bool retry_admitted = fixture.runtime.OnAcpRequestNativeModelOption(fixture.app, session, chat,
	    "autoRouting", "vendor/Latency:Fast+V2", fixture.owner, &error);
	const auto after_retry = fixture.Frames();
	fixture.AssertSameProcess();
	UAM_ASSERT_EQ(after_refresh.at("available"), false); // Intended r1 adapter RED: successful reads erase unsupported switching.
	UAM_ASSERT_EQ(after_event.at("available"), false);
	for (const auto& projection : {after_refresh, after_event})
	{
		UAM_ASSERT_EQ(projection.at("ownership"), fixture.owner);
		UAM_ASSERT_EQ(projection.at("catalogStatus"), "ready");
		UAM_ASSERT_EQ(projection.at("requestPending"), false);
		UAM_ASSERT(!projection.at("disabledCode").get<std::string>().empty());
		const auto reason = projection.at("disabledReason").get<std::string>();
		UAM_ASSERT(!reason.empty() && reason.size() <= 2048 && reason.find("switchAutoTier") != std::string::npos);
		UAM_ASSERT_EQ(projection.at("effective"), observed.at("effective"));
		UAM_ASSERT_EQ(projection.at("pending"), observed.at("pending"));
		UAM_ASSERT_EQ(projection.at("activating"), observed.at("activating"));
	}
	UAM_ASSERT(!retry_admitted);
	UAM_ASSERT(!error.empty() && error.size() <= 2048);
	UAM_ASSERT_EQ(after_retry, before_retry);
	UAM_ASSERT(fixture.Requests().empty() && session.pending_request_methods.empty());
	UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
	UAM_ASSERT(session.config_option_change_id.empty());
	UAM_ASSERT(session.last_error.empty() && session.last_turn_error.empty());
	UAM_ASSERT(!session.processing && session.prompt_request_id == 0 && session.queued_prompt.empty() && session.queued_user_prompts.empty());
	UAM_ASSERT_EQ(session.turn_serial, serial);
	UAM_ASSERT(chat.messages.empty());
}

#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI

UAM_TEST(CopilotNativeAutoRoutingUnsupportedActivationAllowsExplicitDefaultAfterRefresh)
{
	CopilotAutoDeadlineSink fixture;
	fixture.Discover();
	auto& session = *fixture.session;
	auto& chat = fixture.Chat();
	const std::string requested = "vendor/Latency:Fast+V2";
	const auto owner = session.copilot_native_state.at("autoRouting").at("owner");
	const auto effective = fixture.Projection().at("effective");
	const int serial = session.turn_serial;
	const auto switch_key = fixture.Switch();
	const auto before_ack = fixture.Frames();
	// A correlated pending ACK proves method acceptance, without committing this preference.
	fixture.Result(switch_key, {{"status", "pending"}, {"effectiveAutoTier", "balance"}, {"pendingAutoTier", requested}});
	UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
	UAM_ASSERT(session.config_option_change_id.empty());
	UAM_ASSERT_EQ(fixture.Projection().at("effective"), effective);
	UAM_ASSERT_EQ(fixture.Projection().at("lastAckStatus"), "pending");
	const nlohmann::json staged = {{"present", true}, {"value", requested}};
	UAM_ASSERT_EQ(fixture.Projection().at("pending"), staged);
	const auto ack_frames = fixture.Frames();
	UAM_ASSERT_EQ(ack_frames.size(), before_ack.size() + 1);
	UAM_ASSERT_EQ(ack_frames.back().at("method"), "session.model.getCurrent");
	UAM_ASSERT_EQ(ack_frames.back().at("params"), (nlohmann::json{{"sessionId", chat.native_session_id}}));
	const auto ack_reads = fixture.Requests();
	UAM_ASSERT_EQ(ack_reads.size(), std::size_t{1});
	const auto current_key = ack_reads.begin().key();
	UAM_ASSERT_EQ(ack_reads.at(current_key).at("method"), "session.model.getCurrent");
	UAM_ASSERT_EQ(ack_reads.at(current_key).at("owner"), owner);
	// Settle ACK reconciliation before the later activation event, so explicit refresh is unobstructed.
	fixture.Result(current_key, {{"modelId", "auto"}, {"autoTier", "balance"}, {"pendingAutoTier", requested}});
	UAM_ASSERT(fixture.Requests().empty() && session.pending_request_methods.empty());
	UAM_ASSERT_EQ(fixture.Projection().at("pending"), staged);
	fixture.AssertSameProcess();

	// The existing SDK event shape reports failure of this requested preference, not JSON-RPC -32601.
	const auto failure_event = CopilotEvent("session.auto_tier_switch_failed",
	    {{"requestedAutoTier", requested}, {"effectiveAutoTier", "balance"}, {"reason", "unsupported"}},
	    "unsupported-activation-after-pending-ack", chat.native_session_id);
	UAM_ASSERT(fixture.runtime.OnAcpHandleMessage(fixture.app, session, chat, failure_event, nullptr));
	const auto failed = fixture.Projection();
	UAM_ASSERT_EQ(failed.at("ownership"), fixture.owner);
	UAM_ASSERT_EQ(failed.at("available"), false);
	UAM_ASSERT_EQ(failed.at("stateStatus"), "unsupported");
	UAM_ASSERT(failed.at("lastError").get<std::string>().find("unsupported") != std::string::npos);
	UAM_ASSERT_EQ(failed.at("effective"), effective);
	UAM_ASSERT_EQ(failed.at("pending"), staged);
	UAM_ASSERT_EQ(failed.at("activating"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT_EQ(fixture.Frames(), ack_frames); // No implicit retry or Default reset on activation failure.
	UAM_ASSERT(fixture.Requests().empty() && session.pending_request_methods.empty());
	UAM_ASSERT_EQ(session.config_option_change_request_id, 0);
	UAM_ASSERT(session.config_option_change_id.empty());
	fixture.AssertSameProcess();

	std::string error;
	UAM_ASSERT(fixture.runtime.OnAcpRefreshNativeModelOptions(fixture.app, session, chat, &error));
	const auto refresh_frames = fixture.Frames();
	UAM_ASSERT_EQ(refresh_frames.size(), ack_frames.size() + 2);
	UAM_ASSERT_EQ(refresh_frames[ack_frames.size()].at("method"), "session.model.list");
	UAM_ASSERT_EQ(refresh_frames.back().at("method"), "session.model.getCurrent");
	const auto refresh_reads = fixture.Requests();
	UAM_ASSERT_EQ(refresh_reads.size(), std::size_t{2});
	for (auto it = refresh_reads.begin(); it != refresh_reads.end(); ++it)
	{
		const auto method = it->at("method").get<std::string>();
		UAM_ASSERT(method == "session.model.list" || method == "session.model.getCurrent");
		UAM_ASSERT_EQ(it->at("owner"), owner);
		UAM_ASSERT_EQ(session.pending_request_methods.at(std::stoi(it.key())), method);
		fixture.Result(it.key(), method == "session.model.list" ? CopilotAutoRoutingFixtureCatalog() :
		    nlohmann::json{{"modelId", "auto"}, {"autoTier", "balance"}});
	}
	const auto refreshed = fixture.Projection();
	UAM_ASSERT_EQ(refreshed.at("ownership"), fixture.owner);
	UAM_ASSERT_EQ(refreshed.at("catalogStatus"), "ready");
	UAM_ASSERT_EQ(refreshed.at("stateStatus"), "ready");
	UAM_ASSERT_EQ(refreshed.at("requestPending"), false);
	UAM_ASSERT_EQ(refreshed.at("available"), true); // Preserving control: expected PASS on the pre-r3 adapter.
	UAM_ASSERT(refreshed.at("disabledCode").get<std::string>().empty());
	UAM_ASSERT_EQ(refreshed.at("effective"), effective);
	UAM_ASSERT_EQ(refreshed.at("pending"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT_EQ(refreshed.at("activating"), (nlohmann::json{{"present", false}}));
	UAM_ASSERT(fixture.Requests().empty() && session.pending_request_methods.empty());
	UAM_ASSERT_EQ(fixture.Frames(), refresh_frames);

	// This explicit action, after refresh, is the only Default reset emitted in the scenario.
	UAM_ASSERT(fixture.runtime.OnAcpRequestNativeModelOption(fixture.app, session, chat, "autoRouting", nullptr, fixture.owner, &error));
	const auto reset_frames = fixture.Frames();
	UAM_ASSERT_EQ(reset_frames.size(), refresh_frames.size() + 1);
	const auto& reset = reset_frames.back();
	UAM_ASSERT_EQ(reset.at("method"), "session.model.switchAutoTier");
	UAM_ASSERT_EQ(reset.at("params"), (nlohmann::json{{"sessionId", chat.native_session_id}, {"autoTier", nullptr}}));
	const int reset_id = reset.at("id").get<int>();
	UAM_ASSERT(reset_id != std::stoi(switch_key));
	const auto reset_requests = fixture.Requests();
	UAM_ASSERT_EQ(reset_requests.size(), std::size_t{1});
	UAM_ASSERT_EQ(reset_requests.at(std::to_string(reset_id)).at("method"), "session.model.switchAutoTier");
	UAM_ASSERT_EQ(reset_requests.at(std::to_string(reset_id)).at("owner"), owner);
	UAM_ASSERT(reset_requests.at(std::to_string(reset_id)).at("requested").is_null());
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.pending_request_methods.at(reset_id), "session.model.switchAutoTier");
	UAM_ASSERT_EQ(session.config_option_change_request_id, reset_id);
	UAM_ASSERT_EQ(session.config_option_change_id, std::string("autoRouting"));
	UAM_ASSERT_EQ(fixture.Projection().at("requestPending"), true);
	UAM_ASSERT_EQ(fixture.Projection().at("effective"), effective); // Admission never commits Default optimistically.
	UAM_ASSERT_EQ(fixture.Projection().at("pending"), refreshed.at("pending"));
	UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("owner"), owner);
	fixture.AssertSameProcess();
	UAM_ASSERT(session.last_error.empty() && session.last_turn_error.empty());
	UAM_ASSERT_EQ(session.turn_serial, serial);
	UAM_ASSERT(!session.processing && session.prompt_request_id == 0 && session.queued_prompt.empty() && session.queued_user_prompts.empty());
	UAM_ASSERT(chat.messages.empty() && chat.acp_queued_prompts.empty());
}

// Native lifecycle timing tests use only public JSON projections so that the
// missing metadata is a behavioral failure before any model fields are added.
namespace
{

constexpr std::int64_t kCopilotTimingStart = 1792238400000LL;
constexpr const char* kCopilotTimingParent = "550e8400-e29b-41d4-a716-446655440000";

nlohmann::json CopilotTimingEvent(const std::string& type, const std::string& agent, const std::string& tool,
    const std::string& timestamp, const std::string& event_id, const nlohmann::json& duration = nullptr)
{
	nlohmann::json frame = CopilotEvent(type, {{"toolCallId", tool}, {"agentName", agent}, {"agentDisplayName", agent}, {"agentDescription", "Private timing fixture"}}, event_id, kCopilotTimingParent);
	frame["params"]["event"]["agentId"] = agent;
	if (!timestamp.empty()) frame["params"]["event"]["timestamp"] = timestamp;
	if (!duration.is_null()) frame["params"]["event"]["data"]["durationMs"] = duration;
	if (type == "subagent.failed") frame["params"]["event"]["data"]["error"] = "Private child failure";
	return frame;
}

nlohmann::json CopilotTimingTool(const nlohmann::json& serialized, const std::string& tool_id)
{
	for (const auto& message : serialized.at("messages"))
	{
		if (!message.contains("toolCalls")) continue;
		for (const auto& tool : message.at("toolCalls")) if (tool.value("id", "") == tool_id) return tool;
	}
	UAM_ASSERT(false);
	return nullptr;
}

nlohmann::json CopilotTimingChild(const nlohmann::json& tool, const std::string& child_id)
{
	const auto children = tool.find("subAgentChildren");
	if (children != tool.end() && children->is_array())
		for (const auto& child : *children) if (child.value("id", "") == child_id) return child;
	return nullptr;
}

void AssertCopilotChildClock(const nlohmann::json& child, std::int64_t start, std::int64_t end, std::int64_t duration)
{
	UAM_ASSERT(child.is_object());
	UAM_ASSERT_EQ(child.at("startedAtEpochMs"), nlohmann::json(start));
	UAM_ASSERT_EQ(child.at("finishedAtEpochMs"), nlohmann::json(end));
	UAM_ASSERT_EQ(child.at("durationMs"), nlohmann::json(duration));
}

struct CopilotTimingFixture
{
	TempDir root{"uam-copilot-native-child-timing"};
	uam::AppState app;
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("copilot-cli");

	CopilotTimingFixture()
	{
		app.data_root = root.root;
		app.provider_profiles = {ProviderProfileStore::DefaultCopilotProfile()};
		ChatSession chat;
		chat.id = "copilot-native-timing-parent";
		chat.provider_id = "copilot-cli";
		chat.native_session_id = kCopilotTimingParent;
		chat.execution_host_id = "local";
		chat.workspace_directory = root.root.string();
		app.chats.push_back(std::move(chat));
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = Chat().id;
		session->provider_id = Chat().provider_id;
		session->protocol_kind = "copilot-sdk";
		session->session_id = kCopilotTimingParent;
		session->running = session->session_ready = session->processing = true;
		app.acp_sessions.push_back(std::move(session));
		Emit(CopilotEvent("assistant.message", {{"messageId", "timing-parent-message"}, {"content", "Parent work precedes child lifecycle"}}, "timing-parent-text", kCopilotTimingParent));
	}
	ChatSession& Chat() { return app.chats.front(); }
	uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
	void Emit(const nlohmann::json& frame) { UAM_ASSERT(runtime.OnAcpHandleMessage(app, Session(), Chat(), frame, nullptr)); }
	nlohmann::json Saved() { return uam::StateSerializer::SerializeSession(Chat()); }
	std::string ChildId(const std::string& agent) { return BuildCopilotAgentHistoryId(kCopilotTimingParent, agent, "local"); }
};

}

UAM_TEST(CopilotNativeChildTimingSurvivesSaveReloadAndDeferredProjection)
{
	CopilotTimingFixture fixture;
	const auto a_start = CopilotTimingEvent("subagent.started", "clock-a", "clock-tool-a", "2026-10-17T12:00:00Z", "clock-a-start");
	const auto a_end = CopilotTimingEvent("subagent.completed", "clock-a", "clock-tool-a", "2026-10-17T12:00:09Z", "clock-a-end", 9000);
	fixture.Emit(a_start);
	fixture.Emit(CopilotTimingEvent("subagent.started", "clock-b", "clock-tool-b", "2026-10-17T12:00:05Z", "clock-b-start"));
	fixture.Emit(a_end);
	fixture.Emit(CopilotTimingEvent("subagent.failed", "clock-b", "clock-tool-b", "2026-10-17T12:00:26Z", "clock-b-end", 21000));
	const auto before_replay = fixture.Saved();
	auto replay = a_end;
	replay["params"]["event"]["timestamp"] = "2026-10-17T12:59:59Z";
	replay["params"]["event"]["data"]["durationMs"] = 999000;
	fixture.Emit(replay);
	UAM_ASSERT_EQ(fixture.Saved(), before_replay);
	auto foreign = replay;
	foreign["params"]["sessionId"] = "different-native-session";
	foreign["params"]["event"]["id"] = "foreign-child-end";
	fixture.Emit(foreign);
	UAM_ASSERT_EQ(fixture.Saved(), before_replay);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(loaded.has_value());
	const auto saved = uam::StateSerializer::SerializeSession(*loaded);
	const auto page = uam::StateSerializer::SerializeMessagePage(*loaded, 20, std::nullopt, true);
	const auto live = uam::StateSerializer::Serialize(fixture.app).at("chats").at(0).at("acpSession").at("toolCalls");
	for (const auto& [tool_id, agent, start, end, duration] : {
	         std::tuple{std::string("clock-tool-a"), std::string("clock-a"), kCopilotTimingStart, kCopilotTimingStart + 9000, std::int64_t(9000)},
	         std::tuple{std::string("clock-tool-b"), std::string("clock-b"), kCopilotTimingStart + 5000, kCopilotTimingStart + 26000, std::int64_t(21000)}})
	{
		const std::string child_id = fixture.ChildId(agent);
		UAM_ASSERT_EQ(CopilotTimingTool(saved, tool_id).at("subAgentId"), nlohmann::json(child_id));
		AssertCopilotChildClock(CopilotTimingChild(CopilotTimingTool(saved, tool_id), child_id), start, end, duration);
		AssertCopilotChildClock(CopilotTimingChild(CopilotTimingTool(page, tool_id), child_id), start, end, duration);
		bool found_live = false;
		for (const auto& tool : live) if (tool.value("id", "") == tool_id)
		{
			AssertCopilotChildClock(CopilotTimingChild(tool, child_id), start, end, duration);
			found_live = true;
		}
		UAM_ASSERT(found_live);
	}
}

UAM_TEST(CopilotNativeChildTimingLeavesMissingInvalidAndAnonymousEvidenceUnknown)
{
	CopilotTimingFixture fixture;
	const std::vector<nlohmann::json> invalid_durations = {-1, 0.5, 9007199254740992ULL, "9000", true, nlohmann::json::object()};
	for (std::size_t index = 0; index < invalid_durations.size() + 1; ++index)
	{
		const std::string agent = "unknown-clock-" + std::to_string(index);
		const std::string tool_id = "unknown-tool-" + std::to_string(index);
		const bool absent = index == invalid_durations.size();
		fixture.Emit(CopilotTimingEvent("subagent.started", agent, tool_id, absent ? "" : "2026-99-99T25:99:99Z", agent + "-start"));
		fixture.Emit(CopilotTimingEvent("subagent.completed", agent, tool_id, absent ? "" : "not-a-timestamp", agent + "-end", absent ? nlohmann::json(nullptr) : invalid_durations[index]));
		const auto tool = CopilotTimingTool(fixture.Saved(), tool_id);
		UAM_ASSERT_EQ(tool.at("subAgentId"), nlohmann::json(fixture.ChildId(agent)));
		const auto child = CopilotTimingChild(tool, fixture.ChildId(agent));
		UAM_ASSERT(!child.contains("startedAtEpochMs") && !child.contains("finishedAtEpochMs") && !child.contains("durationMs"));
	}
	fixture.Emit(CopilotTimingEvent("subagent.started", "", "anonymous-clock", "2026-10-17T12:00:00Z", "anonymous-clock-start"));
	fixture.Emit(CopilotTimingEvent("subagent.completed", "", "anonymous-clock", "2026-10-17T12:00:09Z", "anonymous-clock-end", 9000));
	const auto anonymous = CopilotTimingTool(fixture.Saved(), "anonymous-clock");
	UAM_ASSERT_EQ(anonymous.at("subAgentId"), nlohmann::json(""));
	const auto children = anonymous.find("subAgentChildren");
	UAM_ASSERT(children == anonymous.end() || children->empty());
}

UAM_TEST(CopilotNativeChildTimingUsesPrivateJournalLifecycleRatherThanParentToolTime)
{
	TempDir root("uam-copilot-child-journal-timing");
	ScopedEnvVar home("COPILOT_HOME", (root.root / "copilot-home").string());
	const auto workspace = root.root / "workspace";
	fs::create_directories(workspace);
	const auto directory = root.root / "copilot-home" / "session-state" / kCopilotTimingParent;
	nlohmann::json records = nlohmann::json::array({
	    {{"id", "journal-start"}, {"type", "session.start"}, {"timestamp", "2026-10-17T11:00:00Z"}, {"data", {{"sessionId", kCopilotTimingParent}, {"context", {{"cwd", workspace.string()}}}}}},
	    {{"id", "journal-user"}, {"type", "user.message"}, {"timestamp", "2026-10-17T11:01:00Z"}, {"data", {{"content", "Private child timing request"}}}},
	    {{"id", "parent-tool-start"}, {"type", "tool.execution_start"}, {"timestamp", "2026-10-17T11:02:00Z"}, {"data", {{"toolCallId", "clock-tool-a"}, {"toolName", "task"}}}},
	});
	for (const auto& frame : {
	         CopilotTimingEvent("subagent.started", "clock-a", "clock-tool-a", "2026-10-17T12:00:00Z", "journal-a-start"),
	         CopilotTimingEvent("subagent.started", "clock-b", "clock-tool-b", "2026-10-17T12:00:05Z", "journal-b-start"),
	         CopilotTimingEvent("subagent.completed", "clock-a", "clock-tool-a", "2026-10-17T12:00:09Z", "journal-a-end", 9000),
	         CopilotTimingEvent("subagent.failed", "clock-b", "clock-tool-b", "2026-10-17T12:00:26Z", "journal-b-end", 21000)})
		records.push_back(frame.at("params").at("event"));
	records.push_back({{"id", "parent-tool-end"}, {"type", "tool.execution_complete"}, {"timestamp", "2026-10-17T13:00:00Z"}, {"data", {{"toolCallId", "clock-tool-a"}, {"success", true}, {"durationMs", 999000}}}});
	std::string text;
	for (const auto& record : records) text += record.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", text));
	const auto imported = LoadCopilotSessionStateChat(directory, workspace);
	UAM_ASSERT(imported.has_value());
	UAM_ASSERT(ChatRepository::SaveChat(root.root / "uam-data", *imported));
	const auto reloaded = ChatRepository::LoadLocalChat(root.root / "uam-data", imported->id);
	UAM_ASSERT(reloaded.has_value());
	const auto page = uam::StateSerializer::SerializeMessagePage(*reloaded, 20, std::nullopt, true);
	AssertCopilotChildClock(CopilotTimingChild(CopilotTimingTool(page, "clock-tool-a"), BuildCopilotAgentHistoryId(kCopilotTimingParent, "clock-a", "local")), kCopilotTimingStart, kCopilotTimingStart + 9000, 9000);
	AssertCopilotChildClock(CopilotTimingChild(CopilotTimingTool(page, "clock-tool-b"), BuildCopilotAgentHistoryId(kCopilotTimingParent, "clock-b", "local")), kCopilotTimingStart + 5000, kCopilotTimingStart + 26000, 21000);
}

UAM_TEST(CopilotNativeChildTimingRejectsUnsafeDeclaredSessionAndMatchesJournalAgent)
{
	CopilotTimingFixture fixture;
	ScopedEnvVar home("COPILOT_HOME", (fixture.root.root / "copilot-home").string());
	auto started = CopilotTimingEvent("subagent.started", "unsafe-session-clock", "unsafe-session-tool", "2026-10-17T12:00:00Z", "unsafe-session-start");
	auto completed = CopilotTimingEvent("subagent.completed", "unsafe-session-clock", "unsafe-session-tool", "2026-10-17T12:00:09Z", "unsafe-session-end", 9000);
	started["params"]["event"]["data"]["sessionId"] = "../foreign";
	completed["params"]["event"]["data"]["sessionId"] = "../foreign";
	fixture.Emit(started);
	fixture.Emit(completed);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(loaded.has_value());
	const auto saved = uam::StateSerializer::SerializeSession(*loaded);
	const auto page = uam::StateSerializer::SerializeMessagePage(*loaded, 20, std::nullopt, true);
	const auto live = uam::StateSerializer::Serialize(fixture.app).at("chats").at(0).at("acpSession").at("toolCalls");
	const auto directory = fixture.root.root / "copilot-home" / "session-state" / kCopilotTimingParent;
	const nlohmann::json records = nlohmann::json::array({
	    {{"id", "unsafe-session-journal-start"}, {"type", "session.start"}, {"data", {{"sessionId", kCopilotTimingParent}, {"context", {{"cwd", fixture.root.root.string()}}}}}},
	    {{"id", "unsafe-session-journal-user"}, {"type", "user.message"}, {"data", {{"content", "Read the exact child clock"}}}},
	    started.at("params").at("event"), completed.at("params").at("event")});
	std::string text;
	for (const auto& record : records) text += record.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", text));
	const auto imported = LoadCopilotSessionStateChat(directory, fixture.root.root);
	UAM_ASSERT(imported.has_value());
	const auto journal = uam::StateSerializer::SerializeSession(*imported);
	const std::string expected_child = fixture.ChildId("unsafe-session-clock");
	for (const auto& projection : {saved, page, journal})
	{
		const auto tool = CopilotTimingTool(projection, "unsafe-session-tool");
		UAM_ASSERT_EQ(tool.at("subAgentId"), nlohmann::json(expected_child));
		AssertCopilotChildClock(CopilotTimingChild(tool, expected_child), kCopilotTimingStart, kCopilotTimingStart + 9000, 9000);
		UAM_ASSERT(CopilotTimingChild(tool, "../foreign").is_null());
	}
	bool found_live = false;
	for (const auto& tool : live) if (tool.value("id", "") == "unsafe-session-tool")
	{
		UAM_ASSERT_EQ(tool.at("subAgentId"), nlohmann::json(expected_child));
		AssertCopilotChildClock(CopilotTimingChild(tool, expected_child), kCopilotTimingStart, kCopilotTimingStart + 9000, 9000);
		UAM_ASSERT(CopilotTimingChild(tool, "../foreign").is_null());
		found_live = true;
	}
	UAM_ASSERT(found_live);
}

UAM_TEST(CopilotNativeCancelledChildTimingRemainsStoppedAfterLaterTerminalObservation)
{
	CopilotTimingFixture fixture;
	fixture.Emit(CopilotTimingEvent("subagent.started", "cancelled-clock", "cancelled-clock-tool", "2026-10-17T12:00:00Z", "cancelled-clock-start"));
	auto completed = CopilotTimingEvent("subagent.completed", "cancelled-clock", "cancelled-clock-tool", "2026-10-17T12:00:09Z", "cancelled-clock-end", 9000);
	completed["params"]["event"]["data"]["cancelled"] = true;
	fixture.Emit(completed);
	auto later = completed;
	later["params"]["event"]["id"] = "cancelled-clock-later-terminal";
	later["params"]["event"]["timestamp"] = "2026-10-17T12:59:59Z";
	later["params"]["event"]["data"]["cancelled"] = false;
	later["params"]["event"]["data"]["durationMs"] = 999000;
	fixture.Emit(later);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(loaded.has_value());
	const auto page = uam::StateSerializer::SerializeMessagePage(*loaded, 20, std::nullopt, true);
	const auto child = CopilotTimingChild(CopilotTimingTool(page, "cancelled-clock-tool"), fixture.ChildId("cancelled-clock"));
	UAM_ASSERT(child.is_object());
	UAM_ASSERT_EQ(child.at("status"), nlohmann::json("cancelled"));
	AssertCopilotChildClock(child, kCopilotTimingStart, kCopilotTimingStart + 9000, 9000);
	const auto live = uam::StateSerializer::Serialize(fixture.app).at("chats").at(0).at("acpSession").at("toolCalls");
	bool found_live = false;
	for (const auto& tool : live) if (tool.value("id", "") == "cancelled-clock-tool")
	{
		UAM_ASSERT_EQ(CopilotTimingChild(tool, fixture.ChildId("cancelled-clock")).at("status"), nlohmann::json("cancelled"));
		AssertCopilotChildClock(CopilotTimingChild(tool, fixture.ChildId("cancelled-clock")), kCopilotTimingStart, kCopilotTimingStart + 9000, 9000);
		found_live = true;
	}
	UAM_ASSERT(found_live);
}

UAM_TEST(CopilotNativeChildTimingPreservesGenuineZeroDurationAcrossSaveAndDeferredRead)
{
	CopilotTimingFixture fixture;
	fixture.Emit(CopilotTimingEvent("subagent.started", "zero-clock", "zero-clock-tool", "2026-10-17T12:00:00Z", "zero-clock-start"));
	fixture.Emit(CopilotTimingEvent("subagent.completed", "zero-clock", "zero-clock-tool", "2026-10-17T12:00:00Z", "zero-clock-end", 0));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.Chat().id);
	UAM_ASSERT(loaded.has_value());
	const auto page = uam::StateSerializer::SerializeMessagePage(*loaded, 20, std::nullopt, true);
	const auto child = CopilotTimingChild(CopilotTimingTool(page, "zero-clock-tool"), fixture.ChildId("zero-clock"));
	AssertCopilotChildClock(child, kCopilotTimingStart, kCopilotTimingStart, 0);
}


namespace
{
std::vector<nlohmann::json> CopilotMcpWitnessEvents(int scenario, const std::string& result)
{
	using Json = nlohmann::json;
	const auto event = [](const char* type, Json data, const char* id) { return Json{{"type", type}, {"id", id}, {"data", std::move(data)}}; };
	Json start = {{"toolCallId", "delegate-call"}, {"toolName", "agent_delegate"}, {"toolTitle", scenario == 9 ? "Native visible title" : "agent_delegate"}, {"arguments", {{"agentId", "plan"}}}};
	const Json owned = {{"mcpServerName", "uam-control"}, {"mcpToolName", "agent_delegate"}};
	std::vector<Json> events;
	if (scenario == 1 || scenario == 4)
	{
		Json request = owned; request["toolCallId"] = "delegate-call"; request["name"] = "agent_delegate";
		events.push_back(event("assistant.message", {{"messageId", "request-message"}, {"content", ""}, {"toolRequests", Json::array({request})}}, "request"));
	}
	if (scenario != 1 && scenario != 3 && scenario != 7) start.update(owned);
	if (scenario == 2 || scenario == 4) start["mcpServerName"] = "foreign";
	if (scenario == 5) start["mcpServerName"] = 7;
	if (scenario == 6) start["mcpConfigServerName"] = "foreign";
	if (scenario == 10) { start["toolName"] = "view"; start["toolTitle"] = "view"; start["mcpToolName"] = "view"; }
	if (scenario != 7) events.push_back(event("tool.execution_start", start, "start"));
	if (scenario == 4)
	{
		Json request = owned; request["toolCallId"] = "delegate-call"; request["name"] = "agent_delegate";
		events.push_back(event("assistant.message", {{"messageId", "contradiction-message"}, {"content", ""}, {"toolRequests", Json::array({request})}}, "contradiction"));
	}
	const Json resume = {{"eventCount", 3}, {"resumeTime", "2026-10-20T20:00:00Z"}};
	if (scenario == 12) events.push_back(event("session.resume", resume, "reset"));
	if (scenario == 14)
	{
		// The same accepted resume event is replayed after the witnessed start.
		events.insert(events.begin(), event("session.resume", resume, "duplicate-resume"));
		events.push_back(event("session.resume", resume, "duplicate-resume"));
	}
	if (scenario == 16)
	{
		Json request = owned; request["toolCallId"] = "delegate-call"; request["name"] = "agent_delegate";
		events.insert(events.begin(), event("assistant.message", {{"messageId", "native-turn-request"}, {"turnId", "native-A"}, {"content", ""}, {"toolRequests", Json::array({request})}}, "native-turn-request"));
		for (auto& row : events) if (row["type"] == "tool.execution_start")
		{
			row["data"].erase("mcpServerName"); row["data"].erase("mcpToolName"); row["data"]["turnId"] = "native-B";
		}
	}
	if (scenario == 17) events.push_back(event("user.message", {{"content", "A later native user turn."}, {"turnId", "native-B"}}, "later-user"));
	if (scenario == 25) events.push_back(event("session.resume", Json::object(), "malformed-resume"));
	if (scenario == 18) events.push_back(event("assistant.turn_start", {{"turnId", "native-B"}}, "later-native-turn"));
	if (scenario == 24) events.push_back(event("assistant.turn_start", {{"parentToolCallId", "child-owner"}, {"turnId", "native-B"}}, "child-native-turn"));
	if (scenario == 15 || scenario >= 17)
		for (auto& row : events) if (row["type"] == "tool.execution_start") row["data"]["turnId"] = scenario == 19 ? Json(7) : Json("native-A");

	events.push_back(event("tool.execution_complete", {{"toolCallId", scenario == 11 ? "other-call" : "delegate-call"}, {"success", scenario != 8}, {"result", {{"content", result}}}}, "complete"));
	if (scenario == 13) events.push_back(event("tool.execution_complete", {{"toolCallId", "delegate-call"}, {"success", true}, {"result", {{"content", result}}}}, "consumed-complete"));
	if (scenario == 15 || scenario == 16) events.back()["data"]["turnId"] = "native-B";
	if (scenario == 20 || scenario == 24) events.back()["data"]["turnId"] = "native-A";
	if (scenario == 22 || scenario == 23)
	{
		events.insert(events.begin(), event("assistant.turn_start", {{"turnId", scenario == 22 ? "native-B" : "native-A"}}, "initial-native-turn"));
		events.back()["data"]["turnId"] = "native-A";
	}
	return events;
}

std::vector<const ToolCall*> CopilotMcpWitnessTools(const ChatSession& chat)
{
	std::vector<const ToolCall*> tools;
	for (const Message& message : chat.messages)
		for (const ToolCall& tool : message.tool_calls) tools.push_back(&tool);
	return tools;
}

void AssertCopilotMcpWitnessTool(const ToolCall& tool, int scenario, const std::string& result, bool live)
{
	UAM_ASSERT_EQ(tool.name, std::string(scenario == 9 ? "Native visible title" : scenario == 7 || scenario == 11 ? (live ? scenario == 7 ? "delegate-call" : "other-call" : "") : scenario == 10 ? "view" : "agent_delegate"));
	UAM_ASSERT_EQ(tool.status, std::string(scenario == 8 ? "failed" : "completed"));
	if (scenario != 7 && scenario != 11)
	{
		UAM_ASSERT_EQ(tool.kind, std::string("other"));
		UAM_ASSERT_EQ(nlohmann::json::parse(tool.args_json), (nlohmann::json{{"agentId", "plan"}}));
	}
	if (scenario == 0 || scenario == 1 || scenario == 9 || scenario == 14 || scenario == 20 || scenario == 21 || scenario == 23 || scenario == 24 || scenario == 25)
	{
		const auto wrapped = nlohmann::json::parse(tool.result_text);
		UAM_ASSERT_EQ(wrapped.at("server"), nlohmann::json("uam-control"));
		UAM_ASSERT_EQ(wrapped.at("tool"), nlohmann::json("agent_delegate"));
		UAM_ASSERT_EQ(wrapped.at("status"), nlohmann::json("completed"));
		UAM_ASSERT_EQ(wrapped.at("result").at("content").at(0).at("text"), nlohmann::json(result));
	}
	else UAM_ASSERT_EQ(tool.result_text, result); // Missing/foreign/conflicting/malformed/completion-only/failed remains ordinary.
}
}

UAM_TEST(CopilotNativeMcpWitnessSurvivesLiveProjectionAndSavedReload)
{
	TempDir temp("uam-copilot-live-mcp-witness");
	const auto& runtime = ProviderRuntimeRegistry::ResolveById("copilot-cli");
	const std::string receipt = R"({"runId":"5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7","status":"queued"})";
	for (int scenario = 0; scenario != 26; ++scenario)
	{
		uam::AppState app; app.data_root = temp.root;
		ChatSession chat; chat.id = "copilot-witness-" + std::to_string(scenario); chat.provider_id = "copilot-cli"; chat.native_session_id = "native-session";
		chat.messages.push_back({MessageRole::User, "One owned presentation fixture."});
		uam::AcpSessionState session; session.chat_id = chat.id; session.provider_id = chat.provider_id; session.session_id = chat.native_session_id; session.protocol_kind = "copilot-sdk"; session.processing = true;
		if (scenario == 12 || scenario == 14 || scenario == 25)
		{
			chat.execution_host_id = "local";
			session.running = true; session.initialized = true; session.process_execution_host_id = "local";
			// Public native setup-result handling creates the real owned routing binding.
			UAM_ASSERT(runtime.OnAcpHandleResult(app, session, chat, "session.create", "101", {{"sessionId", "native-session"}}));
			UAM_ASSERT(session.session_ready && session.running && session.processing);
			const auto& owner = session.copilot_native_state.at("autoRouting").at("owner");
			UAM_ASSERT_EQ(owner.at("chatId"), nlohmann::json(chat.id));
			UAM_ASSERT_EQ(owner.at("sessionId"), nlohmann::json(session.session_id));
			UAM_ASSERT_EQ(owner.at("executionHostId"), nlohmann::json("local"));
		}
		for (const auto& event : CopilotMcpWitnessEvents(scenario, receipt))
		{
			if (scenario == 25 && event.at("id") == "malformed-resume")
			{
				const auto foreign = CopilotEvent("session.resume", {{"eventCount", 3}, {"resumeTime", "2026-10-20T20:00:00Z"}}, "foreign-resume", "foreign-session");
				UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, foreign, nullptr));
				UAM_ASSERT(!session.copilot_native_state.at("events").contains("foreign-resume"));
				UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("revision"), nlohmann::json(0));
			}
			UAM_ASSERT(runtime.OnAcpHandleMessage(app, session, chat, {{"jsonrpc", "2.0"}, {"method", "session.event"}, {"params", {{"sessionId", "native-session"}, {"event", event}}}}, nullptr));
		}
		if (scenario == 12 || scenario == 14 || scenario == 25)
		{
			const char* event_id = scenario == 12 ? "reset" : scenario == 14 ? "duplicate-resume" : "malformed-resume";
			UAM_ASSERT(session.copilot_native_state.at("events").contains(event_id));
			// One actual accepted routing revision proves this was not an ignored true return.
			UAM_ASSERT_EQ(session.copilot_native_state.at("autoRouting").at("revision"), nlohmann::json(1));
		}

		const auto tools = CopilotMcpWitnessTools(chat);
		UAM_ASSERT_EQ(tools.size(), std::size_t(scenario == 11 ? 2 : 1));
		std::printf("COPILOT_MCP_WITNESS scenario=%d stage=live\n", scenario);
		std::fflush(stdout);
		AssertCopilotMcpWitnessTool(*tools.back(), scenario, receipt, true);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		std::printf("COPILOT_MCP_WITNESS scenario=%d stage=reload\n", scenario);
		std::fflush(stdout);
		UAM_ASSERT(saved && saved->messages == chat.messages);
	}
}

UAM_TEST(CopilotNativeMcpWitnessSurvivesColdJournalProjection)
{
	TempDir temp("uam-copilot-cold-mcp-witness");
	const std::string receipt = R"({"runId":"5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7","status":"queued"})";
	for (int scenario = 0; scenario != 26; ++scenario)
	{
		const auto directory = temp.root / ("copilot-mcp-" + std::to_string(scenario));
		std::filesystem::create_directories(directory);
		UAM_ASSERT(uam::io::WriteTextFile(directory / "workspace.yaml", "cwd: \"/workspace\"\n"));
		std::vector<nlohmann::json> events = {{{"type", "session.start"}, {"data", {{"sessionId", directory.filename().string()}}}}, {{"type", "user.message"}, {"data", {{"content", "One owned presentation fixture."}}}}};
		const auto tool_events = CopilotMcpWitnessEvents(scenario, receipt); events.insert(events.end(), tool_events.begin(), tool_events.end());
		std::string journal;
		for (auto& event : events) { event["timestamp"] = "2026-10-20T20:00:00Z"; journal += event.dump() + '\n'; }
		UAM_ASSERT(uam::io::WriteTextFile(directory / "events.jsonl", journal));
		std::string error;
		const auto saved = LoadCopilotSessionStateChat(directory, "/workspace", {}, &error);
		UAM_ASSERT(saved && error.empty());
		UAM_ASSERT_EQ(saved->messages.front().role, MessageRole::User);
		const auto tools = CopilotMcpWitnessTools(*saved);
		UAM_ASSERT_EQ(tools.size(), std::size_t(scenario == 11 ? 2 : 1));
		AssertCopilotMcpWitnessTool(*tools.back(), scenario, receipt, false);
	}
}

#endif
