#include "test_harness.h"
#include "common/provider/opencode_terminal_activity.h"
#include "common/config/execution_host_config.h"
#include <atomic>
#include <thread>

using namespace uam_test;
namespace
{
	using Json = nlohmann::json;
	struct TerminalObservationFixture
	{
		TempDir temp{"uam-opencode-terminal-activity"};
		uam::CliTerminalState terminal;
		ChatSession chat;
		ExecutionHost host;
		std::shared_ptr<std::atomic_int> phase = std::make_shared<std::atomic_int>(0);
		std::shared_ptr<std::atomic_int> queries = std::make_shared<std::atomic_int>(0);
		TerminalObservationFixture(bool remote = false, uam::opencode_terminal::ClockNow clock = {}, const std::string& native_workspace = {})
		{
			terminal.terminal_id = "owned-terminal"; terminal.uses_prompt_activity_tracking = false; terminal.running = true; terminal.startup_time_s = 10.0;
			chat.native_session_id = "ses_exact"; chat.workspace_directory = temp.root.string();
			if (remote) { host.id = "remote"; host.transport = "ssh"; host.ssh_alias = "owned-ssh"; host.platform = "windows"; chat.workspace_directory = "C:\\opaque remote\\workspace"; }
			auto query = [phase = phase, queries = queries, workspace = chat.workspace_directory, native_workspace = native_workspace.empty() ? chat.workspace_directory : native_workspace, remote](const uam::opencode_native::Endpoint& endpoint,
			    const std::optional<ExecutionHost>& captured, const std::string& directory, const Json& operation, Json& result, std::string&, std::stop_token)
			{
				++*queries;
				if (!uam::opencode_native::ValidEndpoint(endpoint) || directory != workspace || captured.has_value() != remote) return false;
				const std::string kind = operation.value("operation", "");
				if (kind == "session") result = {{"id", phase->load() == 9 ? "ses_foreign" : "ses_exact"}, {"directory", native_workspace}};
				else if (kind == "status") result = phase->load() == 1 ? Json{{"ses_exact", {{"type", "busy"}}}} : Json::object();
				else if (kind == "messages") result = Json::array({{{"info", {{"id", phase->load() == 2 ? "msg_new" : "msg_old"}, {"role", "assistant"}, {"time", {{"completed", 1}}}}}}});
				else return false;
				return true;
			};
			std::vector<std::string> argv{"opencode", "--session", chat.native_session_id};
			std::vector<std::pair<std::string, std::string>> env;
			uam::opencode_terminal::PrepareLaunch(terminal, chat, host, argv, env, std::move(query), std::move(clock));
		}
		ProviderTerminalActivity Poll()
		{
			return uam::opencode_terminal::Poll(terminal, chat.native_session_id, uam::paths::PathFromUtf8(chat.workspace_directory), false);
		}
		bool Await(ProviderTerminalActivity expected)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (Poll() == expected) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}
	};
}

UAM_TEST(OpenCodeTerminalObserverPreservesCustomLaunchAndUsesPrivateStableAuthority)
{
	TerminalObservationFixture fixture;
	std::vector<std::string> first{"opencode", "--session", fixture.chat.native_session_id};
	std::vector<std::pair<std::string, std::string>> environment;
	uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, fixture.host, first, environment);
	const auto observer = fixture.terminal.opencode_activity_observer;
	UAM_ASSERT(observer != nullptr);
	UAM_ASSERT_EQ(environment.size(), std::size_t(2));
	UAM_ASSERT_EQ(environment[1].first, std::string("OPENCODE_SERVER_PASSWORD"));
	UAM_ASSERT(std::find(first.begin(), first.end(), environment[1].second) == first.end());
	auto second = std::vector<std::string>{"opencode", "--session", fixture.chat.native_session_id};
	std::vector<std::pair<std::string, std::string>> second_environment;
	uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, fixture.host, second, second_environment);
	UAM_ASSERT_EQ(first, second); UAM_ASSERT_EQ(environment, second_environment);
	for (const std::string custom : {"--port=7777", "--mini", "attach", "--hostname", "--cors"})
	{
		std::vector<std::string> argv{"opencode", custom}; const auto original = argv;
		std::vector<std::pair<std::string, std::string>> env;
		uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, fixture.host, argv, env);
		UAM_ASSERT_EQ(argv, original); UAM_ASSERT(env.empty()); UAM_ASSERT(!fixture.terminal.opencode_activity_observer);
	}
	std::vector<std::string> wrapper{"opencode-wrapper"};
	uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, fixture.host, wrapper, environment);
	UAM_ASSERT(!fixture.terminal.opencode_activity_observer);
	for (const std::string extension : {".sh", ".py", ".wrapper"})
	{
		std::vector<std::string> custom_argv{"/tmp/opencode" + extension}; const auto original = custom_argv;
		std::vector<std::pair<std::string, std::string>> custom_env;
		uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, fixture.host, custom_argv, custom_env);
		UAM_ASSERT_EQ(custom_argv, original); UAM_ASSERT(custom_env.empty()); UAM_ASSERT(!fixture.terminal.opencode_activity_observer);
	}
	ExecutionHost windows; windows.id = "ssh-windows"; windows.platform = "windows"; windows.transport = "ssh";
	std::vector<std::string> windows_argv{"C:\\Program Files\\OpenCode\\OpenCode.exe"};
	std::vector<std::pair<std::string, std::string>> windows_environment;
	uam::opencode_terminal::PrepareLaunch(fixture.terminal, fixture.chat, windows, windows_argv, windows_environment);
	UAM_ASSERT(fixture.terminal.opencode_activity_observer != nullptr);
	UAM_ASSERT_EQ(windows_environment.size(), std::size_t(2));
}

UAM_TEST(OpenCodeTerminalObserverRequiresNativeAdmissionAndCompletedTurnReceipt)
{
	TerminalObservationFixture fixture;
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	UAM_ASSERT(fixture.terminal.uses_prompt_activity_tracking);
	const int baseline_queries = fixture.queries->load();
	for (int i = 0; i < 65; ++i)
	{
		fixture.Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT_EQ(fixture.queries->load(), baseline_queries);
	uam::opencode_terminal::Checkpoint(fixture.terminal);
	for (int i = 0; i < 65; ++i)
	{
		UAM_ASSERT(fixture.Poll() != ProviderTerminalActivity::Complete);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	fixture.phase->store(1);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Busy));
	fixture.phase->store(0);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	uam::opencode_terminal::Checkpoint(fixture.terminal);
	fixture.phase->store(2);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
}

UAM_TEST(OpenCodeTerminalObserverRejectsForeignBindingsAndCapturesRemoteAuthority)
{
	TerminalObservationFixture fixture(true);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	UAM_ASSERT(uam::opencode_terminal::MatchesHost(fixture.terminal, fixture.host));
	ExecutionHost replaced = fixture.host; replaced.ssh_alias = "different-host";
	UAM_ASSERT(!uam::opencode_terminal::MatchesHost(fixture.terminal, replaced));
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById("opencode-cli");
	UAM_ASSERT(runtime.InteractiveObservationMatchesHost(fixture.terminal, fixture.host));
	UAM_ASSERT(!runtime.InteractiveObservationMatchesHost(fixture.terminal, replaced));
#endif
	UAM_ASSERT(uam::opencode_terminal::Poll(fixture.terminal, "ses_other", uam::paths::PathFromUtf8(fixture.chat.workspace_directory), false) == ProviderTerminalActivity::Unavailable);
	TerminalObservationFixture ambiguous;
	UAM_ASSERT(uam::opencode_terminal::Poll(ambiguous.terminal, "ses_exact", ambiguous.temp.root, true) == ProviderTerminalActivity::Unavailable);
	TerminalObservationFixture foreign;
	foreign.phase->store(9);
	foreign.Poll();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	UAM_ASSERT(foreign.Poll() == ProviderTerminalActivity::Unavailable);
	UAM_ASSERT(!foreign.terminal.uses_prompt_activity_tracking);
}

UAM_TEST(OpenCodeTerminalRunnerObservationRejectsMutationAndInvalidAuthority)
{
	TempDir temp("uam-opencode-terminal-readonly-rpc");
	uam::remote::RunnerState state;
	Json request{{"id", "observe"}, {"type", "opencode.terminal.observe"}, {"port", 49152}, {"token", "private"}, {"workspace", temp.root.string()},
	    {"operation", {{"operation", "reply"}, {"requestId", "per_owned"}, {"reply", "once"}}}};
	UAM_ASSERT_EQ(state.HandleProcessRequest(request)["error"]["code"].get<std::string>(), std::string("unsupported_operation"));
	request["operation"] = {{"operation", "status"}}; request["port"] = 80;
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
	request["port"] = 49152; request["token"] = "";
	UAM_ASSERT(!state.HandleProcessRequest(request).value("ok", true));
	std::string url, method, config, error;
	UAM_ASSERT(uam::opencode_native::BuildRequest({49152, "private"}, "/workspace", {{"operation", "messages"}, {"nativeSessionId", "ses_exact"}}, url, method, config, error));
	UAM_ASSERT_EQ(method, std::string("GET"));
	UAM_ASSERT(url.ends_with("/session/ses_exact/message?directory=%2Fworkspace&limit=2"));
}

UAM_TEST(OpenCodeTerminalObserverDiscardsPreSubmissionSnapshots)
{
	TempDir temp("uam-opencode-terminal-stale");
	uam::CliTerminalState terminal;
	terminal.terminal_id = "stale-terminal"; terminal.running = true; terminal.startup_time_s = 4; terminal.uses_prompt_activity_tracking = false;
	ChatSession chat; chat.native_session_id = "ses_owned"; chat.workspace_directory = temp.root.string();
	ExecutionHost host;
	auto entered = std::make_shared<std::atomic_bool>(false), released = std::make_shared<std::atomic_bool>(false);
	auto query = [entered, released, directory = chat.workspace_directory](const uam::opencode_native::Endpoint&, const std::optional<ExecutionHost>&,
	    const std::string&, const Json& operation, Json& result, std::string&, std::stop_token stop)
	{
		if (operation.value("operation", "") == "session")
		{
			entered->store(true);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (!released->load() && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			result = {{"id", "ses_owned"}, {"directory", directory}};
		}
		else if (operation.value("operation", "") == "status") result = Json::object();
		else result = Json::array();
		return true;
	};
	std::vector<std::string> argv{"opencode"}; std::vector<std::pair<std::string, std::string>> env;
	uam::opencode_terminal::PrepareLaunch(terminal, chat, host, argv, env, query);
	UAM_ASSERT(uam::opencode_terminal::Poll(terminal, chat.native_session_id, temp.root, false) == ProviderTerminalActivity::Unavailable);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!entered->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	UAM_ASSERT(entered->load());
	uam::opencode_terminal::Checkpoint(terminal);
	released->store(true);
	for (int i = 0; i < 65; ++i)
	{
		UAM_ASSERT(uam::opencode_terminal::Poll(terminal, chat.native_session_id, temp.root, false) != ProviderTerminalActivity::Complete);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

UAM_TEST(OpenCodeTerminalObserverQuietsUnadmittedInputWithoutCompletingIt)
{
	auto seconds = std::make_shared<std::atomic_int>(1000);
	TerminalObservationFixture fixture(false, [seconds]
	{
		return std::chrono::steady_clock::time_point(std::chrono::seconds(seconds->load()));
	});
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	uam::opencode_terminal::Checkpoint(fixture.terminal);
	fixture.terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	fixture.terminal.turn_state = uam::CliTerminalTurnState::Busy;
	fixture.terminal.generation_in_progress = true;
	seconds->store(1011);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (fixture.terminal.lifecycle_state != uam::CliTerminalLifecycleState::Unknown && std::chrono::steady_clock::now() < deadline)
	{
		UAM_ASSERT(fixture.Poll() != ProviderTerminalActivity::Complete);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(fixture.terminal.lifecycle_state == uam::CliTerminalLifecycleState::Unknown);
	UAM_ASSERT(!fixture.terminal.generation_in_progress);
	const int quiet_queries = fixture.queries->load();
	seconds->store(1100);
	for (int i = 0; i < 10; ++i) fixture.Poll();
	UAM_ASSERT_EQ(fixture.queries->load(), quiet_queries);
	fixture.phase->store(1);
	uam::opencode_terminal::Checkpoint(fixture.terminal);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Busy));
	seconds->store(1200);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Busy));
	UAM_ASSERT(fixture.terminal.uses_prompt_activity_tracking);
}

UAM_TEST(OpenCodeTerminalNativeReceiptsCannotReplaceShutdownState)
{
	TerminalObservationFixture fixture;
	fixture.phase->store(1);
	fixture.Poll();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (fixture.queries->load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	UAM_ASSERT_EQ(fixture.queries->load(), 2);
	fixture.terminal.lifecycle_state = uam::CliTerminalLifecycleState::ShuttingDown;
	fixture.terminal.shutdown_requested_time_s = 123.0;
	const int queries = fixture.queries->load();
	for (int i = 0; i < 5; ++i)
		UAM_ASSERT(fixture.Poll() == ProviderTerminalActivity::Unavailable);
	UAM_ASSERT(fixture.terminal.lifecycle_state == uam::CliTerminalLifecycleState::ShuttingDown);
	UAM_ASSERT_EQ(fixture.terminal.shutdown_requested_time_s, 123.0);
	UAM_ASSERT_EQ(fixture.queries->load(), queries);
}


UAM_TEST(OpenCodeTerminalWindowsSeparatorSnapshotKeepsNativeActivity)
{
	TerminalObservationFixture fixture(true, {}, "C:/opaque remote/workspace");
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	UAM_ASSERT(uam::opencode_terminal::MatchesHost(fixture.terminal, fixture.host));
	uam::opencode_terminal::Checkpoint(fixture.terminal);
	fixture.phase->store(1);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Busy));
	fixture.phase->store(0);
	UAM_ASSERT(fixture.Await(ProviderTerminalActivity::Complete));
	UAM_ASSERT(fixture.terminal.uses_prompt_activity_tracking);
}
