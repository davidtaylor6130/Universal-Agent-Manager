#include "test_harness.h"
#include "app/runtime_activity.h"

#include <cstdlib>
#include <fstream>

#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/command_line_words.h"
#include "common/runtime/terminal/terminal_launch.h"
#include "common/provider/provider_runtime.h"
#include "remote/runner_client.h"
#include "remote/runner_service_posix.h"

#if defined(__APPLE__)
#include <cerrno>
#include <cstdio>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace uam_test;


std::optional<int> RunOpenCodeSessionCreateFixtureIfRequested(int argc, char* argv[])
{
	if (argc >= 2 && std::string_view(argv[1]) == "--uam-test-opencode-terminal")
	{
		std::string line;
		while (std::getline(std::cin, line)) {}
		return 0;
	}
	if (argc != 5 || std::string_view(argv[1]) != "--uam-test-opencode-create" || std::string_view(argv[4]) != "acp") return std::nullopt;
	const std::string mode = argv[2];
	const fs::path marker = argv[3];
	const char* fixture_env = std::getenv("UAM_TEST_SESSION_CREATE");
	if (fixture_env == nullptr || std::string_view(fixture_env) != "isolated") return 8;
	std::string line;
	if (!std::getline(std::cin, line)) return 2;
	const nlohmann::json initialize = nlohmann::json::parse(line);
	if (initialize.value("method", "") != "initialize" || initialize["id"] != 1) return 3;
	if (mode == "eof") return 0;
	if (mode == "unsupported")
	{
		std::cout << R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":2}})" << std::endl;
		while (std::getline(std::cin, line)) {}
		return 0;
	}
	std::cout << R"({"jsonrpc":"2.0","id":99,"error":{"message":"unrelated"}})" << '\n';
	std::cout << R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" << std::endl;
	if (!std::getline(std::cin, line)) return 4;
	const nlohmann::json request = nlohmann::json::parse(line);
	if (request.value("method", "") != "session/new" || request["id"] != 2 ||
	    !fs::equivalent(uam::paths::PathFromUtf8(request["params"]["cwd"].get<std::string>()), fs::current_path()) ||
	    request["params"]["mcpServers"] != nlohmann::json::array()) return 5;
	if (!uam::io::WriteTextFile(marker, "ready")) return 6;
	if (mode == "error") std::cout << R"({"jsonrpc":"2.0","id":2,"error":{"message":"fixture rejected"}})" << std::endl;
	else if (mode == "invalid") std::cout << R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"../invalid"}})" << std::endl;
	else if (mode != "cancel") std::cout << R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"ses_fixture123"}})" << std::endl;
	while (std::getline(std::cin, line)) {}
	if (mode == "teardown_noise")
	{
		if (!uam::io::WriteTextFile(marker, "closed")) return 6;
		std::cout << "shutdown diagnostic\n" << std::flush;
		return 0;
	}
	if (mode == "success" || mode == "teardown_error") return uam::io::WriteTextFile(marker, "closed") && mode == "success" ? 0 : 7;
	return 0;
}

UAM_TEST(OpenCodeWorkerKeepsOptionLikePromptsAsMessageText)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
	profile.interactive_command = "custom-opencode --print-logs";
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	const AppSettings settings;
	for (const std::string& prompt : {"--help", "-m unexpected", "ordinary message"})
	{
		const std::vector<std::string> argv = runtime.BuildWorkerArgv(profile, settings, prompt, "provider/model");
		UAM_ASSERT(argv.size() >= 4);
		UAM_ASSERT_EQ(argv[0], std::string("custom-opencode"));
		UAM_ASSERT_EQ(argv[1], std::string("--print-logs"));
		UAM_ASSERT_EQ(argv[2], std::string("run"));
		UAM_ASSERT_EQ(argv[argv.size() - 2], std::string("--"));
		UAM_ASSERT_EQ(argv.back(), prompt);
	}
	const ChatSession chat;
	const std::vector<std::string> discovery = runtime.BuildNativeDiscoveryArgv(profile);
	UAM_ASSERT_EQ(discovery, (std::vector<std::string>{"custom-opencode", "--print-logs", "session", "list", "--format", "json", "--pure", "--max-count", "200"}));
	const std::vector<std::string> structured = runtime.BuildStructuredLaunchArgv(profile, chat);
	UAM_ASSERT_EQ(structured, (std::vector<std::string>{"custom-opencode", "--print-logs", "acp"}));
#endif
}

UAM_TEST(OpenCodeNativeSessionCreationCorrelatesResponsesAndStopsOnFailure)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-create");
	ScopedEnvVar fixture_env("UAM_TEST_SESSION_CREATE", "isolated");
	const std::string executable = uam::paths::Utf8PathString(PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath());
	for (const std::string mode : {"success", "teardown_error", "teardown_noise", "eof", "error", "invalid", "cancel", "unsupported"})
	{
		const fs::path marker = temp.root / (mode + ".txt");
		std::stop_source stop;
		std::jthread cancel;
		if (mode == "cancel")
		{
			cancel = std::jthread([&](std::stop_token token)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				while (!token.stop_requested() && !fs::exists(marker) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				stop.request_stop();
			});
		}
		const auto started = std::chrono::steady_clock::now();
		std::string error;
		ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
		profile.interactive_command = uam::shell::JoinEscapedArgs(
		    {executable, "--uam-test-opencode-create", mode, uam::paths::Utf8PathString(marker)});
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
		UAM_ASSERT(runtime.RequiresNativeSessionCreation());
		const std::string session = runtime.CreateNativeSession(profile, temp.root, stop.get_token(), &error);
		cancel.request_stop();
		if (cancel.joinable()) cancel.join();
		UAM_ASSERT(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
		if (mode == "success" || mode == "teardown_error" || mode == "teardown_noise")
		{
			if (session != "ses_fixture123") throw std::runtime_error("OpenCode session fixture failed: " + error);
			UAM_ASSERT(error.empty());
			std::ifstream saved(marker);
			std::string contents;
			saved >> contents;
			UAM_ASSERT_EQ(contents, "closed");
		}
		else
		{
			UAM_ASSERT(session.empty());
			UAM_ASSERT(!error.empty());
			if (mode == "cancel")
			{
				UAM_ASSERT(fs::exists(marker));
				UAM_ASSERT(error.find("canceled") != std::string::npos);
			}
			if (mode == "error") UAM_ASSERT(error.find("fixture rejected") != std::string::npos);
			if (mode == "unsupported") UAM_ASSERT(error.find("unsupported ACP protocol version") != std::string::npos);
		}
	}
#endif
}

UAM_TEST(StoppingOrSwitchingToAcpCancelsPendingNativeSessionCreation)
{
	uam::AppState app;
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.front();
	terminal.frontend_chat_id = "pending-native";
	const std::shared_ptr<std::stop_source> first = std::make_shared<std::stop_source>();
	terminal.native_session_setup_cancel = first;
	uam::StopCliTerminal(terminal);
	UAM_ASSERT(first->stop_requested());
	UAM_ASSERT(!terminal.native_session_setup_cancel);
	const std::shared_ptr<std::stop_source> second = std::make_shared<std::stop_source>();
	terminal.native_session_setup_cancel = second;
	std::string error;
	UAM_ASSERT(uam::PrepareCliTerminalForAcpLaunch(app, terminal.frontend_chat_id, &error));
	UAM_ASSERT(second->stop_requested());
	UAM_ASSERT(!terminal.native_session_setup_cancel);
	UAM_ASSERT(!terminal.running);
}

#if defined(__APPLE__)
namespace
{
	void ReportTerminalResize(int)
	{
		constexpr char message[] = "terminal-resized\n";
		(void)write(STDOUT_FILENO, message, sizeof(message) - 1);
	}
}

std::optional<int> RunMacTerminalSignalFixtureIfRequested(int argc, char* argv[])
{
	if (argc != 2 || std::string_view(argv[1]) != "--uam-test-terminal-signals") return std::nullopt;
	sigset_t mask;
	(void)sigprocmask(SIG_SETMASK, nullptr, &mask);
	struct sigaction disposition{};
	(void)sigaction(SIGINT, nullptr, &disposition);
	const bool owns_terminal = isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) == getpgrp() && getsid(0) == getpid();
	struct termios settings{};
	if (tcgetattr(STDIN_FILENO, &settings) != 0) return 2;
	cfmakeraw(&settings);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0) return 3;
	struct sigaction resize_action{};
	resize_action.sa_handler = ReportTerminalResize;
	sigemptyset(&resize_action.sa_mask);
	if (sigaction(SIGWINCH, &resize_action, nullptr) != 0) return 4;
	std::printf("ready owns=%d blocked=%d ignored=%d\n", owns_terminal, sigismember(&mask, SIGWINCH), disposition.sa_handler == SIG_IGN);
	if (uam::env::GetNonEmptyString("UAM_TEST_TERMINAL_PRIVATE") == "one-use terminal fixture")
		std::printf("private-environment-ok\n");
	std::fflush(stdout);
	char byte;
	while (true)
	{
		const ssize_t count = read(STDIN_FILENO, &byte, 1);
		if (count == 1 && byte == 'q') return 0;
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return 5;
	}
}

UAM_TEST(MacTerminalOwnsControllingTtyAndResetsInheritedSignals)
{
	TempDir temp("uam-terminal-signal-inheritance");
	IPlatformTerminalRuntime& runtime = PlatformServicesFactory::Instance().terminal_runtime;
	uam::CliTerminalState terminal;
	terminal.rows = 24;
	terminal.cols = 80;
	const std::string executable = "./" + uam::paths::Utf8PathString(
	    fs::relative(PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()));
	sigset_t blocked, previous_mask;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGWINCH);
	UAM_ASSERT(pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask) == 0);
	struct sigaction ignored{}, previous_int{};
	ignored.sa_handler = SIG_IGN;
	sigemptyset(&ignored.sa_mask);
	const bool ignored_installed = sigaction(SIGINT, &ignored, &previous_int) == 0;
	std::string error;
	const bool started = ignored_installed && runtime.StartCliTerminalProcess(terminal, temp.root, {executable, "--uam-test-terminal-signals"}, &error);
	(void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
	if (ignored_installed) (void)sigaction(SIGINT, &previous_int, nullptr);
	UAM_ASSERT(started);

	std::string output;
	const auto read_until = [&](std::string_view expected)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline && output.find(expected) == std::string::npos)
		{
			char buffer[1024];
			const std::ptrdiff_t count = runtime.ReadCliTerminalOutput(terminal, buffer, sizeof(buffer));
			if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	};
	read_until("ready owns=");
	const bool owned_and_reset = output.find("ready owns=1 blocked=0 ignored=0") != std::string::npos;
	terminal.rows = 31;
	terminal.cols = 99;
	runtime.ResizeCliTerminal(terminal);
	read_until("terminal-resized");
	const bool resize_delivered = output.find("terminal-resized") != std::string::npos;
	(void)runtime.WriteToCliTerminal(terminal, "q", 1);
	uam::StopCliTerminal(terminal);
	UAM_ASSERT(owned_and_reset);
	UAM_ASSERT(resize_delivered);
}

UAM_TEST(MacTerminalWatchdogReadinessSurvivesInterruptedWait)
{
	TempDir temp("uam-watchdog-signals");
	uam::CliTerminalState terminal;
	static volatile sig_atomic_t received_signal = 0;
	received_signal = 0;
	struct sigaction action{}, previous_action{};
	action.sa_handler = [](int) { received_signal = 1; };
	sigemptyset(&action.sa_mask);
	UAM_ASSERT(sigaction(SIGUSR1, &action, &previous_action) == 0);
	const pthread_t launch_thread = pthread_self();
	std::jthread signals([launch_thread](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
			(void)pthread_kill(launch_thread, SIGUSR1);
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
	});
	std::string error;
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(
	    terminal, temp.root, {"/bin/sh", "-c", "sleep 10"}, &error);
	signals.request_stop();
	signals.join();
	(void)sigaction(SIGUSR1, &previous_action, nullptr);
	uam::StopCliTerminal(terminal);
	UAM_ASSERT(received_signal != 0);
	if (!started) throw std::runtime_error(error);
}

UAM_TEST(PrivateTerminalLaunchClaimsEnvironmentOnceAndRetainsPty)
{
	TempDir temp("uam-th");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	IPlatformTerminalRuntime& runtime = PlatformServicesFactory::Instance().terminal_runtime;
	const fs::path executable = service.ResolveCurrentExecutablePath();
	const fs::path runner = executable.parent_path() / "uam-runner";
	const fs::path socket = temp.root / "r.sock";
	uam::platform::StdioProcessPlatformFields server;
	uam::CliTerminalState terminal;
	struct ProcessGuard
	{
		IPlatformProcessService& service;
		uam::platform::StdioProcessPlatformFields& server;
		uam::CliTerminalState& terminal;
		~ProcessGuard()
		{
			uam::StopCliTerminal(terminal);
			service.StopStdioProcess(server, true);
			service.CloseStdioProcessHandles(server);
		}
	} guard{service, server, terminal};
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(server, temp.root,
	    {runner.string(), "serve", "--socket", socket.string()}, &error));
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));
	uam::remote::RunnerClient client(service, {runner.string(), "bridge", "--socket", socket.string()});
	const std::string channel_id = service.GenerateUuid();
	const std::string spec = uam::remote::BuildProcessProxySpec("terminal", temp.root,
	    {executable.string(), "--uam-test-terminal-signals"},
	    {{"UAM_TEST_TERMINAL_PRIVATE", "one-use terminal fixture"}});
	// The command-line entry point must continue refusing private environment values.
	UAM_ASSERT_EQ(uam::remote::RunTerminalProcess(spec), 2);
	UAM_ASSERT(client.OpenChannel(channel_id, &error, false, 10000));
	UAM_ASSERT(client.WriteChannel(channel_id, "desktopToRemote", spec, &error));
	const std::vector<std::string> ssh = uam::remote::BuildRemoteTerminalSshArgv(
	    "test-host", "macos", "test-version", temp.root, {"provider"}, {}, channel_id);
	UAM_ASSERT(!ssh.empty() && ssh.back().find("--channel " + channel_id) != std::string::npos);
	UAM_ASSERT(ssh.back().find(spec) == std::string::npos);
	UAM_ASSERT(ssh.back().find("one-use terminal fixture") == std::string::npos);
	UAM_ASSERT(uam::remote::BuildRemoteTerminalSshArgv(
	    "test-host", "macos", "test-version", temp.root, {"provider"}, {}, "bad;id").empty());
	const std::vector<std::string> windows_ssh = uam::remote::BuildRemoteTerminalSshArgv(
	    "test-host", "windows", "test-version", fs::path("C:\\Work"), {"provider"}, {}, channel_id);
	UAM_ASSERT(!windows_ssh.empty() && windows_ssh.back().ends_with("--channel " + channel_id + "\""));
	terminal.rows = 24;
	terminal.cols = 80;
	UAM_ASSERT(runtime.StartCliTerminalProcess(terminal, temp.root,
	    {runner.string(), "terminal", "--channel", channel_id, "--socket", socket.string()}, &error));
	terminal.running = true;
	std::string output;
	const auto read_until = [&](std::string_view expected)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline && output.find(expected) == std::string::npos)
		{
			char buffer[1024];
			const std::ptrdiff_t count = runtime.ReadCliTerminalOutput(terminal, buffer, sizeof(buffer));
			if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	};
	read_until("private-environment-ok");
	UAM_ASSERT(output.find("ready owns=1 blocked=0 ignored=0") != std::string::npos);
	UAM_ASSERT(output.find("private-environment-ok") != std::string::npos);
	terminal.rows = 31;
	terminal.cols = 99;
	runtime.ResizeCliTerminal(terminal);
	read_until("terminal-resized");
	UAM_ASSERT(output.find("terminal-resized") != std::string::npos);
	std::string reclaimed;
	UAM_ASSERT(!client.TakeChannel(channel_id, "desktopToRemote", reclaimed, &error));
	UAM_ASSERT(reclaimed.empty());
	UAM_ASSERT(runtime.WriteToCliTerminal(terminal, "q", 1));
	uam::StopCliTerminal(terminal);
	client.Disconnect();
	UAM_ASSERT_EQ(uam::remote::StopRunnerService(socket), 0);
	int server_exit_code = -1;
	const std::chrono::steady_clock::time_point shutdown_deadline =
	    std::chrono::steady_clock::now() + std::chrono::seconds(2);
	bool server_exited = service.PollStdioProcessExited(server, &server_exit_code);
	while (!server_exited && std::chrono::steady_clock::now() < shutdown_deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		server_exited = service.PollStdioProcessExited(server, &server_exit_code);
	}
	UAM_ASSERT(server_exited);
	UAM_ASSERT_EQ(server_exit_code, 0);
}
#endif


UAM_TEST(OpenCodeTerminalLaunchPersistsTheSameSessionBeforeStarting)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-launch-persistence");
	uam::AppState app;
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "storage unavailable"));
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	provider.output_mode = uam::provider_profile_constants::kOutputModeCli;
	provider.interactive_command = "\"" + uam::paths::Utf8PathString(PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()) + "\" --uam-test-opencode-terminal";
	app.provider_profiles = {provider};
	app.settings.active_provider_id = provider.id;
	uam::execution_hosts::Normalize(app.settings.execution_hosts);
	ChatSession chat;
	chat.id = "opencode-persist-before-launch";
	chat.provider_id = provider.id;
	chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
	uam::CliTerminalState terminal;
	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(terminal.last_error.find("saved session") != std::string::npos);
	chat.native_session_id = "ses_persistedfixture";
	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(terminal.last_error.find("Could not save") != std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, "ses_persistedfixture");
	app.data_root = temp.root / "writable";
	const bool launched = uam::StartCliTerminalForChat(app, terminal, chat, 24, 80);
	const bool running = terminal.running;
	const std::string launch_error = terminal.last_error;
	const std::string attached = terminal.attached_session_id;
	uam::StopCliTerminal(terminal, false, uam::CliTerminalStopMode::FastExit);
	if (!launched) throw std::runtime_error("OpenCode terminal fixture launch failed: " + launch_error);
	UAM_ASSERT(running);
	UAM_ASSERT_EQ(attached, "ses_persistedfixture");
	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().native_session_id, "ses_persistedfixture");
	UAM_ASSERT_EQ(chat.native_session_id, saved.front().native_session_id);
#endif
}

UAM_TEST(CliTerminalRejectsImportedReadOnlyTranscriptBeforeProviderLaunch)
{
	uam::AppState app;
	uam::CliTerminalState terminal;
	ChatSession chat;
	chat.id = "chat-imported-read-only";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.imported_read_only = true;

	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Disabled);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "Imported transcripts are read-only"));
}

UAM_TEST(RemoteCliHandoffWaitsWithoutErrorUntilStructuredStopIsConfirmed)
{
	TempDir temp("uam-cli-remote-handoff");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "remote-cli-handoff";
	chat.execution_host_id = "ssh-fixture";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->running = true;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, argv, &error));
	app.acp_sessions.push_back(std::move(session));
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(app.acp_sessions.front()->remote_stop_pending);
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(error.empty());
	for (int attempt = 0; attempt < 100 && !app.pending_acp_remote_stops.empty(); ++attempt)
	{
		(void)uam::PollAllAcpSessions(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(app.pending_acp_remote_stops.empty());
	UAM_ASSERT(uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(error.empty());
	app.acp_sessions.front()->remote_stop_unconfirmed = true;
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(!error.empty());
	app.acp_sessions.clear();
	std::unique_ptr<uam::PendingAcpRemoteStop> orphan_stop = std::make_unique<uam::PendingAcpRemoteStop>();
	orphan_stop->chat_id = chat.id;
	app.pending_acp_remote_stops.push_back(std::move(orphan_stop));
	UAM_ASSERT(!uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
	UAM_ASSERT(error.empty());
	app.pending_acp_remote_stops.clear();
	UAM_ASSERT(uam::PrepareAcpSessionForCliTerminalLaunch(app, app.chats.front(), &error));
}

#if defined(__APPLE__)
UAM_TEST(CliTerminalRoutesRemoteChatsThroughSshWithoutLaunchingTheProviderLocally)
{
	TempDir temp("uam-remote-terminal-route");
	const fs::path captured = temp.root / "ssh-argv.txt";
	const fs::path ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    ssh, "#!/bin/sh\nprintf '%s\\n' \"$@\" > " + ShellQuoteForTest(captured.string()) +
	             "\nprintf 'remote-terminal-ready\\n'\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar path("PATH", temp.root.string() + ":" + inherited_path);

	uam::AppState app;
	app.data_root = temp.root;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	provider.output_mode = uam::provider_profile_constants::kOutputModeCli;
	provider.interactive_command = "/usr/bin/printf provider-must-stay-encoded";
	app.provider_profiles = {provider};
	app.settings.active_provider_id = provider.id;
	ExecutionHost host;
	host.id = "lab";
	host.label = "Lab";
	host.transport = "ssh";
	host.ssh_alias = "home-lab";
	host.runner_status = "ready";
	host.runner_version = "4.5.7";
	host.platform = "linux";
	host.architecture = "arm64";
	app.settings.execution_hosts = {host};
	uam::execution_hosts::Normalize(app.settings.execution_hosts);

	ChatSession chat;
	chat.id = "remote-terminal-chat";
	chat.provider_id = provider.id;
	chat.execution_host_id = host.id;
	chat.workspace_directory = temp.root.string();
	chat.native_session_id = "ses_remote_saved";
	uam::CliTerminalState terminal;
	for (const bool resume_enabled : {false, true})
	{
		app.provider_profiles.front().supports_resume = resume_enabled;
		app.provider_profiles.front().resume_argument = resume_enabled ? " " : provider.resume_argument;
		const bool launched = uam::StartCliTerminalForChat(app, terminal, chat, 24, 80);
		const std::string failure = terminal.last_error;
		uam::StopCliTerminal(terminal, true, uam::CliTerminalStopMode::FastExit);
		UAM_ASSERT(!launched);
		UAM_ASSERT(failure.find("enable session resume") != std::string::npos);
		UAM_ASSERT(!fs::exists(captured));
	}
	app.provider_profiles.front() = provider;
	chat.native_session_id.clear();
	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT(terminal.last_error.find("saved session") != std::string::npos);
	UAM_ASSERT(!fs::exists(captured));
	chat.native_session_id = "ses_remote_saved";
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "storage unavailable"));
	UAM_ASSERT(!uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT(terminal.last_error.find("Could not save") != std::string::npos);
	UAM_ASSERT_EQ(chat.native_session_id, "ses_remote_saved");
	UAM_ASSERT(!fs::exists(captured));
	app.data_root = temp.root / "data";
	UAM_ASSERT(uam::StartCliTerminalForChat(app, terminal, chat, 24, 80));
	UAM_ASSERT_EQ(terminal.attached_session_id, chat.native_session_id);
	for (int attempt = 0; attempt < 100 &&
	     (!fs::exists(captured) ||
	      !uam::strings::Contains(uam::io::ReadTextFile(captured), "uam-runner")); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(captured));
	const std::string args = uam::io::ReadTextFile(captured);
	UAM_ASSERT(uam::strings::Contains(args, "home-lab"));
	UAM_ASSERT(uam::strings::Contains(args, "uam-runner"));
	UAM_ASSERT(uam::strings::Contains(args, "terminal"));
	UAM_ASSERT(!uam::strings::Contains(args, "provider-must-stay-encoded"));
	uam::StopCliTerminal(terminal, true, uam::CliTerminalStopMode::FastExit);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, chat.native_session_id);
}
#endif

UAM_TEST(CliTurnInactivityRecoveryIgnoresProviderOutputNoiseAndUsesInterruptGrace)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal.last_busy_time_s = 30.0;
	terminal.last_user_input_time_s = 89.0;
	terminal.last_ai_output_time_s = 89.0;
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 89.0, 60.0), uam::CliTerminalInactivityRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 90.0, 60.0), uam::CliTerminalInactivityRecoveryAction::Interrupt);
	terminal.inactivity_interrupt_requested_time_s = 200.0;
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 204.9, 60.0), uam::CliTerminalInactivityRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 205.0, 60.0), uam::CliTerminalInactivityRecoveryAction::Stop);
}

UAM_TEST(ProviderChildEnvironmentIsolationKeepsOnlySelectedProviderApiKeys)
{
	const auto value_for = [](const std::vector<std::pair<std::string, std::string>>& values, std::string_view name) -> const std::string*
	{
		const auto found = std::ranges::find_if(values, [name](const auto& value) { return value.first == name; });
		return found == values.end() ? nullptr : &found->second;
	};

	ProviderProfile codex = ProviderProfileStore::DefaultCodexProfile();
	const auto codex_environment = ProviderRuntimeRegistry::Resolve(codex).BuildInteractiveEnvironment(codex);
	UAM_ASSERT(value_for(codex_environment, "OPENAI_API_KEY") == nullptr);
	UAM_ASSERT(value_for(codex_environment, "ANTHROPIC_API_KEY") != nullptr);
	UAM_ASSERT(value_for(codex_environment, "ANTHROPIC_API_KEY")->empty());
	UAM_ASSERT(value_for(codex_environment, "GEMINI_API_KEY") != nullptr);
	UAM_ASSERT(value_for(codex_environment, "GOOGLE_API_KEY") != nullptr);

	ProviderProfile claude = ProviderProfileStore::DefaultClaudeProfile();
	const auto claude_environment = ProviderRuntimeRegistry::Resolve(claude).BuildInteractiveEnvironment(claude);
	UAM_ASSERT(value_for(claude_environment, "ANTHROPIC_API_KEY") == nullptr);
	UAM_ASSERT(value_for(claude_environment, "OPENAI_API_KEY") != nullptr);

	ProviderProfile opencode = ProviderProfileStore::DefaultOpenCodeProfile();
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(opencode).BuildInteractiveEnvironment(opencode).empty());

	uam::AppState worker_app;
	const uam::ProviderWorkerInvocation worker = uam::BuildProviderWorkerInvocation(
	    worker_app, codex, AppSettings{}, "Review without unrelated provider credentials.", "",
	    uam::ProviderWorkerPathMode::BasePath);
	UAM_ASSERT(!worker.Empty());
	UAM_ASSERT(value_for(worker.environment_overrides, "OPENAI_API_KEY") == nullptr);
	UAM_ASSERT(value_for(worker.environment_overrides, "ANTHROPIC_API_KEY") != nullptr);
	UAM_ASSERT(value_for(worker.environment_overrides, "ANTHROPIC_API_KEY")->empty());

	ScopedEnvVar preserve("UAM_PRESERVE_PROVIDER_CHILD_SECRETS", "1");
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(codex).BuildInteractiveEnvironment(codex).empty());
}

UAM_TEST(CliSilentTurnInterruptsThenStopsWithoutReplayingPrompt)
{
	TempDir temp("uam-cli-inactivity-timeout");
	uam::AppState app;
	app.settings.active_turn_inactivity_timeout_seconds = 60;
	uam::CliTerminalState terminal;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "trap '' HUP INT TERM; while :; do sleep 1; done"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, argv, &error));
	terminal.running = true;
	terminal.should_launch = true;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	terminal.turn_state = uam::CliTerminalTurnState::Busy;
	terminal.generation_in_progress = true;
	terminal.last_busy_time_s = 1.0;
	terminal.last_user_input_time_s = 1.0;
	terminal.last_ai_output_time_s = 1.0;

	UAM_ASSERT(uam::HandleCliTerminalInactivityTimeout(app, terminal, 61.0));
	UAM_ASSERT(terminal.running);
	UAM_ASSERT_EQ(terminal.inactivity_interrupt_requested_time_s, 61.0);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "will not be replayed"));
	UAM_ASSERT(uam::HandleCliTerminalInactivityTimeout(app, terminal, 66.0));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT(uam::strings::Contains(terminal.last_error, "not replayed"));
}

UAM_TEST(GeminiCliCompatibilityAcceptsCurrentStableVersions)
{
	UAM_ASSERT_EQ(std::string(uam::PreferredGeminiCliVersion()), std::string("latest"));
	UAM_ASSERT_EQ(uam::SupportedGeminiCliVersionsLabel(), std::string("0.55.1 or newer (verified 2026-08-27)"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.38.1"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.55.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.55.1"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("0.56.0"));
	UAM_ASSERT(uam::IsSupportedGeminiCliVersion("1.0.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.30.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0..0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.36."));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("-1.36.0"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("0.39.0-beta"));
	UAM_ASSERT(!uam::IsSupportedGeminiCliVersion("not-a-version"));
}

UAM_TEST(GeminiPromptClassifierStripsAnsiAndDetectsPrompt)
{
	const std::string output = "\x1b[33mThinking...\x1b[0m\r\n\xe2\x94\x82 > Type your message or @path\r\n";
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultGeminiProfile()).RecentOutputIndicatesInputPrompt(output));

	const std::string stripped = uam::StripTerminalControlSequencesForLifecycle("\x1b[31mhello\x1b[0m\b!");
	UAM_ASSERT_EQ(stripped, std::string("hell!"));
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultGeminiProfile()).RecentOutputIndicatesInputPrompt(std::string_view("xx\xe2\x94\x82 > Type your message yy").substr(2, 23)));
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultGeminiProfile()).RecentOutputIndicatesInputPrompt("tool output is still streaming\nno prompt yet"));
}

UAM_TEST(CliInitialPromptCanSettleImmediately)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal, false);
	terminal.last_busy_time_s = 100.0;
	terminal.current_turn_output_bytes = "\xE2\x80\xBA Send message";

	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
}

UAM_TEST(CliStalePromptCannotPoisonLaterTurnOutput)
{
	const std::string old_prompt = "\xE2\x80\xBA Send message";
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.recent_output_bytes = old_prompt;
	terminal.current_turn_output_bytes = old_prompt;

	uam::MarkCliTerminalTurnBusy(terminal);
	terminal.last_busy_time_s = 100.0;

	terminal.current_turn_output_bytes.append(old_prompt);
	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
	UAM_ASSERT(terminal.current_turn_output_bytes.empty());

	terminal.current_turn_output_bytes.append("\nWorking on the next turn");
	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.5));

	terminal.current_turn_output_bytes.append("\n\xE2\x80\xBA Send message");
	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.6));
	UAM_ASSERT_EQ(terminal.recent_output_bytes, old_prompt);
}

UAM_TEST(CliFastPromptSettlesWithoutAdditionalOutput)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal);
	terminal.last_busy_time_s = 100.0;
	terminal.current_turn_output_bytes = "\xE2\x80\xBA Send message";

	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(
	    terminal,
	    ProviderRuntimeRegistry::Resolve(ProviderProfileStore::DefaultCodexProfile()).RecentOutputIndicatesInputPrompt(terminal.current_turn_output_bytes),
	    true,
	    100.05));
	UAM_ASSERT(terminal.current_turn_output_bytes.empty());
	UAM_ASSERT(uam::CliTerminalPromptConfirmsTurnIdle(terminal, false, false, 100.3));
}

UAM_TEST(CliLifecycleTransitionsDriveBackgroundShutdownEligibility)
{
	uam::AppState app;
	app.settings.cli_idle_timeout_seconds = 60;
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.frontend_chat_id = "chat-1";
	terminal.attached_chat_id = "chat-1";
	terminal.attached_session_id = "native-1";

	uam::MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(uam::kCliTerminalProcessingLifecycleStates.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(uam::kCliTerminalQuitCommand, std::string_view("/quit\r\n"));
	UAM_ASSERT_EQ(uam::kCliTerminalDefaultRows, 24);
	UAM_ASSERT_EQ(uam::kCliTerminalDefaultCols, 80);
	UAM_ASSERT_EQ(uam::ClampCliTerminalResizeRows(0), 1);
	UAM_ASSERT_EQ(uam::ClampCliTerminalResizeCols(0), 1);
	UAM_ASSERT_EQ(uam::ClampCliTerminalLaunchRows(0), 8);
	UAM_ASSERT_EQ(uam::ClampCliTerminalLaunchCols(0), 20);
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField("quote\" newline\nslash\\"), std::string("\"quote\\\" newline slash\\\\\""));
	const std::string nearly_full_diagnostic_field(uam::kCliDiagnosticFieldMaxBytes - 1, 'x');
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField(nearly_full_diagnostic_field + "\""), "\"" + nearly_full_diagnostic_field + "...\"");
	UAM_ASSERT_EQ(uam::CliDiagnosticQuotedField(nearly_full_diagnostic_field + "\\"), "\"" + nearly_full_diagnostic_field + "...\"");
	UAM_ASSERT(uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::Busy));
	UAM_ASSERT(uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::ShuttingDown));
	UAM_ASSERT(!uam::CliTerminalLifecycleStateIsProcessing(uam::CliTerminalLifecycleState::Idle));
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Busy);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Busy);
	UAM_ASSERT_EQ(uam::CliLifecycleStateLabel(terminal), std::string(uam::CliTerminalLifecycleStateLabel(terminal)));
	UAM_ASSERT_EQ(std::string(uam::CliTurnStateLabel(terminal)), std::string("busy"));
	UAM_ASSERT(terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 1.0;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 121.0));

	uam::MarkCliTerminalTurnIdle(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Idle);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Idle);
	UAM_ASSERT_EQ(std::string(uam::CliTurnStateLabel(terminal)), std::string("idle"));
	UAM_ASSERT(!terminal.generation_in_progress);
	terminal.last_idle_confirmed_time_s = 59.0;
	UAM_ASSERT(uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	app.settings.cli_idle_timeout_seconds = 120;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	app.settings.cli_idle_timeout_seconds = 60;
	UAM_ASSERT(uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, std::string_view("xxchat-2yy").substr(2, 6), 120.0));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-1", 120.0));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "native-1", 120.0));

	terminal.ui_attached = true;
	terminal.ui_attachment_id = "replacement-view";
	UAM_ASSERT(!uam::DetachCliTerminalUi(terminal, "old-view"));
	UAM_ASSERT(!uam::DetachCliTerminalUi(terminal, ""));
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	UAM_ASSERT(uam::DetachCliTerminalUi(terminal, "replacement-view"));
	UAM_ASSERT(uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "chat-2", 120.0));
	UAM_ASSERT(uam::DetachCliTerminalUi(terminal, "replacement-view"));


	app.chats_with_unseen_updates.insert("chat-1");
	uam::ClearCliReadyForChat(app, std::string_view("xx chat-1 yy").substr(2, 8));
	UAM_ASSERT(app.chats_with_unseen_updates.empty());
}

UAM_TEST(UnknownNativeActivityDoesNotReportReadyOrAllowAutomaticInterruption)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "unknown-native";
	chat.provider_id = ProviderProfileStore::DefaultOpenCodeProfile().id;
	chat.native_session_id = "ses_unknown";
	app.chats.push_back(chat);
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.back();
	terminal.running = true;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.uses_prompt_activity_tracking = ProviderRuntimeRegistry::ResolveById(chat.provider_id).SupportsInteractivePromptTracking();
	UAM_ASSERT(!terminal.uses_prompt_activity_tracking);
	uam::MarkCliTerminalTurnBusy(terminal);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Unknown);
	UAM_ASSERT_EQ(terminal.turn_state, uam::CliTerminalTurnState::Unknown);
	UAM_ASSERT(!terminal.generation_in_progress);
	UAM_ASSERT(!uam::CliTerminalLifecycleIsProcessing(terminal));
	UAM_ASSERT(!uam::CliTerminalLifecycleIsIdleLive(terminal));
	UAM_ASSERT(!uam::CliTerminalPromptConfirmsTurnIdle(terminal, true, true, 1000.0));
	UAM_ASSERT_EQ(uam::CliTerminalInactivityRecovery(terminal, 1000.0, 1.0), uam::CliTerminalInactivityRecoveryAction::None);
	terminal.last_idle_confirmed_time_s = 1.0;
	UAM_ASSERT(!uam::IsCliTerminalEligibleForBackgroundIdleShutdown(app, terminal, "elsewhere", 1000.0));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, chat.id));
	UAM_ASSERT(uam::RuntimeShouldKeepSystemAwake(app));
	std::string error;
	UAM_ASSERT(!uam::PrepareCliTerminalForAcpLaunch(app, chat.id, &error));
	UAM_ASSERT(error.find("Exit the native CLI") != std::string::npos && terminal.running);
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "new instructions", false, &error));
	UAM_ASSERT(terminal.pending_steer_prompt.empty());
	terminal.pending_steer_prompt = "retain pending prompt";
	terminal.pending_steer_started_time_s = 1.0;
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 1000.0), uam::CliTerminalSteerRecoveryAction::None);
	const nlohmann::json state = uam::StateSerializer::Serialize(app);
	const nlohmann::json& serialized = state["chats"][0]["cliTerminal"];
	UAM_ASSERT_EQ(serialized["lifecycleState"], "unknown");
	UAM_ASSERT_EQ(serialized["turnState"], "unknown");
	UAM_ASSERT(!serialized["processing"].get<bool>() && !serialized["active"].get<bool>());
	UAM_ASSERT(serialized["running"].get<bool>());
	terminal.running = false;
	uam::MarkCliTerminalStopped(terminal);
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, chat.id));
#endif
}

UAM_TEST(CliWriteTransportFailureCannotRemainRunning)
{
	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.should_launch = true;
	uam::MarkCliTerminalTurnBusy(terminal);

	constexpr char input = 'x';
	UAM_ASSERT(!uam::WriteToCliTerminal(terminal, &input, 1));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Stopped);
	UAM_ASSERT(!terminal.last_error.empty());
}

UAM_TEST(CliMissingOutputTransportCannotRemainRunning)
{
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	uam::CliTerminalState terminal;
	terminal.running = true;
	terminal.should_launch = true;
	uam::MarkCliTerminalTurnBusy(terminal);

	UAM_ASSERT(uam::PollCliTerminal(nullptr, app, terminal, false));
	UAM_ASSERT(!terminal.running);
	UAM_ASSERT(!terminal.should_launch);
	UAM_ASSERT_EQ(terminal.lifecycle_state, uam::CliTerminalLifecycleState::Stopped);
	UAM_ASSERT(!terminal.last_error.empty());
}

UAM_TEST(CliOutputDoesNotTriggerRedundantStateSerialization)
{
	TempDir temp("uam-terminal-output-change");
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "terminal-output-only";
	chat.provider_id = ProviderProfileStore::DefaultOpenCodeProfile().id;
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.rows = 24;
	terminal.cols = 80;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	terminal.turn_state = uam::CliTerminalTurnState::Idle;
	terminal.last_sync_time_s = uam::GetAppTimeSeconds();
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "<nul set /p=raw-output & ping -n 11 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "printf raw-output; sleep 10"};
#endif
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, argv, &error));
	terminal.running = true;

	bool polled_output = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline && !polled_output)
	{
		const bool changed = uam::PollCliTerminal(nullptr, app, terminal, false);
		if (terminal.recent_output_bytes.find("raw-output") != std::string::npos)
		{
			UAM_ASSERT(!changed);
			polled_output = true;
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	uam::StopCliTerminal(terminal);
	UAM_ASSERT(polled_output);
}

UAM_TEST(CliTerminalSteeringInputIsBracketedAndDropsUnsafeControls)
{
	const std::string input = uam::BuildCliTerminalPromptInput(std::string_view("xx  Change\x1b[31m direction\nnow  yy").substr(2, 30));
	UAM_ASSERT(input.starts_with("\x1b[200~"));
	UAM_ASSERT(input.ends_with("\x1b[201~\r"));
	UAM_ASSERT(input.find('\x1b', 1) == std::string::npos || input.find('\x1b', 1) == input.size() - 7);
	UAM_ASSERT(input.find("Change[31m direction\nnow") != std::string::npos);
	UAM_ASSERT(uam::BuildCliTerminalPromptInput(" \x01\x02 ").empty());

	uam::CliTerminalState terminal;
	terminal.running = true;
	uam::MarkCliTerminalTurnBusy(terminal);
	std::string error;
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Preserve this", false, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));
	UAM_ASSERT(error.find("retained for retry") != std::string::npos);
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Duplicate", false, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));
	UAM_ASSERT(!uam::RequestCliTerminalSteer(terminal, "Changed retry", true, &error));
	UAM_ASSERT_EQ(terminal.pending_steer_prompt, std::string("Preserve this"));

	terminal.pending_steer_started_time_s = 100.0;
	terminal.pending_steer_restart_attempted = false;
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 102.9), uam::CliTerminalSteerRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 103.0), uam::CliTerminalSteerRecoveryAction::Restart);
	terminal.pending_steer_restart_attempted = true;
	terminal.last_error.clear();
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 109.9), uam::CliTerminalSteerRecoveryAction::None);
	UAM_ASSERT_EQ(uam::CliTerminalSteerRecovery(terminal, 110.0), uam::CliTerminalSteerRecoveryAction::ReportTimeout);
}

UAM_TEST(CliTerminalActiveHelpersUseTrimmedIdentity)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.native_session_id = " native-1 ";
	app.chats.push_back(chat);

	UAM_ASSERT(!uam::HasAnyActiveCliTerminal(app));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "chat-1"));
	UAM_ASSERT(uam::ChatExists(app, " chat-1 "));
	UAM_ASSERT(uam::NativeChatMatchesPreferredSyncId(chat, " chat-1 "));
	UAM_ASSERT(uam::NativeChatMatchesPreferredSyncId(chat, " native-1 "));
	UAM_ASSERT(!uam::NativeChatMatchesPreferredSyncId(chat, "   "));
	UAM_ASSERT(!uam::NativeChatMatchesPreferredSyncId(chat, "other-chat"));
	uam::MarkChatUnseen(app, " chat-1 ");
	UAM_ASSERT(app.chats_with_unseen_updates.contains("chat-1"));
	UAM_ASSERT(!app.chats_with_unseen_updates.contains(" chat-1 "));

	app.cli_terminals.push_back(nullptr);
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->running = true;
	terminal->attached_chat_id = " chat-1 ";
	app.cli_terminals.push_back(std::move(terminal));

	UAM_ASSERT(uam::HasAnyActiveCliTerminal(app));
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, "chat-1"));
	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, " chat-1 "));
	UAM_ASSERT(uam::FindCliTerminalForChat(app, std::string_view("xxchat-1yy").substr(2, 6)) != nullptr);
	UAM_ASSERT_EQ(uam::NormalizeChatSyncTargetId(std::string_view("xx chat-1 yy").substr(2, 8)), std::string("chat-1"));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "  "));
	UAM_ASSERT(!uam::ChatHasActiveCliTerminal(app, "other-chat"));
}

UAM_TEST(ChatRuntimeHelpersRecognizeAcpAndCliActiveTurns)
{
	uam::AppState app;
	UAM_ASSERT(!uam::ChatHasActiveAcpSession(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasRunningRuntime(app, "chat-1"));

	auto acp = std::make_unique<uam::AcpSessionState>();
	acp->chat_id = " chat-1 ";
	acp->running = true;
	acp->waiting_for_user_input = true;
	UAM_ASSERT(uam::AcpSessionIsWaitingForInput(*acp));
	UAM_ASSERT(uam::AcpSessionHasActiveTurn(*acp));
	app.acp_sessions.push_back(std::move(acp));

	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, "chat-1"));
	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, " chat-1 "));
	UAM_ASSERT(uam::ChatHasActiveAcpSession(app, std::string_view("xx chat-1 yy").substr(2, 8)));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, "chat-1"));
	UAM_ASSERT(!uam::ChatHasActiveAcpSession(app, "  "));
	UAM_ASSERT(!uam::ChatHasRunningRuntime(app, "  "));

	uam::AcpSessionState canceling_session;
	canceling_session.cancel_requested = true;
	canceling_session.cancel_request_id = 7;
	UAM_ASSERT(!uam::AcpSessionHasActiveTurn(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasCancelableWork(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasPendingCancel(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasPendingRuntimeRequest(canceling_session));
	UAM_ASSERT(uam::AcpSessionHasBlockingRuntimeWork(canceling_session));

	uam::AcpSessionState startup_model_session;
	startup_model_session.startup_model_request_id = 8;
	UAM_ASSERT(!uam::AcpSessionHasActiveTurn(startup_model_session));
	UAM_ASSERT(uam::AcpSessionHasPendingRuntimeRequest(startup_model_session));
	UAM_ASSERT(uam::AcpSessionHasBlockingRuntimeWork(startup_model_session));

	app.acp_sessions.clear();
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->running = true;
	terminal->attached_session_id = " native-1 ";
	terminal->lifecycle_state = uam::CliTerminalLifecycleState::ShuttingDown;
	app.cli_terminals.push_back(std::move(terminal));

	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, "native-1"));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, std::string_view("xx native-1 yy").substr(2, 10)));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, " native-1 "));
}

UAM_TEST(NativeHistorySnapshotDigestTracksSameLengthMessageContentChanges)
{
	ChatSession chat;
	chat.id = "chat-1";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "native-1";
	chat.updated_at = "2026-05-14 10:00:00";

	Message message;
	message.role = MessageRole::Assistant;
	message.created_at = "2026-05-14 10:00:01";
	message.provider = "gemini-cli";
	message.content = "alpha";
	chat.messages.push_back(message);

	ChatSession changed = chat;
	changed.messages.back().content = "bravo";

	UAM_ASSERT_EQ(chat.messages.back().content.size(), changed.messages.back().content.size());
	UAM_ASSERT(uam::NativeHistorySnapshotDigest({chat}) != uam::NativeHistorySnapshotDigest({changed}));
}

UAM_TEST(CliTerminalIdentitySeparatesFrontendChatAndNativeSession)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-local";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = " native-session ";
	app.chats.push_back(chat);

	uam::CliTerminalState terminal;
	terminal.terminal_id = " term-chat-local ";
	terminal.frontend_chat_id = "chat-local";
	terminal.attached_chat_id = "chat-local";
	terminal.attached_session_id = "native-session";

	UAM_ASSERT(uam::CliTerminalMatchesTerminalId(terminal, "term-chat-local"));
	UAM_ASSERT(uam::CliTerminalMatchesTerminalId(terminal, " term-chat-local "));
	UAM_ASSERT(!uam::CliTerminalMatchesTerminalId(terminal, "other-terminal"));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, "chat-local"));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, "native-session"));
	UAM_ASSERT(!uam::CliTerminalMatchesChatId(terminal, "other-chat"));
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, chat));
	UAM_ASSERT_EQ(uam::CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(uam::FindChatIndexForCliTerminal(app, terminal), 0);

	ChatSession invalid_codex_native_id = chat;
	invalid_codex_native_id.provider_id = uam::provider_ids::kCodexCli;
	invalid_codex_native_id.native_session_id = "native-session";
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, invalid_codex_native_id));

	terminal.frontend_chat_id = " chat-local ";
	terminal.attached_chat_id = " chat-local ";
	terminal.attached_session_id = " native-session ";
	UAM_ASSERT_EQ(uam::TrimCliTerminalIdentityView(" native-session "), std::string_view("native-session"));
	UAM_ASSERT(uam::TrimmedCliTerminalIdMatches(" native-session ", " native-session "));
	UAM_ASSERT(uam::TrimmedCliTerminalIdMatches("   ", "   "));
	UAM_ASSERT(uam::CliTerminalMatchesNonEmptyIdentity(" native-session ", " native-session "));
	UAM_ASSERT(!uam::CliTerminalMatchesNonEmptyIdentity("   ", "   "));
	UAM_ASSERT(!uam::CliTerminalMatchesTerminalId(terminal, "   "));
	UAM_ASSERT(!uam::CliTerminalMatchesChatId(terminal, "   "));
	UAM_ASSERT(uam::CliTerminalMatchesChatId(terminal, " native-session "));
	UAM_ASSERT(uam::CliTerminalMatchesChat(terminal, chat));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(terminal), std::string("native-session"));
	UAM_ASSERT_EQ(uam::CliTerminalPrimaryChatId(terminal), std::string("chat-local"));
	UAM_ASSERT_EQ(uam::CliTerminalSyncTargetId(terminal), std::string("native-session"));
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionWhenRawSessionIsMissing)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-opencode";
	terminal.attached_chat_id = "chat-opencode";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT_EQ(uam::FindChatIndexForCliTerminal(app, terminal), 0);
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.front());
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionPrefersLiveRawChatForSessionOnlyTerminal)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "stale-opencode-session";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "other-session";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.back()));
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.back());
}

UAM_TEST(CliTerminalMatchesResolvedNativeSessionPrefersLiveChatOverStaleRawCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	app.chats.push_back(stale_chat);

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "stale-opencode-session";
	app.chats.push_back(live_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-live";
	terminal.attached_chat_id = "chat-live";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.front()));
	UAM_ASSERT(uam::CliTerminalMatchesChat(app, terminal, app.chats.back()));
	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.back());
}

UAM_TEST(FindCliTerminalForChatPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalIndexForChat(app, live_chat), 1);
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, live_chat), app.cli_terminals[1].get());
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, stale_chat), app.cli_terminals[0].get());
}

UAM_TEST(FindCliTerminalForChatPrefersLiveTerminalOverLaterStaleCollision)
{
	uam::AppState app;

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[live_chat.id] = "opencode-session-1";

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->last_activity_time_s = 2.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = false;
	stale_terminal->ui_attached = false;
	stale_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, live_chat), app.cli_terminals[0].get());
	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, stale_chat), app.cli_terminals[1].get());
}

UAM_TEST(FindChatForCliTerminalPrefersLiveChatOverLaterStaleCollision)
{
	uam::AppState app;

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);

	uam::CliTerminalState terminal;
	terminal.frontend_chat_id = "chat-live";
	terminal.attached_chat_id = "chat-live";
	terminal.attached_session_id = "opencode-session-1";

	UAM_ASSERT_EQ(uam::FindChatForCliTerminal(app, terminal), &app.chats.front());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "opencode-session-1", ""), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersLiveTerminalOverStaleTerminalIdCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->terminal_id = "term-chat-live";
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->terminal_id = "term-chat-live";
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->last_activity_time_s = 2.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "", "term-chat-live"), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForRoutingKeyPrefersChatMatchingTerminalOverStaleTerminalIdCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->terminal_id = "term-chat-live";
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->last_activity_time_s = 5.0;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->terminal_id = "term-chat-live";
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->last_activity_time_s = 1.0;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForRoutingKey(app, "chat-live", "term-chat-live"), app.cli_terminals[1].get());
}

UAM_TEST(FindCliTerminalForChatStringPrefersLiveTerminalOverStaleCollision)
{
	uam::AppState app;

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = "chat-stale";
	stale_terminal->attached_chat_id = "chat-stale";
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = false;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = "chat-live";
	live_terminal->attached_chat_id = "chat-live";
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	live_terminal->ui_attached = true;
	live_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT_EQ(uam::FindCliTerminalForChat(app, std::string_view("chat-live")), app.cli_terminals[1].get());
}

UAM_TEST(ChatRuntimeHelpersPreferBestMatchCliTerminalOverStaleCollision)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = true;
	stale_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	stale_terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, live_chat.id));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, stale_chat.id));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, live_chat.id));
	UAM_ASSERT(uam::ChatHasRunningRuntime(app, stale_chat.id));
}

UAM_TEST(ChatRuntimeHelpersPreferBestMatchCliTerminalForNativeSessionLookup)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	stale_chat.native_session_id = "opencode-session-1";
	stale_chat.updated_at = "2026-05-31T12:00:00.000Z";
	stale_chat.last_opened_at = "2026-05-31T12:00:00.000Z";
	app.chats.push_back(stale_chat);
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "opencode-session-1";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	live_chat.native_session_id = "opencode-session-1";
	live_chat.updated_at = "2026-06-01T12:00:00.000Z";
	live_chat.last_opened_at = "2026-06-01T12:00:00.000Z";
	app.chats.push_back(live_chat);

	auto stale_terminal = std::make_unique<uam::CliTerminalState>();
	stale_terminal->frontend_chat_id = stale_chat.id;
	stale_terminal->attached_chat_id = stale_chat.id;
	stale_terminal->attached_session_id = "opencode-session-1";
	stale_terminal->running = true;
	stale_terminal->lifecycle_state = uam::CliTerminalLifecycleState::Busy;
	stale_terminal->turn_state = uam::CliTerminalTurnState::Busy;
	app.cli_terminals.push_back(std::move(stale_terminal));

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = live_chat.id;
	live_terminal->attached_chat_id = live_chat.id;
	live_terminal->attached_session_id = "opencode-session-1";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	UAM_ASSERT(uam::ChatHasActiveCliTerminal(app, "opencode-session-1"));
	UAM_ASSERT(uam::ChatHasBusyCliTerminal(app, stale_chat.id));
	UAM_ASSERT(!uam::ChatHasBusyCliTerminal(app, "opencode-session-1"));
}

UAM_TEST(LoadSidebarChatsPreservesResolvedOpenCodeSessionMappings)
{
	TempDir temp("uam-opencode-sidebar-resolved-mapping");

	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id.clear();
	chat.native_session_id = "opencode-session-1";
	chat.title = "OpenCode";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-1";
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));

	ChatHistorySyncService().LoadSidebarChats(app);

	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().id, std::string("chat-opencode"));
	UAM_ASSERT_EQ(app.chats.front().provider_id, std::string(uam::provider_ids::kOpenCodeCli));
	UAM_ASSERT_EQ(app.resolved_native_sessions_by_chat_id[app.chats.front().id], std::string("opencode-session-1"));
}

UAM_TEST(EnsureCliTerminalRepairsWhitespaceOnlyAttachedSessionId)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-terminal-repair";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	app.chats.push_back(chat);
	UAM_ASSERT_EQ(uam::CliTerminalIdForChat(chat.id), std::string("term-chat-terminal-repair"));

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "   ";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, chat);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), chat.native_session_id);
#endif
}

UAM_TEST(EnsureCliTerminalPrefersLiveAcpSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto session = std::make_unique<uam::AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = uam::provider_ids::kOpenCodeCli;
	session->protocol_kind = uam::provider_profile_constants::kProtocolOpenCodeAcp;
	session->running = true;
	session->session_id = "opencode-session-live";
	app.acp_sessions.push_back(std::move(session));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-live"));
#endif
}

UAM_TEST(EnsureCliTerminalUsesResolvedNativeSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-resolved";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsStaleAttachedSessionIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-stale-attached";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = "opencode-session-stale";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsAttachedChatIdForOpenCode)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-attached-chat-repair";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-resolved";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(ensured.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(ensured.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(ensured), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalRepairsRunningOpenCodeTerminalIdentity)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-running-repair";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-resolved";
	app.chats.push_back(chat);
	app.resolved_native_sessions_by_chat_id[chat.id] = "opencode-session-resolved";

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	terminal->running = true;
	terminal->ui_attached = true;
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& running_terminal = *app.cli_terminals.front();
	uam::RepairCliTerminalIdentityForChat(app, running_terminal, app.chats.front(), ProviderProfileStore::DefaultOpenCodeProfile());
	UAM_ASSERT(running_terminal.running);
	UAM_ASSERT_EQ(running_terminal.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(running_terminal.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(running_terminal), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(running_terminal), std::string("opencode-session-resolved"));
#endif
}

UAM_TEST(EnsureCliTerminalClearsStaleOpenCodeSessionWhenNoResumeIdExists)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-no-resume";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id.clear();
	app.chats.push_back(chat);

	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->frontend_chat_id = "stale-frontend-chat";
	terminal->terminal_id = "term-stale-frontend-chat";
	terminal->attached_chat_id = "stale-chat";
	terminal->attached_session_id = "opencode-session-stale";
	terminal->running = true;
	app.cli_terminals.push_back(std::move(terminal));

	uam::CliTerminalState& ensured = uam::EnsureCliTerminalForChat(app, app.chats.front());
	UAM_ASSERT_EQ(ensured.frontend_chat_id, chat.id);
	UAM_ASSERT_EQ(ensured.terminal_id, uam::CliTerminalIdForChat(chat.id));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(ensured), chat.id);
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(ensured), std::string(""));
#endif
}

UAM_TEST(ClearStoppedCliTerminalAttachmentForChatClearsStoppedOpenCodeDuplicate)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession chat;
	chat.id = "chat-opencode-clear";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.native_session_id = "opencode-session-live";
	app.chats.push_back(chat);

	auto live_terminal = std::make_unique<uam::CliTerminalState>();
	live_terminal->frontend_chat_id = chat.id;
	live_terminal->attached_chat_id = chat.id;
	live_terminal->attached_session_id = "opencode-session-live";
	live_terminal->running = true;
	app.cli_terminals.push_back(std::move(live_terminal));

	auto stopped_terminal = std::make_unique<uam::CliTerminalState>();
	stopped_terminal->frontend_chat_id = chat.id;
	stopped_terminal->attached_chat_id = chat.id;
	stopped_terminal->attached_session_id = "opencode-session-stale";
	stopped_terminal->running = false;
	app.cli_terminals.push_back(std::move(stopped_terminal));

	uam::ClearStoppedCliTerminalAttachmentForChat(app, chat.id);

	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals[0]), std::string("opencode-session-live"));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedSessionId(*app.cli_terminals[1]), std::string(""));
	UAM_ASSERT_EQ(uam::CliTerminalAttachedChatId(*app.cli_terminals[1]), std::string(""));
	UAM_ASSERT_EQ(app.cli_terminals[1]->terminal_id, std::string(""));
#endif
}

UAM_TEST(AttachedTerminalPollingTracksWindowVisibility)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-opencode-discovery-interval");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
	ChatSession chat;
	chat.id = "local-opencode-discovery";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.workspace_directory = temp.root.string();
	app.chats.push_back(chat);

	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.front();
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/cat"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, argv, &error));
	terminal.running = true;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.ui_attached = true;
	terminal.lifecycle_state = uam::CliTerminalLifecycleState::Idle;
	terminal.turn_state = uam::CliTerminalTurnState::Idle;
	while (uam::GetAppTimeSeconds() < 0.04) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const double before_visible_poll = uam::GetAppTimeSeconds() - 0.025;
	terminal.last_polled_time_s = before_visible_poll;
	(void)uam::PollAllCliTerminals(nullptr, app);
	const bool visible_polled = terminal.last_polled_time_s > before_visible_poll;
	const double before_hidden_poll = uam::GetAppTimeSeconds() - 0.025;
	terminal.last_polled_time_s = before_hidden_poll;
	(void)uam::PollAllCliTerminals(nullptr, app, false);
	const bool hidden_waited = terminal.last_polled_time_s == before_hidden_poll;
	(void)uam::PollAllCliTerminals(nullptr, app, true);
	const bool restored_polled = terminal.last_polled_time_s > before_hidden_poll;
	terminal.ui_attached = false;
	const double before_background_poll = uam::GetAppTimeSeconds() - 0.025;
	terminal.last_polled_time_s = before_background_poll;
	(void)uam::PollAllCliTerminals(nullptr, app);
	const bool background_waited = terminal.last_polled_time_s == before_background_poll;
	uam::StopCliTerminal(terminal);

	UAM_ASSERT(visible_polled);
	UAM_ASSERT(hidden_waited);
	UAM_ASSERT(restored_polled);
	UAM_ASSERT(background_waited);
#endif
}

UAM_TEST(NativeTerminalRefreshRetriesTheSameSnapshotAfterStorageRecovers)
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	TempDir temp("uam-native-refresh-save-retry");
	const fs::path data_root = temp.root / "data";
	const fs::path workspace = temp.root / "workspace";
	const fs::path gemini_home = temp.root / "gemini";
	const fs::path source = gemini_home / "tmp" / "workspace-source";
	fs::create_directories(workspace);
	fs::create_directories(source / "chats");
	UAM_ASSERT(uam::io::WriteTextFile(source / ".project_root", workspace.string()));
	ScopedEnvVar gemini_env("GEMINI_CLI_HOME", gemini_home.string());
	uam::AppState app;
	app.data_root = data_root;
	const ProviderProfile provider = ProviderProfileStore::DefaultGeminiProfile();
	app.provider_profiles = {provider};
	ChatSession chat;
	chat.id = "local-native-retry";
	chat.provider_id = provider.id;
	chat.native_session_id = "native-retry";
	chat.workspace_directory = workspace.string();
	chat.messages.push_back(Message{MessageRole::User, "Hello"});
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	ChatSession native = chat;
	native.messages.push_back(Message{MessageRole::Assistant, "Completed"});
	const std::string digest = uam::NativeHistorySnapshotDigest({native});
	const fs::path blocked = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "blocked"));
	uam::CliTerminalState terminal;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/cat"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, workspace, argv, &error));
	terminal.running = true;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.attached_session_id = chat.native_session_id;
	terminal.last_native_history_snapshot_digest = "previous";
	std::string failed_digest;
	std::string failure;
	std::size_t retained_messages = 0;
	for (const bool storage_available : {false, true})
	{
		app.data_root = storage_available ? data_root : blocked;
		uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks);
		uam::platform::AsyncNativeChatLoadTask& task = uam::AsyncNativeChatLoadTaskFor(app, provider.id, source / "chats");
		task.running = true;
		task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
		task.state->chats = {native};
		task.state->snapshot_digest = digest;
		task.state->completed.store(true);
		terminal.last_sync_time_s = -100.0;
		(void)uam::PollCliTerminal(nullptr, app, terminal, true);
		if (!storage_available)
		{
			failed_digest = terminal.last_native_history_snapshot_digest;
			failure = app.status_line;
			const ChatSession* retained = ChatDomainService().FindChatById(app, chat.id);
			retained_messages = retained == nullptr ? 0 : retained->messages.size();
		}
	}
	const std::string final_digest = terminal.last_native_history_snapshot_digest;
	const bool save_failure_cleared = app.status_line.empty();
	const std::string unrelated_status = "Other connection failed.";
	app.status_line = unrelated_status;
	bool load_retry_preserved_status = true;
	bool load_retry_preserved_digest = true;
	for (const std::string& load_error : {std::string("Temporary native history read failure."), std::string{}})
	{
		uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks);
		uam::platform::AsyncNativeChatLoadTask& task = uam::AsyncNativeChatLoadTaskFor(app, provider.id, source / "chats");
		task.running = true;
		task.state = std::make_shared<uam::platform::AsyncNativeChatLoadTask::State>();
		task.state->chats = {native};
		task.state->snapshot_digest = digest;
		task.state->error = load_error;
		task.state->completed.store(true);
		terminal.last_sync_time_s = -100.0;
		(void)uam::PollCliTerminal(nullptr, app, terminal, true);
		load_retry_preserved_status &= app.status_line == unrelated_status;
		load_retry_preserved_digest &= terminal.last_native_history_snapshot_digest == digest;
	}
	uam::StopCliTerminal(terminal, true, uam::CliTerminalStopMode::FastExit);
	uam::platform::ResetAsyncNativeChatLoadTasks(app.native_chat_load_tasks);
	UAM_ASSERT_EQ(failed_digest, std::string("previous"));
	UAM_ASSERT(uam::strings::Contains(failure, "could not save"));
	UAM_ASSERT_EQ(retained_messages, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(final_digest, digest);
	UAM_ASSERT(save_failure_cleared);
	UAM_ASSERT(load_retry_preserved_status);
	UAM_ASSERT(load_retry_preserved_digest);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(data_root, chat.id);
	UAM_ASSERT(saved && saved->messages.size() == 2);
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Completed"));
#endif
}

UAM_TEST(CodexTerminalBindingSaveRetriesAfterTheTerminalStops)
{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	TempDir temp("uam-codex-terminal-save-retry");
	const fs::path workspace = temp.root / "workspace";
	const fs::path codex_home = temp.root / "codex";
	fs::create_directories(workspace);
	fs::create_directories(codex_home / "sessions");
	ScopedEnvVar codex_env("CODEX_HOME", codex_home.string());
	const std::string native_id = "33333333-3333-4333-8333-333333333333";
	const nlohmann::json index = {{"id", native_id}};
	const nlohmann::json rollout = {{"type", "session_meta"}, {"payload", {{"id", native_id}, {"cwd", workspace.string()}}}};
	UAM_ASSERT(uam::io::WriteTextFile(codex_home / "session_index.jsonl", index.dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(codex_home / "sessions" / ("rollout-" + native_id + ".jsonl"), rollout.dump() + "\n"));

	uam::AppState app;
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "storage unavailable"));
	app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
	ChatSession chat;
	chat.id = "codex-terminal-save-retry";
	chat.provider_id = uam::provider_ids::kCodexCli;
	chat.workspace_directory = workspace.string();
	chat.messages.push_back(Message{MessageRole::User, "Keep this draft"});
	app.chats.push_back(chat);
	uam::CliTerminalState terminal;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/cat"};
#endif
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, workspace, argv, &error));
	terminal.running = true;
	terminal.frontend_chat_id = chat.id;
	terminal.attached_chat_id = chat.id;
	terminal.last_sync_time_s = -100.0;
	(void)uam::PollCliTerminal(nullptr, app, terminal, false);
	const std::string attached = terminal.attached_session_id;
	uam::StopCliTerminal(terminal, true, uam::CliTerminalStopMode::FastExit);
	UAM_ASSERT_EQ(attached, native_id);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
	uam::FlushPendingChatSaves(app, true);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));

	fs::remove(codex_home / "session_index.jsonl");
	app.data_root = temp.root / "data";
	uam::FlushPendingChatSaves(app, true);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->native_session_id, native_id);
	UAM_ASSERT_EQ(saved->messages.front().content, chat.messages.front().content);
#endif
}

UAM_TEST(TerminalCompletionUsesPersistedCountsForUnloadedChats)
{
	for (const bool loaded : {false, true})
	{
		uam::AppState app;
		ChatSession chat;
		chat.id = "terminal-counts";
		chat.messages_loaded = loaded;
		chat.persisted_message_count = 2;
		if (loaded) chat.messages.resize(2);
		app.chats.push_back(chat);
		uam::CliTerminalState terminal;
		terminal.frontend_chat_id = chat.id;
		terminal.lifecycle_state = uam::CliTerminalLifecycleState::Busy;
		UAM_ASSERT(!uam::TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, 2, "selected-other-chat"));
		app.chats.front().persisted_message_count = 3;
		if (loaded) app.chats.front().messages.resize(3);
		UAM_ASSERT(uam::TryMarkCliTurnCompleteFromSyncedHistory(app, terminal, 2, "selected-other-chat"));
		UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	}
}

UAM_TEST(NativeHistorySyncClearsOnlyRecoveredTargetFailures)
{
	TempDir temp("uam-native-refresh-error");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "native-import-tombstones.json", "{"));
	UAM_ASSERT(!uam::SyncChatsFromNative(app, "native-chat", true));
	UAM_ASSERT(uam::strings::Contains(app.status_line, "deletion records could not be read"));
	const std::string failure = app.status_line;
	fs::remove(temp.root / "native-import-tombstones.json");
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {}, "other-chat", true));
	UAM_ASSERT_EQ(app.status_line, failure);
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {}, " native-chat ", true));
	UAM_ASSERT(app.status_line.empty());

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "native-import-tombstones.json", "{"));
	UAM_ASSERT(!uam::SyncChatsFromNative(app, "native-chat", true));
	app.status_line = "Other connection failed.";
	fs::remove(temp.root / "native-import-tombstones.json");
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {}, "native-chat", true));
	UAM_ASSERT_EQ(app.status_line, std::string("Other connection failed."));
}

UAM_TEST(ProviderInteractiveTerminalReasonMatchesSupportPredicate)
{
	ProviderProfile unknown_provider;
	unknown_provider.id = "unknown-provider";
	UAM_ASSERT(!uam::ProviderSupportsInteractiveTerminal(unknown_provider));
	UAM_ASSERT(!uam::ProviderInteractiveTerminalUnavailableReason(unknown_provider).empty());

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	ProviderProfile opencode_provider = ProviderProfileStore::DefaultOpenCodeProfile();
	UAM_ASSERT(uam::ProviderSupportsInteractiveTerminal(opencode_provider));
	UAM_ASSERT(uam::ProviderInteractiveTerminalUnavailableReason(opencode_provider).empty());

	opencode_provider.supports_interactive = false;
	UAM_ASSERT(!uam::ProviderSupportsInteractiveTerminal(opencode_provider));
	UAM_ASSERT_EQ(uam::ProviderInteractiveTerminalUnavailableReason(opencode_provider), std::string("Provider does not expose an interactive CLI runtime."));
#endif
}

UAM_TEST(ProviderInteractiveTerminalRejectsPermissionBypassSettings)
{
	uam::AppState app;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	app.settings.provider_extra_flags = "--debug";
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());

	app.settings.provider_extra_flags = "--debug --dangerously-skip-permissions";
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());

	app.settings.provider_extra_flags.clear();
	provider.runtime_flags = {"--ask-for-approval", "never"};
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());

	provider.runtime_flags = {"--auto"};
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
	app.settings.provider_extra_flags = "--auto";
	provider.runtime_flags.clear();
	UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
}

UAM_TEST(OpenCodeInteractiveFlagsPreserveSavedSessionRouting)
{
	uam::AppState app;
	ProviderProfile provider = ProviderProfileStore::DefaultOpenCodeProfile();
	for (const std::string& flags : {"--pure --log-level debug", "--model provider/model", "-mclaude", "--hostname localhost"})
	{
		app.settings.provider_extra_flags = flags;
		UAM_ASSERT(ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
	}
	for (const std::string& flags : {"--fork", "--fork=true", "--session ses_other", "--session=ses_other", "-s ses_other", "-sses_other", "-s=ses_other", "--continue", "--continue=true", "-c", "-c=true", "-cses_other", "-vc", "-vses_other"})
	{
		app.settings.provider_extra_flags = flags;
		UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
		app.settings.provider_extra_flags.clear();
		provider.runtime_flags = uam::command_line::SplitWords(flags);
		UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
		provider.runtime_flags.clear();
		provider.interactive_command = "opencode " + flags;
		UAM_ASSERT(!ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
		provider.interactive_command = "opencode";
	}
	for (const std::string& resume : {"--continue", "--fork", "", "--session", "-s", " --session "})
	{
		provider.resume_argument = resume;
		const bool valid = uam::strings::Trim(resume) == "--session" || resume == "-s";
		UAM_ASSERT_EQ(ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty(), valid);
	}
	provider.id = uam::provider_ids::kCodexCli;
	app.settings.provider_extra_flags = "--session ses_other";
	UAM_ASSERT(ProviderRuntimeRegistry::Resolve(provider).InteractiveConfigurationError(provider, app.settings).empty());
}
