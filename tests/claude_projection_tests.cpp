#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

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
		app.acp_sessions.push_back(std::move(owned));
		uam::acp_detail::AppendToolTurnEventIfNeeded(session, "read-1");
		UAM_ASSERT(uam::acp_detail::SyncAcpToolCallsToAssistantMessage(app.chats.front(), session, true));
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
	fs::create_directories(workspace);
	std::string encoded = workspace.string();
	for (char& character : encoded) if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
	const fs::path project = temp.root / "claude" / "projects" / encoded;
	fs::create_directories(project);
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
