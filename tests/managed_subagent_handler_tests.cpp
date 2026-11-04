#include "test_harness.h"
#include "cef_handler_fixture.h"
#include "cef/uam_query_handler_async.h"

#include <future>
#include "app/uam_control_service.h"
#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;
using namespace uam::query_handler_async;

struct ManagedSubagentHandlerTestAccess
{
	static void Invoke(UamQueryHandler& handler, bool cancel, const nlohmann::json& payload, CefRefPtr<UamQueryHandler::Callback> callback)
	{
		if (cancel)
			handler.HandleCancelManagedSubagent(nullptr, payload, callback);
		else
			handler.HandleListManagedSubagents(nullptr, payload, callback);
	}
	static void Dispatch(UamQueryHandler& handler, std::function<bool(CefRefPtr<UamQueryHandler::Callback>, std::function<AsyncCefResult()>, std::function<void(AsyncCefResult&)>)> dispatch)
	{
		handler.m_managedSubagentDispatcherForTests = std::move(dispatch);
	}
};

namespace
{
	class ManagedCallback final : public UamQueryHandler::Callback
	{
	  public:
		int successes = 0;
		int failures = 0;
		int status = 0;
		nlohmann::json body;
		void Success(const CefString& value) override
		{
			++successes;
			body = nlohmann::json::parse(value.ToString());
		}
		void Success(const void*, size_t) override
		{
			++successes;
		}
		void Failure(int code, const CefString&) override
		{
			++failures;
			status = code;
		}

	  private:
		IMPLEMENT_REFCOUNTING(ManagedCallback);
	};

	struct Query
	{
		CefRefPtr<UamQueryHandler::Callback> callback;
		std::function<AsyncCefResult()> worker;
		std::function<void(AsyncCefResult&)> completion;
		void Finish()
		{
			// Execute the actual value-only I/O closure off the owner, then the real completion on owner.
			auto result = std::async(std::launch::async, worker).get();
			CompleteAsyncCefQuery(callback, result, completion);
		}
	};

	struct BridgeFixture
	{
		TempDir temp{"uam-managed-bridge"};
		uam::AppState app;
		std::unique_ptr<UamQueryHandler> handler;
		std::vector<Query> pending;
		std::string run_id;
		BridgeFixture()
		{
			app.data_root = temp.root / "data";
			ChatSession root;
			root.id = "root";
			root.created_at = "2026-10-16 10:00:00";
			app.chats.push_back(root);
			AgentRun run;
			run.id = run_id = uam::AgentRunLedger::NewRunId();
			run.root_chat_id = root.id;
			run.transcript_chat_id = "owned-transcript";
			run.agent_id = "worker";
			run.provider_id = "codex-cli";
			run.model_id = "requested-model";
			run.status = "queued";
			run.task = "Check owned task title.\nPRIVATE-TASK";
			run.definition_snapshot = "PRIVATE-DEFINITION";
			run.effective_workspace_access = "read";
			run.created_at = run.updated_at = "2026-10-16 10:01:00";
			app.agent_runs.push_back(run);
			app.queued_agent_run_ids.push_back(run.id);
			ChatSession transcript;
			transcript.id = run.transcript_chat_id;
			transcript.agent_run_id = run.id;
			transcript.provider_id = run.provider_id;
			transcript.model_id = run.model_id;
			app.chats.push_back(transcript);
			UAM_ASSERT(uam::AgentRunLedger::Save(app.data_root, run));
			handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
			ManagedSubagentHandlerTestAccess::Dispatch(*handler,
			                                           [this](auto cb, auto worker, auto completion)
			                                           {
				                                           pending.push_back({cb, std::move(worker), std::move(completion)});
				                                           return true;
			                                           });
		}
		CefRefPtr<ManagedCallback> List()
		{
			CefRefPtr<ManagedCallback> cb = new ManagedCallback;
			ManagedSubagentHandlerTestAccess::Invoke(*handler, false, {{"chatId", "root"}}, cb);
			return cb;
		}
		CefRefPtr<ManagedCallback> Cancel(const std::string& scope)
		{
			CefRefPtr<ManagedCallback> cb = new ManagedCallback;
			ManagedSubagentHandlerTestAccess::Invoke(*handler, true, {{"chatId", "root"}, {"runId", run_id}, {"controlScopeId", scope}}, cb);
			return cb;
		}
	};
} // namespace

UAM_TEST(HandlerFixtureSkipsUnrelatedBackgroundReaders)
{
	TempDir temp("uam-handler-reader-isolation");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const auto* profiles = app.provider_profiles.data();
	std::vector<std::pair<std::string, std::string>> original;
	for (const auto& profile : app.provider_profiles)
		original.emplace_back(profile.id, profile.interactive_command);
	const auto handler = UamQueryHandlerTestAccess::Create(app, "file:///private/index.html");
	UAM_ASSERT_EQ(app.provider_profiles.data(), profiles);
	UAM_ASSERT_EQ(app.provider_profiles.size(), original.size());
	for (std::size_t index = 0; index < original.size(); ++index)
	{
		UAM_ASSERT_EQ(app.provider_profiles[index].id, original[index].first);
		UAM_ASSERT_EQ(app.provider_profiles[index].interactive_command, original[index].second);
	}
}

UAM_TEST(ManagedSubagentBridgeListYieldsWhileIoWorkerIsHeld)
{
	BridgeFixture fixture;
	const auto start = std::chrono::steady_clock::now();
	const auto cb = fixture.List();
	UAM_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(fixture.pending.size(), std::size_t{1});
	UAM_ASSERT_EQ(cb->successes, 0);
	std::promise<void> release;
	auto released = release.get_future();
	std::promise<void> held;
	auto holding = held.get_future();
	auto worker = std::async(std::launch::async,
	                         [&]()
	                         {
		                         held.set_value();
		                         released.wait();
		                         return fixture.pending[0].worker();
	                         });
	holding.wait();
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	release.set_value();
	auto result = worker.get();
	CompleteAsyncCefQuery(fixture.pending[0].callback, result, fixture.pending[0].completion);
	UAM_ASSERT_EQ(cb->successes, 1);
	UAM_ASSERT_EQ(cb->body["runs"][0]["transcriptChatId"], nlohmann::json("owned-transcript"));
	UAM_ASSERT_EQ(cb->body["runs"][0]["taskTitle"], nlohmann::json("Check owned task title."));
	UAM_ASSERT(cb->body.dump().find("PRIVATE-") == std::string::npos);
}

UAM_TEST(ManagedSubagentBridgeRejectsReplacedRootAfterDetachedRead)
{
	BridgeFixture fixture;
	const auto cb = fixture.List();
	fixture.app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	fixture.pending[0].Finish();
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT_EQ(cb->status, 409);
}

UAM_TEST(ManagedSubagentBridgeCancellationRevalidatesBeforeMutation)
{
	BridgeFixture fixture;
	const auto listed = fixture.List();
	fixture.pending[0].Finish();
	UAM_ASSERT_EQ(listed->successes, 1);
	const auto cb = fixture.Cancel(listed->body.at("controlScopeId").get<std::string>());
	UAM_ASSERT_EQ(cb->successes, 0);
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
	fixture.app.chats.back().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
	fixture.pending[1].Finish();
	UAM_ASSERT_EQ(cb->status, 409);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("queued"));
	UAM_ASSERT(fixture.app.managed_subagent_cancellation_receipts.empty());
	UAM_ASSERT(fixture.app.pending_agent_run_cancellation_ids.empty());
}

UAM_TEST(ManagedSubagentBridgeKeepsStableScopeAcrossStatusOnlyRefresh)
{
	BridgeFixture fixture;
	const auto first = fixture.List();
	fixture.pending[0].Finish();
	fixture.app.agent_runs.front().status = "running";
	UAM_ASSERT(uam::AgentRunLedger::Save(fixture.app.data_root, fixture.app.agent_runs.front()));
	const auto second = fixture.List();
	fixture.pending[1].Finish();
	UAM_ASSERT_EQ(second->successes, 1);
	UAM_ASSERT_EQ(first->body.at("controlScopeId"), second->body.at("controlScopeId"));
	const auto stopped = fixture.Cancel(second->body.at("controlScopeId").get<std::string>());
	fixture.pending[2].Finish();
	UAM_ASSERT_EQ(stopped->successes, 1);
	UAM_ASSERT_EQ(fixture.app.agent_runs.front().status, std::string("cancelled"));
}

int main(int argc, char** argv)
{
	// Re-enter only the production-advertised one-flag MCP stdio invocation.
	if (argc == 2 && std::string_view(argv[1]) == "--uam-control-mcp")
		return uam::UamControlService::RunStdioServerFromEnvironment();
#if defined(__APPLE__)
	if (const auto watchdog = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog.has_value())
		return *watchdog;
#endif
	if (argc == 2 && std::string_view(argv[1]) == "--list-tests")
	{
		for (const TestCase& test : Registry())
			std::cout << test.name << '\n';
		return 0;
	}
	std::unordered_set<std::string> requested;
	for (int index = 1; index < argc; ++index)
	{
		if (!requested.insert(argv[index]).second)
		{
			std::cerr << "Duplicate requested test: " << argv[index] << '\n';
			return 2;
		}
		if (std::ranges::none_of(Registry(), [&](const TestCase& test) { return test.name == argv[index]; }))
		{
			std::cerr << "Unknown requested test: " << argv[index] << '\n';
			return 2;
		}
	}
	std::size_t selected = 0, passed = 0;
	for (const TestCase& test : Registry())
	{
		if (!requested.empty() && !requested.contains(test.name))
			continue;
		++selected;
		try
		{
			test.fn();
			++passed;
			std::cout << "PASS " << test.name << '\n';
		}
		catch (const std::exception& error)
		{
			std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
		}
	}
	std::size_t expected = 5 + (UAM_ENABLE_RUNTIME_GEMINI_CLI ? 17 : 0) + (UAM_ENABLE_RUNTIME_COPILOT_CLI ? 5 : 0);
#if UAM_ENABLE_RUNTIME_CODEX_CLI && !defined(_WIN32)
	expected += 3; // Public initial receipt, later publication refusal and parent catalogue-growth regressions.
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && !defined(_WIN32)
	expected += 2; // Genuine pure/mixed four-slot version-wait admission controls.
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
	expected += 9; // Prior six Codex controls plus public MCP delegation, child wake and exact RPC receipt.
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	expected += 8; // Native-refresh and cold direct-receipt handler controls use this provider-only guard.
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
	expected += 10; // Prior Copilot cases plus the actual parent MCP delegation/child result journey.
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
	expected += 49; // Prior Claude cases plus joined MCP lifecycle and three pending-control retention controls.
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
	++expected; // ManagedInitialUnconfirmedRootPreventsNativeMediaPrefixReplacement.
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
	++expected; // Genuine completed OpenCode restart inspection.
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
	expected += 4; // Prior three managed OpenCode peers plus public C++ MCP delegation/result.
#endif

#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI
	expected += 3; // Ordinary Gemini delegation plus initial binding and raw-proof refusal controls.
#endif
	std::cout << passed << '/' << selected << " selected managed handler cases passed\n";
	if (requested.empty())
		return passed == Registry().size() && passed == expected ? 0 : 1;
	return selected == requested.size() && passed == selected ? 0 : 1;
}
