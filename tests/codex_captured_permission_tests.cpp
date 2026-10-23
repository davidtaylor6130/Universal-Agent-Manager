#include "test_harness.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	const std::string kCapturedPermissionThread = "08f433b8-6172-4ef6-b3b7-6c2846412b17";

	class CapturedCodexPermissionWire
	{
	public:
		CapturedCodexPermissionWire(uam::AcpSessionState& session, const fs::path& directory) : m_session(session)
		{
#if defined(_WIN32)
			const std::vector<std::string> sink = {"cmd.exe", "/C", "more"};
#else
			const std::vector<std::string> sink = {"/bin/cat"};
#endif
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, directory, sink, &error));
		}

		~CapturedCodexPermissionWire()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.StopStdioProcess(m_session, true);
			process.CloseStdioProcessHandles(m_session);
		}

		std::vector<nlohmann::json> ReadSettled()
		{
			auto& process = PlatformServicesFactory::Instance().process_service;
			process.CloseStdioProcessInput(m_session);
			std::string wire, error;
			char bytes[8192];
			bool exited = false;
			int exit_code = -1;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				const std::ptrdiff_t count = process.ReadStdioProcessStdout(m_session, bytes, sizeof(bytes), &error);
				if (count > 0) wire.append(bytes, static_cast<std::size_t>(count));
				const bool observed_exit = process.PollStdioProcessExited(m_session, &exit_code);
				if (observed_exit && count <= 0) { exited = true; break; }
				std::this_thread::yield();
			}
			UAM_ASSERT(exited);
			UAM_ASSERT_EQ(exit_code, 0);
			std::vector<nlohmann::json> responses;
			std::size_t cursor = 0;
			for (std::size_t end = wire.find('\n', cursor); end != std::string::npos; end = wire.find('\n', cursor))
			{
				responses.push_back(nlohmann::json::parse(wire.substr(cursor, end - cursor)));
				cursor = end + 1;
			}
			UAM_ASSERT_EQ(cursor, wire.size());
			return responses;
		}

	private:
		uam::AcpSessionState& m_session;
	};

	struct CapturedCodexPermissionFixture
	{
		TempDir temp{"uam-codex-captured-permission"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;
		std::unique_ptr<CapturedCodexPermissionWire> wire;

		explicit CapturedCodexPermissionFixture(bool goal_review, bool unrestricted = false)
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			ChatSession chat;
			chat.id = "codex-captured-permission";
			chat.provider_id = "codex-cli";
			chat.native_session_id = kCapturedPermissionThread;
			chat.workspace_directory = temp.root.string();
			chat.execution_host_id = "local";
			chat.approval_mode = "default";
			chat.command_safety_tier = "off";
			chat.memory_enabled = false;
			chat.interaction_at = "2026-01-01T00:00:00.000Z";
			chat.messages.push_back({MessageRole::User, "Inspect the private Codex fixture"});
			chat.messages.push_back({MessageRole::Assistant, ""});
			app.chats.push_back(std::move(chat));
			auto owned = std::make_unique<uam::AcpSessionState>();
			session = owned.get();
			session->chat_id = app.chats.front().id;
			session->provider_id = "codex-cli";
			session->protocol_kind = "codex-app-server";
			session->session_id = session->codex_thread_id = kCapturedPermissionThread;
			session->current_mode_id = "default";
			session->running = session->initialized = session->session_ready = session->processing = true;
			session->turn_serial = 1;
			session->turn_user_message_index = session->turn_first_user_message_index = 0;
			session->goal_review_turn = !unrestricted && goal_review;
			session->active_uam_agent_workspace_access = !unrestricted && !goal_review ? "read" : "";
			session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-agent";
			session->interaction_at = app.chats.front().interaction_at;
			app.acp_sessions.push_back(std::move(owned));
			wire = std::make_unique<CapturedCodexPermissionWire>(*session, temp.root);
		}

		void Command(const std::string& command)
		{
			// Native command decisions are supplied in the real app-server request, not injected into pending state.
			const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", 71}, {"method", "item/commandExecution/requestApproval"},
			    {"params", {{"threadId", kCapturedPermissionThread}, {"itemId", "private-command"}, {"command", command},
			        {"cwd", temp.root.string()}, {"availableDecisions", {"accept", "acceptForSession", "decline", "cancel"}}}}};
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), request.dump()));
		}

		void Edit()
		{
			UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
			    nlohmann::json{{"jsonrpc", "2.0"}, {"id", 71}, {"method", "item/fileChange/requestApproval"},
			        {"params", {{"threadId", kCapturedPermissionThread}, {"itemId", "private-edit"},
			            {"reason", "Change private-output.txt"}, {"grantRoot", temp.root.string()}}}}.dump()));
		}

		bool Resolve(const std::string& decision, bool cancelled, std::string& error)
		{
			return uam::ResolveAcpPermission(app, session->chat_id, "71", decision, cancelled, &error);
		}
	};

	nlohmann::json CapturedCodexPending(const CapturedCodexPermissionFixture& fixture)
	{
		const auto& pending = fixture.session->pending_permission;
		nlohmann::json options = nlohmann::json::array(), tools = nlohmann::json::array();
		for (const auto& option : pending.options) options.push_back({option.id, option.name, option.kind});
		for (const auto& tool : fixture.session->tool_calls) tools.push_back({tool.id, tool.status, tool.approval_status});
		return {{"id", pending.request_id_json}, {"method", pending.provider_request_method}, {"kind", pending.provider_request_kind},
		    {"toolKind", pending.kind}, {"content", pending.content}, {"payload", pending.codex_approval_payload_json}, {"options", options},
		    {"tools", tools}, {"queued", fixture.session->queued_permissions.size()}, {"waiting", fixture.session->waiting_for_permission},
		    {"lifecycle", fixture.session->lifecycle_state}, {"chatInteraction", fixture.app.chats.front().interaction_at},
		    {"sessionInteraction", fixture.session->interaction_at}, {"turn", fixture.session->turn_serial}};
	}

	void AssertCapturedCodexDecision(const std::vector<nlohmann::json>& responses, const std::string& decision)
	{
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		UAM_ASSERT_EQ(responses.front().at("jsonrpc"), nlohmann::json("2.0"));
		UAM_ASSERT_EQ(responses.front().at("id"), nlohmann::json(71));
		UAM_ASSERT(!responses.front().contains("error"));
		UAM_ASSERT_EQ(responses.front().at("result"), (nlohmann::json{{"decision", decision}}));
	}
}
#endif

UAM_TEST(CodexCapturedReadPreservesNativeOneShotAndDenialDecisions)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	for (bool goal_review : {false, true})
	{
		for (const std::string decision : {"accept", "decline", "cancel", "cancelled", "cancel-by-user"})
		{
			CapturedCodexPermissionFixture fixture(goal_review);
			fixture.Command("pwd");
			UAM_ASSERT(fixture.session->waiting_for_permission);
			UAM_ASSERT_EQ(fixture.session->pending_permission.provider_request_kind, std::string("codex-command"));
			const bool cancelled = decision == "cancel-by-user";
			if (!cancelled)
				UAM_ASSERT(std::ranges::any_of(fixture.session->pending_permission.options, [&](const auto& option) { return option.id == decision; }));
			std::string error;
			UAM_ASSERT(fixture.Resolve(cancelled ? "" : decision, cancelled, error));
			const std::string expected = cancelled || decision == "cancel" ? "cancel" : decision == "cancelled" ? "decline" : decision;
			AssertCapturedCodexDecision(fixture.wire->ReadSettled(), expected);
			UAM_ASSERT(!fixture.session->waiting_for_permission);
		}
	}
#endif
}

UAM_TEST(CodexCapturedReadRefusesNativeSessionGrantBeforeAnyWireResponse)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		CapturedCodexPermissionFixture fixture(goal_review);
		fixture.Command("pwd");
		UAM_ASSERT(fixture.session->waiting_for_permission);
		UAM_ASSERT(std::ranges::any_of(fixture.session->pending_permission.options, [](const auto& option) { return option.id == "acceptForSession" && option.kind == "decision"; }));
		const auto before = CapturedCodexPending(fixture);
		const auto messages = fixture.app.chats.front().messages;
		const auto saves = fixture.app.pending_chat_save_at_by_chat_id;
		std::string error;
		const bool blocked = !fixture.Resolve("acceptForSession", false, error);
		refused.push_back(blocked);
		if (blocked)
		{
			UAM_ASSERT(!error.empty());
			UAM_ASSERT_EQ(CapturedCodexPending(fixture), before);
			UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
			UAM_ASSERT_EQ(fixture.app.pending_chat_save_at_by_chat_id, saves);
			UAM_ASSERT(fixture.Resolve("decline", false, error));
		}
		// The complete private sink is drained through EOF: exactly one denial may be delivered after refusal.
		AssertCapturedCodexDecision(fixture.wire->ReadSettled(), blocked ? "decline" : "acceptForSession");
	}
	CapturedCodexPermissionFixture unrestricted(false, true);
	unrestricted.Command("pwd");
	UAM_ASSERT(unrestricted.session->waiting_for_permission);
	std::string error;
	UAM_ASSERT(unrestricted.Resolve("acceptForSession", false, error));
	AssertCapturedCodexDecision(unrestricted.wire->ReadSettled(), "acceptForSession");
	UAM_ASSERT_EQ(refused.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool blocked) { return blocked; }));
#endif
}

UAM_TEST(CodexCapturedReadNeverApprovesMutationUnknownCommandOrFileEdit)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		for (const std::string operation : {"mutation", "unknown", "edit"})
		{
			CapturedCodexPermissionFixture fixture(goal_review);
			if (operation == "edit") fixture.Edit();
			else fixture.Command(operation == "mutation" ? "echo fixture > private-output.txt" : "private-unknown-command");
			bool blocked = true;
			if (fixture.session->waiting_for_permission)
			{
				const auto before = CapturedCodexPending(fixture);
				const auto messages = fixture.app.chats.front().messages;
				std::string error;
				blocked = !fixture.Resolve("accept", false, error);
				if (blocked)
				{
					UAM_ASSERT(!error.empty());
					UAM_ASSERT_EQ(CapturedCodexPending(fixture), before);
					UAM_ASSERT_EQ(fixture.app.chats.front().messages, messages);
					UAM_ASSERT(fixture.Resolve("decline", false, error));
				}
			}
			refused.push_back(blocked);
			// Automatic ceiling denials and guarded manual denials must both retain native decision semantics.
			AssertCapturedCodexDecision(fixture.wire->ReadSettled(), blocked ? "decline" : "accept");
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private-output.txt"));
		}
	}
	UAM_ASSERT_EQ(refused.size(), std::size_t(6));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool blocked) { return blocked; }));
#endif
}
