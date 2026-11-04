#include "test_harness.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	using ReplayJson = nlohmann::json;
	const std::string kReplayNativeSession = "08f433b8-6172-4ef6-b3b7-6c2846412b17";

	struct ReplayPermissionSink
	{
		uam::AcpSessionState& session;
		fs::path captured;

		ReplayPermissionSink(uam::AcpSessionState& target, const fs::path& root, std::size_t capture_generation) : session(target), captured(root / ("replay-wire-" + std::to_string(capture_generation) + ".jsonl"))
		{
#if defined(_WIN32)
			const fs::path script = root / "capture-replay-wire.ps1";
			UAM_ASSERT(uam::io::WriteTextFile(script,
			    "param([string]$CapturePath)\r\n"
			    "$source = [Console]::OpenStandardInput()\r\n"
			    "$target = [IO.File]::Open($CapturePath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)\r\n"
			    "$buffer = New-Object byte[] 4096\r\n"
			    "try { while (($count = $source.Read($buffer, 0, $buffer.Length)) -gt 0) { $target.Write($buffer, 0, $count); $target.Flush() } } finally { $target.Dispose() }\r\n"));
			const std::vector<std::string> command{"powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script.string(), captured.string()};
#else
			const std::vector<std::string> command{"/bin/sh", "-c", "cat > \"$1\"", "private-replay-sink", captured.string()};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, root, command, &error));
		}

		~ReplayPermissionSink()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.StopStdioProcess(session, true);
			process.CloseStdioProcessHandles(session);
		}

		std::vector<ReplayJson> ReadThroughResponse(const ReplayJson& id) const
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				std::string bytes;
				(void)uam::io::TryReadTextFile(captured, bytes);
				std::vector<ReplayJson> responses;
				std::size_t cursor = 0;
				bool found = false;
				for (std::size_t end = bytes.find('\n', cursor); end != std::string::npos; end = bytes.find('\n', cursor))
				{
					responses.push_back(ReplayJson::parse(bytes.substr(cursor, end - cursor)));
					cursor = end + 1;
					const auto& response = responses.back();
					const ReplayJson response_id = response.value("type", std::string()) == "control_response" ? response.at("response").at("request_id") : response.at("id");
					found = found || response_id == id;
				}
				if (found)
				{
					UAM_ASSERT_EQ(cursor, bytes.size());
					return responses;
				}
				std::this_thread::yield();
			}
			UAM_ASSERT(false && "Private saved-read replay fence was not delivered.");
			return {};
		}

		std::vector<ReplayJson> ReadSettled()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.CloseStdioProcessInput(session);
			bool exited = false;
			int exit_code = -1;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (process.PollStdioProcessExited(session, &exit_code)) { exited = true; break; }
				std::this_thread::yield();
			}
			UAM_ASSERT(exited);
			UAM_ASSERT_EQ(exit_code, 0);
			std::string bytes;
			UAM_ASSERT(uam::io::TryReadTextFile(captured, bytes));
			std::vector<ReplayJson> responses;
			std::size_t cursor = 0;
			for (std::size_t end = bytes.find('\n', cursor); end != std::string::npos; end = bytes.find('\n', cursor))
			{
				responses.push_back(ReplayJson::parse(bytes.substr(cursor, end - cursor)));
				cursor = end + 1;
			}
			UAM_ASSERT_EQ(cursor, bytes.size());
			return responses;
		}
	};

	ReplayJson ReplayRequestId(const ReplayJson& request)
	{
		return request.contains("type") ? request.at("request_id") : request.at("id");
	}

	ReplayJson ReplayResponseId(const ReplayJson& response)
	{
		return response.value("type", std::string()) == "control_response" ? response.at("response").at("request_id") : response.at("id");
	}

	struct PermissionReplayFixture
	{
		TempDir temp{"uam-permission-replay-boundary"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		std::unique_ptr<ReplayPermissionSink> sink;
		std::vector<ReplayJson> original_responses;
		std::size_t original_decisions = 0;
		std::size_t attachment_generation = 0;

		explicit PermissionReplayFixture(const std::string& provider)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat;
			chat.id = "permission-replay-boundary";
			chat.provider_id = provider;
			chat.native_session_id = kReplayNativeSession;
			chat.workspace_directory = temp.root.string();
			// This label selects genuine remote-response persistence while the process is
			// exclusively a private local byte sink. No SSH or provider is launched.
			chat.execution_host_id = "private-ssh-replay-host";
			chat.approval_mode = "default";
			chat.command_safety_tier = "off";
			chat.memory_enabled = false;
			chat.messages.push_back({MessageRole::User, "Inspect private fixture"});
			chat.messages.push_back({MessageRole::Assistant, ""});
			app.chats.push_back(std::move(chat));
			Attach(false, false);
		}

		void Attach(bool captured_read, bool goal_review)
		{
			auto owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = app.chats.front().id;
			session->provider_id = app.chats.front().provider_id;
			session->protocol_kind = session->provider_id == "claude-cli" ? "claude-code-stream-json" : session->provider_id == "codex-cli" ? "codex-app-server" : "gemini-acp";
			session->session_id = session->codex_thread_id = kReplayNativeSession;
			session->current_mode_id = "default";
			session->running = session->initialized = session->session_ready = session->processing = true;
			session->turn_serial = 1;
			session->turn_user_message_index = session->turn_first_user_message_index = 0;
			session->goal_review_turn = captured_read && goal_review;
			session->active_uam_agent_workspace_access = captured_read && !goal_review ? "read" : "";
			session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-agent";
			app.acp_sessions.push_back(std::move(owned));
			// A new process may not have opened its capture yet; never read the prior attachment.
			sink = std::make_unique<ReplayPermissionSink>(*session, temp.root, ++attachment_generation);
		}

		ReplayJson Request(int id, bool write, const std::string& read_tool = "Read") const
		{
			const std::string provider = app.chats.front().provider_id;
			if (provider == "claude-cli")
			{
				ReplayJson input = write ? ReplayJson{{"file_path", "private-output.txt"}, {"content", "Do not execute"}} :
				    read_tool == "Grep" ? ReplayJson{{"pattern", "needle"}, {"path", "private-fixture.txt"}} : ReplayJson{{"file_path", "private-fixture.txt"}};
				return {{"type", "control_request"}, {"request_id", "private-replay-" + std::to_string(id)},
				    {"request", {{"subtype", "can_use_tool"}, {"tool_name", write ? "Write" : read_tool}, {"tool_use_id", "private-tool-" + std::to_string(id)}, {"input", input}}}};
			}
			if (provider == "codex-cli")
				return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "item/commandExecution/requestApproval"},
				    {"params", {{"threadId", kReplayNativeSession}, {"itemId", "private-command-" + std::to_string(id)},
				        {"command", write ? "echo fixture > private-output.txt" : "pwd"}, {"cwd", temp.root.string()},
				        {"availableDecisions", {"accept", "acceptForSession", "decline", "cancel"}}}}};
			return {{"jsonrpc", "2.0"}, {"id", id}, {"method", "session/request_permission"},
			    {"params", {{"toolCall", {{"toolCallId", "private-tool-" + std::to_string(id)}, {"title", "Private fixture permission"},
				    {"kind", write ? "edit" : "read"}, {"status", "pending"}, {"rawInput", {{"path", "private-fixture.txt"}}}}},
				    {"options", {{{"optionId", "once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
				        {{"optionId", "persist"}, {"name", "Allow always"}, {"kind", "allow_always"}},
				        {{"optionId", "reject"}, {"name", "Reject once"}, {"kind", "reject_once"}}}}}}};
		}

		std::string Allow() const { return session->provider_id == "claude-cli" ? "allow" : session->provider_id == "codex-cli" ? "accept" : "once"; }
		std::string Deny() const { return session->provider_id == "claude-cli" ? "deny" : session->provider_id == "codex-cli" ? "decline" : "reject"; }
		std::string Persistent() const { return session->provider_id == "codex-cli" ? "acceptForSession" : "persist"; }
		void Feed(const ReplayJson& request) { UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), request.dump())); }
		bool Resolve(const ReplayJson& request, const std::string& option, bool cancelled, std::string& error)
		{
			return uam::ResolveAcpPermission(app, session->chat_id, ReplayRequestId(request).dump(), option, cancelled, &error);
		}
		void Record(const ReplayJson& request, const std::string& option, bool cancelled = false)
		{
			Feed(request);
			UAM_ASSERT_EQ(session->pending_permission.request_id_json, ReplayRequestId(request).dump());
			std::string error;
			UAM_ASSERT(Resolve(request, option, cancelled, error));
			++original_decisions;
		}
		void Restore(bool goal_review)
		{
			original_responses = sink->ReadSettled();
			UAM_ASSERT_EQ(original_responses.size(), original_decisions);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
			UAM_ASSERT(saved);
			UAM_ASSERT_EQ(saved->execution_host_id, std::string("private-ssh-replay-host"));
			UAM_ASSERT_EQ(saved->remote_interaction_responses.size(), original_decisions);
			for (const auto& response : original_responses)
			{
				const std::string id = ReplayResponseId(response).dump();
				const auto receipt = std::ranges::find(saved->remote_interaction_responses, id, &uam::AcpRemoteInteractionResponseState::request_id_json);
				UAM_ASSERT(receipt != saved->remote_interaction_responses.end());
				UAM_ASSERT_EQ(ReplayJson::parse(receipt->response_json), response);
			}
			sink.reset();
			app.acp_sessions.clear();
			app.chats.front() = *saved;
			Attach(true, goal_review);
		}
		bool Approved(const ReplayJson& response) const
		{
			if (session->provider_id == "claude-cli") return response.at("response").at("response").at("behavior") == "allow";
			if (session->provider_id == "codex-cli") return response.at("result").at("decision") == "accept" || response.at("result").at("decision") == "acceptForSession";
			const auto& outcome = response.at("result").at("outcome");
			return outcome.at("outcome") == "selected" && (outcome.at("optionId") == "once" || outcome.at("optionId") == "persist");
		}
		void AssertNoApproval(const std::vector<ReplayJson>& responses, const ReplayJson& request) const
		{
			for (const auto& response : responses)
				if (ReplayResponseId(response) == ReplayRequestId(request)) UAM_ASSERT(!Approved(response));
		}
		void AssertExactReplay(const std::vector<ReplayJson>& responses, const ReplayJson& request) const
		{
			const auto response = std::ranges::find_if(responses, [&](const auto& value) { return ReplayResponseId(value) == ReplayRequestId(request); });
			UAM_ASSERT(response != responses.end());
			UAM_ASSERT_EQ(std::ranges::count_if(responses, [&](const auto& value) { return ReplayResponseId(value) == ReplayRequestId(request); }), std::ptrdiff_t(1));
			const auto original = std::ranges::find_if(original_responses, [&](const auto& value) { return ReplayResponseId(value) == ReplayRequestId(request); });
			UAM_ASSERT(original != original_responses.end());
			UAM_ASSERT_EQ(*response, *original);
		}
	};

	void AssertRestrictedPermissionReplay(const std::string& provider, bool goal_review)
	{
		PermissionReplayFixture fixture(provider);
		const auto write = fixture.Request(11, true), read = fixture.Request(12, false), persistent = fixture.Request(13, false),
		    denied = fixture.Request(14, true), cancelled = fixture.Request(15, false);
		fixture.Record(write, fixture.Allow());
		fixture.Record(read, fixture.Allow());
		const bool has_persistent_offer = provider != "claude-cli";
		if (has_persistent_offer) fixture.Record(persistent, fixture.Persistent());
		fixture.Record(denied, fixture.Deny());
		fixture.Record(cancelled, "", true);
		std::optional<ReplayJson> grep;
		if (provider == "claude-cli") { grep = fixture.Request(16, false, "Grep"); fixture.Record(*grep, fixture.Allow()); }
		fixture.Restore(goal_review);
		fixture.Feed(write);
		bool persistent_is_fresh = false, persistent_cancelled = false;
		if (has_persistent_offer)
		{
			fixture.Feed(persistent);
			persistent_is_fresh = fixture.session->waiting_for_permission && fixture.session->pending_permission.request_id_json == ReplayRequestId(persistent).dump();
			if (persistent_is_fresh)
			{
				std::string error;
				persistent_cancelled = fixture.Resolve(persistent, "", true, error);
			}
		}
		fixture.Feed(read);
		fixture.Feed(denied);
		fixture.Feed(cancelled);
		if (grep) fixture.Feed(*grep);
		const auto replayed = fixture.sink->ReadSettled();
		fixture.AssertNoApproval(replayed, write);
		if (has_persistent_offer) fixture.AssertNoApproval(replayed, persistent);
		fixture.AssertExactReplay(replayed, read);
		fixture.AssertExactReplay(replayed, denied);
		fixture.AssertExactReplay(replayed, cancelled);
		if (grep) fixture.AssertExactReplay(replayed, *grep);
		if (has_persistent_offer)
		{
			UAM_ASSERT(persistent_is_fresh);
			UAM_ASSERT(persistent_cancelled);
			const auto replacement = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
			UAM_ASSERT(replacement);
			const auto receipt = std::ranges::find(replacement->remote_interaction_responses, ReplayRequestId(persistent).dump(), &uam::AcpRemoteInteractionResponseState::request_id_json);
			UAM_ASSERT(receipt != replacement->remote_interaction_responses.end());
			const auto replacement_response = ReplayJson::parse(receipt->response_json);
			UAM_ASSERT(!fixture.Approved(replacement_response));
			UAM_ASSERT(std::ranges::any_of(replayed, [&](const auto& response) { return response == replacement_response; }));
			UAM_ASSERT_EQ(std::ranges::count_if(replayed, [&](const auto& response) { return ReplayResponseId(response) == ReplayRequestId(persistent); }), std::ptrdiff_t(1));
		}
		UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
	}
}
#endif

UAM_TEST(AcpSavedPermissionReplayRevalidatesDecodedReadAndPersistentScope)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (bool goal_review : {false, true}) AssertRestrictedPermissionReplay("gemini-cli", goal_review);
#endif
}

UAM_TEST(ClaudeSavedPermissionReplayRevalidatesNativeToolAndInput)
{
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	for (bool goal_review : {false, true}) AssertRestrictedPermissionReplay("claude-cli", goal_review);
#endif
}

UAM_TEST(CodexSavedPermissionReplayRevalidatesNativeDecisionAndScope)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (bool goal_review : {false, true}) AssertRestrictedPermissionReplay("codex-cli", goal_review);
#endif
}

UAM_TEST(CapturedReadSavedPermissionCannotApproveDuringPendingCancel)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_RUNTIME_CODEX_CLI
	std::vector<std::string> providers;
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	providers.push_back("gemini-cli");
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	providers.push_back("claude-cli");
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	providers.push_back("codex-cli");
#endif
	for (const auto& provider : providers)
	{
		PermissionReplayFixture fixture(provider);
		const auto read = fixture.Request(21, false), cancelled = fixture.Request(22, false);
		fixture.Record(read, fixture.Allow());
		fixture.Record(cancelled, "", true);
		fixture.Restore(false);
		fixture.session->cancel_requested = true;
		fixture.Feed(read);
		fixture.Feed(cancelled);
		const auto replayed = fixture.sink->ReadSettled();
		fixture.AssertNoApproval(replayed, read);
		fixture.AssertNoApproval(replayed, cancelled);
		if (provider == "codex-cli") UAM_ASSERT(replayed.empty());
		else UAM_ASSERT_EQ(replayed.size(), std::size_t(2));
		UAM_ASSERT(!fixture.session->waiting_for_permission);
	}
#endif
}

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{
	ReplayJson ReplayReceiptVector(const ChatSession& chat)
	{
		ReplayJson rows = ReplayJson::array();
		for (const auto& receipt : chat.remote_interaction_responses)
			rows.push_back({receipt.request_id_json, receipt.response_json});
		return rows;
	}

	ReplayJson ReplayPendingAndPolicy(const PermissionReplayFixture& fixture)
	{
		const auto& pending = fixture.session->pending_permission;
		ReplayJson options = ReplayJson::array();
		for (const auto& option : pending.options) options.push_back({option.id, option.name, option.kind});
		return {{"pending", {pending.request_id_json, pending.provider_request_method, pending.provider_request_kind,
			pending.codex_approval_payload_json, pending.provider_input_json, pending.provider_request_params_json,
			pending.tool_call_id, pending.title, pending.kind, pending.status, pending.content, pending.safety_risk, pending.safety_tier}},
			{"options", options}, {"requiresApproval", pending.safety_requires_approval}, {"versionControlled", pending.version_controlled_workspace},
			{"waiting", fixture.session->waiting_for_permission}, {"queued", fixture.session->queued_permissions.size()},
			{"approvalMode", fixture.app.chats.front().approval_mode}, {"safetyTier", fixture.app.chats.front().command_safety_tier},
			{"currentMode", fixture.session->current_mode_id}, {"goalReview", fixture.session->goal_review_turn},
			{"agentAccess", fixture.session->active_uam_agent_workspace_access}, {"agentId", fixture.session->active_uam_agent_id},
			{"turn", fixture.session->turn_serial}, {"cancelRequested", fixture.session->cancel_requested}};
	}
}
#endif

UAM_TEST(AcpSavedPermissionReplacementSaveFailurePreservesOldDecisionUntilGenuineRetry)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	for (bool goal_review : {false, true})
	{
		PermissionReplayFixture fixture("gemini-cli");
		const auto persistent = fixture.Request(31, false), read_fence = fixture.Request(32, false);
		fixture.Record(persistent, fixture.Persistent());
		fixture.Record(read_fence, fixture.Allow());
		fixture.Restore(goal_review);
		fixture.Feed(persistent);
		UAM_ASSERT(fixture.session->waiting_for_permission);
		UAM_ASSERT_EQ(fixture.session->pending_permission.request_id_json, ReplayRequestId(persistent).dump());
		const auto before_receipts = ReplayReceiptVector(fixture.app.chats.front());
		UAM_ASSERT_EQ(before_receipts.size(), std::size_t(2));
		const auto before_pending = ReplayPendingAndPolicy(fixture);
		const auto before_messages = fixture.app.chats.front().messages;
		const fs::path blocked_chats = fixture.app.data_root / "chats";
		const fs::path retained_chats = fixture.app.data_root / "fixture-retained-chats";
		fs::rename(blocked_chats, retained_chats);
		UAM_ASSERT(uam::io::WriteTextFile(blocked_chats, "Private replacement save failure"));
		std::string error;
		UAM_ASSERT(!fixture.Resolve(persistent, "", true, error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(ReplayReceiptVector(fixture.app.chats.front()), before_receipts);
		UAM_ASSERT_EQ(ReplayPendingAndPolicy(fixture), before_pending);
		UAM_ASSERT_EQ(fixture.app.chats.front().messages, before_messages);
		// This existing safe response traverses the real transport after the failed save.
		// Seeing exactly it proves that no earlier stale approval or cancellation was written.
		fixture.Feed(read_fence);
		const auto failed_wire = fixture.sink->ReadThroughResponse(ReplayRequestId(read_fence));
		UAM_ASSERT_EQ(failed_wire.size(), std::size_t(1));
		fixture.AssertExactReplay(failed_wire, read_fence);
		UAM_ASSERT_EQ(ReplayReceiptVector(fixture.app.chats.front()), before_receipts);
		UAM_ASSERT_EQ(ReplayPendingAndPolicy(fixture), before_pending);
		UAM_ASSERT(fs::remove(blocked_chats));
		fs::rename(retained_chats, blocked_chats);
		const auto old_durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(old_durable);
		UAM_ASSERT_EQ(ReplayReceiptVector(*old_durable), before_receipts);
		UAM_ASSERT(fixture.Resolve(persistent, "", true, error));
		const auto retry_wire = fixture.sink->ReadSettled();
		UAM_ASSERT_EQ(retry_wire.size(), std::size_t(2));
		UAM_ASSERT_EQ(ReplayResponseId(retry_wire.front()), ReplayRequestId(read_fence));
		UAM_ASSERT_EQ(ReplayResponseId(retry_wire.back()), ReplayRequestId(persistent));
		fixture.AssertExactReplay(retry_wire, read_fence);
		fixture.AssertNoApproval(retry_wire, persistent);
		const ReplayJson cancellation = {{"jsonrpc", "2.0"}, {"id", 31}, {"result", {{"outcome", {{"outcome", "cancelled"}}}}}};
		UAM_ASSERT_EQ(retry_wire.back(), cancellation);
		UAM_ASSERT(!fixture.session->waiting_for_permission);
		const auto replaced = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(replaced);
		UAM_ASSERT_EQ(replaced->remote_interaction_responses.size(), std::size_t(2));
		const auto replaced_row = std::ranges::find(replaced->remote_interaction_responses, ReplayRequestId(persistent).dump(), &uam::AcpRemoteInteractionResponseState::request_id_json);
		UAM_ASSERT(replaced_row != replaced->remote_interaction_responses.end());
		UAM_ASSERT_EQ(ReplayJson::parse(replaced_row->response_json), cancellation);
		const auto kept_read = std::ranges::find(replaced->remote_interaction_responses, ReplayRequestId(read_fence).dump(), &uam::AcpRemoteInteractionResponseState::request_id_json);
		UAM_ASSERT(kept_read != replaced->remote_interaction_responses.end());
		UAM_ASSERT_EQ(ReplayJson::parse(kept_read->response_json), retry_wire.front());
	}
#endif
}
