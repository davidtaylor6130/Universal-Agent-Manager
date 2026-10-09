#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/acp/acp_polling.h"

#include <tuple>

using namespace uam_test;

UAM_TEST(ToolProjectionPreservesKindApprovalAndDetailsAcrossAllProviders)
{
	for (const std::string provider : {"claude-cli", "codex-cli", "opencode-cli", "gemini-cli", "copilot-cli"})
	{
		TempDir temp("uam-tool-projection");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "projection";
		chat.provider_id = provider;
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& session = *owned;
		session.chat_id = chat.id;
		session.provider_id = provider;
		session.tool_calls.push_back({.id = "read-1", .title = "README.md", .kind = "read", .status = "running", .content = "output", .args_json = "{}", .approval_status = "auto_approved", .task_id = "task-1", .child_activity = "Child output"});
		if (provider == "claude-cli") session.claude_live_background_task_ids.insert("task-1");
		app.acp_sessions.push_back(std::move(owned));
		uam::acp_detail::AppendToolTurnEventIfNeeded(session, "read-1");
		UAM_ASSERT(uam::acp_detail::SyncAcpToolCallsToAssistantMessage(app.chats.front(), session, true));
		const nlohmann::json acp_summary = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"];
		UAM_ASSERT_EQ(acp_summary["backgroundTaskCount"], provider == "claude-cli" ? nlohmann::json(1) : nlohmann::json(0));
		session.claude_live_background_task_ids.clear();
		UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["backgroundTaskCount"], nlohmann::json(0));
		const nlohmann::json live = uam::StateSerializer::Serialize(app)["chats"][0]["acpSession"]["toolCalls"][0];
		const nlohmann::json saved = uam::StateSerializer::SerializeSession(app.chats.front())["messages"][0]["toolCalls"][0];
		for (const std::string field : {"title", "kind", "status", "approvalStatus", "content"}) UAM_ASSERT_EQ(live[field], saved[field]);
		UAM_ASSERT_EQ(saved["kind"], nlohmann::json("read"));
		session.tool_calls.front().status = "completed";
		UAM_ASSERT(uam::acp_detail::SyncAcpToolCallsToAssistantMessage(app.chats.front(), session, true));
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, app.chats.front()));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(loaded->messages.front().tool_calls.front() == app.chats.front().messages.front().tool_calls.front());
		uam::AcpSessionState restored;
		uam::acp_detail::RestoreTurnEventsFromMessageBlocks(restored, loaded->messages.front());
		UAM_ASSERT_EQ(restored.tool_calls.front().kind, std::string("read"));
		UAM_ASSERT_EQ(restored.tool_calls.front().approval_status, std::string("auto_approved"));
		UAM_ASSERT_EQ(restored.tool_calls.front().child_activity, std::string("Child output"));
		const std::string digest = uam::StateSerializer::MessageDigest(*loaded);
		ChatSession changed = *loaded;
		changed.messages.front().tool_calls.front().kind = "execute";
		UAM_ASSERT(digest != uam::StateSerializer::MessageDigest(changed));
	}
}

UAM_TEST(AcpBackgroundTaskCountIncludesOnlyQueuedOrRunningRootChildren)
{
	TempDir temp("uam-background-task-count");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root;
	root.id = "root-chat";
	root.provider_id = "claude-cli";
	app.chats.push_back(root);
	ChatSession child;
	child.id = "child-transcript";
	child.provider_id = "claude-cli";
	app.chats.push_back(child);
	ChatSession codex_root;
	codex_root.id = "codex-root";
	codex_root.provider_id = "codex-cli";
	app.chats.push_back(codex_root);

	for (const auto& [id, root_id, transcript_id, status] : {
	         std::tuple{"queued-run", "root-chat", "child-transcript", "queued"},
	         std::tuple{"running-run", "root-chat", "child-transcript", "running"},
	         std::tuple{"codex-run", "codex-root", "", "queued"},
	         std::tuple{"unrelated-run", "other-root", "", "running"}})
	{
		AgentRun run;
		run.id = id;
		run.root_chat_id = root_id;
		run.transcript_chat_id = transcript_id;
		run.status = status;
		app.agent_runs.push_back(std::move(run));
	}

	auto owned = std::make_unique<uam::AcpSessionState>();
	owned->chat_id = root.id;
	owned->provider_id = root.provider_id;
	owned->claude_live_background_task_ids.insert("native-task");
	app.acp_sessions.push_back(std::move(owned));

	const auto summary_for = [&app](const std::string& chat_id)
	{
		const nlohmann::json state = uam::StateSerializer::Serialize(app);
		for (const nlohmann::json& chat_json : state["chats"])
			if (chat_json.value("id", std::string{}) == chat_id) return chat_json["acpSession"];
		return nlohmann::json{};
	};
	UAM_ASSERT_EQ(summary_for("root-chat")["backgroundTaskCount"], nlohmann::json(3));
	UAM_ASSERT_EQ(summary_for("child-transcript")["backgroundTaskCount"], nlohmann::json(0));
	UAM_ASSERT_EQ(summary_for("codex-root")["backgroundTaskCount"], nlohmann::json(1));

	for (AgentRun& run : app.agent_runs)
		if (run.status == "queued" || run.status == "running") run.status = "completed";
	UAM_ASSERT_EQ(summary_for("root-chat")["backgroundTaskCount"], nlohmann::json(1));
	app.acp_sessions.clear();
	UAM_ASSERT_EQ(summary_for("root-chat")["backgroundTaskCount"], nlohmann::json(0));
	for (AgentRun& run : app.agent_runs)
		if (run.root_chat_id == "root-chat") run.status = "running";
	UAM_ASSERT_EQ(summary_for("root-chat")["backgroundTaskCount"], nlohmann::json(2));
}

UAM_TEST(ClaudeToolTitlesUseArgumentsAndKeepUnknownToolsReadable)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-tool-titles");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "titles";
	chat.provider_id = "claude-cli";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.processing = true;
	const auto check_title = [&](const std::string& name, const nlohmann::json& input, const std::string& expected)
	{
		const std::string id = "tool-" + std::to_string(session.tool_calls.size());
		const nlohmann::json frame = {{"type", "assistant"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}}})}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
		UAM_ASSERT_EQ(session.tool_calls.back().title, expected);
	};
	check_title("Bash", {{"command", "npm test"}}, "npm test");
	for (const std::string name : {"Read", "Write", "Edit", "MultiEdit"})
		check_title(name, {{"file_path", "C:\\work\\README.md"}}, "C:\\work\\README.md");
	check_title("Grep", {{"pattern", "failed"}}, "failed");
	check_title("Agent", {{"description", "Review tests"}}, "Review tests");
	check_title("CustomTool", {{"command", "private argument"}}, "CustomTool");
	check_title("Read", {{"file_path", 42}}, "Read");
#endif
}

UAM_TEST(ClaudeBackgroundAndChildEventsStayWithTheirOriginalTool)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-task-projection");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "claude-tasks";
	chat.provider_id = "claude-cli";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.running = true;
	session.processing = true;
	const auto send = [&](const nlohmann::json& frame)
	{
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
	};
	send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "metadata-only"}, {"patch", {{"description", "Updated description"}}}});
	UAM_ASSERT(session.tool_calls.empty());
	send({{"type", "assistant"}, {"message", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "Parent narration"}}, {{"type", "tool_use"}, {"id", "agent-1"}, {"name", "Agent"}, {"input", {{"description", "Review tests"}}}}})}}}});
	// The launch receipt may arrive before the task lifecycle event.
	send({{"type", "user"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "agent-1"}, {"content", "Launched in background"}}})}}}});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "background-1"}, {"tool_use_id", "agent-1"}, {"task_type", "local_agent"}});
	send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "background-1"}, {"patch", {{"status", "paused"}}}});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("paused"));
	send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "background-1"}, {"patch", {{"status", "running"}}}});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("running"));
	send({{"type", "assistant"}, {"uuid", "child-frame"}, {"parent_tool_use_id", "agent-1"}, {"message", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "Child narration"}}, {{"type", "tool_use"}, {"id", "child-read"}, {"name", "Read"}, {"input", {{"file_path", "tests.cpp"}}}}})}}}});
	send({{"type", "assistant"}, {"uuid", "child-frame"}, {"parent_tool_use_id", "agent-1"}, {"message", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", "Child narration"}}})}}}});
	send({{"type", "assistant"}, {"uuid", "child-string"}, {"parent_tool_use_id", "agent-1"}, {"message", {{"content", "Child string answer"}}}});
	send({{"type", "user"}, {"uuid", "child-image-result"}, {"parent_tool_use_id", "agent-1"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "child-read"}, {"content", nlohmann::json::array({{{"type", "image"}, {"source", {{"type", "base64"}, {"data", "PRIVATE_BINARY_PAYLOAD"}}}}})}}})}}}});
	send({{"type", "user"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "agent-1"}, {"content", "Launched in background"}}})}}}});
	UAM_ASSERT_EQ(session.tool_calls.size(), std::size_t{1});
	UAM_ASSERT(session.tool_calls.front().is_sub_agent);
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("running"));
	UAM_ASSERT_EQ(session.tool_calls.front().child_activity, std::string("Child narration\n\ntests.cpp\n\nChild string answer\n\n[Image]"));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("Parent narration"));
	send({{"type", "result"}, {"subtype", "success"}, {"result", "Done"}});
	// Model the next turn: a late notification must not move the earlier tool into it.
	session.tool_calls.clear();
	session.tool_call_message_indices.clear();
	session.turn_events.clear();
	session.current_assistant_message_index = -1;
	session.turn_assistant_message_index = -1;
	app.chats.front().messages.push_back({MessageRole::User, "Next question"});
	send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "background-1"}, {"status", "completed"}, {"summary", "Tests passed"}});
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(app.chats.front().messages.back().tool_calls.empty());
	send({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "background-1"}});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(session.turn_events.empty());
	// A delayed/replayed tool frame and result retain the earlier owner and terminal task summary.
	send({{"type", "assistant"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_use"}, {"id", "agent-1"}, {"name", "Agent"}, {"input", {{"description", "Review tests"}}}}})}}}});
	send({{"type", "user"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "agent-1"}, {"content", "Delayed launch receipt"}}})}}}});
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{2});
	UAM_ASSERT(app.chats.front().messages.back().tool_calls.empty());
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(session.tool_calls.front().content.find("Tests passed") != std::string::npos);
	send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "background-1"}, {"patch", {{"status", "killed"}}}});
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("cancelled"));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->messages.front().tool_calls.front().child_activity, std::string("Child narration\n\ntests.cpp\n\nChild string answer\n\n[Image]"));
#endif
}

UAM_TEST(ClaudeImportPreservesBlockOrderAndNormalizesTaskNotifications)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-ordered-import");
	const fs::path workspace = temp.root / "workspace";
	std::string encoded = workspace.string();
	for (char& character : encoded) if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
	const fs::path project = temp.root / "claude" / "projects" / encoded;
	// Normalize the leaf before creating its parents; keep ordinary paths in Claude identity.
	fs::path native_history_file;
	std::string native_path_error;
	UAM_ASSERT(uam::io::NativeFilePath(project / "ordered.jsonl", native_history_file, native_path_error));
	struct NativeOwnedRootCleanup
	{
		fs::path root;
		~NativeOwnedRootCleanup()
		{
			std::error_code error;
			fs::remove_all(root, error);
		}
	} cleanup{native_history_file.parent_path().parent_path().parent_path().parent_path()};
	fs::path native_workspace;
	UAM_ASSERT(uam::io::NativeFilePath(workspace, native_workspace, native_path_error));
	fs::create_directories(native_workspace);
	fs::create_directories(native_history_file.parent_path());
	ScopedEnvVar claude_home("CLAUDE_CONFIG_DIR", (temp.root / "claude").string());
	ScopedEnvVar codex_home("CODEX_HOME", (temp.root / "codex").string());
	ScopedEnvVar copilot_home("COPILOT_HOME", (temp.root / "copilot").string());
	ScopedEnvVar gemini_home("GEMINI_CLI_HOME", (temp.root / "gemini").string());
	const auto record = [&](const std::string& id, const std::string& parent, const std::string& role, const nlohmann::json& content)
	{
		return nlohmann::json{{"uuid", id}, {"parentUuid", parent}, {"type", role}, {"cwd", workspace.string()}, {"message", {{"content", content}}}}.dump() + "\n";
	};
	const nlohmann::json content = nlohmann::json::array({{{"type", "text"}, {"text", "Before"}}, {{"type", "thinking"}, {"thinking", "Consider tests"}}, {{"type", "tool_use"}, {"id", "bash-1"}, {"name", "Bash"}, {"input", {{"command", "npm test"}}}}, {{"type", "text"}, {"text", "After"}}});
	const std::string notification = "<task-notification><task-id>task-1</task-id><tool-use-id>bash-1</tool-use-id><status>completed</status><summary>Tests passed</summary></task-notification>";
	UAM_ASSERT(uam::io::WriteTextFile(project / "ordered.jsonl", record("user", "", "user", "Run tests") + record("assistant", "user", "assistant", content) + record("notification", "assistant", "user", notification) + record("quoted", "notification", "user", "<task-notification>ordinary quoted text</task-notification>")));
	const ChatHistorySyncService::LocalHistoryDiscovery discovery = ChatHistorySyncService().DiscoverProviderChatsForFolder({"folder", "Workspace", workspace.string(), false});
	UAM_ASSERT(discovery.result.success);
	UAM_ASSERT_EQ(discovery.chats.size(), std::size_t{1});
	const ChatSession& imported = discovery.chats.front();
	UAM_ASSERT_EQ(imported.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(imported.messages.back().content, std::string("<task-notification>ordinary quoted text</task-notification>"));
	const Message& assistant = imported.messages[1];
	UAM_ASSERT_EQ(assistant.blocks.size(), std::size_t{4});
	UAM_ASSERT_EQ(assistant.blocks[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(assistant.blocks[1].type, std::string("thought"));
	UAM_ASSERT_EQ(assistant.blocks[2].type, std::string("tool_call"));
	UAM_ASSERT_EQ(assistant.blocks[3].text, std::string("After"));
	UAM_ASSERT_EQ(assistant.tool_calls.front().name, std::string("npm test"));
	UAM_ASSERT_EQ(assistant.tool_calls.front().kind, std::string("execute"));
	UAM_ASSERT_EQ(assistant.tool_calls.front().status, std::string("completed"));
	UAM_ASSERT_EQ(assistant.tool_calls.front().result_text, std::string("Tests passed"));
#endif
}

UAM_TEST(ClaudeAutoApprovalDoesNotReplaceExecutionStatus)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-approval-status");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "approval";
	chat.provider_id = "claude-cli";
	chat.command_safety_tier = "yolo";
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.processing = true;
	IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd", "/C", "more > NUL"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
	std::string error;
	UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &error));
	session.running = true;
	struct Cleanup
	{
		IPlatformProcessService& process;
		uam::AcpSessionState& session;
		~Cleanup() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
	} cleanup{process, session};
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"control_request","request_id":"permission","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"bash-1","input":{"command":"npm test"}}})"));
	UAM_ASSERT(!session.waiting_for_permission);
	UAM_ASSERT_EQ(session.tool_calls.front().approval_status, std::string("auto_approved"));
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("pending"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"bash-1","name":"Bash","input":{"command":"npm test"}}]}})"));
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("running"));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"bash-1","content":"Passed"}]}})"));
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls.front().approval_status, std::string("auto_approved"));
#endif
}

UAM_TEST(ToolApprovalDecisionPreservesExecutionAndCancelsDeniedWork)
{
	uam::AcpSessionState session;
	session.tool_calls.push_back({.id = "tool", .title = "npm test", .kind = "execute", .status = "running"});
	session.pending_permission.tool_call_id = "tool";
	session.pending_permission.options = {{"allow", "Allow", "allow_once"}, {"deny", "Deny", "reject_once"}};
	uam::acp_detail::RecordAcpPermissionDecision(session, "allow", false);
	UAM_ASSERT_EQ(session.tool_calls.front().approval_status, std::string("approved"));
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("running"));
	uam::acp_detail::RecordAcpPermissionDecision(session, "deny", false);
	UAM_ASSERT_EQ(session.tool_calls.front().approval_status, std::string("denied"));
	UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("cancelled"));
}

UAM_TEST(LegacyToolApprovalDoesNotInventPendingExecution)
{
	TempDir temp("uam-legacy-tool-approval");
	ChatSession chat;
	chat.id = "legacy-approval";
	chat.messages.push_back({MessageRole::Assistant, "Finished"});
	chat.messages.front().tool_calls.push_back({.id = "tool", .name = "Bash", .status = "auto_approved"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	const ToolCall& tool = loaded->messages.front().tool_calls.front();
	UAM_ASSERT_EQ(tool.approval_status, std::string("auto_approved"));
	UAM_ASSERT_EQ(tool.status, std::string("unknown"));
	UAM_ASSERT(!uam::acp_statuses::IsActiveStatus(tool.status));
}


UAM_TEST(ClaudeNativeBackgroundTasksPreventIdleShutdownUntilTheirOwnTerminalEvent)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string terminal : {"completed", "failed", "cancelled", "killed"})
	{
		TempDir temp("uam-claude-background-idle");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "claude-background-idle";
		chat.provider_id = "claude-cli";
		app.chats.push_back(chat);
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		session.running = true;
		session.session_ready = true;
		session.processing = true;
		const auto send = [&](const nlohmann::json& frame)
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
		};
		send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "background-owned"}, {"tool_use_id", "agent-owned"}, {"task_type", "local_agent"}});
		send({{"type", "result"}, {"subtype", "success"}, {"result", "Parent finished"}});
		UAM_ASSERT(!session.processing);
		UAM_ASSERT_EQ(session.lifecycle_state, std::string("ready"));
		UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("running"));
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		// A new parent turn resets streamed tools, but does not end the existing native task.
		uam::acp_detail::ResetAcpTurnStreamState(session);
		UAM_ASSERT(session.tool_calls.empty());
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		// Saved running tools do not establish ownership for a fresh or resumed process.
		uam::AcpSessionState fresh;
		fresh.provider_id = session.provider_id;
		fresh.running = true;
		fresh.session_ready = true;
		fresh.lifecycle_state = "ready";
		UAM_ASSERT(uam::AcpSessionCanIdleShutdown(fresh, app.chats.front()));
		// The idle parent is disconnected, but an unauthenticated proxy exit proves nothing about its tasks.
		session.process_execution_host_id = "ssh-fixture";
		uam::acp_detail::MarkAcpProcessExited(session, &app.chats.front(), true, 70, false);
		UAM_ASSERT(session.claude_live_background_task_ids.contains("background-owned"));
		session.running = true;
		session.session_ready = true;
		session.lifecycle_state = "ready";
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "background-owned"}, {"patch", {{"status", "paused"}}}});
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "unrelated-task"}, {"status", "completed"}});
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "background-owned"}, {"status", terminal}});
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
		UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		// A replayed progress frame cannot resurrect a settled task's idle ownership.
		send({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "background-owned"}});
		UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		// Fresh runtime reset and authenticated native exit both discard this process's task ownership.
		send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "reset-task"}});
		UAM_ASSERT(!session.claude_live_background_task_ids.empty());
		uam::acp_detail::ResetAcpRuntimeState(app, session, app.chats.front());
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
		session.running = true;
		session.session_ready = true;
		session.lifecycle_state = "ready";
		UAM_ASSERT(uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "native-exit-task"}});
		session.remote_source_exit_reported = true;
		uam::acp_detail::MarkAcpProcessExited(session, &app.chats.front(), true, 0, false);
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
		session.remote_source_exit_reported = false;
		session.process_execution_host_id = "local";
		session.running = true;
		session.session_ready = true;
		session.lifecycle_state = "ready";
		send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "local-exit-task"}});
		uam::acp_detail::MarkAcpProcessExited(session, &app.chats.front(), true, 0, false);
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
	}
	// Exercise the real idle-exit and startup reset paths, without launching SSH or a provider.
	for (const std::string exit_kind : {"proxy", "missing", "native"})
	{
		TempDir temp("uam-claude-background-reconnect");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ExecutionHost host;
		host.id = "background-fixture-host";
		host.transport = "ssh";
		host.ssh_alias = "background-fixture-host";
		host.platform = "linux";
		host.runner_status = "ready";
		host.runner_protocol_version = -1; // Rejected before StartStdioProcess, even with a packaged runner.
		app.settings.execution_hosts = {host};
		ChatSession chat;
		chat.id = "background-reconnect";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = host.id;
		chat.workspace_directory = "/fixture/background-workspace";
		chat.remote_process_exists = true;
		chat.remote_process_control_token = "fixture-authority";
		chat.uam_control_enabled = false;
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& session = *owned;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		session.process_execution_host_id = host.id;
		session.running = true;
		session.session_ready = true;
		session.lifecycle_state = "ready";
		app.acp_sessions.push_back(std::move(owned));
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		struct ProcessCleanup
		{
			IPlatformProcessService& process;
			uam::AcpSessionState& session;
			~ProcessCleanup()
			{
				process.StopStdioProcess(session, true);
				process.CloseStdioProcessHandles(session);
			}
		} cleanup{process, session};
#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", exit_kind == "native" ? "exit /b 0" : exit_kind == "missing" ? "echo The remote process does not exist. 1>&2 & exit /b 70" : "exit /b 70"};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", exit_kind == "native" ? "exit 0" : exit_kind == "missing" ? "printf 'The remote process does not exist.\\n' >&2; exit 70" : "exit 70"};
#endif
		std::string launch_error;
		UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &launch_error));
		session.process_execution_host_id = host.id;
		int exit_code = -1;
		bool exited = false;
		for (int attempt = 0; attempt < 200 && !exited; ++attempt)
		{
			exited = process.PollStdioProcessExited(session, &exit_code);
			if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(exited);
		UAM_ASSERT_EQ(exit_code, exit_kind == "native" ? 0 : 70);
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"system","subtype":"task_started","task_id":"surviving-task"})"));
		session.process_exit_observed = true;
		session.observed_process_exit_code = exit_kind == "native" ? 0 : 70;
		session.remote_source_exit_reported = exit_kind == "native";
		for (int attempt = 0; attempt < 200 && session.running; ++attempt)
		{
			(void)uam::PollAllAcpSessions(app);
			if (session.running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(!session.running);
		const bool surviving_process = exit_kind == "proxy";
		UAM_ASSERT_EQ(session.recovering_remote_process, surviving_process);
		UAM_ASSERT(!session.recovering_remote_turn);
		UAM_ASSERT_EQ(app.chats.front().remote_process_exists, surviving_process);
		UAM_ASSERT(session.queued_prompt.empty());
		UAM_ASSERT(session.queued_user_prompts.empty());
		UAM_ASSERT(app.chats.front().remote_pending_requests.empty());
		UAM_ASSERT(app.chats.front().remote_interaction_responses.empty());
		UAM_ASSERT(app.chats.front().remote_prompt_delivery_id.empty());
		std::string error;
		UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(session.claude_live_background_task_ids.contains("surviving-task"), surviving_process);
		UAM_ASSERT_EQ(app.chats.front().remote_process_exists, surviving_process);
		UAM_ASSERT(!session.running);
		UAM_ASSERT(session.queued_prompt.empty());
		UAM_ASSERT(app.chats.front().remote_pending_requests.empty());
		UAM_ASSERT(app.chats.front().remote_interaction_responses.empty());
	}
#endif
}
