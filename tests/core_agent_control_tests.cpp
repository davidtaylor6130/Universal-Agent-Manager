#include "common/companion/protocol.h"
#include "common/provider/copilot_native_history.h"
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


UAM_TEST(AcpDiagnosticCapsPreserveUtf8AtEveryBoundary)
{
	const std::string text = std::string("A") + "\xC3\xA9" + "B" + "\xE2\x82\xAC" + "C" + "\xF0\x9F\x98\x80" + "D";
	for (std::size_t limit = 0; limit <= text.size(); ++limit)
	{
		const std::string capped = uam::acp_detail::CapDiagnosticString(text, limit);
		const std::string encoded = nlohmann::json{{"diagnostic", capped}}.dump();
		UAM_ASSERT_EQ(nlohmann::json::parse(encoded)["diagnostic"].get<std::string>(), capped);
		if (limit == text.size()) UAM_ASSERT_EQ(capped, text);
	}
}

namespace
{
/// <summary>Captures native requests using a private descriptor, including assertion cleanup.</summary>
class ManualCompactionWriter
{
public:
	explicit ManualCompactionWriter(uam::AcpSessionState& session) : m_session(session), m_file(std::tmpfile())
	{
		UAM_ASSERT(m_file != nullptr);
#if defined(_WIN32)
		m_session.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(m_file)));
		UAM_ASSERT(m_session.stdin_write != INVALID_HANDLE_VALUE && m_session.stdin_write != nullptr);
#else
		m_session.stdin_write_fd = fileno(m_file);
		UAM_ASSERT(m_session.stdin_write_fd >= 0);
#endif
		m_session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
		    [this](const char* bytes, std::size_t count, std::string&) -> std::ptrdiff_t
		    { captured.append(bytes, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
	}
	~ManualCompactionWriter()
	{
		Detach();
		std::fclose(m_file);
	}
	void Detach()
	{
		m_session.stdin_writer.reset();
#if defined(_WIN32)
		m_session.stdin_write = INVALID_HANDLE_VALUE;
#else
		m_session.stdin_write_fd = -1;
#endif
	}
	std::string captured;
private:
	uam::AcpSessionState& m_session;
	std::FILE* m_file;
};
}

UAM_TEST(PermissionReviewQueueAdvanceKeepsNewWorkerOwned)
{
#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-review-queue-ownership");
	const fs::path shim = temp.root / "gemini";
	const fs::path marker = temp.root / "private-review-marker";
	UAM_ASSERT(uam::io::WriteTextFile(shim,
	    "#!/bin/sh\nprintf 'private-stub\\n' > \"$UAM_TEST_REVIEW_VECTOR_MARKER\"\n"
	    "printf '%s\\n' '{\"response\":\"{\\\"decision\\\":\\\"uncertain\\\",\\\"reason\\\":\\\"Controlled fixture.\\\"}\"}'\n"));
	fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
	ScopedEnvVar path("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
	ScopedEnvVar reviewer_marker("UAM_TEST_REVIEW_VECTOR_MARKER", marker.string());
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
	app.settings.permission_reviewer_provider_id = "gemini-cli";
	app.settings.permission_reviewer_model_id = "private-fake-model";
	ChatSession chat;
	chat.id = "review-vector";
	chat.provider_id = "gemini-cli";
	chat.command_safety_tier = "aiReview";
	chat.approval_mode = "manual";
	app.chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = chat.provider_id;
	owned->running = true;
	owned->processing = true;
	owned->waiting_for_permission = true;
	owned->current_mode_id = "default";
	owned->pending_permission.request_id_json = "7";
	owned->pending_permission.options = {{"allow-once", "Allow once", "allow_once"}};
	uam::AcpPendingPermissionState next;
	next.request_id_json = "8";
	next.title = "Inspect the private fixture";
	next.kind = "read";
	next.content = "Read the controlled fixture only.";
	next.provider_input_json = "{}";
	next.options = {{"allow-once", "Allow once", "allow_once"}};
	owned->queued_permissions.push_back(next);
	uam::AcpSessionState& session = *owned;
	app.acp_sessions.push_back(std::move(owned));
	ManualCompactionWriter transport(session);
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::acp_detail::StopPermissionReviewTasks(app); }
	} cleanup{app};
	uam::AsyncPermissionReviewTask completed;
	completed.permission_review_incarnation = session.permission_review_incarnation;
	completed.chat_id = chat.id;
	completed.request_id_json = "7";
	completed.state = std::make_shared<AsyncProcessTaskState>();
	completed.state->result.ok = true;
	completed.state->result.output = R"({"decision":"approve","reason":"Controlled fixture only."})";
	completed.state->completed = true;
	app.permission_review_tasks.reserve(1);
	app.permission_review_tasks.push_back(std::move(completed));
	UAM_ASSERT_EQ(app.permission_review_tasks.capacity(), std::size_t{1});
	UAM_ASSERT(uam::acp_detail::PollPermissionReviewTasks(app));
	UAM_ASSERT_EQ(session.pending_permission.request_id_json, std::string("8"));
	UAM_ASSERT_EQ(app.permission_review_tasks.size(), std::size_t{1});
	UAM_ASSERT_EQ(app.permission_review_tasks.front().request_id_json, std::string("8"));
	UAM_ASSERT(app.permission_review_tasks.front().worker != nullptr);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!app.permission_review_tasks.front().state->completed.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	UAM_ASSERT(app.permission_review_tasks.front().state->completed.load());
	UAM_ASSERT(fs::exists(marker));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(transport.captured.find("\"id\":7") != std::string::npos);
	UAM_ASSERT(transport.captured.find("\"id\":8") == std::string::npos);
#endif
}

UAM_TEST(OpenCodePermissionFeedbackDoesNotWaitForInteractionTimestampStorage)
{
#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-permission-feedback");
	const fs::path shim = temp.root / "gemini";
	UAM_ASSERT(uam::io::WriteTextFile(shim,
	    "#!/bin/sh\nprintf '%s\\n' '" +
	    nlohmann::json{{"decision", "uncertain"}, {"reason", "Private fixture."}}.dump() + "'\n"));
	fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
	ScopedEnvVar path("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
	app.settings.permission_reviewer_provider_id = "gemini-cli";
	app.settings.permission_reviewer_model_id = "private-fake-model";
	ChatSession chat;
	chat.id = "opencode-permission-feedback";
	chat.provider_id = "opencode-cli";
	chat.workspace_directory = temp.root.string();
	chat.command_safety_tier = "aiReview";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->provider_id = chat.provider_id;
	owned->protocol_kind = "opencode-acp";
	owned->running = owned->processing = owned->session_ready = true;
	owned->current_mode_id = "default";
	uam::AcpSessionState& session = *owned;
	app.acp_sessions.push_back(std::move(owned));
	ManualCompactionWriter transport(session);
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::acp_detail::StopPermissionReviewTasks(app); }
	} cleanup{app};
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, app.chats.front());
	UAM_ASSERT(prepared != nullptr);
	std::promise<void> entered;
	std::future<void> locked = entered.get_future();
	std::jthread publication([&entered, prepared]()
	{
		(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
		{
			entered.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return false;
		});
	});
	UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const bool handled = uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"jsonrpc":"2.0","id":51,"method":"session/request_permission","params":{"toolCall":{"toolCallId":"private-read","title":"Read controlled fixture","kind":"read","status":"pending","rawInput":{"command":"cat private-fixture.txt"}},"options":[{"optionId":"once","name":"Allow once","kind":"allow_once"},{"optionId":"reject","name":"Reject","kind":"reject_once"}]}})");
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(handled);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(250));
	UAM_ASSERT(session.waiting_for_permission);
	UAM_ASSERT_EQ(session.pending_permission.request_id_json, std::string("51"));
	UAM_ASSERT_EQ(app.permission_review_tasks.size(), std::size_t{1});
	UAM_ASSERT(app.permission_review_tasks.front().worker != nullptr);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT(!session.interaction_at.empty());
	UAM_ASSERT_EQ(saved->interaction_at, session.interaction_at);
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(transport.captured.empty());
#endif
}

UAM_TEST(OpenCodePermissionReviewUsesIsolatedVerdictsAndPersistsOwnedResults)
{
#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI
	for (const std::string scenario : {"approve", "deny", "uncertain", "malformed", "doom", "sensitive"})
	{
		TempDir temp("uam-opencode-review-result");
		const fs::path shim = temp.root / "gemini";
		const fs::path marker = temp.root / "reviewer-ran";
		const std::string verdict = scenario == "malformed" ? "invalid fixture result" :
		    nlohmann::json{{"decision", scenario}, {"reason", "Private fixture verdict."}}.dump();
		UAM_ASSERT(uam::io::WriteTextFile(shim,
		    "#!/bin/sh\nprintf 'private-fixture\\n' > \"$UAM_TEST_OPENCODE_REVIEW_MARKER\"\nprintf '%s\\n' '" +
		    verdict + "'\n"));
		fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
		ScopedEnvVar path("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
		ScopedEnvVar review_marker("UAM_TEST_OPENCODE_REVIEW_MARKER", marker.string());
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
		app.settings.permission_reviewer_provider_id = "gemini-cli";
		app.settings.permission_reviewer_model_id = "private-fake-model";
		ChatSession chat;
		chat.id = "opencode-review-result";
		chat.provider_id = "opencode-cli";
		chat.workspace_directory = temp.root.string();
		chat.command_safety_tier = "aiReview";
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		owned->chat_id = chat.id;
		owned->provider_id = chat.provider_id;
		owned->protocol_kind = "opencode-acp";
		owned->running = owned->processing = owned->session_ready = true;
		owned->current_mode_id = "default";
		uam::AcpSessionState& session = *owned;
		app.acp_sessions.push_back(std::move(owned));
		ManualCompactionWriter transport(session);
		struct Cleanup
		{
			uam::AppState& app;
			~Cleanup() { uam::acp_detail::StopPermissionReviewTasks(app); }
		} cleanup{app};
		nlohmann::json frame = {{"jsonrpc", "2.0"}, {"id", 88}, {"method", "session/request_permission"},
		    {"params", {{"toolCall", {{"toolCallId", "reviewed-tool"}, {"title", scenario == "doom" ? "doom_loop" : "Read controlled fixture"},
		    {"kind", "read"}, {"status", "pending"}, {"rawInput", scenario == "sensitive" ?
		    nlohmann::json{{"password", "fixture-private"}} : nlohmann::json{{"command", "cat private-fixture.txt"}}}}},
		    {"options", nlohmann::json::array({{{"optionId", "once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
		    {{"optionId", "always"}, {"name", "Allow always"}, {"kind", "allow_always"}},
		    {{"optionId", "reject"}, {"name", "Reject"}, {"kind", "reject_once"}}})}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
		UAM_ASSERT(session.waiting_for_permission);
		if (scenario == "doom" || scenario == "sensitive")
		{
			UAM_ASSERT(app.permission_review_tasks.empty());
			UAM_ASSERT(!fs::exists(marker));
			UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
			UAM_ASSERT(transport.captured.empty());
			continue;
		}
		UAM_ASSERT_EQ(app.permission_review_tasks.size(), std::size_t{1});
		// Complete the first wait snapshot before consuming the independent review result.
		(void)uam::acp_detail::SyncAcpToolCallsToAssistantMessage(app.chats.front(), session, true);
		UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, app.chats.front()));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!app.permission_review_tasks.front().state->completed.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		UAM_ASSERT(app.permission_review_tasks.front().state->completed.load());
		UAM_ASSERT(fs::exists(marker));
		UAM_ASSERT(uam::acp_detail::PollPermissionReviewTasks(app));
		UAM_ASSERT(app.permission_review_tasks.empty());
		const std::chrono::steady_clock::time_point save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < save_deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
		const bool decided = scenario == "approve" || scenario == "deny";
		UAM_ASSERT_EQ(session.waiting_for_permission, !decided);
		if (decided)
		{
			const nlohmann::json response = nlohmann::json::parse(transport.captured);
			UAM_ASSERT_EQ(response["id"].get<int>(), 88);
			UAM_ASSERT_EQ(response["result"]["outcome"]["optionId"].get<std::string>(), scenario == "approve" ? std::string("once") : std::string("reject"));
		}
		else UAM_ASSERT(transport.captured.empty());
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved.has_value());
		bool persisted = false;
		for (const Message& message : saved->messages)
			for (const ToolCall& tool : message.tool_calls)
				if (tool.id == "reviewed-tool")
					persisted = uam::strings::Contains(tool.result_text, "AI Review (" + (scenario == "malformed" ? std::string("error") : scenario) + "):");
		UAM_ASSERT(persisted);
	}
#endif
}

UAM_TEST(ManualCompactionSendsNativeRequestAndPreservesConversationAndGoal)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-manual-compact");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "compact";
	chat.provider_id = "codex-cli";
	Message message;
	message.content = "Existing conversation";
	chat.messages.push_back(message);
	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "Completed answer";
	assistant.processing_time_ms = 1234;
	assistant.stop_reason = "completed";
	chat.messages.push_back(assistant);
	Goal goal;
	goal.id = "goal";
	goal.objective = "Keep working";
	goal.loop_count = 7;
	chat.goals.push_back(goal);
	chat.active_goal_id = goal.id;
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = "thread-compact";
	session.running = true;
	session.session_ready = true;
	session.turn_user_message_index = 0;
	session.turn_first_user_message_index = 0;
	session.current_assistant_message_index = 1;
	session.turn_assistant_message_index = 1;
	session.turn_checkpoint_eligible = true;
	session.turn_started_time_s = uam::GetAppTimeSeconds() - 5.0;
	const double previous_start = session.turn_started_time_s;
	ManualCompactionWriter transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT_EQ(session.turn_user_message_index, -1);
	UAM_ASSERT_EQ(session.turn_first_user_message_index, -1);
	UAM_ASSERT_EQ(session.current_assistant_message_index, -1);
	UAM_ASSERT_EQ(session.turn_assistant_message_index, -1);
	UAM_ASSERT(!session.turn_checkpoint_eligible);
	UAM_ASSERT(session.turn_started_time_s > previous_start);
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	const nlohmann::json request = nlohmann::json::parse(transport.captured);
	UAM_ASSERT_EQ(request["method"].get<std::string>(), std::string("thread/compact/start"));
	UAM_ASSERT_EQ(request["params"]["threadId"].get<std::string>(), session.codex_thread_id);
	const int request_id = request["id"].get<int>();
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"id", request_id}, {"result", nlohmann::json::object()}}.dump()));
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "previous-turn"}, {"status", "completed"}}}}}}.dump()));
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/started"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "compact-turn"}}}}}}.dump()));
	UAM_ASSERT(!uam::SteerAcpPrompt(app, chat.id, "Follow up", {}, {}, false, &error));
	UAM_ASSERT(error.find("compaction") != std::string::npos);
	UAM_ASSERT(transport.captured.find("turn/steer") == std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{2});
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "stale-turn"}, {"status", "completed"}}}}}}.dump()));
	UAM_ASSERT(session.manual_compaction_pending);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "compact-turn"}, {"status", "completed"}}}}}}.dump()));
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, message.content);
	UAM_ASSERT_EQ(app.chats.front().messages.back().content, assistant.content);
	UAM_ASSERT_EQ(app.chats.front().messages.back().processing_time_ms, assistant.processing_time_ms);
	UAM_ASSERT_EQ(app.chats.front().messages.back().stop_reason, assistant.stop_reason);
	UAM_ASSERT(!app.chats.front().messages.back().interrupted);
	UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, 7);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	// Completion can arrive before the empty RPC acknowledgement.
	session.manual_compaction_pending = true;
	session.processing = true;
	session.codex_turn_id = "second-compact-turn";
	session.inactivity_timeout_pending = true;
	session.cancel_requested = true;
	session.cancel_requested_time_s = uam::GetAppTimeSeconds() - 10.0;
	session.pending_request_methods[73] = "thread/compact/start";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "second-compact-turn"}, {"status", "completed"}}}}}}.dump()));
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"id", 73}, {"result", nlohmann::json::object()}}.dump()));
	UAM_ASSERT(!session.processing && !session.manual_compaction_pending);
	UAM_ASSERT(!session.inactivity_timeout_pending && !session.cancel_requested);
	UAM_ASSERT_EQ(session.cancel_requested_time_s, 0.0);
	UAM_ASSERT_EQ(uam::AcpTurnInactivityRecovery(session, uam::GetAppTimeSeconds(), 1.0), uam::AcpTurnInactivityRecoveryAction::None);
	UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, 7);

#endif
}

UAM_TEST(ManualCompactionRejectsUnavailableBusyRemoteAndUnsupportedSessions)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "compact";
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = "codex-cli";
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = "thread";
	session.running = true;
	session.session_ready = true;
	for (int condition = 0; condition < 5; ++condition)
	{
		session.processing = condition == 0;
		session.cancel_requested = condition == 1;
		session.waiting_for_user_input = condition == 2;
		session.queued_user_prompts.clear();
		if (condition == 3) session.queued_user_prompts.emplace_back();
		session.pending_request_methods.clear();
		if (condition == 4) session.pending_request_methods[41] = "control";
		UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT(!error.empty());
	}
	session.pending_request_methods.clear();
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	app.chats.front().execution_host_id = "remote";
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(error.find("protocol 3") != std::string::npos);
	app.chats.front().execution_host_id = "local";
#endif
	session.provider_id = "gemini-cli";
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(error.find("does not support") != std::string::npos);
	UAM_ASSERT(!session.manual_compaction_pending);
	UAM_ASSERT(app.chats.front().messages.empty());
}

UAM_TEST(ManualCompactionTransportAndRpcFailuresDoNotLeaveSessionBusy)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	uam::AppState app;
	ChatSession chat;
	chat.id = "compact";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = "codex-cli";
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = "thread";
	session.running = true;
	session.session_ready = true;
	std::string error;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	session.manual_compaction_pending = true;
	session.processing = true;
	session.pending_request_methods[42] = "thread/compact/start";
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"id", 42}, {"error", {{"code", -32601}, {"message", "Unsupported compaction"}}}}.dump()));
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(session.last_error.find("Unsupported compaction") != std::string::npos);
	UAM_ASSERT(app.chats.front().messages.empty());
#endif
}

UAM_TEST(ManualCompactionCancellationAndFailurePreserveCompletedMessageMetadata)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (int settlement = 0; settlement < 4; ++settlement)
	{
		TempDir temp("uam-compact-settlement");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "compact";
		chat.provider_id = "codex-cli";
		Message message;
		message.role = MessageRole::Assistant;
		message.content = "Already completed";
		message.processing_time_ms = 4567;
		message.stop_reason = "completed";
		chat.messages.push_back(message);
		Goal goal;
		goal.id = "active-provider-goal";
		goal.objective = "Keep the provider goal active";
		goal.execution_owner = "provider";
		goal.provider_command = "/autopilot";
		chat.goals.push_back(goal);
		chat.active_goal_id = goal.id;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.back();
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
		session.codex_thread_id = "thread";
		session.session_id = "thread";
		session.running = true;
		session.session_ready = true;
		session.current_assistant_message_index = 0;
		session.turn_assistant_message_index = 0;
		session.turn_started_time_s = uam::GetAppTimeSeconds() - 30.0;
		session.last_turn_outcome = "completed";
		ManualCompactionWriter transport(session);
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
		const int request_id = nlohmann::json::parse(transport.captured)["id"].get<int>();
		if (settlement != 0)
		{
			session.codex_turn_id = "maintenance";
			if (settlement == 1)
				UAM_ASSERT(uam::CancelAcpTurn(app, chat.id, &error));
			else
			{
				app.settings.active_turn_inactivity_timeout_seconds = 1;
				session.turn_started_time_s = 1.0;
				session.last_runtime_activity_time_s = 1.0;
				UAM_ASSERT(uam::HandleAcpTurnInactivityTimeout(app, session, app.chats.front(), 600.0));
				UAM_ASSERT(session.inactivity_timeout_pending);
				UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
			}
			if (settlement == 3)
			{
				// Forced stop may close session descriptors; the fixture keeps its private FILE ownership.
				UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
				transport.Detach();
				session.cancel_requested_time_s = 1.0;
				UAM_ASSERT(uam::HandleAcpTurnInactivityTimeout(app, session, app.chats.front(), 600.0));
			}
			else
			{
				UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
				    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", session.codex_thread_id}, {"turn", {{"id", "maintenance"}, {"status", "interrupted"}}}}}}.dump()));
			}
		}
		else
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
			    nlohmann::json{{"id", request_id}, {"error", {{"code", -32601}, {"message", "Rejected compact"}}}}.dump()));
		}
		UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
		UAM_ASSERT(!session.inactivity_timeout_pending);
		UAM_ASSERT_EQ(session.cancel_requested_time_s, 0.0);
		UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{1});
		UAM_ASSERT_EQ(app.chats.front().messages.front().content, message.content);
		UAM_ASSERT_EQ(app.chats.front().messages.front().processing_time_ms, message.processing_time_ms);
		UAM_ASSERT_EQ(app.chats.front().messages.front().stop_reason, message.stop_reason);
		UAM_ASSERT(!app.chats.front().messages.front().interrupted);
		UAM_ASSERT_EQ(session.last_turn_outcome, std::string("completed"));
		UAM_ASSERT(!session.goal_resume_suppressed);
		UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	}
#endif
}

UAM_TEST(ManualCompactionRejectionDrainsQueuedUserFollowup)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-followup");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "compact";
	chat.provider_id = "codex-cli";
	chat.workspace_directory = temp.root.string();
	chat.memory_enabled = false;
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = "thread";
	session.session_id = "thread";
	session.initialized = true;
	session.running = true;
	session.session_ready = true;
	session.current_mode_id = "default";
	ManualCompactionWriter transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	const int request_id = nlohmann::json::parse(transport.captured)["id"].get<int>();
	uam::AcpQueuedUserPromptState queued;
	queued.text = "Follow up after compaction";
	session.queued_user_prompts.push_back(queued);
	app.chats.front().acp_queued_prompts.push_back(queued);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"id", request_id}, {"error", {{"code", -32601}, {"message", "Rejected compact"}}}}.dump()));
	AwaitDurablePromptDispatch(app, session, app.chats.front());
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(!session.manual_compaction_pending);
	UAM_ASSERT(session.queued_user_prompts.empty());
	UAM_ASSERT(session.processing && session.prompt_request_id != 0);
	UAM_ASSERT(transport.captured.find("turn/start") != std::string::npos);
	UAM_ASSERT(transport.captured.find(queued.text) != std::string::npos);
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, queued.text);
#endif
}

UAM_TEST(ManualCompactionStreamErrorWaitsForItsCompletionWithoutSettlingGoal)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-stream-error");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "compact";
	chat.provider_id = "codex-cli";
	Goal goal;
	goal.id = "goal";
	goal.objective = "Keep working";
	goal.loop_count = 7;
	chat.goals.push_back(goal);
	chat.active_goal_id = goal.id;
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = uam::provider_profile_constants::kProtocolCodexAppServer;
	session.codex_thread_id = "thread";
	session.session_id = "thread";
	session.running = true;
	session.session_ready = true;
	ManualCompactionWriter transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/started"}, {"params", {{"threadId", "thread"}, {"turn", {{"id", "maintenance"}}}}}}.dump()));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "error"}, {"params", {{"threadId", "thread"}, {"turnId", "maintenance"}, {"error", {{"message", "Context compaction failed"}}}, {"willRetry", false}}}}.dump()));
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(session.last_error.find("Context compaction failed") != std::string::npos);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"method", "turn/completed"}, {"params", {{"threadId", "thread"}, {"turn", {{"id", "maintenance"}, {"status", "failed"}, {"error", {{"message", "Context compaction failed"}}}}}}}}.dump()));
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(app.chats.front().goals.front().loop_count, 7);
	UAM_ASSERT_EQ(app.chats.front().goals.front().status, GoalStatus::Active);
	UAM_ASSERT(app.chats.front().messages.empty());
#endif
}

UAM_TEST(CompanionLanRepairPreservesCredentialsAndRejectsCustomOrUnavailableAddresses)
{
	nlohmann::json config = {{"origin", "https://192.168.1.30:58950"}, {"token_file", "unchanged-token"}};
	nlohmann::json proxy = {
	    {"apps", {{"http", {{"listen", {"127.0.0.1:58950", "192.168.1.30:58950"}},
	                       {"host", {"192.168.1.30"}}, {"upstream", "127.0.0.1:58949"}}},
	              {"tls", {{"subjects", {"192.168.1.30"}}}}}},
	    {"storage", {{"root", "certificate-storage"}}}};
	const nlohmann::json original_config = config;
	const nlohmann::json original_proxy = proxy;
	for (const std::string& address : {"192.168.1.40", "8.8.8.8", "10.invalid", "172.32.1.1"})
	{
		UAM_ASSERT(!uam::companion::RepairLanAddress(config, proxy, address, {"192.168.1.20", "8.8.8.8", "10.invalid", "172.32.1.1"}));
		UAM_ASSERT(config == original_config);
		UAM_ASSERT(proxy == original_proxy);
	}
	nlohmann::json custom = original_config;
	custom["origin"] = "https://uam.example.com:58950";
	UAM_ASSERT(!uam::companion::RepairLanAddress(custom, proxy, "192.168.1.20", {"192.168.1.20"}));
	UAM_ASSERT(uam::companion::RepairLanAddress(config, proxy, "192.168.1.20", {"192.168.1.20"}));
	UAM_ASSERT_EQ(config["origin"].get<std::string>(), std::string("https://192.168.1.20:58950"));
	UAM_ASSERT(config["token_file"] == original_config["token_file"]);
	UAM_ASSERT(proxy["storage"] == original_proxy["storage"]);
	UAM_ASSERT_EQ(proxy["apps"]["http"]["listen"][1].get<std::string>(), std::string("192.168.1.20:58950"));
	UAM_ASSERT_EQ(proxy["apps"]["http"]["listen"][0].get<std::string>(), std::string("127.0.0.1:58950"));
	UAM_ASSERT_EQ(proxy["apps"]["http"]["upstream"].get<std::string>(), std::string("127.0.0.1:58949"));
	UAM_ASSERT_EQ(proxy["apps"]["tls"]["subjects"][0].get<std::string>(), std::string("192.168.1.20"));
}

UAM_TEST(BoundedJsonSerializationCountsEscapingAndFinalNewline)
{
	JsonValue root = uam::json::Object();
	uam::json::SetString(root, "escaped\"key", std::string("\0\n\t\\\"", 5));
	JsonValue items = uam::json::Array();
	uam::json::PushValue(items, uam::json::Number(1.25));
	uam::json::PushValue(items, uam::json::Bool(true));
	uam::json::PushValue(items, JsonValue{});
	uam::json::SetValue(root, "items", std::move(items));
	const std::string expected = SerializeJson(root);
	UAM_ASSERT_EQ(SerializeJson(root, expected.size()), expected);
	for (const std::size_t limit : {std::size_t{0}, expected.size() - 1})
	{
		bool rejected = false;
		try { (void)SerializeJson(root, limit); }
		catch (const std::length_error&) { rejected = true; }
		UAM_ASSERT(rejected);
	}
}

UAM_TEST(JsonSerializationPreservesMixedSpansAndExactByteLimit)
{
	const std::string plain(1024U * 1024U, 'x');
	std::string controls;
	for (int value = 0; value < 32; ++value) controls.push_back(static_cast<char>(value));
	const std::string input = "\"\\" + plain + controls + "é😀" + plain + "\\\"";
	const std::string escaped_controls =
	    "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
	    "\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
	    "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
	    "\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f";
	const std::string expected = "\"\\\"\\\\" + plain + escaped_controls + "é😀" + plain + "\\\\\\\"\"\n";
	const JsonValue value = uam::json::String(input);
	UAM_ASSERT_EQ(SerializeJson(value), expected);
	UAM_ASSERT_EQ(SerializeJson(value, expected.size()), expected);
	const std::optional<JsonValue> parsed = ParseJson(expected);
	UAM_ASSERT(parsed.has_value());
	UAM_ASSERT_EQ(parsed->string_value, input);
	for (const std::size_t limit : {std::size_t{0}, plain.size(), expected.size() - 1})
	{
		bool rejected = false;
		try { (void)SerializeJson(value, limit); }
		catch (const std::length_error&) { rejected = true; }
		UAM_ASSERT(rejected);
	}
	UAM_ASSERT_EQ(SerializeJson(uam::json::String(""), 3), std::string("\"\"\n"));
}

UAM_TEST(JsonParserPreservesLongSpansAndRejectsMalformedBoundaries)
{
	const std::string plain(1024U * 1024U, 'x');
	const std::string escaped = R"(\u0000\/\\\"\b\f\n\r\t\u00e9\uD83D\uDE00)";
	const std::string encoded = "\"" + plain + escaped + plain + "\"";
	const std::string decoded = plain + std::string("\0/\\\"\b\f\n\r\t", 9) + "é😀" + plain;
	const std::optional<JsonValue> value = ParseJson(encoded);
	UAM_ASSERT(value.has_value());
	UAM_ASSERT_EQ(value->string_value, decoded);
	const std::optional<JsonValue> object = ParseJson("{\"" + plain + "\":" + encoded + "}");
	UAM_ASSERT(object.has_value());
	UAM_ASSERT(object->Find(plain) != nullptr);
	UAM_ASSERT_EQ(object->Find(plain)->string_value, decoded);
	UAM_ASSERT(!ParseJson(encoded.substr(0, encoded.size() - 1)).has_value());
	UAM_ASSERT(!ParseJson(encoded + "trailing").has_value());
	for (int control = 0; control < 32; ++control)
		UAM_ASSERT(!ParseJson("\"" + plain + static_cast<char>(control) + "\"").has_value());
	for (const std::string suffix : {std::string("\\"), std::string("\\u"), std::string("\\u00"),
	     std::string("\\uD83D"), std::string("\\uD83D\\u"), std::string("\\q\""), std::string("\\uD83D\\u0000\"")})
		UAM_ASSERT(!ParseJson("\"" + plain + suffix).has_value());
}

UAM_TEST(OversizedChatSavePreservesExistingTranscript)
{
	TempDir temp("oversized-chat-save");
	const fs::path& root = temp.root;
	ChatSession chat;
	chat.id = "oversized-chat";
	chat.messages.push_back(Message{MessageRole::User, "Original transcript"});
	UAM_ASSERT(ChatRepository::SaveChat(root, chat));
	// Control bytes expand sixfold in JSON. Exercise the real save ceiling
	// without allocating a 256 MiB serialized document.
	chat.messages[0].content.assign(45U * 1024U * 1024U, '\0');
	UAM_ASSERT(!ChatRepository::SaveChat(root, chat));
	UAM_ASSERT(!ChatRepository::PrepareChatSave(root, chat));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages[0].content, std::string("Original transcript"));
}

UAM_TEST(AcpSteerKeepsExistingToolsInTheirOriginalMessage)
{
	using namespace uam::acp_detail;
	ChatSession chat;
	uam::AcpSessionState session;
	session.turn_serial = 7;
	session.turn_started_time_s = 12.0;
	session.tool_calls.push_back({.id = "old-tool", .title = "Read", .status = "in_progress"});
	AppendToolTurnEventIfNeeded(session, "old-tool");
	UAM_ASSERT(SyncAcpToolCallsToAssistantMessage(chat, session, true));
	UAM_ASSERT_EQ(chat.messages.size(), static_cast<std::size_t>(1));
	chat.messages.push_back(Message{MessageRole::User, "Focus on tests"});
	session.current_assistant_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_events.clear();
	session.pending_assistant_thoughts.clear();
	session.tool_calls.front().status = "completed";
	session.tool_calls.front().content = "Old result";
	AppendToolTurnEventIfNeeded(session, "old-tool");
	UAM_ASSERT(SyncAcpToolCallsToAssistantMessage(chat, session, true));
	UAM_ASSERT_EQ(chat.messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(session.turn_events.empty());
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.front().result_text, std::string("Old result"));
	UAM_ASSERT_EQ(chat.messages.front().blocks.front().tool_call_id, std::string("old-tool"));
	UAM_ASSERT(chat.messages.back().tool_calls.empty());
	session.tool_calls.push_back({.id = "new-tool", .title = "Test", .status = "in_progress"});
	AppendToolTurnEventIfNeeded(session, "new-tool");
	UAM_ASSERT(SyncAcpToolCallsToAssistantMessage(chat, session, true));
	UAM_ASSERT_EQ(AppendAssistantChunk(chat, session, "Testing now"), std::string("Testing now"));
	UAM_ASSERT_EQ(chat.messages.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(chat.messages.back().tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(chat.messages.back().tool_calls.front().id, std::string("new-tool"));
	UAM_ASSERT_EQ(session.tool_call_message_indices.at("old-tool"), 0);
	UAM_ASSERT_EQ(session.tool_call_message_indices.at("new-tool"), 2);
	session.tool_calls.front().content = "Updated old result";
	AppendToolTurnEventIfNeeded(session, "old-tool");
	UAM_ASSERT(SyncAcpToolCallsToAssistantMessage(chat, session, false));
	UAM_ASSERT_EQ(chat.messages.front().tool_calls.front().result_text, std::string("Updated old result"));
	UAM_ASSERT_EQ(chat.messages.front().blocks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(chat.messages.back().tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(chat.messages.back().blocks.front().tool_call_id, std::string("new-tool"));
	UAM_ASSERT(!HasTurnToolEvent(session, "old-tool"));
	UAM_ASSERT_EQ(session.turn_serial, 7);
	UAM_ASSERT_EQ(session.turn_started_time_s, 12.0);
	for (const int invalid_index : {-1, 1, 100})
	{
		session.tool_call_message_indices["old-tool"] = invalid_index;
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		UAM_ASSERT(chat.messages[1].tool_calls.empty());
		UAM_ASSERT_EQ(chat.messages.back().tool_calls.size(), static_cast<std::size_t>(1));
	}
	ResetAcpTurnStreamState(session);
	UAM_ASSERT(session.tool_call_message_indices.empty());
}

UAM_TEST(DesktopDiagnosticsPersistRotateAndFallBackWithoutTouchingStdout)
{
	TempDir temp("uam-desktop-log");
	const fs::path log = temp.root / fs::path(u8"diagnostics-日志.log");
	fs::path previous = log;
	previous += ".1";
	std::ostringstream errors;
	std::ostringstream output;
	struct RestoreLogs
	{
		std::streambuf* error_buffer;
		std::streambuf* output_buffer;
		~RestoreLogs()
		{
			uam::diagnostics::StopFileLog();
			std::cerr.rdbuf(error_buffer);
			std::cout.rdbuf(output_buffer);
		}
	} restore{std::cerr.rdbuf(errors.rdbuf()), std::cout.rdbuf(output.rdbuf())};
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::AppState app;
	uam::LogCliDiagnosticEvent(app, "test", "cli_failure", nullptr, "CLI diagnostic");
	uam::AcpSessionState session;
	uam::acp_detail::AppendAcpDiagnostic(session, "test", "acp_failure", "", "", false, 0, "ACP diagnostic", "");
	std::cout << "{\"id\":1}\n";
	uam::diagnostics::FlushFileLog();
	const std::string initial = uam::io::ReadTextFile(log);
	UAM_ASSERT(uam::strings::Contains(initial, "CLI diagnostic"));
	UAM_ASSERT(uam::strings::Contains(initial, "ACP diagnostic"));
	UAM_ASSERT(errors.str().empty());

	std::vector<std::jthread> writers;
	for (int worker = 0; worker < 4; ++worker)
	{
		writers.emplace_back([worker]
		{
			for (int entry = 0; entry < 25; ++entry)
				uam::diagnostics::Write("worker=" + std::to_string(worker) + ";entry=" + std::to_string(entry));
		});
	}
	writers.clear();
	uam::diagnostics::FlushFileLog();
	const std::string concurrent = uam::io::ReadTextFile(log);
	UAM_ASSERT_EQ(std::count(concurrent.begin(), concurrent.end(), '\n'), 102);
	for (int worker = 0; worker < 4; ++worker)
		for (int entry = 0; entry < 25; ++entry)
			UAM_ASSERT(uam::strings::Contains(concurrent, "worker=" + std::to_string(worker) + ";entry=" + std::to_string(entry) + "\n"));

	const std::string oversized(uam::diagnostics::kMaxEntryBytes * 2, 'x');
	for (std::size_t entry = 0; entry < 2 * uam::diagnostics::kMaxFileBytes / uam::diagnostics::kMaxEntryBytes + 2; ++entry)
	{
		uam::diagnostics::Write(oversized);
		if (entry % 16 == 0) uam::diagnostics::FlushFileLog();
	}
	uam::diagnostics::Write("latest-entry\nsecond-line");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(fs::file_size(log) <= uam::diagnostics::kMaxFileBytes);
	UAM_ASSERT(fs::file_size(previous) <= uam::diagnostics::kMaxFileBytes);
#if !defined(_WIN32)
	const fs::perms other_access = fs::perms::group_all | fs::perms::others_all;
	UAM_ASSERT((fs::status(log).permissions() & other_access) == fs::perms::none);
	UAM_ASSERT((fs::status(previous).permissions() & other_access) == fs::perms::none);
#endif
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(log), "latest-entry second-line"));
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(previous), "[truncated]"));
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::Write("after-reopen");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(log), "latest-entry second-line"));

	UAM_ASSERT(uam::io::WriteTextFile(log, std::string(uam::diagnostics::kMaxFileBytes, 'x')));
	fs::remove(previous);
	fs::create_directory(previous);
	UAM_ASSERT(uam::io::WriteTextFile(previous / "keep", "keep"));
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::Write("fallback-entry");
	uam::diagnostics::FlushFileLog();
	UAM_ASSERT(uam::strings::Contains(errors.str(), "fallback-entry"));
	UAM_ASSERT_EQ(fs::file_size(log), uam::diagnostics::kMaxFileBytes);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(previous / "keep"), std::string("keep"));
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(!uam::diagnostics::StartFileLog(temp.root / "missing" / "log", error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(uam::io::WriteTextFile(log, std::string(uam::diagnostics::kMaxFileBytes + 1, 'x')));
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT_EQ(fs::file_size(log), uam::diagnostics::kMaxFileBytes + 1);
	uam::diagnostics::Write("after-close");
	UAM_ASSERT(uam::strings::Contains(errors.str(), "after-close"));
	UAM_ASSERT_EQ(output.str(), std::string("{\"id\":1}\n"));
}

UAM_TEST(DesktopDiagnosticsRecoverOversizedLogsWithoutRemovingSpecialBackups)
{
	TempDir temp("uam-log-oversized");
	const fs::path log = temp.root / "uam.log";
	const fs::path previous = temp.root / "uam.log.1";
	const std::string oversized(uam::diagnostics::kMaxFileBytes + 1, 'x');
	UAM_ASSERT(uam::io::WriteTextFile(log, oversized));
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::Write("logging recovered");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT_EQ(uam::io::ReadTextFile(previous), oversized);
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(log), "logging recovered"));
	fs::remove(previous);
	fs::create_directory(previous);
	UAM_ASSERT(uam::io::WriteTextFile(log, oversized));
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fs::is_directory(previous));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(log), oversized);
	fs::remove(previous);
#if !defined(_WIN32)
	const fs::path target = temp.root / "keep.log";
	UAM_ASSERT(uam::io::WriteTextFile(target, "keep"));
	fs::create_symlink(target, previous);
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(previous)));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), "keep");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(log), oversized);
#endif
}

UAM_TEST(DesktopDiagnosticsRejectSpecialCurrentPathsWithoutOpeningThem)
{
	TempDir temp("uam-log-current-special");
	const fs::path log = temp.root / "uam.log";
	std::string error;
	fs::create_directory(log);
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fs::is_directory(log));
	fs::remove(log);
#if !defined(_WIN32)
	const fs::path target = temp.root / "keep";
	UAM_ASSERT(uam::io::WriteTextFile(target, "keep"));
	const fs::perms permissions = fs::status(target).permissions();
	fs::create_symlink(target, log);
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), std::string("keep"));
	UAM_ASSERT_EQ(fs::status(target).permissions(), permissions);
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(log)));
	fs::remove(log);
	fs::create_symlink(temp.root / "missing", log);
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT(!fs::exists(temp.root / "missing"));
	fs::remove(log);
	UAM_ASSERT_EQ(::mkfifo(log.c_str(), 0600), 0);
	UAM_ASSERT(!uam::diagnostics::StartFileLog(log, error));
	UAM_ASSERT(fs::symlink_status(log).type() == fs::file_type::fifo);
	fs::remove(log);
#endif
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::Write("regular logging survives");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(log), "regular logging survives"));
}

UAM_TEST(DesktopDiagnosticRetentionExpiresOnlyOldRegularBackupFiles)
{
	TempDir temp("uam-log-retention");
	const fs::path log = temp.root / "uam.log";
	const fs::path previous = temp.root / "uam.log.1";
	UAM_ASSERT(uam::io::WriteTextFile(previous, "old backup"));
	fs::last_write_time(previous, fs::file_time_type::clock::now() - std::chrono::hours(24 * 15));
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::Write("current survives");
	uam::diagnostics::StopFileLog();
	UAM_ASSERT(!fs::exists(previous));
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(log), "current survives"));
	UAM_ASSERT(uam::io::WriteTextFile(previous, "recent backup"));
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::StopFileLog();
	UAM_ASSERT_EQ(uam::io::ReadTextFile(previous), "recent backup");
	fs::remove(previous);
	fs::create_directory(previous);
	UAM_ASSERT(uam::io::WriteTextFile(previous / "keep", "keep"));
	fs::last_write_time(previous, fs::file_time_type::clock::now() - std::chrono::hours(24 * 15));
	UAM_ASSERT(uam::diagnostics::StartFileLog(log, error));
	uam::diagnostics::StopFileLog();
	UAM_ASSERT_EQ(uam::io::ReadTextFile(previous / "keep"), "keep");
}


UAM_TEST(UamControlCapabilityBoundsGoalAuthorityReplayRateCancellationAndRestart)
{
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("gemini-acp"));
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("opencode-acp"));
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("copilot-acp"));
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("codex-app-server"));
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("claude-code-stream-json"));
	TempDir temp("uam-control-capability");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", "gemini-cli");
	chat.uam_control_enabled = true;
	chat.uam_agent_id = "build";
	chat.command_safety_tier = "off";
	const std::string chat_id = chat.id;
	app.chats.push_back(std::move(chat));

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat_id;
	session->provider_id = "gemini-cli";
	session->running = true;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));

	auto issue = [&]() {
		nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
		UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
		    app, *raw_session, app.chats.front(), "session/new", setup, &error));
		UAM_ASSERT_EQ(setup["params"]["mcpServers"].size(), static_cast<std::size_t>(1));
		const nlohmann::json& server = setup["params"]["mcpServers"][0];
		UAM_ASSERT_EQ(server.value("name", ""), std::string("uam-control"));
		UAM_ASSERT_EQ(server["args"].size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(server["args"][0].get<std::string>(), std::string("--uam-control-mcp"));
		UAM_ASSERT_EQ(server["env"].size(), static_cast<std::size_t>(1));
		UAM_ASSERT(server["args"].dump().find(server["env"][0].value("value", "")) == std::string::npos);
		return raw_session->uam_control_capability_id;
	};
	auto call = [&](const std::string& capability, std::string request_id,
	                std::string method, nlohmann::json arguments = nlohmann::json::object(),
	                int64_t now = 1000000) {
		return uam::UamControlService::HandleRequestForTests(
		    app, capability,
		    {{"requestId", std::move(request_id)}, {"method", std::move(method)},
		     {"arguments", std::move(arguments)}}, now);
	};

	std::string capability = issue();
	app.chats.front().uam_control_enabled = false;
	const nlohmann::json disabled = call(capability, "disabled-control", "goal_get");
	UAM_ASSERT(!disabled.value("ok", true));
	UAM_ASSERT(disabled.value("error", "").find("disabled") != std::string::npos);
	app.chats.front().uam_control_enabled = true;
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(
	    app, capability, {{"requestId", 1}, {"method", "goal_get"}, {"arguments", nlohmann::json::object()}},
	    1000000).value("ok", true));
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(
	    app, capability, {{"requestId", "wrong-method-type"}, {"method", 1}, {"arguments", nlohmann::json::object()}},
	    1000000).value("ok", true));
	UAM_ASSERT(!call(capability, "wrong-objective-type", "goal_create",
	                 {{"objective", 1}, {"idempotencyKey", "wrong-type"}}).value("ok", true));
	const std::vector<std::pair<std::string, nlohmann::json>> strict_arguments = {
	    {"skill_list", {{"unexpected", "value"}}},
	    {"skill_read", {{"id", "skill"}, {"unexpected", "value"}}},
	    {"agent_list", {{"unexpected", "value"}}},
	    {"agent_status", {{"runId", "run"}, {"unexpected", "value"}}},
	    {"agent_delegate", {{"agentId", "agent"}, {"task", "task"}, {"maxDepth", "999"}}},
	    {"agent_cancel", {{"runId", "run"}, {"unexpected", "value"}}},
	    {"goal_get", {{"unexpected", "value"}}},
	    {"goal_create", {{"objective", "goal"}, {"idempotencyKey", "strict"}, {"maxTurns", "999"}}},
	};
	for (std::size_t index = 0; index < strict_arguments.size(); ++index)
	{
		UAM_ASSERT(!call(capability, "strict-" + std::to_string(index), strict_arguments[index].first,
		                 strict_arguments[index].second).value("ok", true));
	}
	UAM_ASSERT(uam::GoalService::FindActiveGoal(app, chat_id) == nullptr);
	UAM_ASSERT(!uam::UamControlService::ValidStdioToolCallForTests(
	    {{"method", 1}, {"params", {{"name", "goal_get"}, {"arguments", nlohmann::json::object()}}}}));
	UAM_ASSERT(!uam::UamControlService::ValidStdioToolCallForTests(
	    {{"method", "tools/call"}, {"params", {{"name", 1}, {"arguments", nlohmann::json::object()}}}}));
	UAM_ASSERT(!uam::UamControlService::ValidStdioToolCallForTests(
	    {{"method", "tools/call"}, {"params", {{"name", "goal_get"}, {"arguments", 1}}}}));
	UAM_ASSERT(uam::UamControlService::ValidStdioToolCallForTests(
	    {{"method", "tools/call"},
	     {"params", {{"name", "goal_get"}, {"arguments", nlohmann::json::object()}}}}));
	const nlohmann::json control_tools = uam::UamControlService::ToolDefinitionsForTests();
	const auto goal_tool = std::ranges::find(control_tools, "goal_create", [](const nlohmann::json& tool) {
		return tool.value("name", "");
	});
	const auto computer_use_tool = std::ranges::find(control_tools, "computer_use_request", [](const nlohmann::json& tool) {
		return tool.value("name", "");
	});
	UAM_ASSERT(goal_tool != control_tools.end());
	UAM_ASSERT(computer_use_tool != control_tools.end());
	UAM_ASSERT((*goal_tool)["inputSchema"]["required"] == nlohmann::json::array({"objective", "idempotencyKey"}));
	UAM_ASSERT((*computer_use_tool)["inputSchema"]["required"] == nlohmann::json::array({"reason"}));
	// Plan children rely on the standard read-only hint; anything that edits or widens access must not carry it.
	std::size_t hinted = 0;
	for (const auto& tool : control_tools)
	{
		const std::string name = tool.value("name", "");
		const bool read_only = name == "agent_list" || name == "agent_status" || name == "agent_wait" || name == "skill_list" ||
		    name == "skill_read" || name == "goal_get" || name == "user_question";
		UAM_ASSERT_EQ(tool.contains("annotations") && tool["annotations"].value("readOnlyHint", false), read_only);
		hinted += read_only ? 1 : 0;
	}
	UAM_ASSERT_EQ(hinted, std::size_t{7});
	const nlohmann::json read_only_goal = call(capability, "policy-read", "goal_get");
	UAM_ASSERT(read_only_goal.value("ok", false));
	UAM_ASSERT(!read_only_goal["result"].value("mutationToolsEnabled", true));
	UAM_ASSERT_EQ(read_only_goal["result"].value("mutationPolicy", ""), std::string("readOnly"));
	for (const std::string tier : {"off", "acceptEdits", "aiReview"})
	{
		app.chats.front().command_safety_tier = tier;
		const nlohmann::json pending = call(
		    capability, "policy-" + tier, "goal_create",
		    {{"objective", "Must remain unchanged."}, {"idempotencyKey", "policy-" + tier}},
		    uam::time::SystemEpochMillisecondsNow());
		UAM_ASSERT(pending.value("pendingApproval", false));
		const std::optional<uam::AcpPendingUserInputState> policy_question =
		    uam::UamControlService::PendingApprovalForChat(app, chat_id);
		UAM_ASSERT(policy_question.has_value());
		UAM_ASSERT_EQ(policy_question->questions.front().id, std::string("goalCreate"));
		UAM_ASSERT(uam::UamControlService::ResolveApproval(
		    app, chat_id, policy_question->request_id_json,
		    {{"goalCreate", {"Deny"}}}, &error));
		UAM_ASSERT(uam::GoalService::FindActiveGoal(app, chat_id) == nullptr);
	}
	app.chats.front().command_safety_tier = "yolo";
	app.chats.front().approval_mode = "plan";
	const nlohmann::json plan_rejected = call(
	    capability, "policy-plan", "goal_create",
	    {{"objective", "Plan must remain read-only."}, {"idempotencyKey", "policy-plan"}});
	UAM_ASSERT(!plan_rejected.value("ok", true));
	UAM_ASSERT(uam::GoalService::FindActiveGoal(app, chat_id) == nullptr);
	app.chats.front().approval_mode = "default";
	capability = issue();
	const nlohmann::json yolo_goal = call(capability, "policy-yolo", "goal_get");
	UAM_ASSERT(yolo_goal.value("ok", false));
	UAM_ASSERT(yolo_goal["result"].value("mutationToolsEnabled", false));
	UAM_ASSERT_EQ(yolo_goal["result"].value("mutationPolicy", ""), std::string("yoloAutoApprove"));
	AgentRun unrelated_root_run;
	unrelated_root_run.id = "unrelated-root-run";
	unrelated_root_run.root_chat_id = chat_id;
	unrelated_root_run.status = "queued";
	app.agent_runs.push_back(unrelated_root_run);
	UAM_ASSERT(!call(capability, "unrelated-run", "agent_status",
	                 {{"runId", unrelated_root_run.id}}).value("ok", true));
	const nlohmann::json created = call(capability, "create-1", "goal_create",
	                                    {{"objective", "Finish the bounded task."},
	                                     {"idempotencyKey", "goal-once"}});
	UAM_ASSERT(created.value("ok", false));
	const Goal* goal = uam::GoalService::FindActiveGoal(app, chat_id);
	UAM_ASSERT(goal != nullptr);
	UAM_ASSERT_EQ(goal->creator, std::string("model"));
	UAM_ASSERT_EQ(goal->creator_provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(goal->creator_agent_id, std::string("build"));
	UAM_ASSERT_EQ(goal->token_budget, static_cast<int64_t>(200000));
	UAM_ASSERT_EQ(goal->execution_owner, std::string("uam"));
	UAM_ASSERT(!call(capability, "create-1", "goal_get").value("ok", true));
	UAM_ASSERT(!call(capability, "create-2", "goal_create",
	                 {{"objective", "Replay."}, {"idempotencyKey", "goal-once"}}).value("ok", true));
	UAM_ASSERT(!call(capability, "cross-chat", "goal_get", {{"chatId", "another-chat"}}).value("ok", true));
	UAM_ASSERT(!call(capability, "oversized", "goal_create",
	                 {{"objective", std::string(70U * 1024U, 'x')},
	                  {"idempotencyKey", "too-large"}}).value("ok", true));
	UAM_ASSERT(!call(capability, "lifecycle", "goal_complete").value("ok", true));
	UAM_ASSERT_EQ(uam::GoalService::FindActiveGoal(app, chat_id)->status, GoalStatus::Active);

	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(temp.root);
	const auto loaded = std::ranges::find(persisted, chat_id, &ChatSession::id);
	UAM_ASSERT(loaded != persisted.end());
	UAM_ASSERT_EQ(loaded->goals.front().creator, std::string("model"));
	UAM_ASSERT(!loaded->goals.front().creator_request_key_hash.empty());
	UAM_ASSERT(!loaded->uam_control_audit.empty());
	Goal* completed_goal = uam::GoalService::FindGoalById(app, chat_id, goal->id);
	UAM_ASSERT(completed_goal != nullptr);
	completed_goal->last_blocker = "Prior blocker";
	completed_goal->last_diagnostic = "verified_terminal_state";
	completed_goal->last_verification = "Focused native test passed";
	completed_goal->current_step = "Final verification";
	completed_goal->completed_items = {"Implemented fix", "Ran focused test"};
	completed_goal->remaining_items = {"Final verification"};
	UAM_ASSERT(uam::GoalService::UpdateGoalStatus(app, chat_id, goal->id, GoalStatus::Complete));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats.front()));
	capability = issue();
	const nlohmann::json terminal_goal_response = call(capability, "terminal-goal-get", "goal_get");
	UAM_ASSERT(terminal_goal_response.value("ok", false));
	const nlohmann::json& terminal_goal = terminal_goal_response["result"]["goal"];
	UAM_ASSERT_EQ(terminal_goal.value("status", ""), std::string("complete"));
	UAM_ASSERT_EQ(terminal_goal.value("lastBlocker", ""), std::string(""));
	UAM_ASSERT_EQ(terminal_goal.value("lastDiagnostic", ""), std::string("verified_terminal_state"));
	UAM_ASSERT_EQ(terminal_goal.value("lastVerification", ""), std::string("Focused native test passed"));
	UAM_ASSERT_EQ(terminal_goal.value("currentStep", ""), std::string(""));
	UAM_ASSERT_EQ(terminal_goal["completedItems"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT(terminal_goal["remainingItems"].empty());
	UAM_ASSERT(!call(capability, "durable-idempotency", "goal_create",
	                 {{"objective", "Must not replay after capability rotation."},
	                  {"idempotencyKey", "goal-once"}}).value("ok", true));

	capability = issue();
	for (int index = 0; index < 30; ++index)
	{
		const nlohmann::json response = call(capability, "rate-" + std::to_string(index), "goal_get", nlohmann::json::object(), 2000000);
		if (!response.value("ok", false))
		{
			const std::string detail = response.dump();
			uam_test::Fail(detail.c_str(), __FILE__, __LINE__);
		}
	}
	UAM_ASSERT(!call(capability, "rate-over", "goal_get", nlohmann::json::object(), 2000000).value("ok", true));

	capability = issue();
	app.uam_control_capabilities.front().expires_at_epoch_ms = 2999999;
	UAM_ASSERT(!call(capability, "expired", "goal_get", nlohmann::json::object(), 3000000).value("ok", true));
	capability = issue();
	uam::UamControlService::RevokeForSession(app, *raw_session);
	UAM_ASSERT(!call(capability, "revoked", "goal_get").value("ok", true));

	capability = issue();
	const std::filesystem::path overflow_directory = app.uam_control_capabilities.front().directory;
	for (int index = 0; index < 33; ++index)
	{
		UAM_ASSERT(uam::io::WriteTextFile(
		    overflow_directory / "requests" / ("queued-" + std::to_string(index) + ".json"), "{}"));
	}
	UAM_ASSERT(uam::UamControlService::ProcessPendingRequests(app));
	UAM_ASSERT(app.uam_control_capabilities.empty());
	UAM_ASSERT(raw_session->uam_control_capability_id.empty());
	UAM_ASSERT(!std::filesystem::exists(overflow_directory));

	capability = issue();
	const std::filesystem::path stale_directory = app.uam_control_capabilities.front().directory;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	UAM_ASSERT(app.uam_control_capabilities.empty());
	UAM_ASSERT(raw_session->uam_control_capability_id.empty());
	UAM_ASSERT(!std::filesystem::exists(stale_directory));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(UamControlComputerUseRequiresVisibleGuiApproval)
{
	TempDir temp("uam-control-computer-use-approval");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", "opencode-cli");
	chat.id = "chat-computer-use-approval";
	chat.uam_control_enabled = true;
	chat.execution_host_id = "local";
	const std::string chat_id = chat.id;
	chat.messages.push_back(Message{MessageRole::User, "Open Safari for my task."});
	chat.messages.back().created_at = "owned-user-turn";
	app.chats.push_back(std::move(chat));
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat_id;
	session->provider_id = "opencode-cli";
	session->running = session->processing = true;
	session->turn_serial = 1;
	session->turn_first_user_message_index = session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_session = session.get();
	app.acp_sessions.push_back(std::move(session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));

	nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    app, *raw_session, app.chats.front(), "session/new", setup, &error));
	UAM_ASSERT_EQ(app.uam_control_capabilities.size(), static_cast<std::size_t>(1));
	const std::string capability_id = app.uam_control_capabilities.front().id;
	const std::filesystem::path capability_directory = app.uam_control_capabilities.front().directory;
	const int64_t now = uam::time::SystemEpochMillisecondsNow();
	const nlohmann::json request = {
	    {"requestId", "computer-use-deny"},
	    {"method", "computer_use_request"},
	    {"arguments", {{"reason", "The task needs a visible browser."}}},
	};
	const nlohmann::json pending = uam::UamControlService::HandleRequestForTests(
	    app, capability_id, request, now);
	UAM_ASSERT(pending.value("pendingApproval", false));
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	const std::optional<uam::AcpPendingUserInputState> question =
	    uam::UamControlService::PendingApprovalForChat(app, chat_id);
	UAM_ASSERT(question.has_value());
	UAM_ASSERT_EQ(question->questions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(question->questions.front().id, std::string("computerUse"));
	UAM_ASSERT_EQ(question->questions.front().options.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(question->request_id_json.find("uam-control:") == 0);

	std::map<std::string, std::vector<std::string>> wrong_answer{{"computerUse", {"Allow"}}};
	UAM_ASSERT(!uam::UamControlService::ResolveApproval(
	    app, "another-chat", question->request_id_json, wrong_answer, &error));
	UAM_ASSERT(!uam::UamControlService::ResolveApproval(
	    app, chat_id, question->request_id_json,
	    {{"computerUse", {"Not an approval"}}}, &error));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, chat_id, question->request_id_json,
	    {{"computerUse", {"Deny"}}}, &error));
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	std::string denied_text;
	UAM_ASSERT(uam::io::TryReadTextFile(
	    capability_directory / "responses" / "computer-use-deny.json", denied_text));
	const nlohmann::json denied = nlohmann::json::parse(denied_text);
	UAM_ASSERT(!denied.value("ok", true));
	UAM_ASSERT(denied.value("error", "").find("denied") != std::string::npos);

	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    app, *raw_session, app.chats.front(), "session/new", setup, &error));
	UAM_ASSERT_EQ(app.uam_control_capabilities.size(), static_cast<std::size_t>(1));
	const std::filesystem::path allow_directory = app.uam_control_capabilities.front().directory;
	nlohmann::json allow_request = request;
	allow_request["requestId"] = "computer-use-allow";
	UAM_ASSERT(uam::io::WriteTextFile(
	    allow_directory / "requests" / "computer-use-allow.json", allow_request.dump()));
	UAM_ASSERT(uam::UamControlService::ProcessPendingRequests(app));
	UAM_ASSERT(!app.chats.front().computer_use_enabled);
	const std::optional<uam::AcpPendingUserInputState> allow_question =
	    uam::UamControlService::PendingApprovalForChat(app, chat_id);
	UAM_ASSERT(allow_question.has_value());
	UAM_ASSERT(!uam::UamControlService::ResolveApproval(
	    app, chat_id, allow_question->request_id_json,
	    {{"computerUse", {"Allow", "Deny"}}}, &error));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, chat_id, allow_question->request_id_json,
	    {{"computerUse", {"Allow"}}}, &error));
	UAM_ASSERT(app.chats.front().computer_use_enabled);
	std::string allowed_text;
	UAM_ASSERT(uam::io::TryReadTextFile(
	    allow_directory / "responses" / "computer-use-allow.json", allowed_text));
	const nlohmann::json allowed = nlohmann::json::parse(allowed_text);
	UAM_ASSERT(allowed.value("ok", false));
	UAM_ASSERT(allowed["result"].value("enabled", false));
	UAM_ASSERT(app.chats.front().uam_control_audit.size() >= static_cast<std::size_t>(2));
	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(temp.root);
	const auto persisted_chat = std::ranges::find(persisted, chat_id, &ChatSession::id);
	UAM_ASSERT(persisted_chat != persisted.end());
	UAM_ASSERT(persisted_chat->uam_control_audit.size() >= static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(persisted_chat->uam_control_audit.back().method,
	              std::string("computer_use_request"));
	UAM_ASSERT_EQ(persisted_chat->uam_control_audit.back().result, std::string("ok"));

	const nlohmann::json expired_request = {
	    {"requestId", "computer-use-expired"}, {"method", "computer_use_request"},
	    {"arguments", {{"reason", "This request should expire without a user answer."}}},
	};
	app.chats.front().computer_use_enabled = false;
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(
	    app, app.uam_control_capabilities.front().id, expired_request, now - 91000)
	               .value("pendingApproval", false));
	UAM_ASSERT(uam::UamControlService::ProcessPendingRequests(app));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(app, chat_id).has_value());
	std::string expired_text;
	UAM_ASSERT(uam::io::TryReadTextFile(
	    allow_directory / "responses" / "computer-use-expired.json", expired_text));
	UAM_ASSERT(nlohmann::json::parse(expired_text).value("error", "").find("timed out") != std::string::npos);

	uam::AppState remote_app;
	remote_app.data_root = temp.root / "remote";
	ChatSession remote_chat = ChatDomainService().CreateNewChat("", "opencode-cli");
	remote_chat.id = "chat-computer-use-remote";
	remote_chat.execution_host_id = "ssh-test";
	remote_chat.uam_control_enabled = true;
	remote_app.chats.push_back(std::move(remote_chat));
	auto remote_session = std::make_unique<uam::AcpSessionState>();
	remote_session->chat_id = "chat-computer-use-remote";
	remote_session->provider_id = "opencode-cli";
	remote_session->running = true;
	uam::AcpSessionState* raw_remote_session = remote_session.get();
	remote_app.acp_sessions.push_back(std::move(remote_session));
	UAM_ASSERT(uam::UamControlService::Initialize(remote_app, &error));
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    remote_app, *raw_remote_session, remote_app.chats.front(), "session/new", setup, &error));
	const nlohmann::json remote_rejected = uam::UamControlService::HandleRequestForTests(
	    remote_app, remote_app.uam_control_capabilities.front().id,
	    {{"requestId", "remote-computer-use"}, {"method", "computer_use_request"},
     {"arguments", {{"reason", "Remote must be rejected."}}}}, now);
	UAM_ASSERT(!remote_rejected.value("ok", true));
	UAM_ASSERT(remote_rejected.value("error", "").find("unavailable") != std::string::npos);
	UAM_ASSERT(!remote_app.chats.front().computer_use_enabled);

	uam::UamControlService::Shutdown(remote_app);
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(UamControlGoalApprovalAndChildComputerUseScope)
{
	TempDir temp("uam-control-goal-and-child-approval");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root = ChatDomainService().CreateNewChat("", "opencode-cli");
	root.id = "chat-goal-approval-root";
	root.uam_control_enabled = true;
	root.execution_host_id = "local";
	const std::string root_id = root.id;
	app.chats.push_back(std::move(root));
	auto root_session = std::make_unique<uam::AcpSessionState>();
	root_session->chat_id = root_id;
	root_session->provider_id = "opencode-cli";
	root_session->running = true;
	uam::AcpSessionState* raw_root_session = root_session.get();
	app.acp_sessions.push_back(std::move(root_session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));

	auto append_server = [&](uam::AcpSessionState& session, const ChatSession& chat) {
		nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
		UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
		    app, session, chat, "session/new", setup, &error));
		const auto capability = std::ranges::find(app.uam_control_capabilities, chat.id,
		                                          &uam::UamControlCapability::session_chat_id);
		UAM_ASSERT(capability != app.uam_control_capabilities.end());
		return capability->id;
	};
	const std::string goal_capability = append_server(*raw_root_session, app.chats.front());
	const std::filesystem::path goal_directory = std::ranges::find(
	    app.uam_control_capabilities, root_id, &uam::UamControlCapability::session_chat_id)->directory;
	const int64_t now = uam::time::SystemEpochMillisecondsNow();
	const nlohmann::json goal_request = {
	    {"requestId", "goal-approval"},
	    {"method", "goal_create"},
	    {"arguments", {{"objective", "Finish the bounded task."}, {"idempotencyKey", "goal-approval-once"}}},
	};
	const nlohmann::json pending_goal = uam::UamControlService::HandleRequestForTests(
	    app, goal_capability, goal_request, now);
	UAM_ASSERT(pending_goal.value("pendingApproval", false));
	UAM_ASSERT(uam::GoalService::FindActiveGoal(app, root_id) == nullptr);
	const std::optional<uam::AcpPendingUserInputState> goal_question =
	    uam::UamControlService::PendingApprovalForChat(app, root_id);
	UAM_ASSERT(goal_question.has_value());
	UAM_ASSERT_EQ(goal_question->questions.front().id, std::string("goalCreate"));
	const nlohmann::json pending_serialized = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(pending_serialized["chats"][0]["acpSession"]["pendingUserInput"]["attentionKind"],
	              std::string("question"));
	UAM_ASSERT_EQ(pending_serialized["chats"][0]["acpSession"]["pendingUserInput"]["questions"][0]["id"],
	              std::string("goalCreate"));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, root_id, goal_question->request_id_json,
	    {{"goalCreate", {"Allow"}}}, &error));
	const Goal* goal = uam::GoalService::FindActiveGoal(app, root_id);
	UAM_ASSERT(goal != nullptr);
	UAM_ASSERT_EQ(goal->objective, std::string("Finish the bounded task."));
	const nlohmann::json resolved_serialized = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT(resolved_serialized["chats"][0]["acpSession"]["pendingUserInput"].is_null());
	std::string goal_response_text;
	UAM_ASSERT(uam::io::TryReadTextFile(
	    goal_directory / "responses" / "goal-approval.json", goal_response_text));
	UAM_ASSERT(nlohmann::json::parse(goal_response_text).value("ok", false));
	UAM_ASSERT(!app.chats.front().uam_control_audit.empty());
	UAM_ASSERT_EQ(app.chats.front().uam_control_audit.back().method, std::string("goal_create"));

	app.chats.front().approval_mode = "plan";
	const std::string plan_capability = append_server(*raw_root_session, app.chats.front());
	const nlohmann::json plan_rejected = uam::UamControlService::HandleRequestForTests(
	    app, plan_capability,
	    {{"requestId", "goal-plan"}, {"method", "goal_create"},
	     {"arguments", {{"objective", "Must not run."}, {"idempotencyKey", "goal-plan"}}}}, now);
	UAM_ASSERT(!plan_rejected.value("ok", true));
	UAM_ASSERT(plan_rejected.value("error", "").find("Plan") != std::string::npos);
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(app, root_id).has_value());

	ChatSession child = ChatDomainService().CreateNewChat("", "opencode-cli");
	child.id = "chat-goal-approval-child";
	child.uam_control_enabled = true;
	child.execution_host_id = "local";
	child.agent_run_id = "child-run";
	const std::string child_id = child.id;
	child.messages.push_back(Message{MessageRole::User, "Inspect Safari for the delegated task."});
	child.messages.back().created_at = "owned-child-user-turn";
	app.chats.push_back(std::move(child));
	AgentRun child_run;
	child_run.id = "child-run";
	child_run.root_chat_id = root_id;
	child_run.transcript_chat_id = child_id;
	child_run.status = "running";
	app.agent_runs.push_back(std::move(child_run));
	auto child_session = std::make_unique<uam::AcpSessionState>();
	child_session->chat_id = child_id;
	child_session->provider_id = "opencode-cli";
	child_session->running = child_session->processing = true;
	child_session->turn_serial = 1;
	child_session->turn_first_user_message_index = child_session->turn_user_message_index = 0;
	uam::AcpSessionState* raw_child_session = child_session.get();
	app.acp_sessions.push_back(std::move(child_session));
	app.chats.front().approval_mode = "default";
	const std::string child_capability = append_server(*raw_child_session, app.chats.back());
	const nlohmann::json child_request = {
	    {"requestId", "child-computer-use"},
	    {"method", "computer_use_request"},
	    {"arguments", {{"reason", "The child task needs browser control."}}},
	};
	const nlohmann::json child_pending = uam::UamControlService::HandleRequestForTests(
	    app, child_capability, child_request, now);
	UAM_ASSERT(child_pending.value("pendingApproval", false));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(app, root_id).has_value());
	const std::optional<uam::AcpPendingUserInputState> child_question =
	    uam::UamControlService::PendingApprovalForChat(app, child_id);
	UAM_ASSERT(child_question.has_value());
	UAM_ASSERT_EQ(child_question->questions.front().id, std::string("computerUse"));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, child_id, child_question->request_id_json,
	    {{"computerUse", {"Allow"}}}, &error));
	UAM_ASSERT(app.chats.back().computer_use_enabled);
	UAM_ASSERT(!app.chats.front().computer_use_enabled);

	uam::UamControlService::Shutdown(app);
}

UAM_TEST(UamControlUserQuestionRoutesThroughTheExistingGuiInput)
{
	TempDir temp("uam-control-user-question");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root = ChatDomainService().CreateNewChat("", "opencode-cli");
	root.id = "chat-user-question-root";
	root.uam_control_enabled = true;
	root.execution_host_id = "local";
	const std::string root_id = root.id;
	app.chats.push_back(std::move(root));
	auto root_session = std::make_unique<uam::AcpSessionState>();
	root_session->chat_id = root_id;
	root_session->provider_id = "opencode-cli";
	root_session->running = true;
	uam::AcpSessionState* raw_root_session = root_session.get();
	app.acp_sessions.push_back(std::move(root_session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	nlohmann::json tools = uam::UamControlService::ToolDefinitionsForTests();
	const auto question_tool = std::ranges::find(tools, "user_question", [](const nlohmann::json& tool) {
		return tool.value("name", "");
	});
	UAM_ASSERT(question_tool != tools.end());
	UAM_ASSERT((*question_tool)["inputSchema"]["required"] == nlohmann::json::array({"question"}));

	nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    app, *raw_root_session, app.chats.front(), "session/new", setup, &error));
	const std::string capability_id = app.uam_control_capabilities.front().id;
	const std::filesystem::path root_directory = app.uam_control_capabilities.front().directory;
	const int64_t now = uam::time::SystemEpochMillisecondsNow();
	UAM_ASSERT_EQ(uam::UamControlService::HandleRequestForTests(
	    app, capability_id, {{"requestId", "invalid-question"}, {"method", "user_question"},
	                         {"arguments", {{"question", "Which browser?"}, {"unexpected", "value"}}}}, now)
	                  .value("error", ""),
	              std::string("Control request has an unknown method or invalid arguments."));
	const nlohmann::json request = {
	    {"requestId", "question-root"}, {"method", "user_question"},
	    {"arguments", {{"question", "Which browser should I use?"}, {"options", {"Firefox", "Safari"}}}},
	};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, capability_id, request, now)
	               .value("pendingApproval", false));
	UAM_ASSERT(!app.chats.front().interaction_at.empty());
	const auto pending = uam::UamControlService::PendingApprovalForChat(app, root_id);
	UAM_ASSERT(pending.has_value());
	UAM_ASSERT_EQ(pending->questions.front().id, std::string("userQuestion"));
	UAM_ASSERT_EQ(pending->questions.front().question, std::string("Which browser should I use?"));
	UAM_ASSERT_EQ(pending->questions.front().options.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(pending->questions.front().is_other);
	const nlohmann::json serialized = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(serialized["chats"][0]["acpSession"]["pendingUserInput"]["questions"][0]["id"],
	              std::string("userQuestion"));
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, root_id, pending->request_id_json,
	    {{"userQuestion", {"Brave"}}}, &error));
	std::string response_text;
	UAM_ASSERT(uam::io::TryReadTextFile(root_directory / "responses" / "question-root.json", response_text));
	UAM_ASSERT_EQ(nlohmann::json::parse(response_text)["result"]["answer"], std::string("Brave"));

	ChatSession child = ChatDomainService().CreateNewChat("", "opencode-cli");
	child.id = "chat-user-question-child";
	child.uam_control_enabled = true;
	child.execution_host_id = "local";
	child.agent_run_id = "question-child-run";
	const std::string child_id = child.id;
	app.chats.push_back(std::move(child));
	AgentRun child_run;
	child_run.id = "question-child-run";
	child_run.root_chat_id = root_id;
	child_run.transcript_chat_id = child_id;
	child_run.status = "running";
	app.agent_runs.push_back(std::move(child_run));
	auto child_session = std::make_unique<uam::AcpSessionState>();
	child_session->chat_id = child_id;
	child_session->provider_id = "opencode-cli";
	child_session->running = true;
	uam::AcpSessionState* raw_child_session = child_session.get();
	app.acp_sessions.push_back(std::move(child_session));
	setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    app, *raw_child_session, app.chats.back(), "session/new", setup, &error));
	const std::string child_capability_id = app.uam_control_capabilities.back().id;
	const nlohmann::json child_request = {
	    {"requestId", "question-child"}, {"method", "user_question"},
	    {"arguments", {{"question", "What should I inspect next?"}}},
	};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(
	    app, child_capability_id, child_request, now).value("pendingApproval", false));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(app, root_id).has_value());
	const auto child_pending = uam::UamControlService::PendingApprovalForChat(app, child_id);
	UAM_ASSERT(child_pending.has_value());
	const nlohmann::json concurrent_root_request = {
	    {"requestId", "question-concurrent-root"}, {"method", "user_question"},
	    {"arguments", {{"question", "Can the root chat proceed?"}}},
	};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(
	    app, capability_id, concurrent_root_request, now).value("pendingApproval", false));
	const auto concurrent_root_pending = uam::UamControlService::PendingApprovalForChat(app, root_id);
	UAM_ASSERT(concurrent_root_pending.has_value());
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, root_id, concurrent_root_pending->request_id_json,
	    {{"userQuestion", {"Yes"}}}, &error));
	UAM_ASSERT(uam::UamControlService::PendingApprovalForChat(app, child_id).has_value());
	UAM_ASSERT(uam::UamControlService::ResolveApproval(
	    app, child_id, child_pending->request_id_json,
	    {{"userQuestion", {"Review the SSH runtime"}}}, &error));

	const nlohmann::json expired_request = {
	    {"requestId", "question-expired"}, {"method", "user_question"},
	    {"arguments", {{"question", "This should expire."}}},
	};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(
	    app, capability_id, expired_request, now).value("pendingApproval", false));
	const auto root_capability = std::ranges::find(app.uam_control_capabilities, capability_id,
	                                                &uam::UamControlCapability::id);
	UAM_ASSERT(root_capability != app.uam_control_capabilities.end());
	root_capability->pending_approval->expires_at_epoch_ms = now - 1;
	UAM_ASSERT(uam::UamControlService::ProcessPendingRequests(app));
	UAM_ASSERT(!uam::UamControlService::PendingApprovalForChat(app, root_id).has_value());

	uam::UamControlService::Shutdown(app);
}

UAM_TEST(CefEditShortcutsReachFocusedFrameCommands)
{
	struct EditTargetProbe
	{
		int command = 0;
		void SelectAll() { command = MENU_ID_SELECT_ALL; }
		void Copy() { command = MENU_ID_COPY; }
		void Paste() { command = MENU_ID_PASTE; }
		void Cut() { command = MENU_ID_CUT; }
	};

	CefKeyEvent event{};
	event.type = KEYEVENT_RAWKEYDOWN;
	EditTargetProbe target;
	const auto assert_dispatch = [&](int key, int expected)
	{
		event.windows_key_code = key;
		target.command = 0;
		UAM_ASSERT(uam::cef::DispatchEditCommandForKeyEvent(event, target));
		UAM_ASSERT_EQ(target.command, expected);
	};

	for (const uint32_t modifier : {static_cast<uint32_t>(EVENTFLAG_CONTROL_DOWN), static_cast<uint32_t>(EVENTFLAG_COMMAND_DOWN)})
	{
		event.modifiers = modifier;
		assert_dispatch('A', MENU_ID_SELECT_ALL);
		assert_dispatch('C', MENU_ID_COPY);
		assert_dispatch('V', MENU_ID_PASTE);
		assert_dispatch('X', MENU_ID_CUT);
	}

	event.windows_key_code = 'A';
	event.modifiers = EVENTFLAG_COMMAND_DOWN | EVENTFLAG_ALT_DOWN;
	UAM_ASSERT(!uam::cef::DispatchEditCommandForKeyEvent(event, target));
	event.modifiers = EVENTFLAG_COMMAND_DOWN;
	event.type = KEYEVENT_KEYUP;
	UAM_ASSERT(!uam::cef::DispatchEditCommandForKeyEvent(event, target));
}

UAM_TEST(AgentDefinitionsAreProviderNeutralBoundedAndWorkspaceOverridable)
{
	TempDir temp("uam-agent-definitions");
	const fs::path workspace = temp.root / "workspace";
	const fs::path global_root = temp.root / "agents";
	const fs::path workspace_root = workspace / ".uam" / "agents";
	fs::create_directories(global_root);
	fs::create_directories(workspace_root);
	UAM_ASSERT(uam::io::WriteTextFile(
	    global_root / "reviewer.md",
	    "---\nversion: 1\nname: reviewer\ndescription: Global reviewer\nmode: both\nworkspaceAccess: read\nskills: [tests]\ndelegates: []\n---\nReview the work.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    workspace_root / "reviewer.md",
	    "---\nversion: 1\nname: reviewer\ndescription: Workspace reviewer\nmode: primary\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nReview this workspace.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    global_root / "provider-owned.md",
	    "---\nversion: 1\nname: provider-owned\ndescription: Invalid\nmode: primary\nworkspaceAccess: read\nprovider: opencode\nskills: []\ndelegates: []\n---\nDo not load.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    global_root / "blocked.md",
	    "---\nversion: 1\nname: blocked\ndescription: Global fallback must not run\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nGlobal instructions.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    workspace_root / "blocked.md",
	    "---\nversion: 1\nname: blocked\ndescription: Broken workspace override\nmode: primary\nworkspaceAccess: read\nprovider: opencode\nskills: []\ndelegates: []\n---\nBroken instructions.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    global_root / "cycle-a.md",
	    "---\nversion: 1\nname: cycle-a\ndescription: Cycle A\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: [cycle-b]\n---\nA.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    global_root / "cycle-b.md",
	    "---\nversion: 1\nname: cycle-b\ndescription: Cycle B\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: [cycle-a]\n---\nB.\n"));

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root, workspace);
	const auto find = [&](std::string_view id) {
		return std::ranges::find(catalog.definitions, id, &uam::AgentDefinition::id);
	};
	UAM_ASSERT(find("build") != catalog.definitions.end());
	UAM_ASSERT(find("plan") != catalog.definitions.end());
	const auto reviewer = find("reviewer");
	UAM_ASSERT(reviewer != catalog.definitions.end());
	UAM_ASSERT(reviewer->workspace_override);
	UAM_ASSERT_EQ(reviewer->description, std::string("Workspace reviewer"));
	UAM_ASSERT(!reviewer->definition_hash.empty());
	UAM_ASSERT(reviewer->markdown_snapshot.find("Review this workspace") != std::string::npos);
	UAM_ASSERT(find("provider-owned") == catalog.definitions.end());
	UAM_ASSERT(find("blocked") == catalog.definitions.end());
	UAM_ASSERT(find("cycle-a") == catalog.definitions.end());
	UAM_ASSERT(find("cycle-b") == catalog.definitions.end());
	UAM_ASSERT(catalog.errors.size() >= static_cast<std::size_t>(2));
}

UAM_TEST(AgentDefinitionDiscoveryBoundsAggregateReadsAndVisibleDiagnostics)
{
	TempDir aggregate_temp("uam-agent-definition-aggregate-cap");
	const fs::path aggregate_root = aggregate_temp.root / "agents";
	fs::create_directories(aggregate_root);
	const std::string instructions(190U * 1024U, 'x');
	for (int index = 0; index < 48; ++index)
	{
		const std::string id = "agent-" + std::to_string(index);
		UAM_ASSERT(uam::io::WriteTextFile(
		    aggregate_root / (id + ".md"),
		    "---\nversion: 1\nname: " + id + "\ndescription: Bounded agent\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\n" + instructions));
	}
	const uam::AgentDefinitionCatalog aggregate_catalog = uam::AgentDefinitionService::Load(aggregate_temp.root, {});
	UAM_ASSERT(aggregate_catalog.definitions.size() < static_cast<std::size_t>(50));
	UAM_ASSERT(std::ranges::any_of(aggregate_catalog.errors, [](const std::string& error) {
		return error.find("8 MiB scan limit") != std::string::npos;
	}));

	TempDir diagnostic_temp("uam-agent-definition-diagnostic-cap");
	const fs::path diagnostic_root = diagnostic_temp.root / "agents";
	fs::create_directories(diagnostic_root);
	for (int index = 0; index < 100; ++index)
	{
		UAM_ASSERT(uam::io::WriteTextFile(diagnostic_root / ("broken-" + std::to_string(index) + ".md"), "not frontmatter"));
	}
	const uam::AgentDefinitionCatalog diagnostic_catalog = uam::AgentDefinitionService::Load(diagnostic_temp.root, {});
	UAM_ASSERT(diagnostic_catalog.errors.size() <= static_cast<std::size_t>(64));
	UAM_ASSERT(diagnostic_catalog.errors.back().find("additional agent definition diagnostics omitted") != std::string::npos);
}

UAM_TEST(GlobalAgentEditorSavesValidDefinitionsAndRejectsInvalidOnes)
{
	TempDir temp("uam-agent-editor");
	uam::AgentDefinition agent{.id = "reviewer", .description = "Reviews diffs", .mode = "primary", .workspace_access = "read", .skills = {"tests"}, .instructions = "Review the work."};
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(temp.root, agent, &error));
	const auto catalog = uam::AgentDefinitionService::Load(temp.root, {});
	const auto saved = std::ranges::find(catalog.definitions, std::string("reviewer"), &uam::AgentDefinition::id);
	UAM_ASSERT(saved != catalog.definitions.end() && saved->skills == std::vector<std::string>{"tests"} && saved->instructions == "Review the work.");
	agent.instructions = "";
	UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(temp.root, agent, &error));
	UAM_ASSERT(uam::AgentDefinitionService::Load(temp.root, {}).errors.empty());
	agent.id = "build";
	UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(temp.root, agent, &error));
	UAM_ASSERT(!uam::AgentDefinitionService::DeleteGlobalAgent(temp.root, "../reviewer", &error));
	UAM_ASSERT(uam::AgentDefinitionService::DeleteGlobalAgent(temp.root, "reviewer", &error));
	UAM_ASSERT(!fs::exists(temp.root / "agents" / "reviewer.md"));
}

UAM_TEST(ProviderMarkdownAgentImportIsProviderNeutralAndNeverSilentlyDropsSecurity)
{
	TempDir temp("uam-provider-agent-import");
	const fs::path sources = temp.root / "native";
	fs::create_directories(sources);
	const fs::path opencode = sources / "reviewer.md";
	const std::string opencode_text =
	    "---\ndescription: Reviews without editing\nmode: subagent\nmodel: vendor/model\n---\nReview the code and report issues.\n";
	UAM_ASSERT(uam::io::WriteTextFile(opencode, opencode_text));

	const uam::ProviderAgentImportPreview preview =
	    uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, opencode);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.suggested_id, std::string("reviewer"));
	UAM_ASSERT_EQ(preview.mode, std::string("subagent"));
	UAM_ASSERT_EQ(preview.ignored_fields, std::vector<std::string>({"model"}));

	uam::ProviderAgentImportRequest request;
	request.provider_id = uam::provider_ids::kOpenCodeCli;
	request.source_path = opencode;
	request.workspace_access = "read";
	std::string error;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(
	    temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(error.find("acknowledge") != std::string::npos);
	request.acknowledge_ignored_fields = true;
	uam::AgentDefinition imported;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(
	    temp.root / "data", {}, request, &imported, &error));
	UAM_ASSERT_EQ(imported.id, std::string("reviewer"));
	UAM_ASSERT_EQ(imported.workspace_access, std::string("read"));
	UAM_ASSERT(imported.markdown_snapshot.find("vendor/model") == std::string::npos);
	UAM_ASSERT(imported.markdown_snapshot.find("opencode") == std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(opencode), opencode_text);

	const std::vector<std::pair<std::string, std::string>> unsafe = {
	    {uam::provider_ids::kOpenCodeCli,
	     "---\ndescription: Restricted\nmode: subagent\npermission:\n  edit:\n    '*': deny\n---\nReview.\n"},
	    {uam::provider_ids::kCopilotCli,
	     "---\nname: copilot-reviewer\ndescription: Restricted\ntools: [read]\n---\nReview.\n"},
	    {uam::provider_ids::kGeminiCli,
	     "---\nname: gemini-reviewer\ndescription: Restricted\nkind: local\nmax_turns: 4\ntimeout_mins: 2\n---\nReview.\n"},
	    {uam::provider_ids::kClaudeCli,
	     "---\nname: claude-reviewer\ndescription: Restricted\ndisallowedTools: [Edit]\n---\nReview.\n"},
	};
	for (std::size_t index = 0; index < unsafe.size(); ++index)
	{
		const fs::path source = sources / ("unsafe-" + std::to_string(index) + ".md");
		UAM_ASSERT(uam::io::WriteTextFile(source, unsafe[index].second));
		const auto blocked = uam::AgentDefinitionService::PreviewProviderAgentImport(unsafe[index].first, source);
		UAM_ASSERT(!blocked.supported);
		UAM_ASSERT(!blocked.security_fields.empty());
		UAM_ASSERT(blocked.error.find("security/tool fields") != std::string::npos);
		if (unsafe[index].first == uam::provider_ids::kGeminiCli)
		{
			UAM_ASSERT(std::ranges::find(blocked.security_fields, "max_turns") != blocked.security_fields.end());
			UAM_ASSERT(std::ranges::find(blocked.security_fields, "timeout_mins") != blocked.security_fields.end());
		}
	}
}

UAM_TEST(CopilotGeminiAndClaudeAgentImportsRemainSubagentsWithExplicitUamAccess)
{
	TempDir temp("uam-provider-agent-import-modes");
	const fs::path sources = temp.root / "native";
	fs::create_directories(sources);
	const fs::path copilot = sources / "docs.agent.md";
	const fs::path gemini = sources / "gemini-agent.md";
	const fs::path claude = sources / "claude-agent.md";
	UAM_ASSERT(uam::io::WriteTextFile(copilot,
	    "---\nname: Docs Writer\ndescription: Writes documentation\n---\nWrite concise documentation.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(gemini,
	    "---\nname: gemini-helper\ndescription: Investigates code\nkind: local\n---\nInvestigate and report.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(claude,
	    "---\nname: claude-helper\ndescription: Reviews code\nmodel: inherit\n---\nReview and report.\n"));

	for (const auto& [provider, source, expected_id, access] :
	     std::vector<std::tuple<std::string, fs::path, std::string, std::string>>{
	         {uam::provider_ids::kCopilotCli, copilot, "docs", "write"},
	         {uam::provider_ids::kGeminiCli, gemini, "gemini-helper", "read"},
	         {uam::provider_ids::kClaudeCli, claude, "claude", "read"}})
	{
		const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(provider, source);
		UAM_ASSERT(preview.supported);
		UAM_ASSERT_EQ(preview.mode, std::string("subagent"));
		if (provider == uam::provider_ids::kClaudeCli)
		{
			UAM_ASSERT_EQ(preview.ignored_fields, std::vector<std::string>({"model"}));
		}
		uam::ProviderAgentImportRequest request;
		request.provider_id = provider;
		request.source_path = source;
		request.canonical_id = expected_id;
		request.workspace_access = access;
		request.acknowledge_ignored_fields = !preview.ignored_fields.empty();
		uam::AgentDefinition imported;
		std::string error;
		UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(
		    temp.root / "data", {}, request, &imported, &error));
		UAM_ASSERT_EQ(imported.id, expected_id);
		UAM_ASSERT_EQ(imported.mode, std::string("subagent"));
		UAM_ASSERT_EQ(imported.workspace_access, access);
	}
}

UAM_TEST(ProviderAgentRuntimeAdaptersAreIsolatedExactAndPermissionMediated)
{
	TempDir temp("uam-provider-agent-runtime-adapters");
	uam::ProviderAgentRuntimeAdapter opencode;
	uam::ProviderAgentRuntimeAdapter copilot;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::PrepareRuntimeAdapter(
	    temp.root, "chat-one", uam::provider_ids::kOpenCodeCli, "reviewer",
	    "Review without changing UAM permissions.", &opencode, &error));
	UAM_ASSERT_EQ(opencode.execution_capability, std::string("opencode-native-agent-config"));
	UAM_ASSERT(!opencode.inject_in_prompt);
	UAM_ASSERT_EQ(opencode.launch_environment.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(opencode.launch_environment.front().first, std::string("OPENCODE_CONFIG_DIR"));
	const nlohmann::json opencode_config = nlohmann::json::parse(
	    uam::io::ReadTextFile(opencode.directory / "opencode.json"));
	UAM_ASSERT_EQ(opencode_config["permission"].value("*", ""), std::string("ask"));
	UAM_ASSERT_EQ(opencode_config["permission"].value("task", ""), std::string("deny"));
	UAM_ASSERT_EQ(opencode_config["permission"].value("uam-computer_computer_observe", ""),
	              std::string("allow"));
	UAM_ASSERT_EQ(opencode_config["permission"].value("uam-computer_computer_action", ""),
	              std::string("allow"));
	UAM_ASSERT_EQ(opencode_config["experimental"].value("mcp_timeout", 0), 120000);
	const std::string opencode_agent = uam::io::ReadTextFile(
	    opencode.directory / "agent" /
	    ("uam-" + opencode.directory.filename().string() + ".md"));
	UAM_ASSERT(uam::strings::Contains(opencode_agent,
	                                  "uam-computer_computer_observe: allow"));
	UAM_ASSERT(uam::strings::Contains(opencode_agent,
	                                  "uam-computer_computer_action: allow"));
	UAM_ASSERT_EQ(opencode_config.value("subagent_depth", -1), 0);

	UAM_ASSERT(uam::AgentDefinitionService::PrepareRuntimeAdapter(
	    temp.root, "chat-one", uam::provider_ids::kCopilotCli, "reviewer",
	    "Review without changing UAM permissions.", &copilot, &error));
	UAM_ASSERT_EQ(copilot.execution_capability, std::string("copilot-native-agent-plugin"));
	UAM_ASSERT(!copilot.inject_in_prompt);
	UAM_ASSERT(copilot.directory != opencode.directory);
	UAM_ASSERT(std::ranges::find(copilot.launch_arguments, std::string("--agent")) !=
	           copilot.launch_arguments.end());
	UAM_ASSERT(std::ranges::find(
	    copilot.launch_arguments,
	    std::string("--excluded-tools=task,read_agent,list_agents,write_agent")) !=
	           copilot.launch_arguments.end());

	for (const char* provider : {uam::provider_ids::kGeminiCli,
	                             uam::provider_ids::kCodexCli,
	                             uam::provider_ids::kClaudeCli})
	{
		uam::ProviderAgentRuntimeAdapter generic;
		UAM_ASSERT(uam::AgentDefinitionService::PrepareRuntimeAdapter(
		    temp.root, "chat-one", provider, "reviewer", "Review.", &generic, &error));
		UAM_ASSERT_EQ(generic.execution_capability, std::string("uam-prompt-injected"));
		UAM_ASSERT(generic.inject_in_prompt);
		UAM_ASSERT(generic.directory.empty());
		UAM_ASSERT(generic.launch_arguments.empty());
		UAM_ASSERT(generic.launch_environment.empty());
	}
}

#if !defined(_WIN32)
UAM_TEST(AgentDefinitionDiscoveryRejectsLinkedFilesAndWorkspaceAgentAncestors)
{
	TempDir temp("uam-agent-definition-links");
	const fs::path external = temp.root / "external";
	const fs::path global_root = temp.root / "agents";
	fs::create_directories(external / "agents");
	fs::create_directories(global_root);
	const std::string definition =
	    "---\nversion: 1\nname: escaped\ndescription: Escaped\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nNever load through a link.\n";
	UAM_ASSERT(uam::io::WriteTextFile(external / "escaped.md", definition));
	fs::create_symlink(external / "escaped.md", global_root / "escaped.md");

	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	UAM_ASSERT(uam::io::WriteTextFile(external / "agents" / "escaped.md", definition));
	fs::create_directory_symlink(external, workspace / ".uam");

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root, workspace);
	UAM_ASSERT(std::ranges::find(catalog.definitions, std::string("escaped"), &uam::AgentDefinition::id) == catalog.definitions.end());
	UAM_ASSERT(catalog.errors.size() >= static_cast<std::size_t>(2));

	TempDir dangling("uam-agent-definition-dangling-root");
	fs::create_directories(dangling.root / "data");
	fs::create_directory_symlink(dangling.root / "missing-target", dangling.root / "data" / "agents");
	const uam::AgentDefinitionCatalog dangling_catalog = uam::AgentDefinitionService::Load(dangling.root / "data", {});
	UAM_ASSERT(std::ranges::any_of(dangling_catalog.errors, [](const std::string& error) {
		return error.find("agent root is linked") != std::string::npos;
	}));
}
#endif

UAM_TEST(AgentRunSchedulerLaunchesThePersistedDefinitionSnapshot)
{
	TempDir temp("uam-agent-scheduler-definition-change");
	const fs::path workspace = temp.root / "workspace";
	const fs::path data_root = temp.root / "data";
	const fs::path agents = data_root / "agents";
	fs::create_directories(agents);
	const fs::path agent_path = agents / "reviewer.md";
	UAM_ASSERT(uam::io::WriteTextFile(agent_path,
	    "---\nversion: 1\nname: reviewer\ndescription: Review\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nReview carefully.\n"));

	uam::AppState app;
	app.data_root = data_root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession root;
	root.id = "root-chat";
	root.provider_id = app.provider_profiles.front().id;
	root.folder_id = "remote-workspace";
	root.execution_host_id = "ssh-homelab";
	root.workspace_directory = workspace.string();
	app.folders.push_back({root.folder_id, "Remote workspace", root.workspace_directory,
	                       false, root.execution_host_id});
	app.chats.push_back(root);

	std::string run_id;
	std::string error;
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(app, root.id, "", "reviewer", "Review this.", &run_id, &error));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT_EQ(app.queued_agent_run_ids.front(), run_id);
	UAM_ASSERT(!app.chats.back().agent_run_id.empty());
	UAM_ASSERT_EQ(app.chats.back().folder_id, root.folder_id);
	UAM_ASSERT_EQ(app.chats.back().execution_host_id, root.execution_host_id);
	UAM_ASSERT_EQ(app.chats.back().workspace_directory, root.workspace_directory);
	UAM_ASSERT_EQ(uam::AgentRunLedger::LoadAll(app.data_root).runs.front().status, std::string("queued"));
	const std::string transcript_id = app.chats.back().id;
	const std::string saved_hash = app.agent_runs.front().definition_hash;
	const std::string saved_snapshot = app.agent_runs.front().definition_snapshot;
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript_id;
	session->provider_id = app.chats.back().provider_id;
	session->processing = true;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::io::WriteTextFile(agent_path,
	    "---\nversion: 1\nname: reviewer\ndescription: Changed\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nDifferent instructions.\n"));
	UAM_ASSERT(uam::AgentRunScheduler::Poll(app));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), static_cast<std::size_t>(1));
	const auto& queued = app.acp_sessions.front()->queued_user_prompts.front();
	UAM_ASSERT_EQ(queued.uam_agent_definition_hash, saved_hash);
	UAM_ASSERT_EQ(queued.uam_agent_definition_snapshot, saved_snapshot);
	UAM_ASSERT_EQ(queued.uam_agent_instructions, std::string("Review carefully."));
	UAM_ASSERT(queued.uam_agent_instructions.find("Different instructions") == std::string::npos);
}

UAM_TEST(AgentRunSchedulerRejectsCyclesDepthFanoutAndCancelsDescendants)
{
	TempDir temp("uam-agent-scheduler-bounds");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "parent.md",
	    "---\nversion: 1\nname: parent\ndescription: Parent\nmode: both\nworkspaceAccess: write\nskills: []\ndelegates: [child]\n---\nParent.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(agents / "child.md",
	    "---\nversion: 1\nname: child\ndescription: Child\nmode: both\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nChild.\n"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession root;
	root.id = "root-chat";
	root.provider_id = app.provider_profiles.front().id;
	root.workspace_directory = workspace.string();
	app.chats.push_back(root);
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
	const auto parent_definition = std::ranges::find(catalog.definitions, std::string("parent"), &uam::AgentDefinition::id);
	UAM_ASSERT(parent_definition != catalog.definitions.end());

	AgentRun parent;
	parent.id = uam::AgentRunLedger::NewRunId();
	parent.root_chat_id = root.id;
	parent.transcript_chat_id = "parent-transcript";
	parent.agent_id = "parent";
	parent.definition_hash = parent_definition->definition_hash;
	parent.definition_snapshot = parent_definition->markdown_snapshot;
	parent.definition_instructions = parent_definition->instructions;
	parent.delegates_snapshot = parent_definition->delegates;
	parent.provider_id = root.provider_id;
	parent.task = "Parent task";
	parent.effective_workspace_access = "write";
	parent.status = "running";
	parent.depth = 1;
	parent.created_at = uam::time::TimestampNow();
	parent.updated_at = parent.created_at;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, parent));
	app.agent_runs.push_back(parent);

	std::string child_id;
	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(app, root.id, parent.id, "parent", "Cycle", nullptr, &error));
	UAM_ASSERT(error.find("cycles") != std::string::npos);
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(app, root.id, parent.id, "child", "Child task", &child_id, &error));
	AgentRun* child = &app.agent_runs.back();
	UAM_ASSERT_EQ(child->depth, 2);
	UAM_ASSERT_EQ(child->effective_workspace_access, std::string("read"));
	child->status = "running";
	UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(app, root.id, child->id, "parent", "Too deep", nullptr, &error));
	UAM_ASSERT(error.find("two levels") != std::string::npos);

	const AgentRun extra_template = *child;
	for (int index = 0; index < 3; ++index)
	{
		AgentRun extra = extra_template;
		extra.id = uam::AgentRunLedger::NewRunId();
		extra.transcript_chat_id = "extra-" + std::to_string(index);
		extra.status = "completed";
		app.agent_runs.push_back(std::move(extra));
	}
	UAM_ASSERT(!uam::AgentRunScheduler::Enqueue(app, root.id, parent.id, "child", "Fifth child", nullptr, &error));
	UAM_ASSERT(error.find("four direct children") != std::string::npos);

	UAM_ASSERT(uam::AgentRunScheduler::CancelTree(app, parent.id, &error));
	for (const AgentRun& run : app.agent_runs)
	{
		if (run.id == parent.id || run.id == child_id) UAM_ASSERT_EQ(run.status, std::string("cancelled"));
	}
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
}

UAM_TEST(AgentRunCancellationPreservesAnUnconfirmedRemoteManagedTurn)
{
	TempDir temp("uam-agent-cancel-unconfirmed-remote-stop");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession root;
	root.id = "managed-root";
	ChatSession transcript;
	transcript.id = "managed-transcript";
	transcript.execution_host_id = "ssh-test";
	transcript.remote_turn_reconnect_pending = true;
	app.chats = {root, transcript};

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "reviewer";
	run.provider_id = uam::provider_ids::kCodexCli;
	run.definition_snapshot = "Review safely.";
	run.task = "Review the remote turn.";
	run.effective_workspace_access = "read";
	run.status = "running";
	run.depth = 1;
	run.created_at = uam::time::TimestampNow();
	run.updated_at = run.created_at;
	app.chats.back().agent_run_id = run.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript.id;
	session->managed_agent_run_id = run.id;
	session->managed_launch_attempted = true;
	session->running = true;
	session->processing = true;
	session->prompt_request_id = 7;
	std::string process_error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 70"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 70"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &process_error));
	app.acp_sessions.push_back(std::move(session));

	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(app, run.id, &error));
	UAM_ASSERT(uam::strings::Contains(error, "stopping"));
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_pending);
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.acp_sessions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.back().remote_turn_reconnect_pending);
	UAM_ASSERT(app.acp_sessions.front()->processing);
	UAM_ASSERT(app.acp_sessions.front()->recovering_remote_turn);
	UAM_ASSERT(app.acp_sessions.front()->reconnect_pending);
	const auto saved = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().status, std::string("running"));
}

UAM_TEST(AgentRunDeadlinePreservesAnUnconfirmedRemoteManagedTurn)
{
	TempDir temp("uam-agent-deadline-unconfirmed-remote-stop");
	uam::AppState app;
	app.data_root = temp.root / "data";
	// The remote identity selects the stop protocol only; no SSH host or provider launcher is configured.
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup()
		{
			uam::FinalizePendingAcpRemoteStopsForExit(app);
			uam::FastStopAcpSessionsForExit(app);
		}
	} cleanup{app};
	ChatSession root;
	root.id = "deadline-root";
	root.provider_id = uam::provider_ids::kCodexCli;
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	root.messages_loaded = true;
	ChatDomainService().AddMessage(root, MessageRole::User, "Review until the deadline.");
	ChatSession transcript;
	transcript.id = "deadline-transcript";
	transcript.provider_id = root.provider_id;
	transcript.execution_host_id = "ssh-test";
	transcript.remote_turn_reconnect_pending = true;
	transcript.created_at = root.created_at;
	transcript.updated_at = root.updated_at;
	app.chats = {root, transcript};

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "reviewer";
	run.provider_id = root.provider_id;
	run.definition_snapshot = "Review safely.";
	run.task = "Review until the deadline.";
	run.effective_workspace_access = "read";
	run.status = "running";
	run.depth = 1;
	run.expected_turn_serial = 1;
	run.started_at_epoch_ms = 1000;
	run.deadline_at_epoch_ms = 2000;
	run.created_at = root.created_at;
	run.started_at = root.created_at;
	run.updated_at = root.created_at;
	run.result_delivery_id = run.id;
	run.deliver_result_to_root_chat = true;
	app.chats.back().agent_run_id = run.id;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.back()));
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);
	app.agent_run_deadline_steady_ms[run.id] = 2000;

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript.id;
	session->provider_id = run.provider_id;
	session->process_execution_host_id = transcript.execution_host_id;
	session->managed_agent_run_id = run.id;
	session->managed_launch_attempted = true;
	session->running = true;
	session->processing = true;
	session->turn_serial = 1;
	session->prompt_request_id = 7;
	std::string process_error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 70"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 70"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &process_error));
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 2000));
	UAM_ASSERT_EQ(app.acp_sessions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_pending);
	UAM_ASSERT_EQ(app.pending_acp_remote_stops.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.pending_acp_remote_stops.front()->chat_id, transcript.id);
	// Capture the deadline transition before observing the private proxy's failed stop receipt.
	const std::string status_while_stopping = app.agent_runs.front().status;
	const bool deadline_while_stopping = app.agent_run_deadline_steady_ms.contains(run.id);
	const bool delivered_while_stopping = app.agent_runs.front().root_result_delivered;
	const std::size_t parent_messages_while_stopping = app.chats.front().messages.size();
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT_EQ(app.acp_sessions.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_unconfirmed);
	UAM_ASSERT(app.acp_sessions.front()->processing);
	UAM_ASSERT(app.acp_sessions.front()->recovering_remote_turn);
	UAM_ASSERT_EQ(app.acp_sessions.front()->managed_agent_run_id, run.id);
	// An expired deadline cannot release this run's scheduler slot or publish its result while Stop is unconfirmed.
	UAM_ASSERT_EQ(status_while_stopping, std::string("running"));
	UAM_ASSERT(deadline_while_stopping);
	UAM_ASSERT(!delivered_while_stopping);
	UAM_ASSERT_EQ(parent_messages_while_stopping, root.messages.size());
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(app.agent_run_deadline_steady_ms.contains(run.id));
	UAM_ASSERT(!app.agent_runs.front().root_result_delivered);
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages, root.messages);
	UAM_ASSERT(app.chats.back().remote_turn_reconnect_pending);
	const auto saved_runs = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	UAM_ASSERT_EQ(saved_runs.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved_runs.front().status, std::string("running"));
	UAM_ASSERT(!saved_runs.front().root_result_delivered);
	const auto saved_root = ChatRepository::LoadLocalChat(app.data_root, root.id);
	UAM_ASSERT(saved_root.has_value());
	UAM_ASSERT_EQ(saved_root->messages, root.messages);
}

UAM_TEST(AgentRunCancellationRetriesWithoutPublishingAPhantomTerminalState)
{
	TempDir temp("uam-agent-cancel-transactional");
	uam::AppState app;
	const fs::path data_root = temp.root / "data";
	app.data_root = data_root;

	ChatSession root;
	root.id = "managed-root";
	ChatSession transcript;
	transcript.id = "managed-transcript";
	app.chats = {root, transcript};

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "reviewer";
	run.provider_id = uam::provider_ids::kCodexCli;
	run.definition_snapshot = "Review safely.";
	run.task = "Review the turn.";
	run.effective_workspace_access = "read";
	run.status = "running";
	run.depth = 1;
	run.created_at = uam::time::TimestampNow();
	run.updated_at = run.created_at;
	app.chats.back().agent_run_id = run.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);

	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));
	app.data_root = blocked_root;
	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(app, run.id, &error));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(app.agent_runs.front().finished_at.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));

	app.data_root = data_root;
	UAM_ASSERT(uam::AgentRunScheduler::CancelTree(app, run.id, &error));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("cancelled"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	const auto saved = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().status, std::string("cancelled"));
}

UAM_TEST(AgentRunQueuedCancellationRetriesWithoutLaunchingAfterSaveFailure)
{
	TempDir temp("uam-agent-cancel-transactional");
	uam::AppState app;
	const fs::path data_root = temp.root / "data";
	app.data_root = data_root;

	ChatSession root;
	root.id = "managed-root";
	ChatSession transcript;
	transcript.id = "managed-transcript";
	app.chats = {root, transcript};

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "reviewer";
	run.provider_id = uam::provider_ids::kCodexCli;
	run.definition_snapshot = "Review safely.";
	run.task = "Review the turn.";
	run.effective_workspace_access = "read";
	run.status = "queued";
	run.depth = 1;
	run.created_at = uam::time::TimestampNow();
	run.updated_at = run.created_at;
	app.chats.back().agent_run_id = run.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);
	app.queued_agent_run_ids.push_back(run.id);

	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));
	app.data_root = blocked_root;
	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(app, run.id, &error));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT(app.agent_runs.front().finished_at.empty());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));

	UAM_ASSERT_EQ(app.queued_agent_run_ids.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.pending_agent_run_cancellation_ids.contains(run.id));
	(void)uam::AgentRunScheduler::PollAtForTests(app, 1000);
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT_EQ(app.queued_agent_run_ids.size(), static_cast<std::size_t>(1));
	app.data_root = data_root;
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 1001));
	UAM_ASSERT(app.queued_agent_run_ids.empty());
	UAM_ASSERT(app.pending_agent_run_cancellation_ids.empty());
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("cancelled"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	const auto saved = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().status, std::string("cancelled"));
}

UAM_TEST(AgentRunCancellationWaitsForAConfirmedRemoteStopMarkerSave)
{
	TempDir temp("uam-agent-cancel-stop-save-retry");
	uam::AppState app;
	const fs::path data_root = temp.root / "data";
	app.data_root = data_root;

	ChatSession root;
	root.id = "managed-root";
	ChatSession transcript;
	transcript.id = "managed-transcript";
	transcript.execution_host_id = "ssh-test";
	transcript.remote_turn_reconnect_pending = true;
	app.chats = {root, transcript};

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "reviewer";
	run.provider_id = uam::provider_ids::kCodexCli;
	run.definition_snapshot = "Review safely.";
	run.task = "Review the remote turn.";
	run.effective_workspace_access = "read";
	run.status = "running";
	run.depth = 1;
	run.created_at = uam::time::TimestampNow();
	run.updated_at = run.created_at;
	app.chats.back().agent_run_id = run.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.back()));
	app.agent_runs.push_back(run);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = transcript.id;
	session->managed_agent_run_id = run.id;
	session->managed_launch_attempted = true;
	session->running = true;
	session->processing = true;
	session->prompt_request_id = 7;
	std::string process_error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {
	    "cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &process_error));
	app.acp_sessions.push_back(std::move(session));

	const fs::path blocked_root = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_root, "blocked"));
	app.data_root = blocked_root;
	std::string error;
	UAM_ASSERT(!uam::AgentRunScheduler::CancelTree(app, run.id, &error));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));
	UAM_ASSERT(app.acp_sessions.front()->managed_cancellation_pending);
	UAM_ASSERT(!app.acp_sessions.front()->remote_stop_pending);
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(transcript.id));
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(app, 1000));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("running"));

	app.data_root = data_root;
	uam::FlushPendingChatSaves(app, true);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	(void)uam::AgentRunScheduler::PollAtForTests(app, 1001);
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 1002));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("cancelled"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_sessions.empty());
}

UAM_TEST(AgentRunSchedulerKeepsMixedProviderChildrenInsideTheParentAccessCeiling)
{
	TempDir temp("uam-agent-mixed-provider-matrix");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "orchestrator.md",
	    "---\nversion: 1\nname: orchestrator\ndescription: Read-only parent\nmode: both\nworkspaceAccess: read\nskills: []\ndelegates: [worker]\n---\nDelegate safely.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
	    "---\nversion: 1\nname: worker\ndescription: Writable worker\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nWork carefully.\n"));

	const std::vector<ProviderProfile> profiles = ProviderProfileStore::BuiltInProfiles();
	std::vector<std::string> providers;
	for (const char* id : {uam::provider_ids::kOpenCodeCli, uam::provider_ids::kCopilotCli,
	                       uam::provider_ids::kGeminiCli, uam::provider_ids::kCodexCli,
	                       uam::provider_ids::kClaudeCli})
	{
		if (std::ranges::any_of(profiles, [id](const ProviderProfile& profile) { return profile.id == id; }))
		{
			providers.emplace_back(id);
		}
	}
	UAM_ASSERT(!providers.empty());
	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(temp.root / "definitions", workspace);
	const auto parent_definition = std::ranges::find(catalog.definitions, std::string("orchestrator"), &uam::AgentDefinition::id);
	UAM_ASSERT(parent_definition != catalog.definitions.end());

	for (const std::string& parent_provider : providers)
	{
		for (const std::string& child_provider : providers)
		{
			uam::AppState app;
			app.data_root = temp.root / "runs" / parent_provider / child_provider;
			app.provider_profiles = profiles;
			ChatSession root;
			root.id = "root-" + parent_provider + "-" + child_provider;
			root.provider_id = parent_provider;
			root.workspace_directory = workspace.string();
			root.uam_agent_id = "orchestrator";
			app.chats.push_back(root);

			AgentRun parent;
			parent.id = uam::AgentRunLedger::NewRunId();
			parent.root_chat_id = root.id;
			parent.transcript_chat_id = "parent-transcript";
			parent.agent_id = "orchestrator";
			parent.definition_hash = parent_definition->definition_hash;
			parent.definition_snapshot = parent_definition->markdown_snapshot;
			parent.definition_instructions = parent_definition->instructions;
			parent.delegates_snapshot = parent_definition->delegates;
			parent.provider_id = parent_provider;
			parent.execution_capability =
			    uam::AgentDefinitionService::ExecutionCapabilityForProvider(parent_provider);
			parent.task = "Coordinate.";
			parent.effective_workspace_access = "read";
			parent.status = "running";
			parent.created_at = uam::time::TimestampNow();
			parent.updated_at = parent.created_at;
			app.agent_runs.push_back(parent);

			std::string run_id;
			std::string error;
			UAM_ASSERT(uam::AgentRunScheduler::Enqueue(
			    app, root.id, parent.id, "worker", "Do the child task.", &run_id, &error, child_provider));
			const AgentRun& child = app.agent_runs.back();
			UAM_ASSERT_EQ(child.provider_id, child_provider);
			UAM_ASSERT_EQ(child.effective_workspace_access, std::string("read"));
			UAM_ASSERT_EQ(child.execution_capability,
			              uam::AgentDefinitionService::ExecutionCapabilityForProvider(child_provider));
			UAM_ASSERT_EQ(app.chats.back().approval_mode, std::string(uam::approval_modes::kPlanApprovalMode));
		}
	}
}

UAM_TEST(AgentRunSchedulerRoutesOnlyUnambiguousLeadingAgentMentions)
{
	TempDir temp("uam-agent-mention-routing");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "reviewer.md",
	    "---\nversion: 1\nname: reviewer\ndescription: Reviewer\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nReview.\n"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession root;
	root.id = "mention-root";
	root.provider_id = app.provider_profiles.front().id;
	root.workspace_directory = workspace.string();
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	app.chats.push_back(root);

	bool handled = true;
	std::string error;
	UAM_ASSERT(uam::AgentRunScheduler::TryEnqueueMention(
	    app, root.id, "Please ask @reviewer about this.", &handled, nullptr, &error));
	UAM_ASSERT(!handled);
	UAM_ASSERT(uam::AgentRunScheduler::TryEnqueueMention(
	    app, root.id, "@unknown keep this ordinary", &handled, nullptr, &error));
	UAM_ASSERT(!handled);
	UAM_ASSERT(uam::AgentRunScheduler::TryEnqueueMention(
	    app, root.id, "@@reviewer is ordinary text", &handled, nullptr, &error));
	UAM_ASSERT(!handled);
	UAM_ASSERT(!uam::AgentRunScheduler::TryEnqueueMention(
	    app, root.id, "@reviewer   ", &handled, nullptr, &error));
	UAM_ASSERT(handled);
	UAM_ASSERT(error.find("Add a task") != std::string::npos);

	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const std::optional<ChatSession> cold_parent = ChatRepository::LoadLocalChat(app.data_root, root.id, false);
	UAM_ASSERT(cold_parent.has_value());
	app.chats.front() = *cold_parent;
	UAM_ASSERT(!app.chats.front().messages_loaded);
	std::string run_id;
	UAM_ASSERT(uam::AgentRunScheduler::TryEnqueueMention(
	    app, root.id, "@reviewer inspect the lifecycle", &handled, &run_id, &error));
	UAM_ASSERT(handled);
	UAM_ASSERT(!run_id.empty());
	UAM_ASSERT_EQ(app.agent_runs.back().task, std::string("inspect the lifecycle"));
	UAM_ASSERT_EQ(app.chats.front().messages.back().content,
	              std::string("@reviewer inspect the lifecycle"));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(app.data_root);
	const std::vector<ChatSession>::const_iterator root_summary = std::ranges::find(summaries, root.id, &ChatSession::id);
	UAM_ASSERT(root_summary != summaries.end());
	app.chats.front() = *root_summary;
	UAM_ASSERT(!app.chats.front().messages_loaded && app.chats.front().messages.empty());
	UAM_ASSERT(uam::AgentRunScheduler::CancelTree(app, run_id, &error));
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 1000));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("@reviewer inspect the lifecycle"));
	const ToolCall& delivered = app.chats.front().messages.back().tool_calls.front();
	UAM_ASSERT_EQ(delivered.id, std::string("uam-agent-result-") + run_id);
	UAM_ASSERT(delivered.is_sub_agent);
	const nlohmann::json result = nlohmann::json::parse(delivered.result_text);
	UAM_ASSERT_EQ(result.value("status", ""), std::string("cancelled"));
	UAM_ASSERT(!result.value("transcriptChatId", "").empty());
	UAM_ASSERT(app.agent_runs.back().root_result_delivered);
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(app, 1001));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(2));
	// Retry delivery after a sidebar reload and a lost delivered-ledger update.
	app.agent_runs.back().root_result_delivered = false;
	app.agent_runs.back().root_result_delivery_attempts = 0;
	summaries = ChatRepository::LoadLocalChatSummaries(app.data_root);
	const std::vector<ChatSession>::const_iterator delivered_summary = std::ranges::find(summaries, root.id, &ChatSession::id);
	UAM_ASSERT(delivered_summary != summaries.end());
	app.chats.front() = *delivered_summary;
	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 1002));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, root.id);
	UAM_ASSERT(saved && saved->messages.size() == 2);
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("@reviewer inspect the lifecycle"));
	UAM_ASSERT_EQ(saved->messages.back().tool_calls.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved->messages.back().tool_calls.front().id, std::string("uam-agent-result-") + run_id);
	const auto durable_runs = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	const auto durable_run = std::ranges::find(durable_runs, run_id, &AgentRun::id);
	UAM_ASSERT(durable_run != durable_runs.end());
	UAM_ASSERT(durable_run->root_result_delivered);
}

UAM_TEST(AgentRunSchedulerResumesInterruptedRunsAsNewDurableContinuations)
{
	TempDir temp("uam-agent-resume-continuation");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession root;
	root.id = "resume-root";
	root.provider_id = app.provider_profiles.front().id;
	root.folder_id = "homelab-workspace";
	root.execution_host_id = "ssh-homelab";
	root.workspace_directory = "/opt/containers/project";
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	app.folders.push_back({root.folder_id, "Homelab workspace", root.workspace_directory,
	                       false, root.execution_host_id});
	app.chats.push_back(root);

	AgentRun old;
	old.id = uam::AgentRunLedger::NewRunId();
	old.root_chat_id = root.id;
	old.transcript_chat_id = "interrupted-transcript";
	old.agent_id = "reviewer";
	old.definition_hash = "snapshot";
	old.definition_snapshot = "snapshot";
	old.definition_instructions = "Review carefully.";
	old.provider_id = root.provider_id;
	old.execution_capability =
	    uam::AgentDefinitionService::ExecutionCapabilityForProvider(root.provider_id);
	old.task = "Original task.";
	old.effective_workspace_access = "read";
	old.status = "interrupted";
	old.created_at = root.created_at;
	old.updated_at = root.updated_at;
	old.finished_at = root.updated_at;
	old.result_delivery_id = old.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, old));
	app.agent_runs.push_back(old);

	ChatSession durable;
	durable.id = old.transcript_chat_id;
	durable.provider_id = old.provider_id;
	durable.folder_id = root.folder_id;
	durable.execution_host_id = "ssh-nas";
	durable.workspace_directory = "/stale/nas/path";
	durable.agent_run_id = old.id;
	durable.created_at = root.created_at;
	durable.updated_at = root.updated_at;
	Message partial;
	partial.role = MessageRole::Assistant;
	partial.content = "Completed the first half.";
	partial.created_at = root.created_at;
	durable.messages.push_back(partial);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, durable));

	std::string resumed_id;
	std::string error;
	UAM_ASSERT(uam::AgentRunScheduler::ResumeInterrupted(
	    app, old.id, &resumed_id, &error));
	UAM_ASSERT(resumed_id != old.id);
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("interrupted"));
	const AgentRun& resumed = app.agent_runs.back();
	UAM_ASSERT_EQ(resumed.id, resumed_id);
	UAM_ASSERT_EQ(resumed.resumed_from_run_id, old.id);
	UAM_ASSERT_EQ(resumed.status, std::string("queued"));
	UAM_ASSERT(resumed.task.find("Continue the interrupted") != std::string::npos);
	UAM_ASSERT_EQ(app.chats.back().messages.back().content,
	              std::string("Completed the first half."));
	UAM_ASSERT_EQ(app.chats.back().folder_id, root.folder_id);
	UAM_ASSERT_EQ(app.chats.back().execution_host_id, root.execution_host_id);
	UAM_ASSERT_EQ(app.chats.back().workspace_directory, root.workspace_directory);
	UAM_ASSERT(app.chats.back().native_session_id.empty());
	UAM_ASSERT_EQ(app.queued_agent_run_ids.back(), resumed_id);
	UAM_ASSERT(!uam::AgentRunScheduler::ResumeInterrupted(
	    app, old.id, nullptr, &error));
	const uam::AgentRunLoadResult loaded = uam::AgentRunLedger::LoadAll(app.data_root);
	const auto persisted = std::ranges::find(loaded.runs, resumed_id, &AgentRun::id);
	UAM_ASSERT(persisted != loaded.runs.end());
	UAM_ASSERT_EQ(persisted->resumed_from_run_id, old.id);
}

UAM_TEST(AgentRunSchedulerResumesCachedTranscriptsWithoutLosingColdOrUnsavedHistory)
{
	for (const int mode : {0, 1, 2})
	{
		const bool cold = mode != 1;
		TempDir temp("uam-agent-resume-cached-history");
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession root;
		root.id = "resume-root";
		root.provider_id = app.provider_profiles.front().id;
		root.created_at = uam::time::TimestampNow();
		root.updated_at = root.created_at;
		app.chats.push_back(root);
		AgentRun interrupted;
		interrupted.id = uam::AgentRunLedger::NewRunId();
		interrupted.root_chat_id = root.id;
		interrupted.transcript_chat_id = "cached-transcript";
		interrupted.provider_id = root.provider_id;
		interrupted.agent_id = "reviewer";
		interrupted.definition_snapshot = "Review the remaining work.";
		interrupted.task = "Original task.";
		interrupted.effective_workspace_access = "read";
		interrupted.created_at = root.created_at;
		interrupted.updated_at = root.updated_at;
		interrupted.status = "interrupted";
		UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, interrupted));
		app.agent_runs.push_back(interrupted);
		ChatSession transcript;
		transcript.id = interrupted.transcript_chat_id;
		transcript.provider_id = root.provider_id;
		transcript.agent_run_id = interrupted.id;
		transcript.created_at = root.created_at;
		transcript.updated_at = root.updated_at;
		Message partial;
		partial.role = MessageRole::Assistant;
		partial.content = "Already completed work.";
		transcript.messages.push_back(partial);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, transcript));
		if (cold)
		{
			transcript.messages.clear();
			transcript.messages_loaded = false;
			transcript.persisted_message_count = 1;
		}
		else
		{
			transcript.messages.front().content = "Authoritative unsaved progress.";
		}
		app.chats.push_back(transcript);
		std::string resumed_id;
		std::string error;
		if (mode == 2)
		{
			UAM_ASSERT(fs::remove(AppPaths::UamChatFilePath(app.data_root, transcript.id)));
			UAM_ASSERT(!uam::AgentRunScheduler::ResumeInterrupted(app, interrupted.id, &resumed_id, &error));
			UAM_ASSERT(!error.empty());
			UAM_ASSERT(resumed_id.empty());
			UAM_ASSERT_EQ(app.agent_runs.size(), static_cast<std::size_t>(1));
			UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
			UAM_ASSERT(app.queued_agent_run_ids.empty());
			continue;
		}
		UAM_ASSERT(uam::AgentRunScheduler::ResumeInterrupted(app, interrupted.id, &resumed_id, &error));
		UAM_ASSERT_EQ(app.chats[1].messages_loaded, !cold);
		UAM_ASSERT_EQ(app.chats.back().messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(app.chats.back().messages.front().content,
		    cold ? std::string("Already completed work.") : std::string("Authoritative unsaved progress."));
		std::string load_warning;
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.back().id, true, &load_warning);
		if (!saved) throw std::runtime_error("Fresh continuation reload failed: " + load_warning);
		UAM_ASSERT_EQ(saved->messages.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(saved->messages.front().content, app.chats.back().messages.front().content);
	}
}

UAM_TEST(CodexAndClaudeGenericAgentRunsCancelAndReturnDurableResults)
{
	TempDir temp("uam-generic-agent-cancel-result");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "reviewer.md",
	    "---\nversion: 1\nname: reviewer\ndescription: Reviewer\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nReview.\n"));
	const std::vector<ProviderProfile> profiles = ProviderProfileStore::BuiltInProfiles();
	for (const char* provider : {uam::provider_ids::kCodexCli,
	                             uam::provider_ids::kClaudeCli})
	{
		uam::AppState app;
		app.data_root = temp.root / provider;
		app.provider_profiles = profiles;
		ChatSession root;
		root.id = std::string("root-") + provider;
		root.provider_id = provider;
		root.workspace_directory = workspace.string();
		root.created_at = uam::time::TimestampNow();
		root.updated_at = root.created_at;
		app.chats.push_back(root);
		std::string run_id;
		std::string error;
		UAM_ASSERT(uam::AgentRunScheduler::Enqueue(
		    app, root.id, {}, "reviewer", "Review this.", &run_id, &error));
		UAM_ASSERT_EQ(app.agent_runs.back().execution_capability,
		              std::string("uam-prompt-injected"));
		UAM_ASSERT(uam::AgentRunScheduler::CancelTree(app, run_id, &error));
		uam::AgentRunResult result;
		UAM_ASSERT(uam::AgentRunScheduler::GetResultForParent(
		    app, run_id, root.id, {}, &result, &error));
		UAM_ASSERT_EQ(result.delivery_id, run_id);
		UAM_ASSERT_EQ(result.status, std::string("cancelled"));
		UAM_ASSERT_EQ(result.diagnostic_code, std::string("user_cancelled"));
		const auto loaded = uam::AgentRunLedger::LoadAll(app.data_root);
		const auto saved = std::ranges::find(loaded.runs, run_id, &AgentRun::id);
		UAM_ASSERT(saved != loaded.runs.end());
		UAM_ASSERT_EQ(saved->status, std::string("cancelled"));
	}
}

UAM_TEST(AgentRunSchedulerTripsAProviderCrashCircuitBeforeLaunchingMoreChildren)
{
	TempDir temp("uam-agent-provider-crash-circuit");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	UAM_ASSERT(uam::io::WriteTextFile(agents / "reviewer.md",
	    "---\nversion: 1\nname: reviewer\ndescription: Reviewer\nmode: subagent\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nReview.\n"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession root;
	root.id = "root-chat";
	root.provider_id = uam::provider_ids::kOpenCodeCli;
	root.workspace_directory = workspace.string();
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	app.chats.push_back(root);

	std::string queued_id;
	std::string error;
	UAM_ASSERT(uam::AgentRunScheduler::Enqueue(
	    app, root.id, "", "reviewer", "Queued after crashes.", &queued_id, &error,
	    uam::provider_ids::kOpenCodeCli));
	for (int index = 0; index < 3; ++index)
	{
		AgentRun crashed;
		crashed.id = uam::AgentRunLedger::NewRunId();
		crashed.root_chat_id = root.id;
		crashed.transcript_chat_id = "crashed-transcript-" + std::to_string(index);
		crashed.agent_id = "reviewer";
		crashed.definition_hash = "snapshot";
		crashed.definition_snapshot = "snapshot";
		crashed.provider_id = uam::provider_ids::kOpenCodeCli;
		crashed.task = "Crash.";
		crashed.effective_workspace_access = "read";
		crashed.status = "running";
		crashed.created_at = root.created_at;
		crashed.updated_at = root.updated_at;
		UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, crashed));
		app.agent_runs.push_back(crashed);

		ChatSession transcript;
		transcript.id = crashed.transcript_chat_id;
		transcript.provider_id = crashed.provider_id;
		transcript.agent_run_id = crashed.id;
		app.chats.push_back(transcript);
		auto session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = crashed.transcript_chat_id;
		session->provider_id = crashed.provider_id;
		session->managed_agent_run_id = crashed.id;
		session->managed_launch_attempted = true;
		app.acp_sessions.push_back(std::move(session));
	}

	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 100000));
	const auto queued = std::ranges::find(app.agent_runs, queued_id, &AgentRun::id);
	UAM_ASSERT(queued != app.agent_runs.end());
	UAM_ASSERT_EQ(queued->status, std::string("failed"));
	UAM_ASSERT_EQ(queued->diagnostic_code, std::string("provider_crash_circuit_open"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.acp_sessions.empty());
}

UAM_TEST(UamControlDelegationUsesCapabilityAndRunSnapshotsAcrossProviders)
{
	TempDir temp("uam-control-agent-snapshots");
	const fs::path workspace = temp.root / "workspace";
	const fs::path agents = workspace / ".uam" / "agents";
	fs::create_directories(agents);
	const fs::path parent_path = agents / "parent.md";
	UAM_ASSERT(uam::io::WriteTextFile(parent_path,
	    "---\nversion: 1\nname: parent\ndescription: Parent\nmode: both\nworkspaceAccess: read\nskills: []\ndelegates: [worker]\n---\nDelegate.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(agents / "worker.md",
	    "---\nversion: 1\nname: worker\ndescription: Worker\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nWork.\n"));
	UAM_ASSERT(uam::io::WriteTextFile(agents / "other.md",
	    "---\nversion: 1\nname: other\ndescription: Other\nmode: subagent\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nOther.\n"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(!app.provider_profiles.empty());
	ChatSession root;
	root.id = "root-chat";
	root.provider_id = app.provider_profiles.front().id;
	root.workspace_directory = workspace.string();
	root.uam_control_enabled = true;
	root.command_safety_tier = "off";
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	app.chats.push_back(root);

	const auto catalog = uam::AgentDefinitionService::Load(app.data_root, workspace);
	const auto definition = std::ranges::find(catalog.definitions, std::string("parent"), &uam::AgentDefinition::id);
	UAM_ASSERT(definition != catalog.definitions.end());
	AgentRun parent;
	parent.id = uam::AgentRunLedger::NewRunId();
	parent.root_chat_id = root.id;
	parent.transcript_chat_id = "parent-transcript";
	parent.agent_id = "parent";
	parent.definition_hash = definition->definition_hash;
	parent.definition_snapshot = definition->markdown_snapshot;
	parent.definition_instructions = definition->instructions;
	parent.skills_snapshot = definition->skills;
	parent.delegates_snapshot = definition->delegates;
	parent.provider_id = root.provider_id;
	parent.execution_capability = "uam-prompt-injected";
	parent.task = "Parent task.";
	parent.effective_workspace_access = "read";
	parent.status = "running";
	parent.created_at = root.created_at;
	parent.updated_at = root.updated_at;
	app.agent_runs.push_back(parent);

	const fs::path capability_directory = temp.root / "capability";
	fs::create_directories(capability_directory);
	uam::UamControlCapability capability;
	capability.id = "capability-id";
	capability.directory = capability_directory;
	capability.chat_id = root.id;
	capability.session_chat_id = parent.transcript_chat_id;
	capability.provider_id = parent.provider_id;
	capability.agent_id = parent.agent_id;
	capability.agent_run_id = parent.id;
	capability.agent_delegates = parent.delegates_snapshot;
	capability.expires_at_epoch_ms = 100000;
	app.uam_control_capabilities.push_back(capability);
	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = capability.session_chat_id;
	session->provider_id = capability.provider_id;
	session->running = true;
	session->uam_control_capability_id = capability.id;
	app.acp_sessions.push_back(std::move(session));

	UAM_ASSERT(uam::io::WriteTextFile(parent_path,
	    "---\nversion: 1\nname: parent\ndescription: Changed parent\nmode: both\nworkspaceAccess: write\nskills: []\ndelegates: [other]\n---\nChanged.\n"));
	const nlohmann::json listed = uam::UamControlService::HandleRequestForTests(
	    app, capability.id, {{"requestId", "list-1"}, {"method", "agent_list"}, {"arguments", nlohmann::json::object()}}, 1000);
	UAM_ASSERT(listed.value("ok", false));
	UAM_ASSERT_EQ(listed["result"].size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(listed["result"][0].value("id", ""), std::string("worker"));

	const std::string child_provider = app.provider_profiles.back().id;
	const nlohmann::json rejected_delegate = uam::UamControlService::HandleRequestForTests(
	    app, capability.id,
	    {{"requestId", "delegate-rejected"}, {"method", "agent_delegate"},
	     {"arguments", {{"agentId", "other"}, {"task", "Must not start."}}}}, 1001);
	UAM_ASSERT(!rejected_delegate.value("ok", true));
	UAM_ASSERT(rejected_delegate.value("error", "").find("saved definition") != std::string::npos);
	UAM_ASSERT_EQ(app.agent_runs.size(), static_cast<std::size_t>(1));
	const nlohmann::json delegated = uam::UamControlService::HandleRequestForTests(
	    app, capability.id,
	    {{"requestId", "delegate-1"}, {"method", "agent_delegate"},
	     {"arguments", {{"agentId", "worker"}, {"task", "Do work."},
	                    {"providerId", child_provider}, {"modelId", "child-model"}}}}, 1002);
	UAM_ASSERT(delegated.value("ok", false));
	const std::string child_id = delegated["result"].value("runId", "");
	const auto child = std::ranges::find(app.agent_runs, child_id, &AgentRun::id);
	UAM_ASSERT(child != app.agent_runs.end());
	UAM_ASSERT_EQ(child->provider_id, child_provider);
	UAM_ASSERT_EQ(child->model_id, std::string("child-model"));
	UAM_ASSERT_EQ(child->effective_workspace_access, std::string("read"));
	const auto saved_child = ChatRepository::LoadLocalChat(app.data_root, child->transcript_chat_id);
	UAM_ASSERT(saved_child.has_value());
	UAM_ASSERT_EQ(saved_child->approval_mode, std::string("plan"));
	UAM_ASSERT_EQ(saved_child->command_safety_tier, std::string("off"));

	const nlohmann::json status = uam::UamControlService::HandleRequestForTests(
	    app, capability.id,
	    {{"requestId", "status-1"}, {"method", "agent_status"}, {"arguments", {{"runId", child_id}}}}, 1003);
	UAM_ASSERT(status.value("ok", false));
	UAM_ASSERT_EQ(status["result"].value("executionCapability", ""),
	              uam::AgentDefinitionService::ExecutionCapabilityForProvider(child_provider));
	UAM_ASSERT_EQ(status["result"].value("definitionHash", ""), child->definition_hash);
	UAM_ASSERT_EQ(status["result"].value("transcriptChatId", ""), child->transcript_chat_id);
	app.chats.front().command_safety_tier = "off";
	const nlohmann::json rejected_cancel = uam::UamControlService::HandleRequestForTests(
	    app, capability.id,
	    {{"requestId", "cancel-rejected"}, {"method", "agent_cancel"},
	     {"arguments", {{"runId", child_id}}}}, 1004);
	UAM_ASSERT(!rejected_cancel.value("ok", true));
	UAM_ASSERT(std::ranges::find(app.agent_runs, child_id, &AgentRun::id)->status != "cancelled");
}

UAM_TEST(AgentRunLedgerRecoversBackupAndInterruptsWithoutRelaunch)
{
	TempDir temp("uam-agent-run-ledger");
	AgentRun queued;
	queued.id = uam::AgentRunLedger::NewRunId();
	queued.root_chat_id = "chat-root";
	queued.agent_id = "reviewer";
	queued.definition_hash = "fnv1a64:1234";
	queued.definition_snapshot = "---\nversion: 1\n---\nReview.";
	queued.provider_id = "opencode-cli";
	queued.task = "Review the change.";
	queued.effective_workspace_access = "read";
	queued.status = "queued";
	queued.created_at = uam::time::TimestampNow();
	queued.updated_at = queued.created_at;
	std::string error;
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, queued, &error));

	AgentRun running = queued;
	running.id = uam::AgentRunLedger::NewRunId();
	running.status = "running";
	running.started_at = queued.created_at;
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, running, &error));
	// A second save creates a known-good retained backup.
	running.expected_turn_serial = 2;
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, running, &error));
	const fs::path running_path = temp.root / "agent-runs" / (running.id + ".json");
	const nlohmann::json valid_primary = nlohmann::json::parse(ReadFile(running_path));
	for (const std::string_view field : {"deliverResultToRootChat", "rootResultDelivered",
	                                     "rootResultDeliveryAttempts"})
	{
		nlohmann::json malformed = valid_primary;
		malformed[std::string(field)] = "wrong-type";
		UAM_ASSERT(uam::io::WriteTextFile(running_path, malformed.dump()));
		const uam::AgentRunLoadResult recovered_wrong_type = uam::AgentRunLedger::LoadAll(temp.root);
		const auto recovered_run = std::ranges::find(recovered_wrong_type.runs, running.id, &AgentRun::id);
		UAM_ASSERT(recovered_run != recovered_wrong_type.runs.end());
		UAM_ASSERT_EQ(recovered_run->expected_turn_serial, 0);
	}
	UAM_ASSERT(uam::io::WriteTextFile(running_path, "not-json"));

	uam::AgentRunLoadResult loaded = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT_EQ(loaded.runs.size(), static_cast<std::size_t>(2));
	const auto recovered = std::ranges::find(loaded.runs, running.id, &AgentRun::id);
	UAM_ASSERT(recovered != loaded.runs.end());
	UAM_ASSERT_EQ(recovered->expected_turn_serial, 0);
	UAM_ASSERT(uam::AgentRunLedger::MarkNonterminalInterrupted(
	    temp.root, &loaded.runs, "manager_restarted", &loaded.errors));
	UAM_ASSERT(std::ranges::all_of(loaded.runs, [](const AgentRun& run) {
		return run.status == "interrupted" && run.diagnostic_code == "manager_restarted";
	}));

	const uam::AgentRunLoadResult restarted = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT_EQ(restarted.runs.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(std::ranges::all_of(restarted.runs, [](const AgentRun& run) {
		return run.status == "interrupted";
	}));
}

UAM_TEST(AgentRunSchedulerEnforcesWallClockDeadlineAndReturnsOneDurableBoundedResult)
{
	TempDir temp("uam-agent-run-deadline");
	uam::AppState app;
	app.data_root = temp.root / "data";

	ChatSession root;
	root.id = "root-chat";
	root.provider_id = "opencode-cli";
	root.created_at = uam::time::TimestampNow();
	root.updated_at = root.created_at;
	app.chats.push_back(root);

	AgentRun run;
	run.id = uam::AgentRunLedger::NewRunId();
	run.root_chat_id = root.id;
	run.transcript_chat_id = "managed-transcript";
	run.agent_id = "reviewer";
	run.definition_hash = "fnv1a64:1234";
	run.definition_snapshot = "---\nversion: 1\n---\nReview.";
	run.provider_id = root.provider_id;
	run.task = "Review until the deadline.";
	run.effective_workspace_access = "read";
	run.status = "running";
	run.depth = 1;
	run.expected_turn_serial = 1;
	run.started_at_epoch_ms = 1000;
	run.deadline_at_epoch_ms = 2000;
	run.created_at = root.created_at;
	run.started_at = root.created_at;
	run.updated_at = root.created_at;
	run.result_delivery_id = run.id;
	UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
	app.agent_runs.push_back(run);

	ChatSession transcript;
	transcript.id = run.transcript_chat_id;
	transcript.provider_id = run.provider_id;
	transcript.agent_run_id = run.id;
	transcript.created_at = root.created_at;
	transcript.updated_at = root.updated_at;
	Message partial;
	partial.role = MessageRole::Assistant;
	partial.content.assign(20U * 1024U, 'x');
	partial.created_at = root.created_at;
	transcript.messages.push_back(std::move(partial));
	app.chats.push_back(std::move(transcript));

	UAM_ASSERT(uam::AgentRunScheduler::PollAtForTests(app, 2000));
	UAM_ASSERT_EQ(app.agent_runs.front().status, std::string("failed"));
	UAM_ASSERT_EQ(app.agent_runs.front().diagnostic_code,
	              std::string("wall_clock_deadline_exceeded"));
	UAM_ASSERT_EQ(app.agent_runs.front().result_excerpt.size(),
	              static_cast<std::size_t>(16U * 1024U));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));

	uam::AgentRunResult first;
	uam::AgentRunResult repeated;
	std::string error;
	UAM_ASSERT(uam::AgentRunScheduler::GetResultForParent(
	    app, run.id, root.id, "", &first, &error));
	UAM_ASSERT(uam::AgentRunScheduler::GetResultForParent(
	    app, run.id, root.id, "", &repeated, &error));
	UAM_ASSERT_EQ(first.delivery_id, run.id);
	UAM_ASSERT_EQ(repeated.delivery_id, first.delivery_id);
	UAM_ASSERT_EQ(repeated.output, first.output);
	UAM_ASSERT(!uam::AgentRunScheduler::GetResultForParent(
	    app, run.id, "another-chat", "", nullptr, &error));

	uam::AppState restarted;
	restarted.agent_runs = uam::AgentRunLedger::LoadAll(app.data_root).runs;
	uam::AgentRunResult durable;
	UAM_ASSERT(uam::AgentRunScheduler::GetResultForParent(
	    restarted, run.id, root.id, "", &durable, &error));
	UAM_ASSERT_EQ(durable.delivery_id, first.delivery_id);
	UAM_ASSERT_EQ(durable.output, first.output);
	UAM_ASSERT(!uam::AgentRunScheduler::PollAtForTests(app, 3000));
}
