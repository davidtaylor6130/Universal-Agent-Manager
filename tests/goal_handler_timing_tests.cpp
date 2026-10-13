#include "test_harness.h"
#include "cef/uam_query_handler.h"
#include "common/runtime/acp/acp_session_internal.h"
#include <future>
#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

/// Invokes only the three real goal endpoints tested below.
struct GoalHandlerTestAccess
{
	static void Invoke(UamQueryHandler& handler, int action, const nlohmann::json& payload,
	                   CefRefPtr<UamQueryHandler::Callback> callback)
	{
		if (action == 0) handler.HandleUpdateGoalStatus(nullptr, payload, callback);
		else if (action == 1) handler.HandleResumeGoal(nullptr, payload, callback);
		else handler.HandleSetGoal(nullptr, payload, callback);
	}
};

namespace
{
	class GoalTimingCallback final : public UamQueryHandler::Callback
	{
	public:
		int successes = 0;
		int failures = 0;
		void Success(const CefString&) override { ++successes; }
		void Success(const void*, size_t) override { ++successes; }
		void Failure(int, const CefString&) override { ++failures; }
	private:
		IMPLEMENT_REFCOUNTING(GoalTimingCallback);
	};

	/// Private stdin sink exercises actual native request writes without a provider call.
	struct GoalHandlerTransport
	{
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		uam::AcpSessionState& session;
		GoalHandlerTransport(uam::AcpSessionState& owned, const TempDir& temp) : session(owned)
		{
			std::string error;
#if defined(_WIN32)
			const std::vector<std::string> command = {"cmd.exe", "/d", "/s", "/c", "more > wire.jsonl"};
#else
			const std::vector<std::string> command = {"/bin/sh", "-c", "cat > wire.jsonl"};
#endif
			UAM_ASSERT(process.StartStdioProcess(session, temp.root, command, &error));
		}
		~GoalHandlerTransport()
		{
			process.StopStdioProcess(session, true);
			process.CloseStdioProcessHandles(session);
		}
	};

	void RunGoalHandlerTiming(int action)
	{
		TempDir temp("uam-connected-goal-handler-timing");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession chat;
		chat.id = "owned-goal-handler-chat";
		chat.provider_id = "codex-cli";
		chat.workspace_directory = temp.root.string();
		chat.created_at = chat.updated_at = "2026-10-13T00:00:00Z";
		chat.goal_command_revision = "owned-handler-revision";
		Goal goal;
		goal.id = "owned-handler-goal";
		goal.objective = "Retain the exact objective.";
		goal.execution_owner = "provider";
		goal.provider_command = "thread/goal";
		goal.token_budget = 100;
		goal.status = action == 1 ? GoalStatus::Paused : GoalStatus::Active;
		if (action != 2) chat.goals.push_back(goal);
		if (action == 0) chat.active_goal_id = goal.id;
		app.chats.push_back(chat);
		app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
		uam::AcpSessionState& session = *app.acp_sessions.front();
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.session_id = "owned-handler-thread";
		session.running = session.session_ready = true;
		GoalHandlerTransport transport(session, temp);
		session.codex_goal_owner = uam::acp_detail::CaptureCodexNativeGoalOwner(app, session, app.chats.front(), goal);
		session.codex_goal_created_at = 100;
		session.codex_goal_owner.binding_created_at = 100;
		session.codex_goal_native_active = action == 0;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		UamQueryHandler handler(app, "file:///private/index.html");
		CefRefPtr<GoalTimingCallback> callback = new GoalTimingCallback;
		ChatSession blocker;
		blocker.id = "unrelated-full-publication";
		const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(temp.root, blocker);
		UAM_ASSERT(prepared != nullptr);
		std::promise<void> locked;
		std::future<void> ready = locked.get_future();
		std::jthread holder([&]()
		{
			(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&]()
			{
				locked.set_value();
				std::this_thread::sleep_for(std::chrono::milliseconds(350));
				return true;
			});
		});
		ready.wait();
		const int next_request_before = session.next_request_id;
		nlohmann::json payload = {{"chatId", chat.id}, {"goalId", goal.id}, {"status", "paused"}};
		if (action == 2) payload = {{"chatId", chat.id}, {"objective", goal.objective}, {"tokenBudget", 100}, {"executionOwner", "provider"}};
		const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		GoalHandlerTestAccess::Invoke(handler, action, payload, callback);
		const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
		const int requests_before_full_release = session.next_request_id;
		const int successes_before_full_release = callback->successes;
		holder.join();
		UAM_ASSERT(elapsed < std::chrono::milliseconds(100));
		UAM_ASSERT_EQ(requests_before_full_release, next_request_before);
		UAM_ASSERT_EQ(successes_before_full_release, 0);
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
		UAM_ASSERT_EQ(app.chats.front().metadata_incarnation, chat.metadata_incarnation);
		for (int attempt = 0; attempt < 500 && !app.pending_chat_save_at_by_chat_id.empty(); ++attempt)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT_EQ(callback->failures, 0);
		UAM_ASSERT_EQ(callback->successes, 1);
		const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(persisted.has_value());
		UAM_ASSERT_EQ(persisted->goals.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(persisted->goals.front().objective, goal.objective);
		if (action == 0) UAM_ASSERT_EQ(persisted->goals.front().status, GoalStatus::Paused);
		UAM_ASSERT(session.next_request_id > next_request_before);
		if (action == 0)
		{
			UAM_ASSERT_EQ(session.codex_goal_pending_requests.size(), static_cast<std::size_t>(1));
			const std::pair<const std::string, uam::AcpSessionState::CodexGoalOwner>& stop = *session.codex_goal_pending_requests.begin();
			UAM_ASSERT_EQ(stop.second.action, uam::AcpSessionState::CodexGoalOwner::Action::StopPause);
			UAM_ASSERT_EQ(stop.second.binding_created_at, static_cast<std::int64_t>(100));
			UAM_ASSERT_EQ(session.pending_request_methods.at(std::stoi(stop.first)), std::string("thread/goal/set"));
		}
		UAM_ASSERT_EQ(session.codex_goal_initial_turn_request_id, 0);
	}
}

UAM_TEST(GoalHandlerPauseDoesNotBlockOrSendBeforeFullReceipt) { RunGoalHandlerTiming(0); }
UAM_TEST(GoalHandlerResumeDoesNotBlockOrSendBeforeFullReceipt) { RunGoalHandlerTiming(1); }
UAM_TEST(GoalHandlerSetDoesNotBlockOrSendBeforeFullReceipt) { RunGoalHandlerTiming(2); }

int main(int argc, char** argv)
{
#if defined(__APPLE__)
	if (const std::optional<int> watchdog = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog.has_value()) return *watchdog;
#endif
	std::size_t passed = 0;
	for (const TestCase& test : Registry())
	{
		try { test.fn(); ++passed; std::cout << "PASS " << test.name << '\n'; }
		catch (const std::exception& error) { std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
	}
	std::cout << passed << '/' << Registry().size() << " connected goal cases passed\n";
	return passed == Registry().size() && Registry().size() == 3 ? 0 : 1;
}
