#include "test_harness.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_OPENCODE_CLI || UAM_ENABLE_RUNTIME_CLAUDE_CLI
namespace
{
	struct ProviderBoundarySink
	{
		uam::AcpSessionState& session;
		fs::path captured;

		ProviderBoundarySink(uam::AcpSessionState& target, const fs::path& root)
		    : session(target), captured(root / "provider-permission-wire.jsonl")
		{
#if defined(_WIN32)
			const fs::path script = root / "capture-permission-wire.ps1";
			UAM_ASSERT(uam::io::WriteTextFile(script,
			    "param([string]$CapturePath)\r\n"
			    "$source = [Console]::OpenStandardInput()\r\n"
			    "$target = [IO.File]::Open($CapturePath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)\r\n"
			    "$buffer = New-Object byte[] 4096\r\n"
			    "try { while (($count = $source.Read($buffer, 0, $buffer.Length)) -gt 0) { $target.Write($buffer, 0, $count); $target.Flush() } } finally { $target.Dispose() }\r\n"));
			const std::vector<std::string> command{"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script.string(), captured.string()};
#else
			// Capture bytes only: no provider, requested tool, network operation or command is run.
			const std::vector<std::string> command{"/bin/sh", "-c", "cat > \"$1\"", "private-provider-permission-sink", captured.string()};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, root, command, &error));
		}

		~ProviderBoundarySink()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.StopStdioProcess(session, true);
			process.CloseStdioProcessHandles(session);
		}

		std::vector<nlohmann::json> ReadThroughResponse(const nlohmann::json& id) const
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				std::string bytes;
				(void)uam::io::TryReadTextFile(captured, bytes);
				std::vector<nlohmann::json> responses;
				std::size_t cursor = 0;
				bool found = false;
				for (std::size_t end = bytes.find('\n', cursor); end != std::string::npos; end = bytes.find('\n', cursor))
				{
					responses.push_back(nlohmann::json::parse(bytes.substr(cursor, end - cursor)));
					cursor = end + 1;
					const auto& response = responses.back();
					if (response.value("type", std::string()) == "control_response")
						found = found || response.at("response").at("request_id") == id;
					else found = found || response.at("id") == id;
				}
				if (found)
				{
					UAM_ASSERT_EQ(cursor, bytes.size());
					return responses;
				}
				std::this_thread::yield();
			}
			UAM_ASSERT(false && "Private provider permission response fence was not delivered.");
			return {};
		}
	};

	struct ProviderPermissionFixture
	{
		TempDir temp{"uam-provider-permission-boundary"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		std::unique_ptr<ProviderBoundarySink> sink;

		ProviderPermissionFixture(const std::string& provider, bool goal_review)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat;
			chat.id = "provider-permission-boundary";
			chat.provider_id = provider;
			chat.execution_host_id = "local";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode = "default";
			chat.command_safety_tier = "off";
			chat.memory_enabled = false;
			chat.interaction_at = "2026-01-01T00:00:00.000Z";
			chat.messages.push_back({MessageRole::User, "Inspect the private fixture"});
			chat.messages.push_back({MessageRole::Assistant, ""});
			app.chats.push_back(std::move(chat));
			auto owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = app.chats.front().id;
			session->provider_id = provider;
			session->protocol_kind = provider == "claude-cli" ? "claude-code-stream-json" : provider == "opencode-cli" ? "opencode-acp" : "gemini-acp";
			session->session_id = "private-provider-session";
			session->current_mode_id = "default";
			session->running = session->initialized = session->session_ready = session->processing = true;
			session->turn_serial = 1;
			session->turn_user_message_index = session->turn_first_user_message_index = 0;
			session->interaction_at = app.chats.front().interaction_at;
			session->goal_review_turn = goal_review;
			session->active_uam_agent_workspace_access = goal_review ? "" : "read";
			session->active_uam_agent_id = goal_review ? "" : "private-read-agent";
			app.acp_sessions.push_back(std::move(owned));
			sink = std::make_unique<ProviderBoundarySink>(*session, temp.root);
		}

		void GenericRequest(int id, const std::string& kind, const std::string& title = "Read private fixture", const std::string& once_label = "Allow once", const nlohmann::json& raw_input = {{"path", "private-fixture.txt"}})
		{
			const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "session/request_permission"},
			    {"params", {{"toolCall", {{"toolCallId", "private-tool-" + std::to_string(id)}, {"title", title}, {"kind", kind}, {"status", "pending"}, {"rawInput", raw_input}}},
			        {"options", {{{"optionId", "persist"}, {"name", "Allow always"}, {"kind", "allow_always"}},
			            {{"optionId", "once"}, {"name", once_label}, {"kind", "allow_once"}},
			            {{"optionId", "reject"}, {"name", "Reject once"}, {"kind", "reject_once"}}}}}}};
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), request.dump()));
		}

		void ClaudeRequest(const std::string& id, const std::string& tool, const nlohmann::json& input)
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
			    nlohmann::json{{"type", "control_request"}, {"request_id", id},
			        {"request", {{"subtype", "can_use_tool"}, {"tool_name", tool}, {"tool_use_id", "private-" + id}, {"input", input}}}}.dump()));
		}

		bool Resolve(const nlohmann::json& id, const std::string& option, bool cancelled, std::string& error)
		{
			return uam::ResolveAcpPermission(app, session->chat_id, id.dump(), option, cancelled, &error);
		}
	};

	nlohmann::json PendingBoundaryState(const ProviderPermissionFixture& fixture)
	{
		const auto& pending = fixture.session->pending_permission;
		nlohmann::json options = nlohmann::json::array(), tools = nlohmann::json::array();
		for (const auto& option : pending.options) options.push_back({option.id, option.name, option.kind});
		for (const auto& tool : fixture.session->tool_calls) tools.push_back({tool.id, tool.status, tool.approval_status});
		return {{"request", pending.request_id_json}, {"method", pending.provider_request_method}, {"params", pending.provider_request_params_json},
		    {"input", pending.provider_input_json}, {"kind", pending.kind}, {"content", pending.content}, {"options", options}, {"tools", tools},
		    {"queued", fixture.session->queued_permissions.size()}, {"waiting", fixture.session->waiting_for_permission},
		    {"lifecycle", fixture.session->lifecycle_state}, {"chatInteraction", fixture.app.chats.front().interaction_at},
		    {"sessionInteraction", fixture.session->interaction_at}, {"turn", fixture.session->turn_serial}};
	}

	void AssertGenericDecision(const std::vector<nlohmann::json>& responses, int id, const std::string& option, bool cancelled = false)
	{
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		const auto& response = responses.front();
		UAM_ASSERT_EQ(response.at("jsonrpc"), nlohmann::json("2.0"));
		UAM_ASSERT_EQ(response.at("id"), nlohmann::json(id));
		UAM_ASSERT(!response.contains("error"));
		const auto& outcome = response.at("result").at("outcome");
		UAM_ASSERT_EQ(outcome.at("outcome"), nlohmann::json(cancelled ? "cancelled" : "selected"));
		if (cancelled) UAM_ASSERT(!outcome.contains("optionId"));
		else UAM_ASSERT_EQ(outcome.at("optionId"), nlohmann::json(option));
	}

	void AssertClaudeDecision(const std::vector<nlohmann::json>& responses, const std::string& id, bool allowed, const nlohmann::json& input)
	{
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		const auto& response = responses.front();
		UAM_ASSERT_EQ(response.at("type"), nlohmann::json("control_response"));
		UAM_ASSERT_EQ(response.at("response").at("subtype"), nlohmann::json("success"));
		UAM_ASSERT_EQ(response.at("response").at("request_id"), nlohmann::json(id));
		const auto& decision = response.at("response").at("response");
		UAM_ASSERT_EQ(decision.at("behavior"), nlohmann::json(allowed ? "allow" : "deny"));
		if (allowed) UAM_ASSERT_EQ(decision.at("updatedInput"), input);
		else UAM_ASSERT(!decision.contains("updatedInput"));
	}
}
#endif

UAM_TEST(AcpCapturedReadRefusesOfferedPersistentGrantAndPreservesOneShotDecisions)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	std::vector<bool> persistent_refused;
	for (bool goal_review : {false, true})
	{
		ProviderPermissionFixture fixture("gemini-cli", goal_review);
		fixture.GenericRequest(31, "read");
		UAM_ASSERT(fixture.session->waiting_for_permission);
		UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("31"));
		UAM_ASSERT(std::ranges::any_of(fixture.session->pending_permission.options, [](const auto& option) { return option.id == "persist" && option.kind == "allow_always"; }));
		const auto before = PendingBoundaryState(fixture);
		const auto messages = fixture.app.chats.front().messages;
		const auto saves = fixture.app.pending_chat_save_at_by_chat_id;
		std::string error;
		const bool refused = !fixture.Resolve(31, "persist", false, error);
		persistent_refused.push_back(refused);
		if (refused)
		{
			UAM_ASSERT(!error.empty());
			UAM_ASSERT_EQ(PendingBoundaryState(fixture), before);
			UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
			UAM_ASSERT_EQ(fixture.app.pending_chat_save_at_by_chat_id, saves);
			UAM_ASSERT(fixture.Resolve(31, "reject", false, error));
		}
		AssertGenericDecision(fixture.sink->ReadThroughResponse(31), 31, refused ? "reject" : "persist");
		for (const std::string decision : {"once", "reject", "cancel"})
		{
			ProviderPermissionFixture control("gemini-cli", goal_review);
			control.GenericRequest(32, "read");
			UAM_ASSERT(control.session->waiting_for_permission);
			UAM_ASSERT(control.Resolve(32, decision == "cancel" ? "" : decision, decision == "cancel", error));
			AssertGenericDecision(control.sink->ReadThroughResponse(32), 32, decision, decision == "cancel");
		}
	}
	UAM_ASSERT_EQ(persistent_refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(persistent_refused, [](bool refused) { return refused; }));
#endif
}

UAM_TEST(OpenCodeDoomLoopActionCannotUseMisleadingDenialLabelToEscapeReadCeiling)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	std::vector<bool> edit_refused;
	for (bool goal_review : {false, true})
	{
		ProviderPermissionFixture fixture("opencode-cli", goal_review);
		fixture.GenericRequest(41, "edit", "doom_loop", "Allow once; deny future prompts");
		UAM_ASSERT(fixture.session->waiting_for_permission);
		UAM_ASSERT(ProviderRuntimeRegistry::ResolveById("opencode-cli").AcpPermissionRequiresUserDecision(fixture.session->pending_permission));
		const auto before = PendingBoundaryState(fixture);
		const auto messages = fixture.app.chats.front().messages;
		const auto saves = fixture.app.pending_chat_save_at_by_chat_id;
		std::string error;
		const bool refused = !fixture.Resolve(41, "once", false, error);
		edit_refused.push_back(refused);
		if (refused)
		{
			UAM_ASSERT(!error.empty());
			UAM_ASSERT_EQ(PendingBoundaryState(fixture), before);
			UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
			UAM_ASSERT_EQ(fixture.app.pending_chat_save_at_by_chat_id, saves);
			UAM_ASSERT(fixture.Resolve(41, "reject", false, error));
		}
		// The denial is an ordered wire fence: no earlier selected allow may precede it.
		AssertGenericDecision(fixture.sink->ReadThroughResponse(41), 41, refused ? "reject" : "once");
		ProviderPermissionFixture nonaction("opencode-cli", goal_review);
		nonaction.GenericRequest(42, "other", "doom_loop", "Allow once", {{"tool", "read"}, {"input", {{"filePath", "private-fixture.txt"}}}});
		UAM_ASSERT(nonaction.session->waiting_for_permission);
		UAM_ASSERT(nonaction.app.permission_review_tasks.empty());
		UAM_ASSERT_EQ(nonaction.session->pending_permission.kind, std::string("other"));
		UAM_ASSERT(nonaction.Resolve(42, "once", false, error));
		AssertGenericDecision(nonaction.sink->ReadThroughResponse(42), 42, "once");
	}
	UAM_ASSERT_EQ(edit_refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(edit_refused, [](bool refused) { return refused; }));
#endif
}

UAM_TEST(ClaudeCapturedReadRecognizesNativeReadToolsAndRejectsMutationOrUnknownTools)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	const std::vector<std::pair<std::string, nlohmann::json>> tools = {
	    {"Read", {{"file_path", "private-fixture.txt"}, {"offset", 1}}},
	    {"Grep", {{"pattern", "needle"}, {"path", "private-fixture.txt"}}},
	    {"Glob", {{"pattern", "*.txt"}}},
	    {"WebFetch", {{"url", "https://example.invalid/private-fixture"}, {"prompt", "Inspect fixture"}}},
	    {"WebSearch", {{"query", "private fixture"}}},
	    {"Write", {{"file_path", "private-output.txt"}, {"content", "Do not execute"}}},
	    {"UnknownPrivateTool", {{"opaque", "private fixture"}}}};
	std::vector<bool> read_allowed, mutations_denied;
	for (bool goal_review : {false, true})
	{
		for (std::size_t index = 0; index < tools.size(); ++index)
		{
			const auto& [tool, input] = tools[index];
			const bool read = index < 5;
			const std::string id = "native-private-" + std::to_string(index);
			ProviderPermissionFixture fixture("claude-cli", goal_review);
			fixture.ClaudeRequest(id, tool, input);
			std::string error;
			bool allowed = false;
			if (fixture.session->waiting_for_permission)
			{
				UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, nlohmann::json(id).dump());
				const auto before = PendingBoundaryState(fixture);
				allowed = fixture.Resolve(id, "allow", false, error);
				if (!allowed)
				{
					UAM_ASSERT_EQ(PendingBoundaryState(fixture), before);
					UAM_ASSERT(fixture.Resolve(id, "deny", false, error));
				}
			}
			// Automatic denials and manual decisions are both observed on the real native wire.
			AssertClaudeDecision(fixture.sink->ReadThroughResponse(id), id, allowed, input);
			if (read) read_allowed.push_back(allowed);
			else mutations_denied.push_back(!allowed);
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
		}
	}
	UAM_ASSERT_EQ(read_allowed.size(), std::size_t(10));
	UAM_ASSERT_EQ(mutations_denied.size(), std::size_t(4));
	UAM_ASSERT(std::ranges::all_of(read_allowed, [](bool allowed) { return allowed; }));
	UAM_ASSERT(std::ranges::all_of(mutations_denied, [](bool denied) { return denied; }));
#endif
}


UAM_TEST(OpenCodeCapturedReadDoomLoopRequiresNativeQuestionMetadata)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	// Pinned official v1.18.35: session/processor.ts348-355 emits metadata
	// {tool,input}; session/tools.ts393 emits {} for an MCP action, including
	// server doom + tool loop, whose mcp/catalog.ts112 name is doom_loop.
	std::vector<bool> mcp_refused;
	for (bool goal_review : {false, true})
	{
		for (bool builtin_question : {true, false})
		{
			ProviderPermissionFixture fixture("opencode-cli", goal_review);
			const nlohmann::json input = builtin_question
			    ? nlohmann::json{{"tool", "read"}, {"input", {{"filePath", "private-fixture.txt"}}}}
			    : nlohmann::json::object();
			const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", 71}, {"method", "session/request_permission"},
			    {"params", {{"toolCall", {{"toolCallId", "private-native-loop-or-mcp-call"}, {"title", "doom_loop"},
			        {"kind", "other"}, {"status", "pending"}, {"rawInput", input}}},
			        {"options", {{{"optionId", "once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
			            {{"optionId", "always"}, {"name", "Always allow"}, {"kind", "allow_always"}},
			            {{"optionId", "reject"}, {"name", "Reject"}, {"kind", "reject_once"}}}}}}};
			UAM_ASSERT(uam::ProcessAcpLineForTests(fixture.app, *fixture.session, fixture.app.chats.front(), request.dump()));
			UAM_ASSERT(fixture.session->waiting_for_permission);
			UAM_ASSERT(fixture.app.permission_review_tasks.empty());
			std::string error;
			const bool accepted = fixture.Resolve(71, "once", false, error);
			if (builtin_question) UAM_ASSERT(accepted);
			else
			{
				mcp_refused.push_back(!accepted);
				if (!accepted)
				{
					UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, std::string("71"));
					UAM_ASSERT(fixture.Resolve(71, "reject", false, error));
				}
			}
			// An explicit denial, when needed, is the real ordered output fence.
			AssertGenericDecision(fixture.sink->ReadThroughResponse(71), 71, accepted ? "once" : "reject");
		}
	}
	UAM_ASSERT_EQ(mcp_refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(mcp_refused, [](bool value) { return value; }));
#endif
}
