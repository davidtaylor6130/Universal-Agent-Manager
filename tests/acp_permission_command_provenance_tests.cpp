#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_CODEX_CLI || UAM_ENABLE_RUNTIME_GEMINI_CLI
namespace
{

class CommandProvenanceWire
{
public:
	explicit CommandProvenanceWire(uam::AcpSessionState& session, const fs::path& directory) : m_session(session)
	{
#if defined(_WIN32)
		const std::vector<std::string> sink{"cmd.exe", "/C", "more"};
#else
		const std::vector<std::string> sink{"/bin/cat"};
#endif
		std::string error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, directory, sink, &error));
	}
	~CommandProvenanceWire()
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
			const auto count = process.ReadStdioProcessStdout(m_session, bytes, sizeof(bytes), &error);
			if (count > 0) wire.append(bytes, static_cast<std::size_t>(count));
			if (process.PollStdioProcessExited(m_session, &exit_code) && count <= 0) { exited = true; break; }
			std::this_thread::yield();
		}
		// Absence is checked only after the real sink has consumed EOF and exited.
		UAM_ASSERT(exited);
		UAM_ASSERT_EQ(exit_code, 0);
		std::vector<nlohmann::json> responses;
		std::size_t cursor = 0;
		for (auto end = wire.find('\n', cursor); end != std::string::npos; end = wire.find('\n', cursor))
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

struct CommandProvenanceFixture
{
	TempDir temp{"uam-command-provenance"};
	uam::AppState app;
	uam::AcpSessionState* session = nullptr;
	std::unique_ptr<CommandProvenanceWire> wire;
	const std::string thread = "08f433b8-6172-4ef6-b3b7-6c2846412b17";

	CommandProvenanceFixture(bool codex, bool goal_review, bool automatic, bool unrestricted = false)
	{
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "command-provenance";
		chat.provider_id = codex ? "codex-cli" : "gemini-cli";
		chat.native_session_id = thread;
		chat.workspace_directory = temp.root.string();
		chat.execution_host_id = "local";
		chat.approval_mode = "default";
		chat.command_safety_tier = automatic ? "yolo" : "off";
		chat.memory_enabled = false;
		chat.messages.push_back({MessageRole::User, "Inspect the private fixture"});
		chat.messages.push_back({MessageRole::Assistant, ""});
		app.chats.push_back(std::move(chat));
		auto owned = std::make_unique<uam::AcpSessionState>();
		session = owned.get();
		session->chat_id = app.chats.front().id;
		session->provider_id = app.chats.front().provider_id;
		session->protocol_kind = codex ? "codex-app-server" : "gemini-acp";
		session->session_id = thread;
		if (codex) session->codex_thread_id = thread;
		session->current_mode_id = "default";
		session->running = session->initialized = session->session_ready = session->processing = true;
		session->turn_serial = 1;
		session->turn_user_message_index = session->turn_first_user_message_index = 0;
		session->goal_review_turn = !unrestricted && goal_review;
		session->active_uam_agent_workspace_access = !unrestricted && !goal_review ? "read" : "";
		session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-agent";
		app.acp_sessions.push_back(std::move(owned));
		wire = std::make_unique<CommandProvenanceWire>(*session, temp.root);
	}

	void CodexRequest(const std::string& command_shape, bool session_first = false)
	{
		nlohmann::json params = {{"threadId", thread}, {"itemId", "private-command"}, {"reason", "pwd"}, {"cwd", temp.root.string()},
		    {"availableDecisions", session_first ? nlohmann::json::array({"acceptForSession", "accept", "decline", "cancel"}) : nlohmann::json::array({"accept", "decline", "cancel"})}};
		if (command_shape == "null") params["command"] = nullptr;
		if (command_shape == "typed") params["command"] = "pwd";
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
		    nlohmann::json{{"jsonrpc", "2.0"}, {"id", 71}, {"method", "item/commandExecution/requestApproval"}, {"params", params}}.dump()));
	}

	void GenericRequest(const std::string& input_shape)
	{
		nlohmann::json tool = {{"toolCallId", "private-command"}, {"title", "Run command"}, {"kind", "execute"}, {"status", "pending"},
		    {"content", {{"type", "text"}, {"text", "pwd"}}}};
		if (input_shape == "null") tool["rawInput"] = nullptr;
		if (input_shape == "string") tool["rawInput"] = "pwd";
		if (input_shape == "typed") tool["rawInput"] = {{"command", "pwd"}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(),
		    nlohmann::json{{"jsonrpc", "2.0"}, {"id", 71}, {"method", "session/request_permission"}, {"params", {{"toolCall", tool},
		        {"options", {{{"optionId", "once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
		            {{"optionId", "reject"}, {"name", "Reject"}, {"kind", "reject_once"}}}}}}}.dump()));
	}

	bool Resolve(const std::string& option)
	{
		std::string error;
		return uam::ResolveAcpPermission(app, session->chat_id, "71", option, false, &error);
	}

	void DenyIfPending(bool codex)
	{
		if (session->pending_permission.request_id_json == "71") UAM_ASSERT(Resolve(codex ? "decline" : "reject"));
	}
};

bool IsGrant(const nlohmann::json& response, bool codex)
{
	UAM_ASSERT_EQ(response.at("id"), nlohmann::json(71));
	if (codex)
		return response.at("result").at("decision") == "accept" || response.at("result").at("decision") == "acceptForSession";
	const auto& outcome = response.at("result").at("outcome");
	return outcome.at("outcome") == "selected" && outcome.value("optionId", std::string()) == "once";
}

bool UnknownCommandIsRefused(bool codex, bool goal_review, bool automatic, const std::string& shape)
{
	CommandProvenanceFixture fixture(codex, goal_review, automatic);
	if (codex) fixture.CodexRequest(shape);
	else fixture.GenericRequest(shape);
	// Attempt the public manual grant as well as inspecting automatic delivery.
	// An earlier automatic denial may already have settled the request.
	bool manual_granted = false;
	if (!automatic) manual_granted = fixture.Resolve(codex ? "accept" : "once");
	fixture.DenyIfPending(codex);
	const auto responses = fixture.wire->ReadSettled();
	UAM_ASSERT_EQ(responses.size(), std::size_t(1));
	const bool wire_grant = std::ranges::any_of(responses, [codex](const auto& response) { return IsGrant(response, codex); });
	std::cout << "command provenance: codex=" << codex << " goal=" << goal_review << " auto=" << automatic
	          << " shape=" << shape << " manualGrant=" << manual_granted << " wire=" << nlohmann::json(responses).dump() << '\n';
	return !manual_granted && !wire_grant;
}

void AssertTypedCommandControl(bool codex, bool goal_review, bool automatic, bool unrestricted = false)
{
	CommandProvenanceFixture fixture(codex, goal_review, automatic, unrestricted);
	if (codex) fixture.CodexRequest("typed");
	else fixture.GenericRequest("typed");
	if (!automatic) UAM_ASSERT(fixture.Resolve(codex ? "accept" : "once"));
	const auto responses = fixture.wire->ReadSettled();
	UAM_ASSERT_EQ(responses.size(), std::size_t(1));
	UAM_ASSERT(IsGrant(responses.front(), codex));
}

}
#endif

UAM_TEST(CodexCapturedCommandApprovalRequiresNativeCommandProvenance)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		for (bool automatic : {false, true})
			for (const std::string shape : {"missing", "null"})
				refused.push_back(UnknownCommandIsRefused(true, goal_review, automatic, shape));
		AssertTypedCommandControl(true, goal_review, false);
	}
	AssertTypedCommandControl(true, false, false, true);
	UAM_ASSERT_EQ(refused.size(), std::size_t(8));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool value) { return value; }));
#endif
}

UAM_TEST(GenericCapturedCommandApprovalRejectsReadLookingDisplayFallback)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	std::vector<bool> refused;
	for (bool goal_review : {false, true})
	{
		for (bool automatic : {false, true})
			for (const std::string shape : {"missing", "null", "string"})
				refused.push_back(UnknownCommandIsRefused(false, goal_review, automatic, shape));
		AssertTypedCommandControl(false, goal_review, true);
	}
	AssertTypedCommandControl(false, false, false, true);
	UAM_ASSERT_EQ(refused.size(), std::size_t(12));
	UAM_ASSERT(std::ranges::all_of(refused, [](bool value) { return value; }));
#endif
}

UAM_TEST(CodexCapturedReadAutoApprovalSelectsOfferedOneTimeDecision)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	std::vector<bool> selected_once;
	for (bool goal_review : {false, true})
	{
		CommandProvenanceFixture fixture(true, goal_review, true);
		fixture.CodexRequest("typed", true);
		// If automatic selection stalls on the persistent choice, deny the still
		// pending request so the exact EOF wire remains observable without an absence timer.
		fixture.DenyIfPending(true);
		const auto responses = fixture.wire->ReadSettled();
		UAM_ASSERT_EQ(responses.size(), std::size_t(1));
		UAM_ASSERT_EQ(responses.front().at("id"), nlohmann::json(71));
		selected_once.push_back(responses.front().at("result").at("decision") == "accept");
	}
	UAM_ASSERT_EQ(selected_once.size(), std::size_t(2));
	UAM_ASSERT(std::ranges::all_of(selected_once, [](bool value) { return value; }));
#endif
}
