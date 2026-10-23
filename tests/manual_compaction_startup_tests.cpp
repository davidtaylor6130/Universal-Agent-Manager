#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_polling.h"

#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace uam_test;

namespace
{
	class CompactionTransport
	{
	public:
		explicit CompactionTransport(uam::AcpSessionState& session) : m_session(session), m_file(std::tmpfile())
		{
			UAM_ASSERT(m_file != nullptr);
#if defined(_WIN32)
			m_session.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(m_file)));
			UAM_ASSERT(CreatePipe(&m_session.stdout_read, &m_stdout_write, nullptr, 0));
#else
			m_session.stdin_write_fd = fileno(m_file);
			int descriptors[2];
			UAM_ASSERT_EQ(pipe(descriptors), 0);
			m_session.stdout_read_fd = descriptors[0];
			m_stdout_write = descriptors[1];
			UAM_ASSERT(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) >= 0);
#endif
			m_session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
			    [this](const char* bytes, std::size_t count, std::string&) -> std::ptrdiff_t
			    { captured.append(bytes, count); return static_cast<std::ptrdiff_t>(count); }, [] {});
		}
		~CompactionTransport()
		{
			m_session.stdin_writer.reset();
#if defined(_WIN32)
			m_session.stdin_write = INVALID_HANDLE_VALUE;
			CloseHandle(m_session.stdout_read);
			CloseHandle(m_stdout_write);
			m_session.stdout_read = INVALID_HANDLE_VALUE;
#else
			m_session.stdin_write_fd = -1;
			close(m_session.stdout_read_fd);
			close(m_stdout_write);
			m_session.stdout_read_fd = -1;
#endif
			std::fclose(m_file);
		}
		std::string captured;
	private:
		uam::AcpSessionState& m_session;
		std::FILE* m_file;
#if defined(_WIN32)
		HANDLE m_stdout_write = INVALID_HANDLE_VALUE;
#else
		int m_stdout_write = -1;
#endif
	};
}

UAM_TEST(ManualCompactionAcceptsStartingSessionWithoutPromptDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-starting");
	uam::AppState app;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "compact-starting";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "local";
	chat.native_session_id = "existing-thread";
	Message message;
	message.content = "Preserved conversation";
	chat.messages.push_back(message);
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.running = true;
	session.session_ready = false;
	session.pending_request_methods[17] = "initialize";
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(session.queued_user_prompts.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t(1));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, message.content);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t(1));
#endif
}

UAM_TEST(ManualCompactionUnsupportedProviderDoesNotCreateRuntime)
{
	uam::AppState app;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-unsupported";
	chat.provider_id = "gemini-cli";
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(error.find("does not support") != std::string::npos);
	UAM_ASSERT(app.acp_sessions.empty());
	UAM_ASSERT(app.chats.front().messages.empty());
}

UAM_TEST(ManualCompactionDefersAbsentAndStoppedSessionsDuringOwnedVersionProbe)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (bool existing : {false, true})
	{
		TempDir temp("uam-compact-deferred");
		uam::AppState app;
		app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "compact-deferred";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.native_session_id = "preserved-thread";
		app.chats.push_back(chat);
		if (existing)
		{
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			app.acp_sessions.back()->chat_id = chat.id;
			app.acp_sessions.back()->provider_id = chat.provider_id;
		}
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_provider_id = chat.provider_id;
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT_EQ(app.acp_sessions.size(), std::size_t(1));
		UAM_ASSERT(!app.acp_sessions.front()->running);
		UAM_ASSERT(app.acp_sessions.front()->queued_prompt.empty());
		UAM_ASSERT(app.chats.front().messages.empty());
		UAM_ASSERT_EQ(app.chats.front().native_session_id, chat.native_session_id);
		UAM_ASSERT(uam::StopAcpSession(app, chat.id));
		UAM_ASSERT(!app.chats.front().acp_queue_held);
	}
#endif
}

UAM_TEST(ManualCompactionDeferredOwnershipChangesRejectWithoutPromptDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (int mutation = 0; mutation < 5; ++mutation)
	{
		TempDir temp("uam-compact-owned");
		uam::AppState app;
		app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "compact-owned";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.native_session_id = "owned-thread";
		app.chats.push_back(chat);
		app.runtime_cli_version_check_task.running = true;
		app.runtime_cli_version_provider_id = chat.provider_id;
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
		if (mutation == 0) app.chats.front().native_session_id = "foreign-thread";
		if (mutation == 1) app.chats.front().model_id = "foreign-model";
		if (mutation == 2) app.chats.front().workspace_directory = "/foreign-workspace";
		if (mutation == 3) app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (mutation == 4) app.data_root = temp.root / "foreign-root";
		UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT(error.find("ownership") != std::string::npos);
		UAM_ASSERT(!app.acp_sessions.front()->running);
		UAM_ASSERT(app.acp_sessions.front()->queued_prompt.empty());
		UAM_ASSERT(app.chats.front().messages.empty());
		UAM_ASSERT(!app.chats.front().acp_queue_held);
	}
#endif
}

UAM_TEST(ManualCompactionStartingIntentDispatchesOnceAfterReady)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-ready");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-ready";
	chat.provider_id = "codex-cli";
	chat.native_session_id = "thread-ready";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.running = true;
	session.session_ready = false;
	CompactionTransport transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(transport.captured.empty());
	session.session_ready = true;
	session.session_id = chat.native_session_id;
	session.codex_thread_id = chat.native_session_id;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	const nlohmann::json request = nlohmann::json::parse(transport.captured);
	UAM_ASSERT_EQ(request["method"].get<std::string>(), std::string("thread/compact/start"));
	UAM_ASSERT_EQ(request["params"]["threadId"].get<std::string>(), chat.native_session_id);
	const std::string sent = transport.captured;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1)));
	UAM_ASSERT_EQ(transport.captured, sent);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(session.queued_user_prompts.empty());
#endif
}

UAM_TEST(ManualCompactionPendingIntentBlocksNewPromptAdmission)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-compact-input-guard");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "compact-input-guard";
	chat.provider_id = "codex-cli";
	app.chats.push_back(chat);
	app.runtime_cli_version_check_task.running = true;
	app.runtime_cli_version_provider_id = chat.provider_id;
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	// A second admission gate prevents a real launch if the maintenance guard regresses.
	app.worktree_operation_chat_ids.insert(chat.id);
	UAM_ASSERT(!uam::SendAcpPrompt(app, chat.id, "Keep this input", &error));
	UAM_ASSERT(error.find("compaction") != std::string::npos);
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT(app.acp_sessions.front()->queued_user_prompts.empty());
	UAM_ASSERT(app.chats.front().messages.empty());
#endif
}

UAM_TEST(RemoteManualCompactionPersistsAndRestoresItsOwnedDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-remote-compact-receipt");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "remote-compact";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "remote-host";
	chat.native_session_id = "native-thread";
	chat.remote_process_exists = true;
	chat.remote_structured_protocol = "codex-app-server";
	chat.remote_process_control_token = "remote-compact-private-token";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	auto& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "codex-app-server";
	session.codex_thread_id = chat.native_session_id;
	session.session_id = chat.native_session_id;
	session.process_execution_host_id = chat.execution_host_id;
	session.remote_output_delivery_token = chat.remote_process_control_token;
	session.remote_runner_protocol_version = 3;
	session.running = session.session_ready = true;
	CompactionTransport transport(session);
	std::string error;
	UAM_ASSERT(uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(session.manual_compaction_pending && session.processing);
	UAM_ASSERT(app.chats.front().messages.empty());
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT(saved->remote_manual_compaction_pending && saved->remote_turn_reconnect_pending);
	UAM_ASSERT_EQ(saved->remote_pending_requests.size(), std::size_t{1});
	const auto& request = saved->remote_pending_requests.front();
	UAM_ASSERT_EQ(request.method, std::string("thread/compact/start"));
	UAM_ASSERT(!request.delivery_id.empty() && !request.payload.empty());
	UAM_ASSERT(session.stdin_writer->Flush(std::chrono::seconds(1), &error));
	UAM_ASSERT_EQ(transport.captured, uam::remote::BuildRemoteInputDeliveryLine(chat.remote_process_control_token, request.delivery_id, request.payload));
	uam::AcpSessionState recovered;
	recovered.provider_id = chat.provider_id;
	recovered.protocol_kind = "codex-app-server";
	recovered.process_execution_host_id = chat.execution_host_id;
	recovered.remote_output_delivery_token = chat.remote_process_control_token;
	recovered.session_id = chat.native_session_id;
	uam::acp_detail::RestoreRemoteAcpRequests(recovered, *saved);
	UAM_ASSERT(recovered.manual_compaction_pending && recovered.processing);
	UAM_ASSERT_EQ(recovered.turn_user_message_index, -1);
	UAM_ASSERT(!recovered.turn_checkpoint_eligible);
	UAM_ASSERT_EQ(recovered.pending_request_methods.at(request.request_id), request.method);
	uam::AcpSessionState wrong_host;
	wrong_host.provider_id = chat.provider_id;
	wrong_host.process_execution_host_id = "other-host";
	wrong_host.remote_output_delivery_token = chat.remote_process_control_token;
	uam::acp_detail::RestoreRemoteAcpRequests(wrong_host, *saved);
	UAM_ASSERT(!wrong_host.manual_compaction_pending && !wrong_host.processing);
#endif
}

UAM_TEST(RemoteManualCompactionRejectsUnsavedReceiptsBeforeDelivery)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-remote-compact-save-failure");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "blocked", "file, not data directory"));
	uam::AppState app;
	app.data_root = temp.root / "blocked";
	app.provider_profiles.push_back(ProviderProfileStore::DefaultCodexProfile());
	ChatSession chat;
	chat.id = "remote-compact-save-failure";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "remote-host";
	chat.remote_process_exists = true;
	chat.remote_process_control_token = "remote-compact-private-token";
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	auto& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "codex-app-server";
	session.codex_thread_id = "native-thread";
	session.process_execution_host_id = chat.execution_host_id;
	session.remote_output_delivery_token = chat.remote_process_control_token;
	session.remote_runner_protocol_version = 3;
	session.running = session.session_ready = true;
	CompactionTransport transport(session);
	std::string error;
	UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
	UAM_ASSERT(error.find("receipt") != std::string::npos);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	UAM_ASSERT(app.chats.front().remote_pending_requests.empty());
	UAM_ASSERT(!app.chats.front().remote_manual_compaction_pending);
	UAM_ASSERT_EQ(session.stdin_writer->PendingBytes(), std::size_t{0});
	UAM_ASSERT(transport.captured.empty());
#endif
}

UAM_TEST(RemoteClaudeCompactionRecoveryRejectsMalformedBoundaryFields)
{
	uam::AcpSessionState session;
	session.provider_id = "claude-cli";
	session.protocol_kind = "claude-code-stream-json";
	session.session_id = "native-session";
	session.process_execution_host_id = "remote-host";
	session.remote_output_delivery_token = "remote-private-token";
	ChatSession chat;
	chat.provider_id = session.provider_id;
	chat.execution_host_id = session.process_execution_host_id;
	chat.remote_process_exists = chat.remote_manual_compaction_pending = true;
	chat.remote_process_control_token = session.remote_output_delivery_token;
	for (const auto& boundary : std::vector<nlohmann::json>{
	    {{"sessionId", 12}, {"summary", "invalid"}, {"identity", "boundary"}},
	    {{"sessionId", session.session_id}, {"summary", "invalid"}, {"identity", "boundary"}, {"tokensBefore", "wrong type"}},
	    {{"sessionId", session.session_id}, {"summary", "invalid"}, {"identity", "boundary"}, {"tokensAfter", nlohmann::json::object()}}})
	{
		chat.remote_manual_compaction_boundary_json = boundary.dump();
		uam::acp_detail::RestoreRemoteAcpRequests(session, chat);
		UAM_ASSERT(!session.claude_manual_compaction_boundary_seen);
		UAM_ASSERT(session.turn_events.empty());
	}
}

UAM_TEST(RemoteRecoveredCompactionTerminalResponseRetiresMaintenance)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-recovered-compact-settle");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "recovered-compact";
	chat.provider_id = "codex-cli";
	chat.execution_host_id = "remote-host";
	chat.remote_process_exists = chat.remote_manual_compaction_pending = chat.remote_turn_reconnect_pending = true;
	chat.remote_process_control_token = "recovered-private-token";
	chat.remote_manual_compaction_boundary_json = "{}";
	uam::AcpRemotePendingRequestState request;
	request.request_id = 42;
	request.method = "thread/compact/start";
	request.delivery_id = "owned-compact-delivery";
	chat.remote_pending_requests.push_back(request);
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "codex-app-server";
	session.process_execution_host_id = chat.execution_host_id;
	session.remote_output_delivery_token = chat.remote_process_control_token;
	session.recovering_remote_turn = true;
	session.running = session.session_ready = true;
	uam::acp_detail::RestoreRemoteAcpRequests(session, app.chats.front());
	UAM_ASSERT(session.processing && session.manual_compaction_pending);
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(),
	    nlohmann::json{{"id", 42}, {"error", {{"code", -32000}, {"message", "Native compaction interrupted"}}}}.dump()));
	UAM_ASSERT(!session.processing && !session.manual_compaction_pending && !session.recovering_remote_turn);
	UAM_ASSERT(!app.chats.front().remote_manual_compaction_pending && !app.chats.front().remote_turn_reconnect_pending);
	UAM_ASSERT(app.chats.front().remote_manual_compaction_boundary_json.empty());
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(saved.has_value() && !saved->remote_manual_compaction_pending);
	UAM_ASSERT(saved->messages.empty());
#endif
}

UAM_TEST(ClaudeRemoteMaintenanceTerminalReceiptSurvivesFailedSaveAndPreAckRestart)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-terminal-receipt");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = {ProviderProfileStore::DefaultClaudeProfile()};
	ChatSession chat;
	chat.id = "claude-terminal-receipt"; chat.provider_id = "claude-cli";
	chat.native_session_id = "11111111-2222-4333-8444-555555555555";
	chat.execution_host_id = "remote-host"; chat.remote_process_exists = true;
	chat.remote_process_control_token = "private-owned-claude-process";
	chat.remote_manual_compaction_pending = chat.remote_turn_reconnect_pending = true;
	chat.active_goal_id = "existing-goal";
	chat.messages = {{MessageRole::User, "Existing request"}, {MessageRole::Assistant, "Existing answer"}};
	chat.remote_pending_requests.push_back({.request_id = 42, .method = "claude/compact", .delivery_id = "owned-maintenance-delivery", .payload = "native compact request"});
	app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	auto& session = *app.acp_sessions.front();
	session.chat_id = chat.id; session.provider_id = chat.provider_id; session.protocol_kind = "claude-code-stream-json";
	session.session_id = chat.native_session_id; session.process_execution_host_id = chat.execution_host_id;
	session.remote_output_delivery_token = chat.remote_process_control_token;
	session.running = session.initialized = session.session_ready = session.processing = session.manual_compaction_pending = true;
	session.pending_request_methods[42] = "claude/compact"; session.turn_serial = 7;
	CompactionTransport transport(session);
	const nlohmann::json boundary = {{"type", "system"}, {"subtype", "compact_boundary"}, {"session_id", session.session_id},
	    {"uuid", "22222222-3333-4444-8555-666666666666"}, {"compact_metadata", {{"trigger", "manual"}, {"pre_tokens", 80000}, {"post_tokens", 10000}}}};
	const nlohmann::json terminal = {{"type", "result"}, {"subtype", "success"}, {"session_id", session.session_id},
	    {"uuid", "33333333-4444-4555-8666-777777777777"}, {"result", "Compacted maintenance output"}};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), boundary.dump()));
	const auto prior = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(prior && prior->remote_manual_compaction_pending);
	const fs::path data_root = app.data_root;
	const fs::path blocked = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "not a directory"));
	app.data_root = blocked;
	session.pending_remote_output_ack_line = "ack\n"; session.pending_remote_stdout_cursor = 123;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), terminal.dump()));
	UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
	(void)uam::acp_detail::DrainStdout(app, session, app.chats.front(), nullptr);
	UAM_ASSERT(!session.pending_remote_output_ack_line.empty());
	UAM_ASSERT_EQ(app.chats.front().remote_delivered_stdout_cursor, std::uintmax_t{0});
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), boundary.dump()));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), terminal.dump()));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(session.last_settled_turn_serial, std::uint64_t{0});
	app.data_root = data_root;
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, app.chats.front()));
	const auto saved = ChatRepository::LoadLocalChat(data_root, chat.id);
	UAM_ASSERT(saved && !saved->remote_manual_compaction_pending);
	UAM_ASSERT_EQ(saved->remote_delivered_stdout_cursor, std::uintmax_t{0});
	const auto receipt = nlohmann::json::parse(saved->remote_manual_compaction_boundary_json);
	UAM_ASSERT_EQ(receipt["kind"], "claude_manual_compaction_terminal");
	UAM_ASSERT_EQ(receipt["resultId"], terminal["uuid"]);
	UAM_ASSERT_EQ(receipt["requestId"], 42);
	for (const std::string mismatch : {"provider", "session", "host", "control", "process", "missing-uuid"})
	{
		ChatSession foreign = *saved;
		foreign.id = "foreign-receipt-" + mismatch;
		if (mismatch == "provider") foreign.provider_id = "copilot-cli";
		if (mismatch == "session") foreign.native_session_id = "55555555-6666-4777-8888-999999999999";
		if (mismatch == "host") foreign.execution_host_id = "another-host";
		if (mismatch == "control") foreign.remote_process_control_token = "another-process-token";
		if (mismatch == "process") foreign.remote_process_exists = false;
		if (mismatch == "missing-uuid")
		{
			auto invalid = receipt; invalid.erase("resultId"); foreign.remote_manual_compaction_boundary_json = invalid.dump();
		}
		UAM_ASSERT(ChatRepository::SaveChat(data_root, foreign));
		const auto filtered = ChatRepository::LoadLocalChat(data_root, foreign.id);
		UAM_ASSERT(filtered && filtered->remote_manual_compaction_boundary_json.empty());
	}
	// A controller crash here restores the settled receipt before its output ACK.
	uam::AppState recovered_app;
	recovered_app.data_root = data_root; recovered_app.chats.push_back(*saved);
	uam::AcpSessionState recovered;
	recovered.chat_id = chat.id; recovered.provider_id = chat.provider_id; recovered.protocol_kind = session.protocol_kind;
	recovered.session_id = chat.native_session_id; recovered.process_execution_host_id = chat.execution_host_id;
	recovered.remote_output_delivery_token = chat.remote_process_control_token; recovered.running = recovered.session_ready = true;
	uam::acp_detail::RestoreRemoteAcpRequests(recovered, recovered_app.chats.front());
	UAM_ASSERT(!recovered.manual_compaction_pending && !recovered.processing);
	UAM_ASSERT(uam::ProcessAcpLineForTests(recovered_app, recovered, recovered_app.chats.front(), boundary.dump()));
	UAM_ASSERT(uam::ProcessAcpLineForTests(recovered_app, recovered, recovered_app.chats.front(), terminal.dump()));
	UAM_ASSERT_EQ(recovered_app.chats.front().messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(recovered_app.chats.front().active_goal_id, std::string("existing-goal"));
	UAM_ASSERT_EQ(recovered.last_settled_turn_serial, std::uint64_t{0});
	UAM_ASSERT_EQ(recovered.turn_serial, std::uint64_t{7});
	UAM_ASSERT(!recovered.turn_checkpoint_commit_pending && !recovered.turn_checkpoint_preflight_pending);
	(void)uam::acp_detail::DrainStdout(app, session, app.chats.front(), nullptr);
	UAM_ASSERT(session.pending_remote_output_ack_line.empty());
	const auto acknowledged = ChatRepository::LoadLocalChat(data_root, chat.id);
	UAM_ASSERT(acknowledged && acknowledged->remote_delivered_stdout_cursor == 123 && acknowledged->remote_pending_requests.empty());
	// Neither a fresh native UUID nor a missing UUID can match the retired result.
	for (const bool has_uuid : {true, false})
	{
		auto ordinary = terminal;
		if (has_uuid) ordinary["uuid"] = "44444444-5555-4666-8777-888888888888";
		else ordinary.erase("uuid");
		ordinary["result"] = has_uuid ? "Actual next response" : "Actual response without UUID";
		recovered.processing = true; ++recovered.turn_serial;
		recovered.turn_events.clear(); recovered.current_assistant_message_index = recovered.turn_assistant_message_index = -1;
		UAM_ASSERT(uam::ProcessAcpLineForTests(recovered_app, recovered, recovered_app.chats.front(), ordinary.dump()));
		UAM_ASSERT_EQ(recovered_app.chats.front().messages.back().content, ordinary["result"].get<std::string>());
	}
#endif
}

UAM_TEST(ClaudeRemoteCompactionWaitsForPriorOutputAndDeliveryReceipts)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (int pending = 0; pending < 3; ++pending)
	{
		uam::AppState app;
		app.provider_profiles = {ProviderProfileStore::DefaultClaudeProfile()};
		ChatSession chat; chat.id = "claude-pending-control"; chat.provider_id = "claude-cli"; chat.execution_host_id = "remote-host";
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		auto& session = *app.acp_sessions.front();
		session.chat_id = chat.id; session.provider_id = chat.provider_id; session.running = session.initialized = session.session_ready = true;
		if (pending == 0) app.chats.front().remote_pending_requests.push_back({.request_id = 42, .method = "claude/compact", .response_consumed = true});
		if (pending == 1) session.pending_remote_output_ack_line = "unacknowledged-output";
		if (pending == 2) session.pending_remote_source_exit_ack_line = "unacknowledged-source-exit";
		std::string error;
		UAM_ASSERT(!uam::CompactAcpSession(app, chat.id, &error));
		UAM_ASSERT(error.find("pending provider controls") != std::string::npos);
		UAM_ASSERT(!session.manual_compaction_pending && !session.processing);
		UAM_ASSERT(session.pending_request_methods.empty());
	}
#endif
}

UAM_TEST(RemoteOutputAcknowledgmentPreservesUnconfirmedNativeHttpDecisions)
{
	TempDir temp("uam-native-http-output-ack");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "native-http-ack";
	chat.provider_id = "opencode-cli";
	chat.execution_host_id = "remote-host";
	chat.remote_process_exists = true;
	chat.remote_process_control_token = "private-runner-token";
	chat.remote_interaction_responses.push_back({"\"ordinary-acp\"", "{\"result\":{}}"});
	chat.remote_interaction_responses.push_back({"\"opencode-native:per_child\"", "{\"result\":{}}"});
	uam::AcpSessionState session;
	session.running = true;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.pending_remote_output_ack_line = "ack\n";
	session.pending_remote_stdout_cursor = 123;
	CompactionTransport transport(session);
	(void)uam::acp_detail::DrainStdout(app, session, chat, nullptr);
	UAM_ASSERT(session.pending_remote_output_ack_line.empty());
	UAM_ASSERT_EQ(chat.remote_interaction_responses.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.remote_interaction_responses.front().request_id_json, std::string("\"opencode-native:per_child\""));
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->remote_interaction_responses.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->remote_delivered_stdout_cursor, std::uintmax_t{123});
}

namespace
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	enum class BranchReservationR69
	{
		Absent,
		Live,
		Expired
	};

	struct DeferredCompactionBranchFixtureR69
	{
		TempDir temp{"uam-expired-branch-compaction"};
		uam::AppState app;
		DeferredCompactionBranchFixtureR69()
		{
			app.data_root = temp.root;
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			app.settings.git_auto_worktree = false;
			ChatSession chat;
			chat.id = "expired-branch-compaction";
			chat.provider_id = "codex-cli";
			chat.execution_host_id = "local";
			chat.native_session_id = "retained-native-thread";
			chat.workspace_directory = temp.root.string();
			chat.model_id = "retained-model";
			chat.messages.push_back({MessageRole::User, "Preserve the existing authored row."});
			app.chats.push_back(std::move(chat));
			// Existing public maintenance fixture barrier: never complete this task.
			app.runtime_cli_version_check_task.running = true;
			app.runtime_cli_version_check_task.execution_host = uam::execution_hosts::LocalHost();
			app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
			app.runtime_cli_version_provider_id = "codex-cli";
		}
		std::shared_ptr<uam::BranchPreparationOwnership> Reserve(BranchReservationR69 condition)
		{
			if (condition == BranchReservationR69::Absent)
			{
				UAM_ASSERT(!app.branch_preparations.contains(app.chats.front().id));
				return {};
			}
			auto owner = ChatDomainService().BeginBranchPreparation(app, app.chats.front().id, "");
			UAM_ASSERT(owner && app.branch_preparations.at(app.chats.front().id).lock() == owner);
			if (condition == BranchReservationR69::Expired)
			{
				owner.reset(); // Release the sole real owner; never fabricate or erase a weak entry.
				UAM_ASSERT(app.branch_preparations.contains(app.chats.front().id));
				UAM_ASSERT(app.branch_preparations.at(app.chats.front().id).expired());
			}
			return owner;
		}
		void AssertInert()
		{
			UAM_ASSERT(app.runtime_cli_version_check_task.running);
			UAM_ASSERT(app.runtime_cli_version_check_task.state && !app.runtime_cli_version_check_task.state->completed.load());
			UAM_ASSERT(!app.runtime_cli_version_check_task.worker);
			UAM_ASSERT(app.runtime_cli_version_check_task.state->result.output.empty());
			UAM_ASSERT(app.runtime_cli_version_check_task.state->result.error.empty());
			UAM_ASSERT_EQ(app.runtime_cli_version_provider_id, std::string("codex-cli"));
			UAM_ASSERT(app.runtime_cli_version_check_queue.empty());
			UAM_ASSERT(app.cli_terminals.empty() && app.acp_process_stop_tasks.empty() && app.pending_acp_remote_stops.empty());
			for (const auto& owned : app.acp_sessions)
			{
				UAM_ASSERT(owned && !owned->running && !owned->processing && !owned->session_ready && !owned->stdin_writer);
				UAM_ASSERT(owned->last_process_id.empty() && owned->prompt_request_id == 0);
				UAM_ASSERT(owned->pending_request_methods.empty() && owned->queued_prompt.empty() && owned->queued_user_prompts.empty());
				UAM_ASSERT(owned->stdout_buffer.empty() && owned->stderr_buffer.empty());
#if defined(_WIN32)
				UAM_ASSERT(owned->process_info.dwProcessId == 0);
				UAM_ASSERT(owned->stdin_write == INVALID_HANDLE_VALUE && owned->stdout_read == INVALID_HANDLE_VALUE && owned->stderr_read == INVALID_HANDLE_VALUE);
#else
				UAM_ASSERT(owned->child_pid == -1 && owned->watchdog_pid == -1);
				UAM_ASSERT(owned->stdin_write_fd == -1 && owned->stdout_read_fd == -1 && owned->stderr_read_fd == -1);
#endif
			}
		}
	};
#endif
} // namespace

UAM_TEST(ManualCompactionExpiredBranchReservationAllowsDeferredAdmission)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	// Baseline and live-owner controls run before the intended expired-entry RED.
	for (const auto condition : {BranchReservationR69::Absent, BranchReservationR69::Live, BranchReservationR69::Expired})
	{
		DeferredCompactionBranchFixtureR69 fixture;
		const auto messages = fixture.app.chats.front().messages;
		const auto native_id = fixture.app.chats.front().native_session_id;
		const auto model = fixture.app.chats.front().model_id;
		const auto check = fixture.app.runtime_cli_version_check_task.state;
		auto owner = fixture.Reserve(condition);
		fixture.AssertInert();
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		std::string error;
		const bool accepted = uam::CompactAcpSession(fixture.app, fixture.app.chats.front().id, &error);
		UAM_ASSERT_EQ(accepted, condition != BranchReservationR69::Live); // Intended expired-entry RED: wrong public admission result.
		if (condition == BranchReservationR69::Live)
		{
			UAM_ASSERT(error.find("preparation") != std::string::npos);
			UAM_ASSERT(fixture.app.acp_sessions.empty());
			UAM_ASSERT(owner && !owner->cancellation.stop_requested());
		}
		else
		{
			UAM_ASSERT(error.empty());
			UAM_ASSERT_EQ(fixture.app.acp_sessions.size(), std::size_t{1});
			UAM_ASSERT(fixture.app.acp_sessions.front()->pending_compaction != nullptr);
			UAM_ASSERT_EQ(fixture.app.acp_sessions.front()->next_request_id, 1);
		}
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
		UAM_ASSERT_EQ(fixture.app.chats.front().native_session_id, native_id);
		UAM_ASSERT_EQ(fixture.app.chats.front().model_id, model);
		UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty() && !fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT(fixture.app.runtime_cli_version_check_task.state == check);
		fixture.AssertInert();
	}
#endif
}

UAM_TEST(ManualCompactionExpiredBranchReservationKeepsPendingIntent)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (const auto condition : {BranchReservationR69::Absent, BranchReservationR69::Live, BranchReservationR69::Expired})
	{
		DeferredCompactionBranchFixtureR69 fixture;
		std::string error;
		UAM_ASSERT(uam::CompactAcpSession(fixture.app, fixture.app.chats.front().id, &error));
		UAM_ASSERT(error.empty() && fixture.app.acp_sessions.size() == 1);
		auto& session = *fixture.app.acp_sessions.front();
		const auto pending = session.pending_compaction;
		UAM_ASSERT(pending);
		const auto messages = fixture.app.chats.front().messages;
		const auto native_id = fixture.app.chats.front().native_session_id;
		const auto model = fixture.app.chats.front().model_id;
		const auto check = fixture.app.runtime_cli_version_check_task.state;
		const int next_request = session.next_request_id;
		auto owner = fixture.Reserve(condition);
		fixture.AssertInert();
		const bool accepted = uam::CompactAcpSession(fixture.app, fixture.app.chats.front().id, &error);
		UAM_ASSERT_EQ(accepted, condition != BranchReservationR69::Live); // Intended expired-entry RED: a valid pending operation is rejected.
		if (condition == BranchReservationR69::Live)
		{
			UAM_ASSERT(!session.pending_compaction);
			UAM_ASSERT(error.find("ownership") != std::string::npos);
			UAM_ASSERT(owner && !owner->cancellation.stop_requested());
		}
		else
		{
			UAM_ASSERT(session.pending_compaction == pending);
			UAM_ASSERT(error.empty());
		}
		UAM_ASSERT_EQ(session.next_request_id, next_request);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
		UAM_ASSERT_EQ(fixture.app.chats.front().native_session_id, native_id);
		UAM_ASSERT_EQ(fixture.app.chats.front().model_id, model);
		UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.empty() && !fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT(fixture.app.runtime_cli_version_check_task.state == check);
		fixture.AssertInert();
	}
#endif
}
