#include "common/runtime/acp/claude_quota_continuation.h"
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
	send({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "background-1"}, {"description", "Reading package.json"}, {"last_tool_name", "Read"}});
	send({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "background-1"}, {"description", "Reading package.json"}});
	UAM_ASSERT(session.tool_calls.front().child_activity.ends_with("Reading package.json"));
	UAM_ASSERT_EQ(session.tool_calls.front().child_activity.find("Reading package.json"), session.tool_calls.front().child_activity.rfind("Reading package.json"));
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
	UAM_ASSERT_EQ(loaded->messages.front().tool_calls.front().child_activity, std::string("Child narration\n\ntests.cpp\n\nChild string answer\n\n[Image]\n\nReading package.json"));
#endif
}

UAM_TEST(ClaudeFirstTaskUpdateOwnsWorkAfterTheLaunchReceipt)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string status : {"pending", "running", "paused"})
	{
		TempDir temp("uam-claude-first-task-update");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "first-task-update";
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
		send({{"type", "assistant"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_use"}, {"id", "agent"}, {"name", "Agent"}, {"input", {{"description", "Review"}}}}})}}}});
		send({{"type", "user"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_result"}, {"tool_use_id", "agent"}, {"content", "Launched"}}})}}}});
		UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
		send({{"type", "result"}, {"subtype", "success"}, {"result", "Parent finished"}});
		uam::acp_detail::ResetAcpTurnStreamState(session);
		send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "child"}, {"tool_use_id", "agent"}, {"patch", {{"status", status}}}});
		UAM_ASSERT_EQ(session.tool_calls.front().status, status);
		UAM_ASSERT(session.claude_live_background_task_ids.contains("child"));
		UAM_ASSERT(!uam::AcpSessionCanIdleShutdown(session, app.chats.front()));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{1});
		UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls.front().status, status);
		for (const std::string terminal : {"completed", "failed", "cancelled"})
		{
			send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "child"}, {"status", terminal}});
			send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "child"}, {"tool_use_id", "agent"}, {"patch", {{"status", status}}}});
			send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "child"}, {"tool_use_id", "agent"}});
			UAM_ASSERT(session.claude_live_background_task_ids.empty());
			UAM_ASSERT_EQ(session.tool_calls.front().status, terminal);
		}
		// A first task update cannot undo an already-cancelled launch while Cancel is pending.
		session.tool_calls.push_back({.id = "cancelled-agent", .status = "cancelled"});
		session.cancel_requested = true;
		send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "cancelled-child"}, {"tool_use_id", "cancelled-agent"}, {"patch", {{"status", status}}}});
		UAM_ASSERT_EQ(session.tool_calls.back().status, std::string("cancelled"));
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
	}
#endif
}

UAM_TEST(ClaudeParentFailureDoesNotSettleItsLiveNativeChildren)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-parent-failure");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "parent-failure";
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
	send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "child"}, {"tool_use_id", "agent"}, {"task_type", "local_agent"}});
	send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "paused-child"}, {"tool_use_id", "paused-agent"}, {"patch", {{"status", "paused"}}}});
	send({{"type", "assistant"}, {"message", {{"content", nlohmann::json::array({{{"type", "tool_use"}, {"id", "read"}, {"name", "Read"}, {"input", {{"file_path", "parent.txt"}}}}})}}}});
	send({{"type", "result"}, {"subtype", "error_during_execution"}, {"is_error", true}, {"error", "Parent failed"}});
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("error"));
	UAM_ASSERT_EQ(session.tool_calls[0].status, std::string("running"));
	UAM_ASSERT_EQ(session.tool_calls[1].status, std::string("paused"));
	UAM_ASSERT_EQ(session.tool_calls[2].status, std::string("failed"));
	UAM_ASSERT(session.claude_live_background_task_ids.contains("child"));
	UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls.front().status, std::string("running"));
	uam::acp_detail::ResetAcpTurnStreamState(session);
	send({{"type", "system"}, {"subtype", "task_progress"}, {"task_id", "child"}});
	UAM_ASSERT(session.claude_live_background_task_ids.contains("child"));
	send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "child"}, {"status", "completed"}, {"summary", "Child finished"}});
	UAM_ASSERT(session.claude_live_background_task_ids.contains("paused-child"));
	send({{"type", "system"}, {"subtype", "task_notification"}, {"task_id", "paused-child"}, {"status", "cancelled"}});
	UAM_ASSERT(session.claude_live_background_task_ids.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.front().tool_calls.front().status, std::string("completed"));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id).has_value());
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
		UAM_ASSERT_EQ(session.claude_live_background_task_ids.contains("surviving-task"), surviving_process);
		const std::optional<ChatSession> stopped_history = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(stopped_history.has_value());
		UAM_ASSERT_EQ(stopped_history->messages.front().tool_calls.front().status, std::string(surviving_process ? "running" : "failed"));
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

UAM_TEST(ClaudeConfirmedProcessExitSettlesChildrenAcrossParentTurns)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string exit_kind : {"proxy", "native", "local"})
	{
		TempDir temp("uam-claude-child-process-exit");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "child-exit";
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
		for (const std::string id : {"running", "paused", "completed"})
		{
			send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", id}, {"tool_use_id", "agent-" + id}});
			if (id != "running") send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", id}, {"patch", {{"status", id}}}});
		}
		send({{"type", "result"}, {"subtype", "success"}, {"result", "Parent finished"}});
		uam::acp_detail::ResetAcpTurnStreamState(session);
		// Restore one live child into the current projection, leaving the other only in history.
		send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "paused"}, {"patch", {{"status", "paused"}}}});
		const bool confirmed_exit = exit_kind != "proxy";
		session.process_execution_host_id = exit_kind == "local" ? "local" : "ssh-fixture";
		session.remote_source_exit_reported = exit_kind == "native";
		uam::acp_detail::MarkAcpProcessExited(session, &app.chats.front(), true, exit_kind == "local" ? 0 : 70, false);
		UAM_ASSERT_EQ(session.claude_live_background_task_ids.empty(), confirmed_exit);
		const Message& saved = app.chats.front().messages.front();
		UAM_ASSERT_EQ(saved.tool_calls[0].status, std::string(confirmed_exit ? "failed" : "running"));
		UAM_ASSERT_EQ(saved.tool_calls[1].status, std::string(confirmed_exit ? "failed" : "paused"));
		UAM_ASSERT_EQ(session.tool_calls.front().status, std::string(confirmed_exit ? "failed" : "paused"));
		UAM_ASSERT_EQ(saved.tool_calls[2].status, std::string("completed"));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{1});
	}
#endif
}

UAM_TEST(ClaudeIdleLocalChildExitPersistsSettlementThroughProductionPolling)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const int code : {0, 70})
	{
		TempDir temp("uam-claude-idle-child-poll");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "idle-child-poll";
		chat.provider_id = "claude-cli";
		chat.workspace_directory = temp.root.string();
		chat.uam_control_enabled = false;
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& session = *owned;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		app.acp_sessions.push_back(std::move(owned));
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		struct Cleanup
		{
			IPlatformProcessService& process;
			uam::AcpSessionState& session;
			~Cleanup() { process.StopStdioProcess(session, true); process.CloseStdioProcessHandles(session); }
		} cleanup{process, session};
#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", "exit /b " + std::to_string(code)};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", "exit " + std::to_string(code)};
#endif
		std::string error;
		UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &error));
		session.process_execution_host_id = "local";
		session.running = true;
		session.session_ready = true;
		session.processing = true;
		const auto send = [&](const nlohmann::json& frame)
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
		};
		for (const std::string id : {"running", "paused"})
		{
			send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", id}, {"tool_use_id", "agent-" + id}});
			if (id == "paused") send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", id}, {"patch", {{"status", id}}}});
		}
		send({{"type", "result"}, {"subtype", "success"}, {"result", "Parent finished"}});
		uam::acp_detail::ResetAcpTurnStreamState(session);
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		const std::optional<ChatSession> before = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(before.has_value());
		UAM_ASSERT_EQ(before->messages.front().tool_calls.front().status, std::string("running"));
		for (int attempt = 0; attempt < 200 && session.running; ++attempt)
		{
			(void)uam::PollAllAcpSessions(app);
			if (session.running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(!session.running);
		UAM_ASSERT(session.claude_live_background_task_ids.empty());
		UAM_ASSERT_EQ(session.last_exit_code, code);
		const std::optional<ChatSession> after = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(after.has_value());
		UAM_ASSERT_EQ(after->messages.front().tool_calls[0].status, std::string("failed"));
		UAM_ASSERT_EQ(after->messages.front().tool_calls[1].status, std::string("failed"));
		UAM_ASSERT_EQ(after->messages.size(), before->messages.size());
	}
#endif
}

namespace
{
	void CheckClaudeExplicitStop(const std::string& kind)
	{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		TempDir temp("uam-claude-child-explicit-stop");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "child-explicit-stop";
		chat.provider_id = "claude-cli";
		chat.execution_host_id = kind.starts_with("local") || kind == "shutdown-local" ? "local" : "ssh-fixture";
		chat.uam_control_enabled = false;
		app.chats.push_back(chat);
		std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
		uam::AcpSessionState& session = *owned;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		app.acp_sessions.push_back(std::move(owned));
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		struct Cleanup
		{
			IPlatformProcessService& process;
			uam::AppState& app;
			~Cleanup()
			{
				for (const auto& stop : app.pending_acp_remote_stops)
					if (stop != nullptr) { process.StopStdioProcess(*stop, true); process.CloseStdioProcessHandles(*stop); }
				for (const auto& current : app.acp_sessions)
					if (current != nullptr) { process.StopStdioProcess(*current, true); process.CloseStdioProcessHandles(*current); }
			}
		} cleanup{process, app};
		const int code = kind.ends_with("unconfirmed") ? 70 : 0;
	#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", kind == "local-delayed" ? "set /p line= & echo {\"type\":\"system\",\"subtype\":\"task_updated\",\"task_id\":\"running\",\"patch\":{\"status\":\"completed\"}} & echo late stop stderr 1>&2 & exit /b 0" : "set /p line= & exit /b " + std::to_string(code)};
	#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", kind == "local-delayed" ? "IFS= read -r line; printf '%s\\n' '{\"type\":\"system\",\"subtype\":\"task_updated\",\"task_id\":\"running\",\"patch\":{\"status\":\"completed\"}}'; printf 'late stop stderr\\n' >&2; exit 0" : "IFS= read -r line; exit " + std::to_string(code)};
	#endif
		std::string error;
		UAM_ASSERT(process.StartStdioProcess(session, temp.root, argv, &error));
		session.process_execution_host_id = chat.execution_host_id;
		session.running = true;
		session.session_ready = true;
		session.processing = true;
		const auto send = [&](const nlohmann::json& frame)
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame.dump()));
		};
		for (const std::string id : {"running", "paused", "completed"})
		{
			if (kind == "local-current" && id == "paused") continue;
			send({{"type", "system"}, {"subtype", "task_started"}, {"task_id", id}, {"tool_use_id", "agent-" + id}});
			if (id != "running") send({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", id}, {"patch", {{"status", id}}}});
		}
		send({{"type", "result"}, {"subtype", "success"}, {"result", "Parent finished"}});
		if (kind != "local-current") uam::acp_detail::ResetAcpTurnStreamState(session);
		const bool shutdown = kind.starts_with("shutdown");
		if (kind == "shutdown-local")
		{
			session.queue_held = true;
			session.queued_user_prompts.push_back({.text = "Keep held intent", .id = "held"});
			app.chats.front().acp_queue_held = true;
			app.chats.front().acp_queued_prompts.assign(session.queued_user_prompts.begin(), session.queued_user_prompts.end());
		}
		if (shutdown) uam::FastStopAcpSessionsForExit(app);
		else
		{
			if (kind == "local-delayed")
			{
				// Reproduce an observer which returned before this owned process's final exit/output.
				UAM_ASSERT(process.WriteToStdioProcess(session, "finish\n", 7, &error));
				bool exited = false;
				for (int attempt = 0; attempt < 200 && !exited; ++attempt)
				{
					exited = process.PollStdioProcessExited(session);
					if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				UAM_ASSERT(exited);
				uam::AsyncAcpProcessStopTask task;
				task.chat_id = chat.id;
				task.turn_serial = session.turn_serial;
				task.observe_exit = true;
				task.result = std::make_shared<uam::platform::ObservedProcessStopResult>();
				task.finished = std::make_shared<std::atomic<bool>>(true);
				task.owned_process = std::make_shared<uam::platform::StdioProcessPlatformFields>();
				uam::TransferStdioProcessFields(session, *task.owned_process);
				app.acp_process_stop_tasks.push_back(std::move(task));
				session.running = false;
				session.local_stop_pending = true;
			}
			else (void)uam::StopAcpSession(app, chat.id);
			UAM_ASSERT_EQ(session.claude_live_background_task_ids.size(), kind == "local-current" ? std::size_t{1} : std::size_t{2});
			if (kind == "local-relaunch")
			{
				// Fail before attempting a launch if Stop has not established its admission gate.
				UAM_ASSERT(session.local_stop_pending);
				UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(app, session, app.chats.front(), &error));
				UAM_ASSERT(error.find("stopping") != std::string::npos);
				UAM_ASSERT(!session.claude_live_background_task_ids.empty());
				session.stdout_buffer += nlohmann::json({{"type", "system"}, {"subtype", "task_started"}, {"task_id", "late"}, {"tool_use_id", "agent-late"}}).dump() + "\n";
				session.stdout_buffer += nlohmann::json({{"type", "system"}, {"subtype", "task_updated"}, {"task_id", "late-paused"}, {"tool_use_id", "agent-late-paused"}, {"patch", {{"status", "paused"}}}}).dump() + "\n";
			}
		}
		bool stop_published = false;
		for (int attempt = 0; attempt < 300 && (!app.pending_acp_remote_stops.empty() || !app.acp_process_stop_tasks.empty()); ++attempt)
		{
			stop_published = uam::PollAllAcpSessions(app) || stop_published;
			if (!app.pending_acp_remote_stops.empty() || !app.acp_process_stop_tasks.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(app.pending_acp_remote_stops.empty());
		UAM_ASSERT(app.acp_process_stop_tasks.empty());
		const bool confirmed = !kind.ends_with("unconfirmed");
		if (shutdown) UAM_ASSERT(app.acp_sessions.empty());
		else
		{
			UAM_ASSERT_EQ(session.claude_live_background_task_ids.empty(), confirmed);
			if (confirmed) UAM_ASSERT(stop_published);
		}
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages.front().tool_calls[0].status, std::string(kind == "local-delayed" ? "completed" : confirmed ? "cancelled" : "running"));
		if (kind == "local-delayed") UAM_ASSERT(session.recent_stderr.find("late stop stderr") != std::string::npos);
		if (kind != "local-current") UAM_ASSERT_EQ(saved->messages.front().tool_calls[1].status, std::string(confirmed ? "cancelled" : "paused"));
		const std::size_t completed_index = kind == "local-current" ? 1 : 2;
		UAM_ASSERT_EQ(saved->messages.front().tool_calls[completed_index].status, std::string("completed"));
		if (kind == "local-relaunch")
		{
			std::size_t late_tasks = 0;
			for (const Message& message : saved->messages)
				for (const ToolCall& tool : message.tool_calls)
					if (tool.task_id == "late" || tool.task_id == "late-paused")
					{
						++late_tasks;
						UAM_ASSERT_EQ(tool.status, std::string("cancelled"));
					}
			UAM_ASSERT_EQ(late_tasks, std::size_t{2});
		}
		if (kind == "shutdown-local")
		{
			UAM_ASSERT(saved->acp_queue_held);
			UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(saved->acp_queued_prompts.front().id, std::string("held"));
		}
#else
		(void)kind;
#endif
	}
}

UAM_TEST(ClaudeExplicitStopsSettleChildrenOnlyAfterConfirmedTermination)
{
	for (const std::string kind : {"local", "local-current", "local-delayed", "remote-confirmed", "remote-unconfirmed", "shutdown-confirmed", "shutdown-unconfirmed"})
		CheckClaudeExplicitStop(kind);
}

UAM_TEST(ClaudeLocalChildStopBlocksImmediateReplacementLaunch)
{
	CheckClaudeExplicitStop("local-relaunch");
}

UAM_TEST(ClaudeLocalShutdownPersistsHistoricalChildSettlement)
{
	CheckClaudeExplicitStop("shutdown-local");
}

UAM_TEST(ClaudeFiveHourQuotaOwnsSettledTurnWithoutReplayingAcceptedWork)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	TempDir temp("uam-claude-quota");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "quota-chat";
	chat.provider_id = "claude-cli";
	chat.native_session_id = "quota-native";
	Message user;
	user.role = MessageRole::User;
	user.content = "Already accepted prompt";
	chat.messages.push_back(user);
	app.chats.push_back(chat);
	uam::AcpSessionState session;
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.protocol_kind = "claude-code-stream-json";
	session.session_id = chat.native_session_id;
	session.processing = true;
	session.prompt_request_id = 0;
	session.turn_serial = 1;
	session.turn_user_message_index = 0;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json({
	    {"type", "assistant"}, {"session_id", chat.native_session_id}, {"error", "rate_limit"},
	    {"message", {{"content", nlohmann::json::array()}}}}).dump()));
	const int64_t reset = uam::time::TimestampNowSec() + 3600;
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json({
	    {"type", "rate_limit_event"}, {"session_id", chat.native_session_id},
	    {"rate_limit_info", {{"status", "rejected"}, {"rateLimitType", "five_hour"}, {"resetsAt", reset}}}}).dump()));
	UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), nlohmann::json({
	    {"type", "result"}, {"session_id", chat.native_session_id}, {"subtype", "error_during_execution"},
	    {"is_error", true}, {"errors", nlohmann::json::array({"Usage limit"})}}).dump()));
	UAM_ASSERT_EQ(session.lifecycle_state, std::string("waiting_quota"));
	UAM_ASSERT(session.queued_prompt.empty());
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, user.content);
#endif
}

namespace
{
	struct ClaudeQuotaFixture
	{
		TempDir temp{"uam-quota-boundary"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		int64_t now = uam::time::TimestampNowSec();
		std::unique_ptr<ScopedEnvVar> fixture_path;
		ClaudeQuotaFixture()
		{
#if !defined(_WIN32)
			const fs::path stub = temp.root / "claude";
			UAM_ASSERT(uam::io::WriteTextFile(stub, "#!/bin/sh\nexit 93\n"));
			fs::permissions(stub, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
			fixture_path = std::make_unique<ScopedEnvVar>("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or(""));
#endif
			app.data_root = temp.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.runtime_cli_versions_by_provider_id["claude-cli"].checked = true;
			app.runtime_cli_versions_by_provider_id["claude-cli"].supported = true;
			ChatSession chat;
			chat.id = "quota-boundary";
			chat.provider_id = "claude-cli";
			chat.native_session_id = "33333333-3333-4333-8333-333333333333";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode = "default";
			chat.messages.push_back(Message{MessageRole::User, "ACCEPTED-PROMPT-MUST-NOT-REPLAY"});
			app.chats.push_back(chat);
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = chat.id;
			session->provider_id = chat.provider_id;
			session->protocol_kind = "claude-code-stream-json";
			session->session_id = chat.native_session_id;
			session->process_execution_host_id = "local";
			session->processing = true;
			session->prompt_request_id = 0;
			session->turn_serial = 1;
			session->turn_user_message_index = 0;
			session->active_uam_agent_definition_hash = "builtin-build-v1";
			app.acp_sessions.push_back(std::move(owned));
		}
		~ClaudeQuotaFixture()
		{
			uam::FastStopAcpSessionsForExit(app);
		}
		nlohmann::json Event() const
		{
			return {{"type", "rate_limit_event"}, {"session_id", app.chats.front().native_session_id},
			    {"rate_limit_info", {{"status", "rejected"}, {"rateLimitType", "five_hour"}, {"resetsAt", now + 60}}}};
		}
		nlohmann::json Result() const
		{
			return {{"type", "result"}, {"session_id", app.chats.front().native_session_id},
			    {"subtype", "error_during_execution"}, {"is_error", true}, {"errors", nlohmann::json::array({"Quota rejected"})}};
		}
		void Send(const nlohmann::json& message)
		{
			if (message.value("type", "") == "rate_limit_event")
				UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), nlohmann::json({
				    {"type", "assistant"}, {"session_id", app.chats.front().native_session_id}, {"error", "rate_limit"},
				    {"message", {{"content", nlohmann::json::array()}}}}).dump()));
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), message.dump()));
		}
		void Wait()
		{
			Send(Event());
			Send(Result());
			UAM_ASSERT_EQ(app.chats.front().claude_quota_continuation.state, std::string("waiting"));
		}
	};
}

UAM_TEST(ClaudeQuotaRejectsUntrustworthyResetsAndNonFiveHourWindows)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string invalid : {"allowed", "weekly", "overage", "missing", "string", "fraction", "past", "far", "overflow", "wrong-session", "mixed-week", "bad-windows", "mismatched-reset", "overage-rejected"})
	{
		ClaudeQuotaFixture fixture;
		nlohmann::json event = fixture.Event();
		nlohmann::json& info = event["rate_limit_info"];
		if (invalid == "allowed") info["status"] = "allowed_warning";
		if (invalid == "weekly") info["rateLimitType"] = "seven_day_opus";
		if (invalid == "overage") info["rateLimitType"] = "overage";
		if (invalid == "missing") info.erase("resetsAt");
		if (invalid == "string") info["resetsAt"] = std::to_string(fixture.now + 60);
		if (invalid == "fraction") info["resetsAt"] = static_cast<double>(fixture.now) + 60.5;
		if (invalid == "past") info["resetsAt"] = fixture.now - 1;
		if (invalid == "far") info["resetsAt"] = fixture.now + 20000;
		if (invalid == "overflow") info["resetsAt"] = std::numeric_limits<uint64_t>::max();
		if (invalid == "wrong-session") event["session_id"] = "foreign";
		if (invalid == "mixed-week") info["unifiedWindows"] = {{"seven_day", {{"utilization", 1.0}, {"resetsAt", fixture.now + 86400}}}};
		if (invalid == "bad-windows") info["unifiedWindows"] = nlohmann::json::array();
		if (invalid == "mismatched-reset") info["unifiedWindows"] = {{"five_hour", {{"utilization", 1.0}, {"resetsAt", fixture.now + 120}}}};
		if (invalid == "overage-rejected") info["overageStatus"] = "rejected";
		fixture.Send(event);
		fixture.Send(fixture.Result());
		UAM_ASSERT(fixture.app.chats.front().claude_quota_continuation.id.empty());
	}
#endif
}

UAM_TEST(ClaudeQuotaRequiresAcceptedTerminalFailureAndClearsStaleEvidence)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string boundary : {"undelivered", "cancelled", "new-turn", "allowed-later", "success", "foreign-result", "duplicate-result", "event-alone"})
	{
		ClaudeQuotaFixture fixture;
		fixture.Send(fixture.Event());
		if (boundary == "event-alone") { UAM_ASSERT(fixture.app.chats.front().claude_quota_continuation.id.empty()); continue; }
		if (boundary == "undelivered") fixture.app.chats.front().messages.front().acp_prompt_not_sent = true;
		if (boundary == "cancelled") fixture.session->cancel_requested = true;
		if (boundary == "new-turn") ++fixture.session->turn_serial;
		if (boundary == "allowed-later") { nlohmann::json event = fixture.Event(); event["rate_limit_info"]["status"] = "allowed"; fixture.Send(event); }
		nlohmann::json result = fixture.Result();
		if (boundary == "success") { result["subtype"] = "success"; result["is_error"] = false; }
		if (boundary == "foreign-result") result["session_id"] = "foreign";
		fixture.Send(result);
		if (boundary == "duplicate-result")
		{
			const ClaudeQuotaContinuation owner = fixture.app.chats.front().claude_quota_continuation;
			fixture.Send(result);
			UAM_ASSERT(fixture.app.chats.front().claude_quota_continuation == owner);
		}
		else UAM_ASSERT(fixture.app.chats.front().claude_quota_continuation.id.empty());
	}
#endif
}

UAM_TEST(ClaudeQuotaPersistsResetOwnershipAndCrashClaimsWithoutTranscriptReplay)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ClaudeQuotaFixture fixture;
	fixture.session->tool_calls.push_back({.id = "acknowledged-write", .title = "Completed write", .status = "completed", .content = "COMPLETED-TOOL-MUST-NOT-REPLAY"});
	fixture.Wait();
	const ClaudeQuotaContinuation original = fixture.app.chats.front().claude_quota_continuation;
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->claude_quota_continuation == original);
	UAM_ASSERT_EQ(loaded->messages.front().content, std::string("ACCEPTED-PROMPT-MUST-NOT-REPLAY"));
	for (const std::string state : {"claimed", "continued", "cancelled", "held", "completed"})
	{
		fixture.app.chats.front().claude_quota_continuation.state = state;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.app.chats.front()));
		const std::optional<ChatSession> reloaded = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
		UAM_ASSERT(reloaded.has_value());
		UAM_ASSERT_EQ(reloaded->claude_quota_continuation.state, state == "claimed" || state == "continued" ? std::string("held") : state);
		UAM_ASSERT_EQ(reloaded->claude_quota_continuation.resets_at_epoch_s, original.resets_at_epoch_s);
	}
#endif
}

UAM_TEST(ClaudeQuotaHoldsUnresolvedToolsBackgroundChildrenAndManagedDeadlines)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string kind : {"running", "paused", "failed", "background", "managed", "retry"})
	{
		ClaudeQuotaFixture fixture;
		if (kind == "background") fixture.session->claude_live_background_task_ids.insert("child");
		else if (kind == "managed") fixture.app.chats.front().agent_run_id = "bounded-run";
		else if (kind == "retry") fixture.session->claude_quota_delivery_id = "already-attempted";
		else fixture.session->tool_calls.push_back({.id = "unresolved-write", .status = kind});
		fixture.Send(fixture.Event());
		fixture.Send(fixture.Result());
		UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("held"));
		UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 61));
		UAM_ASSERT(fixture.session->queued_prompt.empty());
	}
#endif
}

UAM_TEST(ClaudeQuotaCancellationAndIdentityChangesPreventWakeAcrossRestart)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string boundary : {"cancel", "stop", "native", "provider", "workspace", "model", "approval", "host", "new-message", "edited-message", "branch"})
	{
		ClaudeQuotaFixture fixture;
		fixture.Wait();
		UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 59));
		ChatSession& chat = fixture.app.chats.front();
		if (boundary == "cancel") UAM_ASSERT(uam::CancelAcpTurn(fixture.app, chat.id));
		if (boundary == "stop") UAM_ASSERT(uam::StopAcpSession(fixture.app, chat.id));
		if (boundary == "native") chat.native_session_id = "replacement";
		if (boundary == "provider") chat.provider_id = "codex-cli";
		if (boundary == "workspace") chat.workspace_directory += "/different";
		if (boundary == "model") chat.model_id = "different";
		if (boundary == "approval") chat.approval_mode = "plan";
		if (boundary == "host") fixture.app.settings.execution_hosts.front().instruction_file = "different";
		if (boundary == "new-message") chat.messages.push_back(Message{MessageRole::User, "new work"});
		if (boundary == "edited-message") chat.messages.front().content = "replacement";
		if (boundary == "branch") chat.claude_quota_continuation.chat_id = "parent";
		(void)uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 61);
		UAM_ASSERT_EQ(chat.claude_quota_continuation.state, std::string("cancelled"));
		UAM_ASSERT(fixture.session->queued_prompt.empty());
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, chat.id);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(loaded->claude_quota_continuation.state != "waiting");
	}
#endif
}

UAM_TEST(ClaudeQuotaNativeContinuationWritesOnlyOneNewInstructionAfterWarmOrColdReset)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && !defined(_WIN32)
	for (const std::string kind : {"warm", "cold", "queued", "queued-settings", "goal"})
	{
		const bool cold = kind == "cold";
		ClaudeQuotaFixture fixture;
		const fs::path shim = fixture.temp.root / "claude";
		UAM_ASSERT(uam::io::WriteTextFile(shim, "#!/bin/sh\nprintf '%s\\n' \"$@\" > launch-args.txt\nexec /bin/cat >> received.jsonl\n"));
		fs::permissions(shim, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
		const std::string inherited_path = uam::env::GetNonEmptyString("PATH").value_or("");
		ScopedEnvVar path_env("PATH", fixture.temp.root.string() + ":" + inherited_path);
		fixture.session->tool_calls.push_back({.id = "completed-write", .status = "completed", .content = "COMPLETED-TOOL-MUST-NOT-REPLAY"});
		if (kind.starts_with("queued"))
		{
			uam::AcpQueuedUserPromptState pending;
			pending.text = "FOLLOWUP-MUST-WAIT-FOR-CONTINUATION";
			pending.id = "future-prompt";
			fixture.session->queued_user_prompts.push_back(pending);
			fixture.app.chats.front().acp_queued_prompts.push_back(pending);
		}
		fixture.Wait();
		const std::string native_id = fixture.app.chats.front().native_session_id;
		const ClaudeQuotaContinuation owner = fixture.app.chats.front().claude_quota_continuation;
		if (cold)
		{
			fixture.app.acp_sessions.clear();
			fixture.session = nullptr;
			fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.temp.root);
			UAM_ASSERT(!fixture.app.chats.front().messages_loaded);
			UAM_ASSERT(fixture.app.chats.front().claude_quota_continuation == owner);
		}
		else
		{
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(*fixture.session, fixture.temp.root, {shim.string(), "--resume", native_id}, &error));
			fixture.session->running = true;
			fixture.session->initialized = true;
			fixture.session->session_ready = true;
			fixture.session->current_mode_id = kind == "queued-settings" ? "plan" : "default";
		}
		struct Cleanup
		{
			uam::AppState& app;
			~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
		} cleanup{fixture.app};
		UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 59));
		std::string received;
		bool answered_init = false;
		for (int attempt = 0; attempt < 300; ++attempt)
		{
			(void)uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 60);
			fixture.session = uam::FindAcpSessionForChat(fixture.app, "quota-boundary");
			if (fixture.session != nullptr && fixture.session->initialize_request_id != 0 && !answered_init)
			{
				fixture.Send({{"type", "control_response"}, {"response", {{"subtype", "success"},
				    {"request_id", std::to_string(fixture.session->initialize_request_id)}, {"response", nlohmann::json::object()}}}});
				answered_init = true;
			}
			(void)uam::PollAllAcpSessions(fixture.app);
			(void)uam::io::TryReadTextFile(fixture.temp.root / "received.jsonl", received);
			if (received.find(uam::acp_detail::kClaudeQuotaContinuationPrompt) != std::string::npos) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(received.find(uam::acp_detail::kClaudeQuotaContinuationPrompt) != std::string::npos);
		UAM_ASSERT(received.find("ACCEPTED-PROMPT-MUST-NOT-REPLAY") == std::string::npos);
		UAM_ASSERT(received.find("COMPLETED-TOOL-MUST-NOT-REPLAY") == std::string::npos);
		UAM_ASSERT_EQ(fixture.app.chats.front().native_session_id, native_id);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.front().content, std::string("ACCEPTED-PROMPT-MUST-NOT-REPLAY"));
		UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("continued"));
		std::string arguments;
		UAM_ASSERT(uam::io::TryReadTextFile(fixture.temp.root / "launch-args.txt", arguments));
		UAM_ASSERT(arguments.find("--resume\n" + native_id) != std::string::npos);
		const std::string before = received;
		for (int iteration = 0; iteration < 5; ++iteration)
			UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 120));
		UAM_ASSERT(uam::io::TryReadTextFile(fixture.temp.root / "received.jsonl", received));
		UAM_ASSERT_EQ(received, before);
		std::size_t users = 0;
		std::istringstream lines(received);
		for (std::string line; std::getline(lines, line);)
			if (nlohmann::json::parse(line).value("type", "") == "user") ++users;
		UAM_ASSERT_EQ(users, std::size_t{1});
		if (kind == "goal")
		{
			fixture.Send({{"type", "result"}, {"session_id", native_id}, {"subtype", "success"}, {"is_error", false}});
			UAM_ASSERT(uam::acp_detail::QueueGoalInternalPrompt(fixture.app, *fixture.session, fixture.app.chats.front(), "NEW-INTERNAL-GOAL-TURN", false, "", false));
			UAM_ASSERT(fixture.session->claude_quota_delivery_id.empty());
			UAM_ASSERT(fixture.session->last_error.empty());
		}
		if (kind.starts_with("queued"))
		{
			UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(fixture.app.chats.front().acp_queued_prompts.size(), std::size_t{1});
			const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
			UAM_ASSERT(persisted.has_value());
			UAM_ASSERT_EQ(persisted->acp_queued_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(persisted->acp_queued_prompts.front().id, std::string("future-prompt"));
			fixture.Send({{"type", "result"}, {"session_id", native_id}, {"subtype", "success"}, {"is_error", false}});
			for (int attempt = 0; attempt < 200; ++attempt)
			{
				(void)uam::PollAllAcpSessions(fixture.app);
				(void)uam::io::TryReadTextFile(fixture.temp.root / "received.jsonl", received);
				if (received.find("FOLLOWUP-MUST-WAIT-FOR-CONTINUATION") != std::string::npos) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			UAM_ASSERT(received.find("FOLLOWUP-MUST-WAIT-FOR-CONTINUATION") != std::string::npos);
			UAM_ASSERT(fixture.session->queued_user_prompts.empty());
		}
	}
#endif
}

UAM_TEST(ClaudeQuotaPersistenceFailureNeverDispatchesOrDropsCancellation)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ClaudeQuotaFixture fixture;
	fixture.Wait();
	const fs::path blocker = fixture.temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocker, "block"));
	fixture.app.data_root = blocker;
	UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 60));
	UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("waiting"));
	UAM_ASSERT(!uam::CancelAcpTurn(fixture.app, "quota-boundary"));
	UAM_ASSERT(!uam::CancelAcpTurn(fixture.app, "quota-boundary"));
	UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("cancelled"));
	UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 120));
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	fixture.app.data_root = fixture.temp.root;
	UAM_ASSERT(uam::CancelAcpTurn(fixture.app, "quota-boundary"));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->claude_quota_continuation.state, std::string("cancelled"));
#endif
}

UAM_TEST(ClaudeQuotaRevalidatesNativeOwnershipAtTheActualWriteBoundary)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string change : {"cancel", "host", "model", "native", "reset", "new-owner", "resolved", "edited-message", "new-message"})
	{
		ClaudeQuotaFixture fixture;
		fixture.Wait();
		ChatSession& chat = fixture.app.chats.front();
		chat.claude_quota_continuation.state = "claimed";
		fixture.session->claude_quota_delivery_id = chat.claude_quota_continuation.id;
		fixture.session->queued_prompt = uam::acp_detail::kClaudeQuotaContinuationPrompt;
		fixture.session->processing = true;
		fixture.session->running = true;
		fixture.session->initialized = true;
		fixture.session->session_ready = true;
		fixture.session->current_mode_id = "default";
		chat.messages.push_back(Message{MessageRole::User, uam::acp_detail::kClaudeQuotaContinuationPrompt});
		UAM_ASSERT(uam::acp_detail::ValidateClaudeQuotaDelivery(fixture.app, *fixture.session, chat));
		if (change == "cancel") UAM_ASSERT(uam::acp_detail::CancelClaudeQuotaWait(fixture.app, chat));
		if (change == "host") fixture.app.settings.execution_hosts.front().ssh_alias = "different";
		if (change == "model") chat.model_id = "different";
		if (change == "native") chat.native_session_id = "replacement";
		if (change == "reset") chat.native_session_reset_pending = true;
		if (change == "new-owner") chat.claude_quota_continuation.id = "replacement-owner";
		if (change == "resolved") fixture.app.resolved_native_sessions_by_chat_id[chat.id] = "replacement";
		if (change == "edited-message") chat.messages.front().content = "replacement";
		if (change == "new-message") chat.messages.push_back(Message{MessageRole::User, "other work"});
		UAM_ASSERT(!uam::acp_detail::ValidateClaudeQuotaDelivery(fixture.app, *fixture.session, chat));
		UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, chat));
		UAM_ASSERT(fixture.session->queued_prompt.empty());
		UAM_ASSERT_EQ(fixture.session->lifecycle_state, std::string("error"));
		UAM_ASSERT_EQ(chat.claude_quota_continuation.state, std::string("cancelled"));
		fixture.session->running = false;
	}
#endif
}

UAM_TEST(ClaudeQuotaSnapshotReportsWaitAsBusyWithoutChangingNativeTurnState)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ClaudeQuotaFixture fixture;
	fixture.Wait();
	UAM_ASSERT(!fixture.session->processing);
	UAM_ASSERT(uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["processing"] == true);
	fixture.app.acp_sessions.clear();
	const nlohmann::json stopped = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"];
	UAM_ASSERT(stopped["processing"] == true);
	UAM_ASSERT_EQ(stopped["lifecycleState"], nlohmann::json("waiting_quota"));
	UAM_ASSERT(uam::CancelAcpTurn(fixture.app, "quota-boundary"));
	UAM_ASSERT(uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"]["processing"] == false);
#endif
}

UAM_TEST(ClaudeQuotaWaitsForCompatibilityWithoutCreatingAnUnownedDeferredPrompt)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ClaudeQuotaFixture fixture;
	fixture.Wait();
	fixture.app.runtime_cli_versions_by_provider_id["claude-cli"].checked = false;
	fixture.app.runtime_cli_versions_by_provider_id["claude-cli"].supported = false;
	UAM_ASSERT(!uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 60));
	UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("waiting"));
	UAM_ASSERT(fixture.session->queued_user_prompts.empty());
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT(fixture.session->claude_quota_delivery_id.empty());
#endif
}

UAM_TEST(ClaudeQuotaRestartHoldsPreparedRemoteContinuationWithoutGenericRecovery)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	{
		ClaudeQuotaFixture fixture;
		fixture.Wait();
		fixture.app.chats.front().claude_quota_continuation.state = "held";
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.app.chats.front()));
		fixture.app.acp_sessions.clear();
		fixture.session = nullptr;
		fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.temp.root);
		UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app), std::size_t{0});
		UAM_ASSERT(!fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT(uam::CancelAcpTurn(fixture.app, "quota-boundary"));
		UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("cancelled"));
	}
	for (const std::string state : {"claimed", "continued", "cancelled"})
	{
		ClaudeQuotaFixture fixture;
		fixture.Wait();
		ChatSession& chat = fixture.app.chats.front();
		chat.execution_host_id = "missing-isolated-host";
		chat.claude_quota_continuation.state = state;
		uam::AcpQueuedUserPromptState pending;
		pending.text = uam::acp_detail::kClaudeQuotaContinuationPrompt;
		pending.prepared_for_delivery = true;
		pending.append_user_message = false;
		pending.claude_quota_continuation_id = chat.claude_quota_continuation.id;
		chat.acp_queued_prompts.push_back(pending);
		chat.acp_dispatched_queued_prompt_count = 1;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
		fixture.app.acp_sessions.clear();
		fixture.session = nullptr;
		fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.temp.root);
		UAM_ASSERT_EQ(fixture.app.chats.front().acp_queued_prompts.front().claude_quota_continuation_id, pending.claude_quota_continuation_id);
		UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app), std::size_t{0});
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app, false), std::size_t{0});
		UAM_ASSERT(fixture.app.acp_sessions.empty());
		UAM_ASSERT(fixture.app.chats.front().acp_queue_held);
		UAM_ASSERT_EQ(fixture.app.chats.front().acp_queued_prompts.size(), std::size_t{1});
	}
#endif
}

UAM_TEST(ClaudeQuotaHoldsSameIdAgentDefinitionChangesBeforeNativeStartup)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string access : {"write", "read"})
	{
		ClaudeQuotaFixture fixture;
		fs::create_directories(fixture.temp.root / "agents");
		const fs::path definition = fixture.temp.root / "agents" / "quota-agent.md";
		UAM_ASSERT(uam::io::WriteTextFile(definition, "---\nversion: 1\nname: quota-agent\ndescription: fixture\nmode: primary\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\nOriginal instructions.\n"));
		const uam::AgentDefinitionCatalog original = uam::AgentDefinitionService::Load(fixture.temp.root, fixture.temp.root);
		const std::vector<uam::AgentDefinition>::const_iterator accepted = std::ranges::find(original.definitions, std::string("quota-agent"), &uam::AgentDefinition::id);
		UAM_ASSERT(accepted != original.definitions.end());
		fixture.app.chats.front().uam_agent_id = "quota-agent";
		fixture.session->active_uam_agent_id = "quota-agent";
		fixture.session->active_uam_agent_definition_hash = accepted->definition_hash;
		fixture.Wait();
		UAM_ASSERT(uam::io::WriteTextFile(definition, "---\nversion: 1\nname: quota-agent\ndescription: fixture\nmode: primary\nworkspaceAccess: " + access + "\nskills: []\ndelegates: []\n---\nChanged same-ID instructions.\n"));
		const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(fixture.temp.root, fixture.temp.root);
		const std::vector<uam::AgentDefinition>::const_iterator changed = std::ranges::find(catalog.definitions, std::string("quota-agent"), &uam::AgentDefinition::id);
		UAM_ASSERT(changed != catalog.definitions.end());
		UAM_ASSERT(changed->definition_hash != accepted->definition_hash);
		UAM_ASSERT(uam::acp_detail::PollClaudeQuotaContinuations(fixture.app, fixture.now + 60));
		UAM_ASSERT_EQ(fixture.app.chats.front().claude_quota_continuation.state, std::string("held"));
		UAM_ASSERT(!fixture.session->running);
		UAM_ASSERT(fixture.session->queued_prompt.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().messages.size(), std::size_t{1});
	}
#endif
}

UAM_TEST(ClaudeQuotaHeldFutureQueueRemainsVisibleEditableAndCannotBeOverwrittenAfterRestart)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string boundary : std::vector<std::string>{"local-visible", "remote-visible", "local-send"})
	{
		ClaudeQuotaFixture fixture;
		fixture.Wait();
		ChatSession& chat = fixture.app.chats.front();
		if (boundary == "remote-visible") chat.execution_host_id = "missing-isolated-host";
		chat.claude_quota_continuation.state = "claimed";
		uam::AcpQueuedUserPromptState uncertain;
		uncertain.text = uam::acp_detail::kClaudeQuotaContinuationPrompt;
		uncertain.prepared_for_delivery = true;
		uncertain.claude_quota_continuation_id = chat.claude_quota_continuation.id;
		uam::AcpQueuedUserPromptState future;
		future.id = "future-visible";
		future.text = "Preserve future work";
		chat.acp_queued_prompts = {uncertain, future};
		chat.acp_dispatched_queued_prompt_count = 1;
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, chat));
		fixture.app.acp_sessions.clear();
		fixture.session = nullptr;
		fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.temp.root);
		UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app), std::size_t{0});
		std::string error;
		if (boundary == "local-send")
		{
			UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, "quota-boundary", "Replacement must not erase saved work", &error));
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
			UAM_ASSERT(saved.has_value());
			UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{2});
			UAM_ASSERT_EQ(saved->acp_queued_prompts.back().id, future.id);
			UAM_ASSERT_EQ(saved->claude_quota_continuation.state, std::string("held"));
			continue;
		}
		fixture.session = uam::FindAcpSessionForChat(fixture.app, "quota-boundary");
		UAM_ASSERT(fixture.session != nullptr);
		UAM_ASSERT(!fixture.session->running && !fixture.session->reconnect_pending && fixture.session->queue_held);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		const nlohmann::json snapshot = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"];
		UAM_ASSERT_EQ(snapshot["queuedPrompts"].size(), std::size_t{1});
		UAM_ASSERT(snapshot["queueHeld"] == true);
		UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "quota-boundary", future.id, 1, "Edited future work", &error));
		const std::optional<ChatSession> edited = ChatRepository::LoadLocalChat(fixture.temp.root, "quota-boundary");
		UAM_ASSERT(edited.has_value());
		UAM_ASSERT_EQ(edited->acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(edited->acp_queued_prompts.front().claude_quota_continuation_id, uncertain.claude_quota_continuation_id);
		UAM_ASSERT_EQ(edited->acp_queued_prompts.back().text, std::string("Edited future work"));
		UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "quota-boundary", {{future.id, 2}}, &error));
		UAM_ASSERT(!fixture.session->running && !fixture.session->reconnect_pending && fixture.session->queue_held);
		UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, "quota-boundary", 0, &error));
		UAM_ASSERT(fixture.session->queued_user_prompts.empty());
		UAM_ASSERT_EQ(fixture.app.chats.front().acp_queued_prompts.size(), std::size_t{1});
	}
#endif
}

UAM_TEST(ClaudeChildStreamingDefersWritesButPersistsItsTerminalBarrier)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (const std::string frame : {
	    R"({"type":"system","subtype":"task_progress","task_id":"owned-task","description":"Read fixture file"})",
	    R"({"type":"system","subtype":"task_updated","task_id":"owned-task","patch":{"status":"running"}})",
	    R"({"type":"assistant","uuid":"owned-child-message","parent_tool_use_id":"owned-tool","message":{"content":[{"type":"text","text":"Read fixture file."}]}})",
	    R"({"type":"user","uuid":"owned-child-result","parent_tool_use_id":"owned-tool","message":{"content":[{"type":"tool_result","tool_use_id":"owned-read","content":"Fixture bytes."}]}})"})
	{
		TempDir temp("uam-claude-progress-save");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat;
		chat.id = "owned-claude-progress";
		chat.provider_id = "claude-cli";
		chat.messages.push_back({MessageRole::User, "Inspect the fixture."});
		app.chats.push_back(chat);
		uam::AcpSessionState session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.protocol_kind = "claude-code-stream-json";
		session.running = true;
		session.processing = true;
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"system","subtype":"task_started","task_id":"owned-task","tool_use_id":"owned-tool","task_type":"local_agent","description":"Inspect fixture"})"));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		const fs::path file = AppPaths::UamChatFilePath(app.data_root, chat.id);
		const std::string before = ReadFile(file);
		const std::optional<std::string> revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame));
		UAM_ASSERT_EQ(ReadFile(file), before);
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, chat.id), revision);
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
		const std::vector<Message> expected = app.chats.front().messages;
		uam::FlushPendingChatSaves(app, true);
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(restored.has_value());
		UAM_ASSERT_EQ(restored->messages, expected);
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), frame));
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, session, app.chats.front(), R"({"type":"system","subtype":"task_notification","task_id":"owned-task","status":"completed","summary":"Fixture finished."})"));
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		const std::optional<ChatSession> final = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(final.has_value());
		UAM_ASSERT_EQ(final->messages, app.chats.front().messages);
		UAM_ASSERT_EQ(session.tool_calls.front().status, std::string("completed"));
	}
#endif
}
