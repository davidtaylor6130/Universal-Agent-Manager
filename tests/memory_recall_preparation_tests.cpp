#include "test_harness.h"
#include "app/memory_service.h"
#include "common/runtime/acp/acp_session_internal.h"
using namespace uam_test;

UAM_TEST(MemoryRecallPreparationRejectsStaleOwnersBeforeStartupWait)
{
	TempDir temp("uam-recall-owner");
	const std::vector<std::function<void(uam::AppState&, uam::AcpSessionState&, ChatSession&)>> changes = {
		[](uam::AppState& app, uam::AcpSessionState&, ChatSession&) { app.data_root /= "changed"; },
		[](uam::AppState& app, uam::AcpSessionState&, ChatSession&) { app.settings.memory_recall_budget_bytes = 128; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.memory_enabled = false; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.memory_level = "off"; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.small_model_mode = true; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.id = "other-chat"; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.workspace_directory += "/other"; },
		[](uam::AppState&, uam::AcpSessionState&, ChatSession& chat) { chat.execution_host_id = "missing-remote"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.session_id = "other-session"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.provider_id = "codex-cli"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { ++session.turn_serial; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.queued_prompt += "changed"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.memory_recall_query += "changed"; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.memory_recall_position = 2; },
		[](uam::AppState&, uam::AcpSessionState& session, ChatSession&) { session.memory_recall_position.reset(); }
	};
	for (std::size_t index = 0; index < changes.size() * 2; ++index)
	{
		uam::AppState app;
		app.data_root = temp.root / "data";
		app.settings.memory_recall_budget_bytes = 2048;
		ChatSession chat;
		chat.id = "recall-owner";
		chat.workspace_directory = temp.root.string();
		chat.memory_level = "strict";
		uam::AcpSessionState session;
		session.provider_id = "gemini-cli";
		session.session_id = "native-owner";
		session.queued_prompt = "accepted prompt";
		session.memory_recall_position = 0;
		session.memory_recall_query = "accepted prompt";
		session.startup_model_request_id = 1;
		const std::shared_ptr<uam::MemoryRecallPreparation> state = std::make_shared<uam::MemoryRecallPreparation>();
		state->identity = {app.data_root, uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)),
			chat.id, session.provider_id, session.session_id, session.queued_prompt, session.memory_recall_query,
			0, session.turn_serial, 2048, "strict", false};
		state->context = "Obsolete memory";
		state->finished.store(index % 2 != 0, std::memory_order_release);
		session.memory_recall_preparation = std::make_unique<uam::MemoryRecallPreparationLease>(state);
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(!state->cancel.stop_requested());
		changes[index / 2](app, session, chat);
		UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT(session.memory_recall_preparation == nullptr);
		UAM_ASSERT(app.memory_recall_tasks.empty());
	}
	UAM_ASSERT(!fs::exists(temp.root / "data"));
}

UAM_TEST(MemoryRecallCancellationAndCompletionRetainWorkerOwnership)
{
	const std::shared_ptr<uam::MemoryRecallPreparation> state = std::make_shared<uam::MemoryRecallPreparation>();
	uam::AppState app;
	uam::MemoryRecallTask task;
	task.state = state;
	app.memory_recall_tasks.push_back(std::move(task));
	uam::AcpSessionState session;
	session.memory_recall_position = 0;
	session.memory_recall_query = "accepted query";
	session.queued_prompt = "accepted prompt";
	session.memory_recall_preparation = std::make_unique<uam::MemoryRecallPreparationLease>(state);
	uam::acp_detail::CompletePromptTurn(session, "stopped");
	UAM_ASSERT(state->cancel.stop_requested());
	UAM_ASSERT(!session.memory_recall_position.has_value());
	UAM_ASSERT(session.memory_recall_query.empty());
	UAM_ASSERT(session.memory_recall_preparation == nullptr);
	(void)uam::PollAllAcpSessions(app);
	UAM_ASSERT_EQ(app.memory_recall_tasks.size(), std::size_t(1));
	state->finished.store(true, std::memory_order_release);
	(void)uam::PollAllAcpSessions(app);
	UAM_ASSERT(app.memory_recall_tasks.empty());
	std::stop_source canceled;
	canceled.request_stop();
	ChatSession chat;
	UAM_ASSERT(MemoryService::BuildRecallPreface(app, chat, "query", canceled.get_token()).empty());
}

UAM_TEST(ClaudeImageDeliveryIncludesWorkerRecallAtOriginalPromptPosition)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && !defined(_WIN32)
	TempDir temp("uam-recall-image-delivery");
	const fs::path workspace = temp.root / "workspace";
	fs::create_directories(workspace);
	std::string image;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", image));
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "image.png", image));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.memory_recall_budget_bytes = 2048;
	ChatSession chat;
	chat.id = "owned-recall-image";
	chat.provider_id = "claude-cli";
	chat.workspace_directory = workspace.string();
	chat.memory_level = "strict";
	chat.approval_mode = "default";
	Message user;
	user.role = MessageRole::User;
	user.content = "Inspect image";
	MessageAttachment attachment;
	attachment.kind = "image";
	attachment.mime_type = "image/png";
	attachment.path = "image.png";
	user.attachments.push_back(attachment);
	chat.messages.push_back(user);
	const fs::path category = MemoryService::CategoryPath(MemoryService::LocalMemoryRoot(workspace), "Lessons/User_Lessons");
	fs::create_directories(category);
	UAM_ASSERT(uam::io::WriteTextFile(category / "preference.md", "## Memory\nUse the saved convention.\n"));
	const std::string expected_recall = MemoryService::BuildRecallPreface(app, chat, "Inspect image");
	UAM_ASSERT(!expected_recall.empty());
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.session_id = "owned-session";
	session.current_mode_id = "default";
	session.running = true;
	session.processing = true;
	session.session_ready = true;
	session.turn_first_user_message_index = 0;
	session.turn_user_message_index = 0;
	const std::string header = "Agent instructions\n";
	session.queued_prompt = header + "Inspect image";
	session.memory_recall_query = "Inspect image";
	session.memory_recall_position = header.size();
	const fs::path wire = temp.root / "wire.jsonl";
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
	std::string error;
	UAM_ASSERT(process.StartStdioProcess(session, workspace, {"/bin/sh", "-c", "cat > '" + wire.string() + "'"}, &error));
	struct ProcessGuard
	{
		IPlatformProcessService& process;
		uam::AcpSessionState& session;
		~ProcessGuard() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
	} guard{process, session};
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(session.memory_recall_preparation != nullptr);
	UAM_ASSERT_EQ(session.queued_prompt, header + "Inspect image");
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool sent = false;
	while (!sent && std::chrono::steady_clock::now() < deadline)
	{
		sent = uam::acp_detail::SendQueuedPromptIfReady(app, session, chat);
		std::this_thread::yield();
	}
	UAM_ASSERT(sent);
	UAM_ASSERT(session.last_error.empty());
	std::string text;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (uam::io::TryReadTextFile(wire, text) && text.ends_with('\n')) break;
		std::this_thread::yield();
	}
	const nlohmann::json payload = nlohmann::json::parse(text);
	UAM_ASSERT_EQ(payload["message"]["content"][0]["text"], nlohmann::json(header + expected_recall + "Inspect image"));
	UAM_ASSERT_EQ(payload["message"]["content"][1]["type"], nlohmann::json("image"));
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(!session.memory_recall_position.has_value());
	UAM_ASSERT_EQ(chat.messages.front().content, std::string("Inspect image"));
#endif
}

UAM_TEST(AcpCancelReleasesPendingMemoryRecallWithoutTransport)
{
	TempDir temp("uam-recall-cancel");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "recall-cancel";
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);
	std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = chat.id;
	owned->queued_prompt = "accepted prompt";
	owned->memory_recall_query = "accepted query";
	owned->memory_recall_position = 0;
	const std::shared_ptr<uam::MemoryRecallPreparation> state = std::make_shared<uam::MemoryRecallPreparation>();
	owned->memory_recall_preparation = std::make_unique<uam::MemoryRecallPreparationLease>(state);
	app.acp_sessions.push_back(std::move(owned));
	std::string error;
	UAM_ASSERT(uam::CancelAcpTurn(app, chat.id, &error, false));
	UAM_ASSERT(state->cancel.stop_requested());
	UAM_ASSERT(app.acp_sessions.front()->memory_recall_preparation == nullptr);
	UAM_ASSERT(!app.acp_sessions.front()->memory_recall_position.has_value());
	UAM_ASSERT(app.acp_sessions.front()->memory_recall_query.empty());
	UAM_ASSERT(app.acp_sessions.front()->queued_prompt.empty());
}

UAM_TEST(MemoryRecallCapacityAndFailureNeverSendUnpreparedInput)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-recall-capacity");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.memory_recall_budget_bytes = 2048;
	ChatSession chat;
	chat.id = "capacity-owner";
	chat.provider_id = "claude-cli";
	chat.workspace_directory = temp.root.string();
	chat.approval_mode = "default";
	chat.memory_level = "strict";
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.session_id = "owned-session";
	session.current_mode_id = "default";
	session.running = true;
	session.session_ready = true;
	session.processing = true;
	session.queued_prompt = "accepted input";
	session.memory_recall_query = "accepted input";
	session.memory_recall_position = 0;
	for (int index = 0; index < 2; ++index)
	{
		uam::MemoryRecallTask task;
		task.state = std::make_shared<uam::MemoryRecallPreparation>();
		app.memory_recall_tasks.push_back(std::move(task));
	}
	UAM_ASSERT(!uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(session.memory_recall_preparation == nullptr);
	UAM_ASSERT_EQ(app.memory_recall_tasks.size(), std::size_t(2));
	UAM_ASSERT_EQ(session.queued_prompt, std::string("accepted input"));
	const std::shared_ptr<uam::MemoryRecallPreparation> failed = std::make_shared<uam::MemoryRecallPreparation>();
	failed->identity = {app.data_root, uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)),
		chat.id, session.provider_id, session.session_id, session.queued_prompt, session.memory_recall_query,
		0, session.turn_serial, 2048, "strict", false};
	failed->context = "Never deliver partial recall";
	failed->error = "Owned fixture preparation failure";
	failed->finished.store(true, std::memory_order_release);
	session.memory_recall_preparation = std::make_unique<uam::MemoryRecallPreparationLease>(failed);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("error"));
	UAM_ASSERT_EQ(session.last_error, failed->error);
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(failed->cancel.stop_requested());
	UAM_ASSERT(!fs::exists(app.data_root));
#endif
}
