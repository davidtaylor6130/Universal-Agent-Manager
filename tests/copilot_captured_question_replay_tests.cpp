#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/structured_framing.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
namespace
{

class CapturedPlanWire
{
public:
	CapturedPlanWire(uam::AcpSessionState& session, const fs::path& directory) : m_session(session)
	{
#if defined(_WIN32)
		const std::vector<std::string> sink{"cmd.exe", "/C", "more"};
#else
		const std::vector<std::string> sink{"/bin/cat"};
#endif
		std::string error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, directory, sink, &error));
	}
	~CapturedPlanWire()
	{
		auto& process = PlatformServicesFactory::Instance().process_service;
		process.StopStdioProcess(m_session, true);
		process.CloseStdioProcessHandles(m_session);
	}
	std::vector<nlohmann::json> ReadSettled()
	{
		auto& process = PlatformServicesFactory::Instance().process_service;
		process.CloseStdioProcessInput(m_session);
		std::string bytes, error;
		char buffer[8192];
		bool exited = false;
		int exit_code = -1;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const auto count = process.ReadStdioProcessStdout(m_session, buffer, sizeof(buffer), &error);
			if (count > 0) bytes.append(buffer, static_cast<std::size_t>(count));
			if (process.PollStdioProcessExited(m_session, &exit_code) && count <= 0) { exited = true; break; }
			std::this_thread::yield();
		}
		// A complete EOF transcript, rather than a timed absence, proves no earlier approval escaped.
		UAM_ASSERT(exited);
		UAM_ASSERT_EQ(exit_code, 0);
		std::vector<nlohmann::json> responses;
		std::size_t consumed = 0;
		while (consumed < bytes.size())
		{
			const auto remaining = std::string_view(bytes).substr(consumed);
			const auto frame = uam::structured_framing::Read(remaining);
			UAM_ASSERT(frame.status == uam::structured_framing::Status::Complete);
			responses.push_back(nlohmann::json::parse(remaining.substr(frame.body_offset, frame.body_bytes)));
			consumed += frame.consumed_bytes;
		}
		UAM_ASSERT_EQ(consumed, bytes.size());
		return responses;
	}
private:
	uam::AcpSessionState& m_session;
};

struct CapturedPlanReplayFixture
{
	TempDir temp{"uam-copilot-captured-plan-replay"};
	uam::AppState app;
	uam::AcpSessionState* session = nullptr;
	std::unique_ptr<CapturedPlanWire> wire;
	const std::string native_session = "11111111-1111-4111-8111-111111111111";
	const std::string native_request = "private-plan-review";
	const std::string stable_request = "\"private-plan-review\"";

	CapturedPlanReplayFixture()
	{
		app.data_root = temp.root / "data";
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "copilot-captured-plan-replay";
		chat.provider_id = "copilot-cli";
		chat.native_session_id = native_session;
		chat.workspace_directory = temp.root.string();
		// This chooses actual remote-response persistence while all transport is
		// a private local byte sink. No SSH connection or provider is started.
		chat.execution_host_id = "private-ssh-plan-replay-host";
		chat.approval_mode = "plan";
		chat.command_safety_tier = "off";
		chat.memory_enabled = false;
		chat.messages.push_back({MessageRole::User, "Review the private fixture plan"});
		chat.messages.push_back({MessageRole::Assistant, ""});
		app.chats.push_back(std::move(chat));
		Attach(false, false);
	}

	void Attach(bool captured_read, bool goal_review)
	{
		auto owned = std::make_unique<uam::AcpSessionState>();
		session = owned.get();
		session->chat_id = app.chats.front().id;
		session->provider_id = "copilot-cli";
		session->protocol_kind = "copilot-sdk";
		session->session_id = native_session;
		session->current_mode_id = "plan";
		session->running = session->processing = session->initialized = session->session_ready = true;
		session->turn_serial = 1;
		session->turn_user_message_index = session->turn_first_user_message_index = 0;
		session->goal_review_turn = captured_read && goal_review;
		session->active_uam_agent_workspace_access = captured_read && !goal_review ? "read" : "";
		session->active_uam_agent_id = session->active_uam_agent_workspace_access.empty() ? "" : "private-read-agent";
		session->copilot_native_state["protocolVersion"] = 3;
		app.acp_sessions.push_back(std::move(owned));
		wire = std::make_unique<CapturedPlanWire>(*session, temp.root);
	}

	void FeedReview()
	{
		const nlohmann::json event = {{"jsonrpc", "2.0"}, {"method", "session.event"}, {"params", {
		    {"sessionId", native_session}, {"event", {{"id", "private-plan-event"}, {"type", "exit_plan_mode.requested"}, {"data", {
		        {"requestId", native_request}, {"summary", "Proposed private implementation"}, {"planContent", "Inspect and update the private fixture."},
		        {"actions", {"interactive", "exit_only"}}, {"recommendedAction", "interactive"}}}}}}}};
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *session, app.chats.front(), event.dump()));
	}

	bool Resolve(const std::map<std::string, std::vector<std::string>>& answers, std::string& error)
	{
		return uam::ResolveAcpUserInput(app, session->chat_id, stable_request, answers, &error);
	}
};

nlohmann::json PendingPlanSnapshot(const uam::AcpSessionState& session)
{
	nlohmann::json questions = nlohmann::json::array();
	for (const auto& question : session.pending_user_input.questions)
	{
		nlohmann::json options = nlohmann::json::array();
		for (const auto& option : question.options) options.push_back({{"label", option.label}, {"description", option.description}});
		questions.push_back({{"id", question.id}, {"question", question.question}, {"options", std::move(options)}});
	}
	return {{"waiting", session.waiting_for_user_input}, {"request", session.pending_user_input.request_id_json},
	    {"metadata", session.pending_user_input.provider_input_json}, {"status", session.pending_user_input.status}, {"questions", std::move(questions)}};
}

void AssertPlanWire(const nlohmann::json& response, const CapturedPlanReplayFixture& fixture, bool approved)
{
	UAM_ASSERT_EQ(response.at("method"), nlohmann::json("session.ui.handlePendingExitPlanMode"));
	UAM_ASSERT_EQ(response.at("params").at("sessionId"), nlohmann::json(fixture.native_session));
	UAM_ASSERT_EQ(response.at("params").at("requestId"), nlohmann::json(fixture.native_request));
	UAM_ASSERT_EQ(response.at("params").at("response").at("approved"), nlohmann::json(approved));
}

}
#endif

UAM_TEST(CopilotCapturedPlanReplayRequiresFreshCeilingSafeCancellation)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	for (bool goal_review : {false, true})
	{
		CapturedPlanReplayFixture fixture;
		fixture.FeedReview();
		UAM_ASSERT(fixture.session->waiting_for_user_input);
		const std::map<std::string, std::vector<std::string>> implementation{{"plan-review", {"Approve plan and implement interactively"}}};
		std::string error;
		UAM_ASSERT(fixture.Resolve(implementation, error));
		const auto original_wire = fixture.wire->ReadSettled();
		UAM_ASSERT_EQ(original_wire.size(), std::size_t(1));
		AssertPlanWire(original_wire.front(), fixture, true);
		UAM_ASSERT_EQ(original_wire.front().at("params").at("response").at("selectedAction"), nlohmann::json("interactive"));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(saved);
		UAM_ASSERT_EQ(saved->approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(saved->remote_interaction_responses.size(), std::size_t(1));
		UAM_ASSERT_EQ(saved->remote_interaction_responses.front().request_id_json, fixture.stable_request);
		const std::string original_receipt = saved->remote_interaction_responses.front().response_json;
		UAM_ASSERT_EQ(nlohmann::json::parse(original_receipt), original_wire.front());
		// Use only the genuine saved public decision, with a fresh runtime that
		// captures a stricter ceiling. No response or replay marker is fabricated.
		fixture.wire.reset();
		fixture.app.acp_sessions.clear();
		fixture.app.chats.front() = *saved;
		fixture.Attach(true, goal_review);
		fixture.FeedReview();
		UAM_ASSERT(fixture.session->waiting_for_user_input);
		UAM_ASSERT_EQ(fixture.session->pending_user_input.request_id_json, fixture.stable_request);
		UAM_ASSERT_EQ(fixture.session->pending_user_input.questions.size(), std::size_t(1));
		UAM_ASSERT(std::ranges::none_of(fixture.session->pending_user_input.questions.front().options,
		    [](const auto& option) { return option.label == "Approve plan and implement interactively"; }));
		const auto pending_before = PendingPlanSnapshot(*fixture.session);
		const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.app.chats.front().id);
		const std::string disk_before = ReadFile(primary);
		UAM_ASSERT(!fixture.Resolve(implementation, error));
		UAM_ASSERT_EQ(PendingPlanSnapshot(*fixture.session), pending_before);
		UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.size(), std::size_t(1));
		UAM_ASSERT_EQ(fixture.app.chats.front().remote_interaction_responses.front().response_json, original_receipt);
		UAM_ASSERT_EQ(ReadFile(primary), disk_before);
		UAM_ASSERT_EQ(fixture.app.chats.front().approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(fixture.session->current_mode_id, std::string("plan"));
		// Cancellation must supersede the incompatible saved approval durably,
		// without first replaying that approval or consuming the question on rejection.
		UAM_ASSERT(fixture.Resolve({}, error));
		UAM_ASSERT(!fixture.session->waiting_for_user_input);
		const auto cancelled = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id);
		UAM_ASSERT(cancelled);
		UAM_ASSERT_EQ(cancelled->approval_mode, std::string("plan"));
		UAM_ASSERT_EQ(cancelled->remote_interaction_responses.size(), std::size_t(1));
		UAM_ASSERT_EQ(cancelled->remote_interaction_responses.front().request_id_json, fixture.stable_request);
		UAM_ASSERT(cancelled->remote_interaction_responses.front().response_json != original_receipt);
		const auto cancellation = nlohmann::json::parse(cancelled->remote_interaction_responses.front().response_json);
		AssertPlanWire(cancellation, fixture, false);
		UAM_ASSERT_EQ(cancellation.at("params").at("response"), nlohmann::json({{"approved", false}}));
		const auto fresh_wire = fixture.wire->ReadSettled();
		UAM_ASSERT_EQ(fresh_wire.size(), std::size_t(1));
		UAM_ASSERT_EQ(fresh_wire.front(), cancellation);
		UAM_ASSERT_EQ(fixture.session->current_mode_id, std::string("plan"));
	}
#endif
}
