#include "test_harness.h"
#include "app/uam_control_service.h"
#include "common/provider/provider_runtime.h"
#include "remote/runner_service_posix.h"

using namespace uam_test;

namespace
{
	constexpr std::string_view kProcessControlToken = "test-process-control-token";
}

std::optional<int> RunRemoteOpenCodeCreateFixture(int argc, char** argv)
{
	if (argc != 2 || std::string_view(argv[1]) != "--uam-test-remote-opencode-create") return std::nullopt;
	const std::string log_path = uam::env::GetNonEmptyString("UAM_TEST_CREATE_LOG").value_or("");
	const std::string mode = uam::env::GetNonEmptyString("UAM_TEST_CREATE_MODE").value_or("");
	uam::remote::RunnerState state;
	std::string output;
	std::uint64_t input_sequence = 0;
	int command_poll_count = 0;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		{
			std::ofstream log(log_path, std::ios::app);
			log << request.dump() << '\n';
		}
		const std::string type = request.value("type", "");
		nlohmann::json result = nlohmann::json::object();
		if (mode == "command-lost-start" && type == "process.start") return 0;
		if (mode == "teardown-lost-reply" && type == "process.closeInput") return 0;
		if (mode == "command-lost-start" && type == "process.poll")
		{
			++command_poll_count;
			result = {{"running", command_poll_count == 1}, {"exitCode", 7},
			          {"stdoutBase64", uam::base64::Encode(command_poll_count == 1 ? "first\n" : "last\n")},
			          {"stderrBase64", uam::base64::Encode(command_poll_count == 1 ? "warning\n" : "")},
			          {"stdoutCursor", std::uint64_t(command_poll_count == 1 ? 6 : 11)},
			          {"stderrCursor", std::uint64_t{8}}};
		}
		else if (type == "process.write")
		{
			std::string decoded;
			if (!uam::base64::Decode(request.value("dataBase64", ""), decoded)) return 2;
			const nlohmann::json rpc = nlohmann::json::parse(decoded);
			input_sequence = request.value("inputSequence", std::uint64_t{0});
			if (rpc.value("method", "") == "initialize")
				output = R"({"jsonrpc":"2.0","id":999,"error":{"message":"unrelated"}})" "\n"
				         R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1}})" "\n";
			else if (rpc.value("method", "") == "session/new")
			{
				if (rpc["params"]["cwd"] != R"(C:\Work\Project)" || rpc["params"]["mcpServers"] != nlohmann::json::array()) return 3;
				if (!uam::io::WriteTextFile(log_path + ".session-new", "accepted")) return 6;
				if (mode == "error") output = R"({"jsonrpc":"2.0","id":2,"error":{"message":"creation refused"}})" "\n";
				else if (mode == "invalid") output = R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"../invalid"}})" "\n";
				else if (mode == "cancel") output.clear();
				else output = R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"ses_remotecreated"}})" "\n";
			}
			else return 4;
			result["inputSequence"] = input_sequence;
		}
		else if (type == "process.poll")
		{
			result = {{"running", true}, {"stdoutBase64", uam::base64::Encode(output)},
			          {"stderrBase64", ""}, {"inputSequence", input_sequence}};
			output.clear();
		}
		const bool teardown_error = mode == "teardown-error" &&
		    (type == "process.closeInput" || type == "process.stop" || type == "process.remove");
		const nlohmann::json response = teardown_error
		    ? nlohmann::json{{"id", request["id"]}, {"ok", false}, {"error", {{"message", "fixture teardown rejected"}}}}
		    : type == "hello"
		    ? uam::remote::HandleRunnerRequest(request, "development", &state)
		    : nlohmann::json{{"id", request["id"]}, {"ok", true}, {"result", result}};
		if (!uam::remote::WriteFrame(std::cout, response)) return 5;
		std::cout.flush();
	}
	return 0;
}

UAM_TEST(RemoteCommandRecoversLostStartWithoutReplayingAndPreservesOutputAndExit)
{
	TempDir temp("uam-remote-command");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path log = temp.root / "requests.jsonl";
	ScopedEnvVar log_env("UAM_TEST_CREATE_LOG", log.string());
	ScopedEnvVar mode_env("UAM_TEST_CREATE_MODE", "command-lost-start");
	uam::remote::RunnerClient client(service,
	    {service.ResolveCurrentExecutablePath().string(), "--uam-test-remote-opencode-create"});
	const ProcessExecutionResult result = client.ExecuteCommand(
	    "cli-update-fixture", uam::paths::PathFromUtf8(R"(C:\Work\Project)"), {"fixture"}, 5000);
	client.Disconnect();
	UAM_ASSERT(!result.ok && result.exit_code == 7 && result.error.empty());
	UAM_ASSERT_EQ(result.output, std::string("first\nwarning\nlast\n"));
	std::ifstream stream(log);
	std::string line, token;
	int starts = 0, polls = 0, acknowledgements = 0, removes = 0, closes = 0;
	while (std::getline(stream, line))
	{
		const nlohmann::json request = nlohmann::json::parse(line);
		const std::string type = request.value("type", "");
		if (type == "hello") continue;
		if (type == "process.start")
		{
			++starts;
			token = request.at("controlToken").get<std::string>();
			UAM_ASSERT(!request.at("attachIfExists").get<bool>());
			UAM_ASSERT_EQ(request.at("transientLeaseMs").get<int>(), 60000);
		}
		UAM_ASSERT_EQ(request.at("sessionId").get<std::string>(), std::string("cli-update-fixture"));
		UAM_ASSERT_EQ(request.at("controlToken").get<std::string>(), token);
		polls += type == "process.poll";
		acknowledgements += type == "process.ack";
		removes += type == "process.remove";
		closes += type == "process.closeInput";
	}
	UAM_ASSERT_EQ(starts, 1);
	UAM_ASSERT_EQ(polls, 2);
	UAM_ASSERT_EQ(acknowledgements, 2);
	UAM_ASSERT_EQ(removes, 1);
	UAM_ASSERT_EQ(closes, 1);
	std::stop_source canceled;
	canceled.request_stop();
	UAM_ASSERT(client.ExecuteCommand("never-started", temp.root, {"fixture"}, 5000,
	    canceled.get_token()).canceled);
}

UAM_TEST(RemoteOpenCodeCreationUsesCorrelatedHandshakeAndCleansUp)
{
#if !defined(_WIN32) && UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-remote-opencode-create");
	const fs::path ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(ssh, "#!/bin/sh\nexec \"$UAM_TEST_CREATE_EXECUTABLE\" --uam-test-remote-opencode-create\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
	ScopedEnvVar path("PATH", temp.root.string() + ":/usr/bin:/bin");
	ScopedEnvVar executable("UAM_TEST_CREATE_EXECUTABLE", PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string());
	ExecutionHost host;
	host.id = "remote-create";
	host.transport = "ssh";
	host.ssh_alias = "fixture";
	host.platform = "windows";
	host.runner_status = "ready";
	host.runner_version = "development";
	host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	for (const std::string mode : {"success", "invalid", "error", "cancel", "teardown-error", "teardown-lost-reply"})
	{
		const fs::path log = temp.root / (mode + ".jsonl");
		ScopedEnvVar log_env("UAM_TEST_CREATE_LOG", log.string());
		ScopedEnvVar mode_env("UAM_TEST_CREATE_MODE", mode);
		std::stop_source stop;
		std::jthread cancel;
		if (mode == "cancel") cancel = std::jthread([&](std::stop_token token)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				if (fs::exists(log.string() + ".session-new")) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			stop.request_stop();
		});
		std::string error;
		const ProviderProfile profile = ProviderProfileStore::DefaultOpenCodeProfile();
		const std::string id = ProviderRuntimeRegistry::Resolve(profile).CreateNativeSession(
		    profile, fs::path(R"(C:\Work\Project)"), stop.get_token(), &error, &host);
		cancel.request_stop();
		if (cancel.joinable()) cancel.join();
		const bool expected_success = mode == "success" || mode == "teardown-error" || mode == "teardown-lost-reply";
		if (expected_success && id != "ses_remotecreated") throw std::runtime_error("Remote creation fixture failed: " + error);
		if (!expected_success) UAM_ASSERT(id.empty() && !error.empty());
		if (mode == "error") UAM_ASSERT(error.find("creation refused") != std::string::npos);
		if (mode == "cancel") UAM_ASSERT(error.find("canceled") != std::string::npos);
		std::ifstream stream(log);
		std::string line, process_id, control_token;
		bool stopped = false, removed = false, input_closed = false;
		int session_new_count = 0;
		while (std::getline(stream, line))
		{
			const nlohmann::json request = nlohmann::json::parse(line);
			const std::string type = request.value("type", "");
			if (type == "hello") continue;
			if (type == "process.start")
			{
				process_id = request.at("sessionId").get<std::string>();
				control_token = request.at("controlToken").get<std::string>();
				UAM_ASSERT(!process_id.empty() && !control_token.empty());
				UAM_ASSERT_EQ(request.at("cwd").get<std::string>(), std::string(R"(C:\Work\Project)"));
				UAM_ASSERT_EQ(request.at("transientLeaseMs").get<int>(), 60000);
			}
			UAM_ASSERT_EQ(request.at("sessionId").get<std::string>(), process_id);
			UAM_ASSERT_EQ(request.at("controlToken").get<std::string>(), control_token);
			if (type == "process.write")
			{
				std::string decoded;
				UAM_ASSERT(uam::base64::Decode(request.at("dataBase64").get<std::string>(), decoded));
				if (nlohmann::json::parse(decoded).value("method", "") == "session/new") ++session_new_count;
			}
			input_closed = input_closed || type == "process.closeInput";
			stopped = stopped || type == "process.stop";
			removed = removed || type == "process.remove";
		}
		UAM_ASSERT(stopped && removed);
		UAM_ASSERT_EQ(session_new_count, 1);
		if (expected_success) UAM_ASSERT(input_closed);
	}
#endif
}

std::optional<int> RunRemoteMcpLostAckFixture(int argc, char** argv)
{
#if !defined(_WIN32)
	const std::optional<std::string> log_path = uam::env::GetNonEmptyString("UAM_TEST_MCP_LOST_ACK_LOG");
	const std::optional<std::string> idle_poll_log = uam::env::GetNonEmptyString("UAM_TEST_MCP_IDLE_POLL_LOG");
	if ((!log_path && !idle_poll_log) || argc < 2) return std::nullopt;
	if (std::string_view(argv[1]) == "--uam-test-mcp-lost-ack")
		return uam::remote::RunRemoteMcpShim("lost-ack", fs::path("unused.sock"));
	if (std::string_view(argv[1]) != "bridge") return std::nullopt;
	uam::remote::RunnerState state;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		const std::string type = request.value("type", "");
		if (idle_poll_log && type == "channel.poll")
		{
			std::ofstream log(*idle_poll_log, std::ios::app);
			log << "poll\n";
		}
		if (log_path && type == "channel.write")
		{
			std::ofstream log(*log_path, std::ios::app);
			log << request.at("writeSequence").get<std::uint64_t>() << '\n';
			// Accept the bytes, then lose every response, including the same-sequence retry.
			return 0;
		}
		const nlohmann::json response = type == "hello"
		    ? uam::remote::HandleRunnerRequest(request, "development", &state)
		    : idle_poll_log && type == "channel.poll"
		    ? nlohmann::json{{"id", request["id"]}, {"ok", true},
		        {"result", {{"dataBase64", ""}, {"cursor", 0}}}}
		    : nlohmann::json{{"id", request["id"]}, {"ok", true},
		        {"result", {{"remoteToDesktopWriteSequence", log_path && fs::exists(*log_path) ? 1 : 0}}}};
		if (!uam::remote::WriteFrame(std::cout, response)) return 1;
		std::cout.flush();
	}
	return 0;
#else
	(void)argc;
	(void)argv;
	return std::nullopt;
#endif
}

UAM_TEST(RemoteMcpShimBoundsIdleChannelPolling)
{
#if !defined(_WIN32)
	TempDir temp("uam-mcp-idle-poll");
	const fs::path log = temp.root / "polls.txt";
	ScopedEnvVar idle_log("UAM_TEST_MCP_IDLE_POLL_LOG", log.string());
	auto& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields shim;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(
	    shim, temp.root,
	    {service.ResolveCurrentExecutablePath().string(), "--uam-test-mcp-lost-ack"},
	    &error));
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	service.StopStdioProcess(shim, true);
	const std::string polls = ReadFile(log);
	UAM_ASSERT(!polls.empty());
	UAM_ASSERT(std::count(polls.begin(), polls.end(), '\n') <= 10);
#endif
}

UAM_TEST(RemoteMcpShimDoesNotReplayAcceptedInputAfterLostAcknowledgements)
{
#if !defined(_WIN32)
	TempDir temp("uam-mcp-lost-ack");
	const fs::path log = temp.root / "writes.txt";
	auto& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields shim;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(shim, temp.root,
	    {service.ResolveCurrentExecutablePath().string(), "--uam-test-mcp-lost-ack"}, &error,
	    {{"UAM_TEST_MCP_LOST_ACK_LOG", log.string()}}));
	struct ProcessGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ProcessGuard() { PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true); }
	} guard{shim};
	const std::string input = "{\"id\":1,\"method\":\"tools/call\"}\n";
	UAM_ASSERT(service.WriteToStdioProcess(shim, input.data(), input.size(), &error));
	int exit_code = -1;
	bool exited = false;
	for (int attempt = 0; attempt < 300 && !exited; ++attempt)
	{
		exited = service.PollStdioProcessExited(shim, &exit_code);
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(exit_code, 70);
	UAM_ASSERT_EQ(ReadFile(log), std::string("1\n1\n"));
#endif
}

UAM_TEST(RemoteRunnerClientRejectsMalformedHandshakeFieldsWithoutThrowing)
{
#if !defined(_WIN32)
	TempDir temp("uam-runner-malformed-response");
	const std::vector<nlohmann::json> malformed = {
	    {{"id", 12}, {"ok", true}},
	    {{"id", "1"}, {"ok", "yes"}},
	    {{"id", "1"}, {"ok", false}, {"error", {{"message", nlohmann::json::array()}}}},
	    {{"id", "1"}, {"ok", true}, {"nonce", 12}}
	};
	for (const auto& response : malformed)
	{
		std::ostringstream wire;
		std::string error;
		UAM_ASSERT(uam::remote::WriteFrame(wire, response, &error));
		const fs::path reply = temp.root / "response.bin";
		UAM_ASSERT(uam::io::WriteTextFile(reply, wire.str()));
		uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		    {"/bin/sh", "-c", "/bin/cat \"$1\"; /bin/sleep 5", "uam-malformed-bridge", reply.string()});
		UAM_ASSERT(!client.Connect(&error));
		UAM_ASSERT(!error.empty());
	}
#endif
}

UAM_TEST(RemoteRunnerClientRejectsMalformedResultsBeforeUpdatingSessionState)
{
#if !defined(_WIN32)
	const std::vector<std::pair<std::string, nlohmann::json>> cases = {
	    {"poll", nullptr},
	    {"poll", {{"running", "yes"}}},
	    {"poll", {{"stdoutBase64", 12}}},
	    {"poll", {{"stderrBase64", nlohmann::json::array()}}},
	    {"poll", {{"stdoutCursor", -1}}},
	    {"poll", {{"stderrCursor", 1.5}}},
	    {"poll", {{"stdoutBase64", "eA=="}, {"stdoutCursor", 0}}},
	    {"poll", {{"stdoutBase64", "eA=="}}},
	    {"poll", {{"exitCode", 4294967296ULL}}},
	    {"poll", {{"running", false}, {"stdoutBase64", "eA=="}, {"stderrBase64", "!"}}},
	    {"start", {{"inputSequence", "bad"}}},
	    {"write", {{"inputSequence", -1}}},
	    {"open", {{"remoteToDesktopCursor", 9}, {"desktopToRemoteCursor", "bad"}}},
	    {"channel", {{"dataBase64", "eA=="}, {"cursor", -1}}},
	    {"channel", {{"dataBase64", "!"}}}
	};
	const std::string executable = PlatformServicesFactory::Instance().process_service
	    .ResolveCurrentExecutablePath().string();
	for (const auto& [operation, result] : cases)
	{
		uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		    {executable, "--uam-test-runner-result", result.dump()});
		std::string error;
		UAM_ASSERT(client.Connect(&error));
		client.SetProcessControlToken("session", "token");
		if (operation == "poll")
		{
			uam::remote::ProcessPollResult polled;
			polled.running = true;
			polled.standard_output = "retained";
			polled.stdout_cursor = 7;
			UAM_ASSERT(!client.PollProcess("session", polled, &error));
			UAM_ASSERT(polled.running);
			UAM_ASSERT_EQ(polled.standard_output, std::string("retained"));
			UAM_ASSERT_EQ(polled.stdout_cursor, std::uintmax_t{7});
		}
		else if (operation == "start")
			UAM_ASSERT(!client.StartProcess("session", fs::current_path(), {"unused"}, {}, &error));
		else if (operation == "write")
			UAM_ASSERT(!client.WriteProcess("session", "input", &error));
		else if (operation == "open")
			UAM_ASSERT(!client.OpenChannel("channel", &error));
		else
		{
			std::string bytes = "retained";
			std::uintmax_t cursor = 7;
			UAM_ASSERT(!client.PollChannel("channel", "remoteToDesktop", bytes, &error, &cursor));
			UAM_ASSERT_EQ(bytes, std::string("retained"));
			UAM_ASSERT_EQ(cursor, std::uintmax_t{7});
		}
		UAM_ASSERT(!error.empty());
	}
#endif
}

UAM_TEST(RemoteNativeHistoryCancellationInterruptsStalledPollAndCleansUp)
{
#if !defined(_WIN32)
	TempDir temp("uam-remote-history-cancel");
	const fs::path log = temp.root / "requests.txt";
	const fs::path ssh = temp.root / "ssh";
	const std::string executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string();
	UAM_ASSERT(uam::io::WriteTextFile(ssh, "#!/bin/sh\nexec " + ShellQuoteForTest(executable) +
	    " --uam-test-runner-result '{}' " + ShellQuoteForTest(log.string()) + "\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
	ScopedEnvVar path("PATH", temp.root.string() + ":" + uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin"));
	ExecutionHost host;
	host.id = "history-lab";
	host.ssh_alias = "history-lab";
	host.platform = "windows";
	host.runner_status = "ready";
	host.runner_version = "test";
	host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	ChatSession chat;
	chat.id = "history-cancel";
	chat.execution_host_id = host.id;
	chat.workspace_directory = "C:\\workspace";
	for (const bool codex : {true, false})
	{
		fs::remove(log);
		chat.native_session_id = codex ? "33333333-3333-4333-8333-333333333333" : "ses_cancel";
		std::stop_source stop;
		std::jthread cancel([&](std::stop_token done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!done.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				if (uam::io::ReadTextFile(log).find("process.poll ") != std::string::npos)
				{
					stop.request_stop();
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		});
		const auto started = std::chrono::steady_clock::now();
		const std::string error = codex
		    ? ChatHistorySyncService().LoadRemoteCodexTranscript(host, chat, stop.get_token()).error
		    : ChatHistorySyncService().LoadRemoteOpenCodeTranscript(host, chat, ProviderProfileStore::DefaultOpenCodeProfile(), stop.get_token()).error;
		const auto elapsed = std::chrono::steady_clock::now() - started;
		cancel.request_stop();
		cancel.join();
		UAM_ASSERT(stop.stop_requested());
		UAM_ASSERT(elapsed < std::chrono::seconds(3));
		UAM_ASSERT(error.find("canceled") != std::string::npos);
		const std::string requests = uam::io::ReadTextFile(log);
		UAM_ASSERT(requests.find("process.stop ") != std::string::npos);
		UAM_ASSERT(requests.find("process.remove ") != std::string::npos);
		fs::remove(log);
		if (codex) (void)ChatHistorySyncService().LoadRemoteCodexTranscript(host, chat, stop.get_token());
		else (void)ChatHistorySyncService().LoadRemoteOpenCodeTranscript(host, chat, ProviderProfileStore::DefaultOpenCodeProfile(), stop.get_token());
		UAM_ASSERT(!fs::exists(log));
	}
#endif
}

UAM_TEST(RemoteRunnerProtocolIsBoundedVersionedAndComputerUseFree)
{
	const nlohmann::json hello = {
	    {"id", "request-1"}, {"type", "hello"},
	    {"protocolVersion", uam::remote::kRunnerProtocolVersion}, {"nonce", "nonce-1"}};
	std::stringstream wire;
	std::string error;
	UAM_ASSERT(uam::remote::WriteFrame(wire, hello, &error));
	nlohmann::json decoded;
	UAM_ASSERT_EQ(uam::remote::ReadFrame(wire, decoded, &error),
	              uam::remote::FrameReadResult::Ok);
	UAM_ASSERT_EQ(decoded, hello);

	const nlohmann::json response = uam::remote::HandleRunnerRequest(decoded, "test-version");
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT_EQ(response.value("nonce", ""), std::string("nonce-1"));
	UAM_ASSERT_EQ(response.value("runnerVersion", ""), std::string("test-version"));
	UAM_ASSERT(!response["capabilities"].value("computerUse", true));
	UAM_ASSERT(!response["capabilities"].value("directoryBrowsing", true));
	UAM_ASSERT(!response["capabilities"].value("fileCopy", true));
	UAM_ASSERT(!response["capabilities"].value("processExecution", true));
	UAM_ASSERT(!response["capabilities"].value("processOutputAcknowledgement", true));

	const nlohmann::json mismatch = uam::remote::HandleRunnerRequest(
	    {{"id", "request-2"}, {"type", "hello"}, {"protocolVersion", 99},
	     {"nonce", "nonce-2"}},
	    "test-version");
	UAM_ASSERT(!mismatch.value("ok", true));
	UAM_ASSERT_EQ(mismatch["error"].value("code", ""), std::string("protocol_mismatch"));

	std::string oversized_header("\x00\x10\x00\x01", 4);
	std::stringstream oversized(oversized_header);
	UAM_ASSERT_EQ(uam::remote::ReadFrame(oversized, decoded, &error),
	              uam::remote::FrameReadResult::Error);
	UAM_ASSERT(error.find("size") != std::string::npos);

	std::stringstream truncated(std::string("\x00\x00\x00\x05{}", 6));
	UAM_ASSERT_EQ(uam::remote::ReadFrame(truncated, decoded, &error),
	              uam::remote::FrameReadResult::Error);
	UAM_ASSERT(error.find("truncated") != std::string::npos);
}

UAM_TEST(RemoteRunnerEndpointsCanSelectTheLegacyProtocolForReconnect)
{
	UAM_ASSERT_EQ(uam::remote::RunnerEndpointName("4.9.0-alpha-2", 2),
	              std::string("uam"));
	const std::vector<std::string> argv = uam::remote::SshBridgeArgv(
	    "test-host", "linux", "4.9.0-alpha-2", {}, 2);
	UAM_ASSERT(!argv.empty());
	UAM_ASSERT(argv.back().find("/uam.sock") != std::string::npos);
	// Protocol 2 has no output ACK/source-exit handshake. Its final poll bytes must
	// be written before the proxy removes the exited process, even when a modern
	// GUI supplied a non-empty delivery token while recovering an old chat.
	UAM_ASSERT(!uam::remote::UsesDurableRemoteOutputHandshake(2, "delivery-token"));
	UAM_ASSERT(uam::remote::UsesDurableRemoteOutputHandshake(3, "delivery-token"));
}

UAM_TEST(RemoteRunnerListsOnlyBoundedDirectChildDirectories)
{
	TempDir temp("uam-runner-directory-list");
	for (int index = 0; index < 205; ++index)
		fs::create_directory(temp.root / ("folder-" + std::to_string(1000 + index)));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "not-a-directory.txt", "ignored"));

	uam::remote::RunnerState state;
	const nlohmann::json hello = uam::remote::HandleRunnerRequest(
	    {{"id", "hello-list"}, {"type", "hello"},
	     {"protocolVersion", uam::remote::kRunnerProtocolVersion}, {"nonce", "list"}},
	    "test-version", &state);
	UAM_ASSERT(hello["capabilities"].value("directoryBrowsing", false));
	UAM_ASSERT(hello["capabilities"].value("fileCopy", false));
	const std::filesystem::path home = uam::env::GetUserHomePath().value_or(std::filesystem::path{});
	UAM_ASSERT(home.is_absolute());
	const nlohmann::json listed_home = uam::remote::HandleRunnerRequest(
	    {{"id", "list-home"}, {"type", "directory.list"}, {"path", ""}},
	    "test-version", &state);
	UAM_ASSERT(listed_home.value("ok", false));
	UAM_ASSERT_EQ(listed_home["result"].value("directory", ""),
	              uam::paths::Utf8PathString(home.lexically_normal()));

	const nlohmann::json listed = uam::remote::HandleRunnerRequest(
	    {{"id", "list-1"}, {"type", "directory.list"}, {"path", temp.root.string()}},
	    "test-version", &state);
	UAM_ASSERT(listed.value("ok", false));
	UAM_ASSERT_EQ(listed["result"].value("directory", ""), temp.root.string());
	UAM_ASSERT_EQ(listed["result"]["directories"].size(), static_cast<std::size_t>(200));
	UAM_ASSERT(listed["result"].value("truncated", false));
	std::string previous;
	for (const nlohmann::json& entry : listed["result"]["directories"])
	{
		const std::string name = entry.value("name", "");
		UAM_ASSERT(name.starts_with("folder-"));
		UAM_ASSERT(previous.empty() || previous < name);
		previous = name;
	}

	const nlohmann::json relative = uam::remote::HandleRunnerRequest(
	    {{"id", "list-2"}, {"type", "directory.list"}, {"path", "relative"}},
	    "test-version", &state);
	UAM_ASSERT(!relative.value("ok", true));
	UAM_ASSERT_EQ(relative["error"].value("code", ""), std::string("invalid_request"));

	const nlohmann::json file = uam::remote::HandleRunnerRequest(
	    {{"id", "list-3"}, {"type", "directory.list"},
	     {"path", (temp.root / "not-a-directory.txt").string()}},
	    "test-version", &state);
	UAM_ASSERT(!file.value("ok", true));
	UAM_ASSERT_EQ(file["error"].value("code", ""), std::string("not_directory"));
}

UAM_TEST(RemoteRunnerExecutesOnlyValidatedTypedProcessRequests)
{
	uam::remote::RunnerState state;
	const nlohmann::json invalid = uam::remote::HandleRunnerRequest(
	    {{"id", "bad-1"}, {"type", "process.start"}, {"sessionId", "bad session"},
	     {"cwd", fs::temp_directory_path().string()}, {"argv", nlohmann::json::array({"printf"})}},
	    "test-version", &state);
	UAM_ASSERT(!invalid.value("ok", true));
	UAM_ASSERT_EQ(invalid["error"].value("code", ""), std::string("invalid_request"));

#if defined(__APPLE__)
	const std::vector<std::string> arguments = {"/usr/bin/printf", "remote-ok"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {"cmd.exe", "/d", "/s", "/c", "<nul set /p =remote-ok"};
#endif

	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "start-1"}, {"type", "process.start"}, {"sessionId", "session-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", fs::temp_directory_path().string()}, {"argv", arguments}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));

	std::string output;
	bool exited = false;
	for (int attempt = 0; attempt < 100 && !exited; ++attempt)
	{
		const nlohmann::json polled = uam::remote::HandleRunnerRequest(
		    {{"id", "poll-1"}, {"type", "process.poll"}, {"sessionId", "session-1"},
		     {"controlToken", kProcessControlToken}},
		    "test-version", &state);
		UAM_ASSERT(polled.value("ok", false));
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(polled["result"].value("stdoutBase64", ""), chunk));
		output += chunk;
		exited = !polled["result"].value("running", true);
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(output, std::string("remote-ok"));

	const nlohmann::json removed = uam::remote::HandleRunnerRequest(
	    {{"id", "remove-1"}, {"type", "process.remove"}, {"sessionId", "session-1"},
	     {"controlToken", kProcessControlToken}},
	    "test-version", &state);
	UAM_ASSERT(removed.value("ok", false));
}

UAM_TEST(RemoteRunnerReplaysPolledOutputUntilItIsAcknowledged)
{
#if defined(__APPLE__)
	uam::remote::RunnerState state;
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "start-ack"}, {"type", "process.start"}, {"sessionId", "session-ack"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", fs::temp_directory_path().string()},
	     {"argv", nlohmann::json::array({"/usr/bin/printf", "ack-once"})}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));

	nlohmann::json first;
	std::string first_output;
	for (int attempt = 0; attempt < 100 && first_output.empty(); ++attempt)
	{
		first = uam::remote::HandleRunnerRequest(
		    {{"id", "poll-ack-1"}, {"type", "process.poll"},
		     {"sessionId", "session-ack"}, {"controlToken", kProcessControlToken},
		     {"acknowledgedOutput", true}},
		    "test-version", &state);
		UAM_ASSERT(first.value("ok", false));
		UAM_ASSERT(uam::base64::Decode(first["result"].value("stdoutBase64", ""), first_output));
		if (first_output.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(first_output, std::string("ack-once"));

	const nlohmann::json replay = uam::remote::HandleRunnerRequest(
	    {{"id", "poll-ack-2"}, {"type", "process.poll"},
	     {"sessionId", "session-ack"}, {"controlToken", kProcessControlToken},
	     {"acknowledgedOutput", true}},
	    "test-version", &state);
	std::string replay_output;
	UAM_ASSERT(uam::base64::Decode(replay["result"].value("stdoutBase64", ""), replay_output));
	UAM_ASSERT_EQ(replay_output, first_output);

	const nlohmann::json acknowledged = uam::remote::HandleRunnerRequest(
	    {{"id", "ack-1"}, {"type", "process.ack"}, {"sessionId", "session-ack"},
	     {"controlToken", kProcessControlToken},
	     {"stdoutCursor", first["result"]["stdoutCursor"]},
	     {"stderrCursor", first["result"]["stderrCursor"]}},
	    "test-version", &state);
	UAM_ASSERT(acknowledged.value("ok", false));

	const nlohmann::json final_poll = uam::remote::HandleRunnerRequest(
	    {{"id", "poll-ack-3"}, {"type", "process.poll"},
	     {"sessionId", "session-ack"}, {"controlToken", kProcessControlToken},
	     {"acknowledgedOutput", true}},
	    "test-version", &state);
	UAM_ASSERT(final_poll.value("ok", false));
	UAM_ASSERT(!final_poll["result"].value("running", true));
	UAM_ASSERT_EQ(final_poll["result"].value("stdoutBase64", ""), std::string(""));

	const nlohmann::json removed = uam::remote::HandleRunnerRequest(
	    {{"id", "remove-ack"}, {"type", "process.remove"}, {"sessionId", "session-ack"},
	     {"controlToken", kProcessControlToken}},
	    "test-version", &state);
	UAM_ASSERT(removed.value("ok", false));
#endif
}

UAM_TEST(RemoteRunnerReclaimsAcknowledgedSpoolPrefixesWithoutChangingCursors)
{
#if defined(__APPLE__) || defined(__linux__)
	uam::remote::RunnerState state(8);
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "rollover-start"}, {"type", "process.start"}, {"sessionId", "rollover"},
	     {"controlToken", kProcessControlToken}, {"cwd", fs::temp_directory_path().string()},
	     {"argv", nlohmann::json::array({"/bin/sh", "-c",
	         "printf 12345678; read reply; printf abcdefgh; read reply; printf ABCDEFGH"})}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	const auto poll = [&](const char* id, std::uint64_t cursor)
	{
		return uam::remote::HandleRunnerRequest(
		    {{"id", id}, {"type", "process.poll"}, {"sessionId", "rollover"},
		     {"controlToken", kProcessControlToken}, {"acknowledgedOutput", true},
		     {"stdoutCursor", cursor}, {"stderrCursor", std::uint64_t{0}}},
		    "test-version", &state);
	};
	nlohmann::json first;
	std::string first_output;
	for (int attempt = 0; attempt < 100 && first_output.empty(); ++attempt)
	{
		first = poll("rollover-poll-1", 0);
		UAM_ASSERT(first.value("ok", false));
		UAM_ASSERT(uam::base64::Decode(first["result"].value("stdoutBase64", ""), first_output));
		if (first_output.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(first_output, std::string("12345678"));
	const std::uint64_t first_cursor = first["result"]["stdoutCursor"].get<std::uint64_t>();
	UAM_ASSERT_EQ(first_cursor, std::uint64_t{8});
	const nlohmann::json ack = uam::remote::HandleRunnerRequest(
	    {{"id", "rollover-ack-1"}, {"type", "process.ack"}, {"sessionId", "rollover"},
	     {"controlToken", kProcessControlToken}, {"stdoutCursor", first_cursor},
	     {"stderrCursor", std::uint64_t{0}}}, "test-version", &state);
	UAM_ASSERT(ack.value("ok", false));
	const auto advance = [&](const char* id, std::uint64_t input_sequence)
	{
		return uam::remote::HandleRunnerRequest(
		    {{"id", id}, {"type", "process.write"}, {"sessionId", "rollover"},
		     {"controlToken", kProcessControlToken}, {"dataBase64", uam::base64::Encode("\n")},
		     {"inputSequence", input_sequence}}, "test-version", &state);
	};
	UAM_ASSERT(advance("rollover-write-1", 1).value("ok", false));
	nlohmann::json second;
	std::string second_output;
	for (int attempt = 0; attempt < 100 && second_output.empty(); ++attempt)
	{
		second = poll("rollover-poll-2", first_cursor);
		UAM_ASSERT(second.value("ok", false));
		UAM_ASSERT(uam::base64::Decode(second["result"].value("stdoutBase64", ""), second_output));
		if (second_output.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(second_output, std::string("abcdefgh"));
	UAM_ASSERT_EQ(second["result"]["stdoutCursor"].get<std::uint64_t>(), std::uint64_t{16});
	const nlohmann::json duplicate_ack = uam::remote::HandleRunnerRequest(
	    {{"id", "rollover-ack-replay"}, {"type", "process.ack"}, {"sessionId", "rollover"},
	     {"controlToken", kProcessControlToken}, {"stdoutCursor", first_cursor},
	     {"stderrCursor", std::uint64_t{0}}}, "test-version", &state);
	UAM_ASSERT(duplicate_ack.value("ok", false));
	const nlohmann::json ack_second = uam::remote::HandleRunnerRequest(
	    {{"id", "rollover-ack-2"}, {"type", "process.ack"}, {"sessionId", "rollover"},
	     {"controlToken", kProcessControlToken}, {"stdoutCursor", std::uint64_t{16}},
	     {"stderrCursor", std::uint64_t{0}}}, "test-version", &state);
	UAM_ASSERT(ack_second.value("ok", false));
	UAM_ASSERT(advance("rollover-write-2", 2).value("ok", false));
	nlohmann::json third;
	std::string third_output;
	for (int attempt = 0; attempt < 100 && third_output.empty(); ++attempt)
	{
		third = poll("rollover-poll-3", 16);
		UAM_ASSERT(third.value("ok", false));
		UAM_ASSERT(uam::base64::Decode(third["result"].value("stdoutBase64", ""), third_output));
		if (third_output.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(third_output, std::string("ABCDEFGH"));
	for (int attempt = 0; attempt < 100 && third["result"].value("running", true); ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		third = poll("rollover-poll-exit", 16);
		UAM_ASSERT(third.value("ok", false));
	}
	UAM_ASSERT(!third["result"].value("running", true));
	const nlohmann::json removed = uam::remote::HandleRunnerRequest(
	    {{"id", "rollover-remove"}, {"type", "process.remove"}, {"sessionId", "rollover"},
	     {"controlToken", kProcessControlToken}}, "test-version", &state);
	UAM_ASSERT(removed.value("ok", false));
#endif
}

UAM_TEST(RemoteRunnerKeepsTheSpoolLimitForUnreadDisconnectedOutput)
{
#if defined(__APPLE__) || defined(__linux__)
	uam::remote::RunnerState state(8);
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "unread-cap-start"}, {"type", "process.start"}, {"sessionId", "unread-cap"},
	     {"controlToken", kProcessControlToken}, {"cwd", fs::temp_directory_path().string()},
	     {"argv", nlohmann::json::array({"/bin/sh", "-c", "printf 12345678abcdefgh"})}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	nlohmann::json polled;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		polled = uam::remote::HandleRunnerRequest(
		    {{"id", "unread-cap-poll"}, {"type", "process.poll"}, {"sessionId", "unread-cap"},
		     {"controlToken", kProcessControlToken}, {"acknowledgedOutput", true}},
		    "test-version", &state);
		if (!polled.value("ok", false)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	UAM_ASSERT(!polled.value("ok", true));
	UAM_ASSERT_EQ(polled["error"].value("code", ""), std::string("read_failed"));
	UAM_ASSERT(polled["error"].value("message", "").find("disconnect spool limit") !=
	           std::string::npos);
#endif
}

UAM_TEST(RemoteRunnerProcessProxySpecStaysOffTheCommandLineAndReconnectCanAttach)
{
	TempDir temp("uam-proxy-unicode");
	const fs::path workspace = temp.root / uam::paths::PathFromUtf8("r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae");
	fs::create_directories(workspace);
	const std::string encoded = uam::remote::BuildProcessProxySpec(
	    "acp-chat-1", workspace, {"provider", "--stdio"},
	    {{"PATH", "/usr/bin"}}, true, "delivery-token", 123, 45);
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(encoded, decoded));
	const nlohmann::json spec = nlohmann::json::parse(decoded);
	UAM_ASSERT_EQ(spec.value("sessionId", ""), std::string("acp-chat-1"));
	UAM_ASSERT_EQ(spec.value("cwd", ""), uam::paths::Utf8PathString(workspace));
	UAM_ASSERT_EQ(spec["argv"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(spec["environment"].value("PATH", ""), std::string("/usr/bin"));
	UAM_ASSERT(spec.value("attachOnly", false));
	UAM_ASSERT_EQ(spec.value("deliveryToken", ""), std::string("delivery-token"));
	UAM_ASSERT_EQ(spec.value("deliveredStdoutCursor", 0), 123);
	UAM_ASSERT_EQ(spec.value("deliveredStderrCursor", 0), 45);

	uam::remote::RunnerState state;
#if defined(__APPLE__)
	const std::vector<std::string> arguments = {"/bin/sh", "-c", "sleep 0.2"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {"cmd.exe", "/d", "/s", "/c", "ping -n 2 127.0.0.1 >nul"};
#endif
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "start"}, {"type", "process.start"}, {"sessionId", "acp-chat-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", uam::paths::Utf8PathString(workspace)}, {"argv", arguments}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	const nlohmann::json attached = uam::remote::HandleRunnerRequest(
	    {{"id", "attach"}, {"type", "process.start"}, {"sessionId", "acp-chat-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", uam::paths::Utf8PathString(workspace)}, {"argv", arguments},
	     {"attachIfExists", true}},
	    "test-version", &state);
	UAM_ASSERT(attached.value("ok", false));
	UAM_ASSERT(attached["result"].value("attached", false));
	const nlohmann::json conflict = uam::remote::HandleRunnerRequest(
	    {{"id", "conflict"}, {"type", "process.start"}, {"sessionId", "acp-chat-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", uam::paths::Utf8PathString(workspace)},
	     {"argv", nlohmann::json::array({"/usr/bin/printf", "different"})},
	     {"attachIfExists", true}},
	    "test-version", &state);
	UAM_ASSERT(!conflict.value("ok", true));
	UAM_ASSERT_EQ(conflict["error"].value("code", ""), std::string("session_conflict"));
}

UAM_TEST(RemoteRunnerInputDeliveryIsAcknowledgedAndDeduplicatedByStableId)
{
	const std::string delivery_line = uam::remote::BuildRemoteInputDeliveryLine(
	    "delivery-token", "prompt-17", "{\"jsonrpc\":\"2.0\"}\n");
	UAM_ASSERT(delivery_line.starts_with(
	    std::string(uam::remote::kRemoteInputDeliveryPrefix) +
	    "delivery-token prompt-17 "));
	UAM_ASSERT(delivery_line.ends_with("\n"));
	UAM_ASSERT(uam::remote::BuildRemoteInputDeliveryLine(
	               "bad token", "prompt-17", "payload").empty());

	uam::remote::RunnerState state;
#if defined(__APPLE__)
	const std::vector<std::string> arguments = {"/bin/cat"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {
	    "cmd.exe", "/d", "/s", "/c", "more"};
#else
	return;
#endif
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-start"}, {"type", "process.start"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken},
	     {"cwd", fs::temp_directory_path().string()}, {"argv", arguments}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	const std::string encoded = uam::base64::Encode("prompt-once\n");
	const nlohmann::json first = uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-write-1"}, {"type", "process.write"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken},
	     {"dataBase64", encoded}, {"inputSequence", std::uint64_t{1}},
	     {"deliveryId", "prompt-17.0.1"}},
	    "test-version", &state);
	UAM_ASSERT(first.value("ok", false));
	UAM_ASSERT(!first["result"].value("duplicate", false));
	const nlohmann::json replay = uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-write-2"}, {"type", "process.write"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken},
	     {"dataBase64", encoded}, {"inputSequence", std::uint64_t{2}},
	     {"deliveryId", "prompt-17.0.1"}},
	    "test-version", &state);
	UAM_ASSERT(replay.value("ok", false));
	UAM_ASSERT(replay["result"].value("duplicate", false));
	UAM_ASSERT_EQ(replay["result"].value("inputSequence", 0), 1);
	const nlohmann::json conflict = uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-write-3"}, {"type", "process.write"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken},
	     {"dataBase64", uam::base64::Encode("different\n")},
	     {"inputSequence", std::uint64_t{2}}, {"deliveryId", "prompt-17.0.1"}},
	    "test-version", &state);
	UAM_ASSERT(!conflict.value("ok", true));
	UAM_ASSERT_EQ(conflict["error"].value("code", ""), std::string("input_conflict"));
	(void)uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-stop"}, {"type", "process.stop"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken}},
	    "test-version", &state);
	(void)uam::remote::HandleRunnerRequest(
	    {{"id", "delivery-remove"}, {"type", "process.remove"},
	     {"sessionId", "delivery-session"}, {"controlToken", kProcessControlToken}},
	    "test-version", &state);
}

UAM_TEST(RemoteRunnerTransientLeaseReclaimsAnAbandonedLiveProcess)
{
#if defined(__APPLE__)
	const std::vector<std::string> arguments = {"/bin/sh", "-c", "sleep 10"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {
	    "cmd.exe", "/d", "/s", "/c", "ping -n 11 127.0.0.1 >nul"};
#else
	return;
#endif
	uam::remote::RunnerState state;
	const nlohmann::json started = uam::remote::HandleRunnerRequest(
	    {{"id", "lease-start"}, {"type", "process.start"},
	     {"sessionId", "leased-session"}, {"controlToken", "leased-token"},
	     {"transientLeaseMs", 250}, {"cwd", fs::temp_directory_path().string()},
	     {"argv", arguments}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	UAM_ASSERT(state.HasManagedProcesses());
	std::this_thread::sleep_for(std::chrono::milliseconds(350));
	UAM_ASSERT(!state.HasManagedProcesses());
}

UAM_TEST(RemoteTerminalUsesAForcedSshPtyAndExecutesOnlyTheEncodedArgv)
{
	TempDir temp("uam-terminal-unicode");
#if defined(__APPLE__)
	const fs::path cwd = temp.root / uam::paths::PathFromUtf8("r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae");
	fs::create_directories(cwd);
#else
	const fs::path cwd = uam::paths::PathFromUtf8("/tmp/r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae");
#endif
	const std::vector<std::string> provider_argv = {"/usr/bin/printf", "terminal-ok"};
	const std::vector<std::string> ssh_argv =
	    uam::remote::BuildRemoteTerminalSshArgv("home-lab", "linux", "4.5.7", cwd,
	                                             provider_argv);
	UAM_ASSERT(!ssh_argv.empty());
	UAM_ASSERT_EQ(ssh_argv.front(), std::string("ssh"));
	UAM_ASSERT(uam::ranges::Contains(ssh_argv, std::string("-tt")));
	UAM_ASSERT(ssh_argv.back().starts_with(
	    "~/.local/share/uam/runner/4.5.7/uam-runner terminal "));
	UAM_ASSERT(!uam::remote::BuildRemoteTerminalSshArgv(
	                 "-oProxyCommand=bad", "linux", "4.5.7", cwd, provider_argv)
	                 .size());

	const std::string encoded_spec =
	    ssh_argv.back().substr(ssh_argv.back().rfind(' ') + 1);
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(encoded_spec, decoded));
	const nlohmann::json spec = nlohmann::json::parse(decoded);
	UAM_ASSERT_EQ(spec.value("cwd", ""), uam::paths::Utf8PathString(cwd));
	UAM_ASSERT_EQ(spec["argv"].get<std::vector<std::string>>(), provider_argv);
	UAM_ASSERT(spec["environment"].empty());

	const std::vector<std::string> windows_ssh =
	    uam::remote::BuildRemoteTerminalSshArgv(
	        "windows-lab", "windows", "4.5.7", uam::paths::PathFromUtf8("C:\\Work\\r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae"),
	        {"opencode.cmd", "--help"});
	UAM_ASSERT(!windows_ssh.empty());
	UAM_ASSERT(windows_ssh.back().starts_with("powershell.exe "));
	const std::string windows_command = windows_ssh.back();
	const std::size_t spec_end = windows_command.rfind("'\"");
	UAM_ASSERT(spec_end != std::string::npos);
	const std::size_t spec_start = windows_command.rfind("'", spec_end - 1);
	UAM_ASSERT(spec_start != std::string::npos);
	std::string windows_decoded;
	UAM_ASSERT(uam::base64::Decode(windows_command.substr(spec_start + 1, spec_end - spec_start - 1), windows_decoded));
	UAM_ASSERT_EQ(nlohmann::json::parse(windows_decoded).value("cwd", ""), std::string("C:\\Work\\r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae"));
	UAM_ASSERT(windows_ssh.back().find(".uam/runner/4.5.7/uam-runner.exe") !=
	           std::string::npos);

#if defined(__APPLE__)
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
	auto& service = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(service.StartStdioProcess(process, cwd,
	                                     {runner.string(), "terminal", encoded_spec},
	                                     &error));
	std::string output;
	std::array<char, 64> buffer{};
	int exit_code = -1;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		const std::ptrdiff_t read = service.ReadStdioProcessStdout(
		    process, buffer.data(), buffer.size(), &error);
		if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
		if (service.PollStdioProcessExited(process, &exit_code)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	service.CloseStdioProcessHandles(process);
	UAM_ASSERT_EQ(exit_code, 0);
	UAM_ASSERT_EQ(output, std::string("terminal-ok"));
#endif
}

UAM_TEST(RemoteRunnerBootstrapUsesOnlyAValidatedSshAliasAndVerifiedUserInstall)
{
	TempDir temp("uam-runner-bootstrap-artifact");
	const fs::path runner = temp.root / "uam-runner";
	UAM_ASSERT(uam::io::WriteTextFile(runner, "runner"));
	uam::remote::BootstrapPlan plan;
	std::string error;
	const std::string checksum(64, 'a');
	const std::vector<uam::remote::RunnerArtifact> artifacts = {
	    {"linux", "arm64", runner, checksum}};
	UAM_ASSERT(uam::remote::BuildBootstrapPlan("home-lab", "4.5.7", "install-1",
	                                           artifacts, plan, &error));
	UAM_ASSERT_EQ(plan.steps.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(plan.install_directory,
	              std::string("the recommended private UAM folder under the remote user's home directory"));
	UAM_ASSERT(plan.runner_directory.empty());
	UAM_ASSERT_EQ(plan.artifacts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(plan.artifacts.front().sha256, checksum);
	const std::string preview = uam::remote::BootstrapPlanPreview(plan);
	UAM_ASSERT(preview.find("password") == std::string::npos);
	UAM_ASSERT(preview.find("identity_file") == std::string::npos);
	UAM_ASSERT(preview.find("SHA-256") != std::string::npos);

	UAM_ASSERT(!uam::remote::BuildBootstrapPlan("-oProxyCommand=bad", "4.5.7",
	                                            "install-1", artifacts, plan, &error));
	UAM_ASSERT(!uam::remote::BuildBootstrapPlan("home-lab;touch-bad", "4.5.7",
	                                            "install-1", artifacts, plan, &error));
	UAM_ASSERT(!uam::remote::BuildBootstrapPlan("home-lab", "4.5.7", "install-1",
	                                            artifacts, plan, &error, "../outside"));
	UAM_ASSERT(!uam::remote::BuildBootstrapPlan("home-lab", "4.5.7", "install-1",
	                                            artifacts, plan, &error, "/opt/uam"));
	UAM_ASSERT(uam::remote::SshBridgeArgv("-oProxyCommand=bad", "linux", "4.5.7").empty());
	const std::vector<std::string> windows_bridge =
	    uam::remote::SshBridgeArgv("windows-lab", "windows", "4.5.7");
	UAM_ASSERT(!windows_bridge.empty());
	UAM_ASSERT(windows_bridge.back().find("Join-Path $HOME '.uam/runner/4.5.7/uam-runner.exe'") !=
	           std::string::npos);
	UAM_ASSERT(windows_bridge.back().find("'$HOME") == std::string::npos);
	const std::vector<std::string> custom_bridge =
	    uam::remote::SshBridgeArgv("windows-lab", "windows", "4.5.7", "tools/uam");
	UAM_ASSERT(custom_bridge.back().find("Join-Path $HOME 'tools/uam/4.5.7/uam-runner.exe'") !=
	           std::string::npos);
	const std::vector<std::string> linux_bridge =
	    uam::remote::SshBridgeArgv("linux-lab", "linux", "4.5.7", "tools/uam");
	UAM_ASSERT(linux_bridge.back().find(
	    "tools/uam/uam-4.5.7-p" +
	    std::to_string(uam::remote::kRunnerProtocolVersion) + ".sock") !=
	    std::string::npos);
}

#if defined(__APPLE__)
UAM_TEST(RemoteRunnerPathsFailRecoverablyWhenProcessWorkingDirectoryDisappears)
{
	TempDir temp("uam-missing-runner-cwd");
	const fs::path original_path = fs::current_path();
	struct CurrentPathGuard
	{
		fs::path path;
		~CurrentPathGuard()
		{
			std::error_code error;
			fs::current_path(path, error);
		}
	} restore_path{original_path};
	const fs::path missing_path = temp.root / "gone";
	fs::create_directories(missing_path);
	fs::current_path(missing_path);
	fs::remove_all(missing_path);

	std::string error;
	uam::remote::RunnerClient client(
	    PlatformServicesFactory::Instance().process_service,
	    {PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().string()});
	UAM_ASSERT(!client.Connect(&error));
	UAM_ASSERT_EQ(error, std::string("The remote runner bridge working directory is unavailable."));

	uam::remote::BootstrapPlan plan;
	plan.ssh_alias = "missing-cwd";
	plan.runner_directory = {};
	plan.steps = {{"probe", {"ssh", "missing-cwd"}, ""},
	              {"fallback", {"ssh", "missing-cwd"}, ""}};
	uam::remote::RunnerArtifact artifact;
	artifact.platform = "linux";
	artifact.architecture = "x86_64";
	artifact.sha256 = std::string(64, 'a');
	plan.artifacts.push_back(std::move(artifact));
	const uam::remote::BootstrapResult bootstrap = uam::remote::ExecuteBootstrapPlan(plan);
	UAM_ASSERT(!bootstrap.ok);
	UAM_ASSERT_EQ(bootstrap.error,
	              std::string("The remote setup working directory is unavailable."));
}

UAM_TEST(RemoteRunnerBootstrapCancellationPreventsSshLaunch)
{
	TempDir temp("uam-runner-canceled-bootstrap");
	const fs::path runner = temp.root / "uam-runner";
	const fs::path ssh = temp.root / "ssh";
	const fs::path log = temp.root / "ssh.log";
	UAM_ASSERT(uam::io::WriteTextFile(runner, "unused-runner"));
	UAM_ASSERT(uam::io::WriteTextFile(ssh,
	    "#!/bin/sh\nprintf launched >> \"$UAM_TEST_BOOTSTRAP_LOG\"\nexit 1\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	ScopedEnvVar scoped_path("PATH", temp.root.string());
	ScopedEnvVar scoped_log("UAM_TEST_BOOTSTRAP_LOG", log.string());
	uam::remote::BootstrapPlan plan;
	std::string error;
	UAM_ASSERT(uam::remote::BuildBootstrapPlan(
	    "canceled-lab", "4.5.7", "nonce-1",
	    {{"linux", "x86_64", runner, std::string(64, 'a')}}, plan, &error));
	std::stop_source canceled;
	canceled.request_stop();
	const uam::remote::BootstrapResult result =
	    uam::remote::ExecuteBootstrapPlan(plan, canceled.get_token());
	UAM_ASSERT(!result.ok);
	UAM_ASSERT_EQ(result.error, std::string("Remote setup was canceled."));
	uam::remote::BootstrapResult installed;
	installed.platform = "linux";
	UAM_ASSERT(!uam::remote::FinalizeBootstrapPlan(
	    plan, installed, false, &error, canceled.get_token()));
	UAM_ASSERT_EQ(error, std::string("Remote setup was canceled."));
	UAM_ASSERT(!fs::exists(log));
}

UAM_TEST(RemoteRunnerBootstrapRollbackNeverStopsThePreviousLinuxService)
{
	TempDir temp("uam-runner-linux-rollback");
	const fs::path runner = temp.root / "uam-runner";
	const fs::path log = temp.root / "ssh.log";
	const fs::path ssh = temp.root / "ssh";
	const fs::path scp = temp.root / "scp";
	UAM_ASSERT(uam::io::WriteTextFile(runner, "linux-runner"));
	UAM_ASSERT(uam::io::WriteTextFile(ssh, R"(#!/bin/sh
last=
for arg in "$@"; do last=$arg; done
printf '%s\n' "$last" >> "$UAM_TEST_BOOTSTRAP_LOG"
lines=$(wc -l < "$UAM_TEST_BOOTSTRAP_LOG")
if [ "$lines" -eq 1 ]; then printf 'Linux\nx86_64\n'; fi
if [ "$lines" -eq 5 ]; then printf '4.5.7\n'; fi
if [ "$lines" -eq 6 ]; then printf '%s\n' ")" +
	    std::to_string(uam::remote::kRunnerProtocolVersion) + R"("; fi
)"));
	UAM_ASSERT(uam::io::WriteTextFile(scp, "#!/bin/sh\nexit 0\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	fs::permissions(scp, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar scoped_path("PATH", temp.root.string() + ":" + inherited_path);
	ScopedEnvVar scoped_log("UAM_TEST_BOOTSTRAP_LOG", log.string());
	uam::remote::BootstrapPlan plan;
	std::string error;
	UAM_ASSERT(uam::remote::BuildBootstrapPlan(
	    "linux-lab", "4.5.7", "nonce-1",
	    {{"linux", "x86_64", runner, std::string(64, 'a')}}, plan, &error,
	    "tools/uam"));
	plan.previous_platform = "linux";
	plan.previous_version = "4.4.0";
	plan.previous_runner_directory = "old-tools/uam";
	plan.previous_protocol_version = uam::remote::kRunnerProtocolVersion;
	const uam::remote::BootstrapResult result = uam::remote::ExecuteBootstrapPlan(plan);
	UAM_ASSERT(result.ok);
	UAM_ASSERT(uam::remote::FinalizeBootstrapPlan(plan, result, false, &error));

	const std::string commands = uam::io::ReadTextFile(log);
	UAM_ASSERT(commands.find("old-tools/uam/4.4.0/uam-runner") == std::string::npos);
	UAM_ASSERT(commands.find("installed=~/tools/uam/4.5.7/uam-runner") !=
	           std::string::npos);
	UAM_ASSERT(commands.find("\"$installed\" stop --socket ~/tools/uam/uam-4.5.7-p" +
	                         std::to_string(uam::remote::kRunnerProtocolVersion) + ".sock") !=
	           std::string::npos);
}

UAM_TEST(RemoteRunnerBootstrapRollsBackWhenSshLosesActivationAcknowledgement)
{
	TempDir temp("uam-runner-linux-activation-transport-failure");
	const fs::path runner = temp.root / "uam-runner";
	const std::string temp_name = temp.root.filename().string();
	const std::string temp_prefix = "uam-runner-linux-activation-transport-failure-";
	const fs::path remote_home = fs::path("/tmp") / ("uam-" + temp_name.substr(temp_prefix.size()));
	struct RemoteHomeCleanup
	{
		fs::path path;
		~RemoteHomeCleanup()
		{
			std::error_code error;
			fs::remove_all(path, error);
		}
	} remote_home_cleanup{remote_home};
	fs::create_directories(remote_home);
	const fs::path installed = remote_home / "tools/uam/4.5.7/uam-runner";
	const fs::path log = temp.root / "ssh.log";
	const fs::path ssh = temp.root / "ssh";
	const fs::path scp = temp.root / "scp";
	const fs::path sha256sum = temp.root / "sha256sum";
	const std::string new_runner = "#!/bin/sh\ncase \"$1\" in stop|start) exit 0;; --version) echo 4.5.7;; --protocol-version) echo " +
	    std::to_string(uam::remote::kRunnerProtocolVersion) + ";; esac\n";
	const std::string old_runner = "#!/bin/sh\ncase \"$1\" in stop|start) exit 0;; --version) echo 4.4.0;; --protocol-version) echo " +
	    std::to_string(uam::remote::kRunnerProtocolVersion) + ";; esac\n";
	UAM_ASSERT(uam::io::WriteTextFile(runner, new_runner));
	UAM_ASSERT(uam::io::WriteTextFile(installed, old_runner));
	fs::permissions(installed, fs::perms::owner_read | fs::perms::owner_write |
	                            fs::perms::owner_exec);
	UAM_ASSERT(uam::io::WriteTextFile(ssh, R"(#!/bin/sh
last=
for arg in "$@"; do last=$arg; done
printf '%s\n' "$last" >> "$UAM_TEST_BOOTSTRAP_LOG"
if [ "$last" = "uname -s && uname -m" ]; then printf 'Linux\nx86_64\n'; exit 0; fi
eval "$last"
status=$?
case "$last" in *': > "$marker"; if mv'*) if [ "$status" -eq 0 ]; then exit 255; fi;; esac
exit "$status"
)"));
	UAM_ASSERT(uam::io::WriteTextFile(scp, R"(#!/bin/sh
previous=
last=
for arg in "$@"; do previous=$last; last=$arg; done
destination=${last#*:}
cp "$previous" "$HOME/$destination"
)"));
	UAM_ASSERT(uam::io::WriteTextFile(sha256sum, "#!/bin/sh\ncat >/dev/null\nexit 0\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	fs::permissions(scp, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	fs::permissions(sha256sum, fs::perms::owner_read | fs::perms::owner_write |
	                             fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar scoped_path("PATH", temp.root.string() + ":" + inherited_path);
	ScopedEnvVar scoped_home("HOME", remote_home.string());
	ScopedEnvVar scoped_log("UAM_TEST_BOOTSTRAP_LOG", log.string());
	uam::remote::BootstrapPlan plan;
	std::string error;
	UAM_ASSERT(uam::remote::BuildBootstrapPlan(
	    "linux-lab", "4.5.7", "nonce-transport",
	    {{"linux", "x86_64", runner, std::string(64, 'a')}}, plan, &error,
	    "tools/uam"));
	const uam::remote::BootstrapResult result = uam::remote::ExecuteBootstrapPlan(plan);
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(result.error.find("Verify and activate runner") != std::string::npos);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(installed), old_runner);
	UAM_ASSERT(!fs::exists(remote_home / "tools/uam/4.5.7/uam-runner.rollback-nonce-transport"));
	UAM_ASSERT(!fs::exists(remote_home / "tools/uam/4.5.7/uam-runner.activation-nonce-transport"));
	const std::string commands = uam::io::ReadTextFile(log);
	UAM_ASSERT(commands.find("rm -f \"$marker\"") != std::string::npos);
	UAM_ASSERT(commands.find("rm -f \"$backup\"") != std::string::npos);
	UAM_ASSERT(commands.find("exit \"$status\"; fi") != std::string::npos);
}

UAM_TEST(RemoteRunnerBootstrapRejectsAnOversizedLinuxSocketBeforeCopy)
{
	TempDir temp("uam-runner-linux-long-socket");
	const fs::path runner = temp.root / "uam-runner";
	const fs::path command_log = temp.root / "ssh.log";
	const fs::path copy_log = temp.root / "scp.log";
	const fs::path ssh = temp.root / "ssh";
	const fs::path scp = temp.root / "scp";
	UAM_ASSERT(uam::io::WriteTextFile(runner, "linux-runner"));
	UAM_ASSERT(uam::io::WriteTextFile(ssh, R"(#!/bin/sh
last=
for arg in "$@"; do last=$arg; done
printf '%s\n' "$last" >> "$UAM_TEST_BOOTSTRAP_LOG"
case "$last" in
  "uname -s && uname -m") printf 'Linux\nx86_64\n' ;;
  *'socket="$HOME/'*) HOME="$UAM_TEST_LONG_HOME" /bin/sh -c "$last" ;;
  *) exit 9 ;;
esac
)"));
	UAM_ASSERT(uam::io::WriteTextFile(
	    scp, "#!/bin/sh\nprintf 'copied' > \"$UAM_TEST_COPY_LOG\"\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	fs::permissions(scp, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar scoped_path("PATH", temp.root.string() + ":" + inherited_path);
	ScopedEnvVar scoped_command_log("UAM_TEST_BOOTSTRAP_LOG", command_log.string());
	ScopedEnvVar scoped_copy_log("UAM_TEST_COPY_LOG", copy_log.string());
	ScopedEnvVar scoped_long_home("UAM_TEST_LONG_HOME", "/" + std::string(128, 'a'));
	uam::remote::BootstrapPlan plan;
	std::string error;
	UAM_ASSERT(uam::remote::BuildBootstrapPlan(
	    "linux-lab", "4.9.0-alpha-8", "nonce-1",
	    {{"linux", "x86_64", runner, std::string(64, 'a')}}, plan, &error,
	    "tools/uam"));
	const uam::remote::BootstrapResult result = uam::remote::ExecuteBootstrapPlan(plan);
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(result.error.find("runner socket path too long") != std::string::npos);
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(copy_log));
	const std::string commands = uam::io::ReadTextFile(command_log);
	UAM_ASSERT(commands.find("socket=\"$HOME/tools/uam/") != std::string::npos);
	UAM_ASSERT(commands.find("mkdir -p") == std::string::npos);
}

UAM_TEST(RemoteRunnerBootstrapSelectsAndHardensTheWindowsArtifact)
{
	TempDir temp("uam-runner-windows-bootstrap");
	const fs::path runner = temp.root / "uam-runner.exe";
	const fs::path log = temp.root / "ssh.log";
	const fs::path ssh = temp.root / "ssh";
	const fs::path scp = temp.root / "scp";
	UAM_ASSERT(uam::io::WriteTextFile(runner, "windows-runner"));
	UAM_ASSERT(uam::io::WriteTextFile(ssh, R"(#!/bin/sh
last=
for arg in "$@"; do last=$arg; done
printf '%s\n' "$last" >> "$UAM_TEST_BOOTSTRAP_LOG"
case "$last" in
  "uname -s && uname -m") exit 1 ;;
  *-EncodedCommand*)
    lines=$(wc -l < "$UAM_TEST_BOOTSTRAP_LOG")
    if [ "$lines" -eq 2 ]; then printf 'Windows\nAMD64\n'; fi
    if [ "$lines" -eq 5 ]; then printf '4.5.7\n'; fi
    if [ "$lines" -eq 6 ]; then printf '%s\n' ")" +
	    std::to_string(uam::remote::kRunnerProtocolVersion) + R"("; fi
    ;;
esac
)"));
	UAM_ASSERT(uam::io::WriteTextFile(scp, "#!/bin/sh\nexit 0\n"));
	fs::permissions(ssh, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	fs::permissions(scp, fs::perms::owner_read | fs::perms::owner_write |
	                         fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar scoped_path("PATH", temp.root.string() + ":" + inherited_path);
	ScopedEnvVar scoped_log("UAM_TEST_BOOTSTRAP_LOG", log.string());
	uam::remote::BootstrapPlan plan;
	std::string error;
	UAM_ASSERT(uam::remote::BuildBootstrapPlan(
	    "windows-lab", "4.5.7", "nonce-1",
	    {{"windows", "x86_64", runner, std::string(64, 'a')}}, plan, &error,
	    "tools/uam"));
	plan.previous_platform = "windows";
	plan.previous_version = "4.4.0";
	plan.previous_runner_directory = "old-tools/uam";
	plan.previous_protocol_version = uam::remote::kRunnerProtocolVersion;
	const uam::remote::BootstrapResult result =
	    uam::remote::ExecuteBootstrapPlan(plan);
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(result.platform, std::string("windows"));
	UAM_ASSERT_EQ(result.architecture, std::string("x86_64"));
	const std::string commands = uam::io::ReadTextFile(log);
	UAM_ASSERT(commands.find("-EncodedCommand") != std::string::npos);
	UAM_ASSERT(commands.find("-Command \"") == std::string::npos);
	std::string decoded_commands;
	std::istringstream command_lines(commands);
	for (std::string command; std::getline(command_lines, command);)
	{
		const std::string marker = "-EncodedCommand ";
		const std::size_t offset = command.find(marker);
		if (offset == std::string::npos) continue;
		std::string utf16_le;
		UAM_ASSERT(uam::base64::Decode(command.substr(offset + marker.size()), utf16_le));
		for (std::size_t index = 0; index + 1 < utf16_le.size(); index += 2)
			decoded_commands.push_back(utf16_le[index]);
		decoded_commands.push_back('\n');
	}
	UAM_ASSERT(decoded_commands.find("Get-FileHash") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("Copy-Item -LiteralPath $installed -Destination $backup") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("New-Item -ItemType File -Path $marker") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("if (Test-Path -LiteralPath $marker)") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("Remove-Item -LiteralPath $marker -Force -ErrorAction Stop") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("Copy-Item -LiteralPath $backup -Destination $installed -Force -ErrorAction Stop") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("Runner protocol verification failed") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("old-tools/uam/4.4.0/uam-runner.exe") ==
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("& $installed stop") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("Previous runner") == std::string::npos);
	UAM_ASSERT(decoded_commands.find("Start-Sleep -Milliseconds 100") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("if (Test-Path -LiteralPath $file)") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("tools/uam/4.5.7/uam-runner.exe") != std::string::npos);
	UAM_ASSERT(decoded_commands.find("Join-Path $HOME 'tools/uam/4.5.7/uam-runner.exe'") !=
	           std::string::npos);
	UAM_ASSERT(decoded_commands.find("'$HOME") == std::string::npos);
	UAM_ASSERT(commands.find(runner.string()) == std::string::npos);
}
#endif

UAM_TEST(RemoteRunnerClientRoundTripsThroughTheRealBridgeProcess)
{
	const fs::path test_executable =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	const fs::path runner = test_executable.parent_path() /
#if defined(_WIN32)
	                        "uam-runner.exe";
#else
	                        "uam-runner";
#endif
	UAM_ASSERT(fs::exists(runner));
	uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
	                                 {uam::paths::Utf8PathString(runner), "bridge-direct"});
	std::string error;
	UAM_ASSERT(client.Connect(&error));
	TempDir upload("uam-runner-upload");
	const fs::path workspace = upload.root / uam::paths::PathFromUtf8("r\xc3\xa9sum\xc3\xa9-\xe9\xa1\xb9\xe7\x9b\xae");
	fs::create_directories(workspace);
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "cwd-marker.txt", "bridge-ok"));
#if !defined(_WIN32)
	const std::vector<std::string> arguments = {"/bin/cat", "cwd-marker.txt"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {"cmd.exe", "/d", "/s", "/c", "type cwd-marker.txt"};
#endif
	UAM_ASSERT(client.StartProcess("bridge-session", workspace, arguments, {},
	                               &error));
	std::string output;
	bool exited = false;
	for (int attempt = 0; attempt < 100 && !exited; ++attempt)
	{
		uam::remote::ProcessPollResult polled;
		UAM_ASSERT(client.PollProcess("bridge-session", polled, &error));
		output += polled.standard_output;
		UAM_ASSERT(client.AcknowledgeProcessOutput("bridge-session", polled, &error));
		exited = !polled.running;
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(output, std::string("bridge-ok"));
	UAM_ASSERT(client.RemoveProcess("bridge-session", &error));
	const fs::path nested = workspace / uam::paths::PathFromUtf8("caf\xc3\xa9-\xe6\x96\x87\xe6\xa1\xa3");
	const fs::path uploaded = nested / uam::paths::PathFromUtf8("pi\xc3\xa8" "ce-\xe9\x99\x84\xe4\xbb\xb6.txt");
	UAM_ASSERT(client.UploadFile("upload-1", uploaded, "attachment-ok", &error));
	std::string uploaded_bytes;
	UAM_ASSERT(uam::io::TryReadBinaryFile(uploaded, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	const fs::path copied = nested / uam::paths::PathFromUtf8("copi\xc3\xa9-\xe5\x89\xaf\xe6\x9c\xac.txt");
	UAM_ASSERT(client.CopyFile("copy-1", uploaded, copied, false, &error));
	UAM_ASSERT(uam::io::TryReadBinaryFile(copied, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	UAM_ASSERT(!client.CopyFile("copy-2", uploaded, copied, false, &error));
	UAM_ASSERT(client.CopyFile("copy-3", uploaded, copied, true, &error));
	UAM_ASSERT(!client.UploadFile("upload-2", uploaded, "must-not-overwrite", &error));
	UAM_ASSERT(uam::io::TryReadBinaryFile(uploaded, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	uam::remote::DirectoryListing listing;
	UAM_ASSERT(client.ListDirectories(workspace, listing, &error));
	UAM_ASSERT_EQ(listing.directory, uam::paths::Utf8PathString(workspace));
	UAM_ASSERT_EQ(listing.directories.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(listing.directories.front().first, uam::paths::Utf8PathString(nested.filename()));
	UAM_ASSERT_EQ(listing.directories.front().second, uam::paths::Utf8PathString(nested));
	UAM_ASSERT(client.RemoveFile("remove-copy", copied, &error));
	UAM_ASSERT(!fs::exists(copied));
	UAM_ASSERT(client.RemoveFile("remove-upload", uploaded, &error));
	UAM_ASSERT(!fs::exists(uploaded));
	client.Disconnect();
}

UAM_TEST(RemoteRunnerRejectsIncompleteOrCorruptFileUploadsWithoutPublishingThem)
{
	TempDir temp("uam-runner-upload-validation");
	uam::remote::RunnerState state;
	const auto request = [&](std::string id, nlohmann::json body)
	{
		body["id"] = std::move(id);
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
	const fs::path target = temp.root / "attachment.txt";
	UAM_ASSERT(request("begin", {{"type", "file.begin"}, {"uploadId", "upload-bad"},
	                              {"path", target.string()}, {"size", 4},
	                              {"digest", "0000000000000000"}})
	               .value("ok", false));
	UAM_ASSERT(request("write", {{"type", "file.write"}, {"uploadId", "upload-bad"},
	                              {"dataBase64", uam::base64::Encode("data")}})
	               .value("ok", false));
	const nlohmann::json commit = request(
	    "commit", {{"type", "file.commit"}, {"uploadId", "upload-bad"}});
	UAM_ASSERT(!commit.value("ok", true));
	UAM_ASSERT_EQ(commit["error"].value("code", ""), std::string("digest_mismatch"));
	UAM_ASSERT(!fs::exists(target));
	UAM_ASSERT(request("abort", {{"type", "file.abort"}, {"uploadId", "upload-bad"}})
	               .value("ok", false));
	UAM_ASSERT(!fs::exists(temp.root / ".uam-upload-upload-bad.tmp"));
}

UAM_TEST(RemoteRunnerConcurrentProcessRemovalHasExactlyOneOwner)
{
	uam::remote::RunnerState state;
	const auto request = [&](std::string id, nlohmann::json body)
	{
		body["id"] = std::move(id);
		body["controlToken"] = kProcessControlToken;
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
#if defined(_WIN32)
	const std::vector<std::string> arguments = {
	    "cmd.exe", "/d", "/s", "/c", "ping -n 30 127.0.0.1 >nul"};
#else
	const std::vector<std::string> arguments = {"/bin/sh", "-c", "sleep 30"};
#endif
	UAM_ASSERT(request("start", {{"type", "process.start"}, {"sessionId", "remove-race"},
	                             {"cwd", fs::temp_directory_path().string()},
	                             {"argv", arguments}})
	               .value("ok", false));
	const nlohmann::json forged_stop = uam::remote::HandleRunnerRequest(
	    {{"id", "forged-stop"}, {"type", "process.stop"},
	     {"sessionId", "remove-race"}, {"controlToken", "wrong-token"}},
	    "test-version", &state);
	UAM_ASSERT(!forged_stop.value("ok", true));
	UAM_ASSERT_EQ(forged_stop["error"].value("code", ""), std::string("unauthorized"));
	UAM_ASSERT(request("stop", {{"type", "process.stop"}, {"sessionId", "remove-race"}})
	               .value("ok", false));

	constexpr std::size_t request_count = 16;
	std::atomic<std::size_t> ready{0};
	std::atomic<bool> start{false};
	std::vector<nlohmann::json> results(request_count);
	std::vector<std::jthread> workers;
	workers.reserve(request_count);
	for (std::size_t index = 0; index < request_count; ++index)
	{
		workers.emplace_back([&, index]
		{
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = request(
			    "remove-" + std::to_string(index),
			    {{"type", "process.remove"}, {"sessionId", "remove-race"}});
		});
	}
	while (ready.load(std::memory_order_acquire) != request_count) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	workers.clear();

	std::size_t successes = 0;
	for (const nlohmann::json& result : results)
	{
		if (result.value("ok", false))
		{
			++successes;
			continue;
		}
		UAM_ASSERT_EQ(result["error"].value("code", ""), std::string("session_not_found"));
	}
	UAM_ASSERT_EQ(successes, static_cast<std::size_t>(1));
}

UAM_TEST(RemoteRunnerConcurrentUploadFinalizationRejectsLateRequestsAndLeavesNoTemporaryFile)
{
	TempDir temp("uam-runner-upload-race");
	uam::remote::RunnerState state;
	const auto request = [&](std::string id, nlohmann::json body)
	{
		body["id"] = std::move(id);
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
	const std::string bytes = "data";
	const fs::path target = temp.root / "attachment.txt";
	const fs::path temporary = temp.root / ".uam-upload-upload-race.tmp";
	UAM_ASSERT(request("begin", {{"type", "file.begin"}, {"uploadId", "upload-race"},
	                             {"path", target.string()}, {"size", bytes.size()},
	                             {"digest", uam::hashing::Hex64Padded(
	                                            uam::hashing::Fnv1a64(bytes))}})
	               .value("ok", false));
	UAM_ASSERT(request("write", {{"type", "file.write"}, {"uploadId", "upload-race"},
	                             {"dataBase64", uam::base64::Encode(bytes)}})
	               .value("ok", false));

	constexpr std::size_t request_count = 16;
	std::atomic<std::size_t> ready{0};
	std::atomic<bool> start{false};
	std::vector<nlohmann::json> results(request_count);
	std::vector<std::jthread> workers;
	workers.reserve(request_count);
	for (std::size_t index = 0; index < request_count; ++index)
	{
		workers.emplace_back([&, index]
		{
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = request(
			    "finish-" + std::to_string(index),
			    {{"type", index % 2 == 0 ? "file.commit" : "file.abort"},
			     {"uploadId", "upload-race"}});
		});
	}
	while (ready.load(std::memory_order_acquire) != request_count) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	workers.clear();

	std::size_t successes = 0;
	bool committed = false;
	for (std::size_t index = 0; index < results.size(); ++index)
	{
		if (results[index].value("ok", false))
		{
			++successes;
			committed = index % 2 == 0;
			continue;
		}
		const std::string code = results[index]["error"].value("code", "");
		UAM_ASSERT(code == "upload_finished" || code == "upload_not_found");
	}
	UAM_ASSERT_EQ(successes, static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(fs::exists(target), committed);
	if (committed) UAM_ASSERT_EQ(uam::io::ReadTextFile(target), bytes);
	UAM_ASSERT(!fs::exists(temporary));

	for (const std::string_view type : {"file.write", "file.commit", "file.abort"})
	{
		nlohmann::json body = {{"type", type}, {"uploadId", "upload-race"}};
		if (type == "file.write") body["dataBase64"] = uam::base64::Encode("late");
		const nlohmann::json late = request("late-" + std::string(type), std::move(body));
		UAM_ASSERT(!late.value("ok", true));
		UAM_ASSERT_EQ(late["error"].value("code", ""), std::string("upload_not_found"));
		UAM_ASSERT(!fs::exists(temporary));
	}
}

UAM_TEST(RemoteRunnerLeasedChannelsAreBoundedAndConsumedOnce)
{
	uam::remote::RunnerState state;
	const std::function<nlohmann::json(nlohmann::json)> request = [&](nlohmann::json body)
	{
		body["id"] = "lease-test";
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
	const std::function<bool(const std::string&, std::int64_t)> open = [&](const std::string& id, std::int64_t lease)
	{
		return request({{"type", "channel.open"}, {"channelId", id}, {"leaseMs", lease}, {"attachIfExists", false}}).value("ok", false);
	};
	const std::function<nlohmann::json(const std::string&)> take = [&](const std::string& id)
	{
		return request({{"type", "channel.take"}, {"channelId", id}, {"direction", "desktopToRemote"}});
	};
	UAM_ASSERT(!open("invalid", -1));
	UAM_ASSERT(!open("invalid", 60001));
	UAM_ASSERT(!request({{"type", "channel.open"}, {"channelId", "invalid"}, {"leaseMs", "100"}}).value("ok", false));
	UAM_ASSERT(open("ordinary", 0));
	UAM_ASSERT(!take("ordinary").value("ok", false));
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(open("secret", 60000));
	UAM_ASSERT(state.HasManagedProcesses());
	UAM_ASSERT(!open("secret", 60000));
	UAM_ASSERT(!request({{"type", "channel.open"}, {"channelId", "secret"}, {"attachIfExists", true}}).value("ok", false));
	const std::string payload(65536, 'x');
	const nlohmann::json write = {{"type", "channel.write"}, {"channelId", "secret"},
	    {"direction", "desktopToRemote"}, {"writeSequence", std::uint64_t{1}}, {"dataBase64", uam::base64::Encode(payload)}};
	UAM_ASSERT(request(write).value("ok", false));
	UAM_ASSERT(request(write)["result"].value("duplicate", false));
	UAM_ASSERT(!request({{"type", "channel.write"}, {"channelId", "secret"}, {"direction", "remoteToDesktop"},
	    {"dataBase64", uam::base64::Encode("x")}}).value("ok", false));
	UAM_ASSERT(!request({{"type", "channel.poll"}, {"channelId", "secret"}, {"direction", "desktopToRemote"}}).value("ok", false));
	UAM_ASSERT(!request({{"type", "channel.take"}, {"channelId", "secret"}, {"direction", "invalid"}}).value("ok", false));
	const nlohmann::json claimed = take("secret");
	std::string decoded;
	UAM_ASSERT(claimed.value("ok", false));
	UAM_ASSERT(uam::base64::Decode(claimed["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, payload);
	UAM_ASSERT(!take("secret").value("ok", false));
	UAM_ASSERT(!request(write).value("ok", false));
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(open("canceled", 60000));
	UAM_ASSERT(request({{"type", "channel.close"}, {"channelId", "canceled"}}).value("ok", false));
	UAM_ASSERT(!take("canceled").value("ok", false));
	UAM_ASSERT(open("expired", 1));
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	UAM_ASSERT(!take("expired").value("ok", false));
	UAM_ASSERT(!state.HasManagedProcesses());
	for (int index = 0; index < 64; ++index) UAM_ASSERT(open("bounded-" + std::to_string(index), 60000));
	UAM_ASSERT(!open("over-limit", 60000));
}

UAM_TEST(RemoteRunnerChannelsRelayBoundedBytesInBothDirections)
{
	uam::remote::RunnerState state;
	const auto request = [&](std::string id, nlohmann::json body)
	{
		body["id"] = std::move(id);
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
	UAM_ASSERT(request("open", {{"type", "channel.open"}, {"channelId", "control-1"}})
	               .value("ok", false));
	UAM_ASSERT(request("write-up", {{"type", "channel.write"}, {"channelId", "control-1"},
	                                {"direction", "remoteToDesktop"},
	                                {"dataBase64", uam::base64::Encode("request\n")}})
	               .value("ok", false));
	const nlohmann::json upstream = request(
	    "poll-up", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                {"direction", "remoteToDesktop"}});
	std::string decoded;
	UAM_ASSERT(upstream.value("ok", false));
	UAM_ASSERT(uam::base64::Decode(upstream["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, std::string("request\n"));

	UAM_ASSERT(request("write-down", {{"type", "channel.write"}, {"channelId", "control-1"},
	                                  {"direction", "desktopToRemote"},
	                                  {"dataBase64", uam::base64::Encode("response\n")}})
	               .value("ok", false));
	const nlohmann::json downstream = request(
	    "poll-down", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                  {"direction", "desktopToRemote"}});
	UAM_ASSERT(downstream.value("ok", false));
	UAM_ASSERT(uam::base64::Decode(downstream["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, std::string("response\n"));

	UAM_ASSERT(request("write-durable", {{"type", "channel.write"},
	                                     {"channelId", "control-1"},
	                                     {"direction", "remoteToDesktop"},
	                                     {"writeSequence", std::uint64_t{1}},
	                                     {"dataBase64", uam::base64::Encode("durable\n")}})
	               .value("ok", false));
	const nlohmann::json duplicate_write = request(
	    "write-durable-retry", {{"type", "channel.write"}, {"channelId", "control-1"},
	                              {"direction", "remoteToDesktop"},
	                              {"writeSequence", std::uint64_t{1}},
	                              {"dataBase64", uam::base64::Encode("durable\n")}});
	UAM_ASSERT(duplicate_write["result"].value("duplicate", false));
	const nlohmann::json durable = request(
	    "poll-durable", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                     {"direction", "remoteToDesktop"},
	                     {"acknowledgedOutput", true},
	                     {"cursor", std::uintmax_t{8}}});
	UAM_ASSERT(durable.value("ok", false));
	UAM_ASSERT(uam::base64::Decode(durable["result"].value("dataBase64", ""), decoded));
	UAM_ASSERT_EQ(decoded, std::string("durable\n"));
	const nlohmann::json replay = request(
	    "poll-durable-replay", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                            {"direction", "remoteToDesktop"},
	                            {"acknowledgedOutput", true},
	                            {"cursor", std::uintmax_t{8}}});
	std::string replayed;
	UAM_ASSERT(uam::base64::Decode(replay["result"].value("dataBase64", ""), replayed));
	UAM_ASSERT_EQ(replayed, decoded);
	UAM_ASSERT(request("ack-durable", {{"type", "channel.ack"},
	                                   {"channelId", "control-1"},
	                                   {"direction", "remoteToDesktop"},
	                                   {"cursor", durable["result"]["cursor"]}})
	               .value("ok", false));
	const nlohmann::json after_ack = request(
	    "poll-after-ack", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                       {"direction", "remoteToDesktop"},
	                       {"acknowledgedOutput", true},
	                       {"cursor", durable["result"]["cursor"]}});
	UAM_ASSERT_EQ(after_ack["result"].value("dataBase64", ""), std::string(""));

	const nlohmann::json bad_direction = request(
	    "bad-direction", {{"type", "channel.poll"}, {"channelId", "control-1"},
	                      {"direction", "sideways"}});
	UAM_ASSERT(!bad_direction.value("ok", true));
	const nlohmann::json bad_base64 = request(
	    "bad-base64", {{"type", "channel.write"}, {"channelId", "control-1"},
	                   {"direction", "remoteToDesktop"}, {"dataBase64", "%%%"}});
	UAM_ASSERT(!bad_base64.value("ok", true));
	UAM_ASSERT_EQ(bad_base64["error"].value("code", ""), std::string("invalid_request"));
	UAM_ASSERT(request("close", {{"type", "channel.close"}, {"channelId", "control-1"}})
	               .value("ok", false));
}

UAM_TEST(RemoteRunnerProcessInputSequenceIsIdempotent)
{
#if defined(__APPLE__)
	uam::remote::RunnerState state;
	const auto request = [&](std::string id, nlohmann::json body)
	{
		body["id"] = std::move(id);
		body["controlToken"] = kProcessControlToken;
		return uam::remote::HandleRunnerRequest(body, "test-version", &state);
	};
	UAM_ASSERT(request("start", {{"type", "process.start"}, {"sessionId", "input-seq"},
	                             {"cwd", fs::temp_directory_path().string()},
	                             {"argv", nlohmann::json::array({"/bin/cat"})}})
	               .value("ok", false));
	const nlohmann::json write = {{"type", "process.write"}, {"sessionId", "input-seq"},
	                              {"inputSequence", std::uint64_t{1}},
	                              {"dataBase64", uam::base64::Encode("once\n")}};
	UAM_ASSERT(request("write", write).value("ok", false));
	UAM_ASSERT(request("write-retry", write)["result"].value("duplicate", false));
	UAM_ASSERT(request("close", {{"type", "process.closeInput"},
	                             {"sessionId", "input-seq"}}).value("ok", false));
	std::string output;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		const nlohmann::json poll = request(
		    "poll-" + std::to_string(attempt),
		    {{"type", "process.poll"}, {"sessionId", "input-seq"}});
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(poll["result"].value("stdoutBase64", ""), chunk));
		output += chunk;
		if (!poll["result"].value("running", true)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(output, std::string("once\n"));
	UAM_ASSERT(request("remove", {{"type", "process.remove"},
	                              {"sessionId", "input-seq"}}).value("ok", false));
#endif
}

#if defined(_WIN32)
UAM_TEST(WindowsRemoteRunnerServiceSupportsReconnectConcurrentChatsAndCleanShutdown)
{
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner.exe";
	UAM_ASSERT(fs::exists(runner));
	auto& service = PlatformServicesFactory::Instance().process_service;
	const auto run_runner = [&](const std::string& command)
	{
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		UAM_ASSERT(service.StartStdioProcess(process, fs::temp_directory_path(),
		                                     {runner.string(), command}, &error));
		service.CloseStdioProcessInput(process);
		int exit_code = -1;
		for (int attempt = 0; attempt < 500 &&
		     !service.PollStdioProcessExited(process, &exit_code); ++attempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		service.CloseStdioProcessHandles(process);
		UAM_ASSERT_EQ(exit_code, 0);
	};
	run_runner("stop");
	uam::platform::StdioProcessPlatformFields runner_service;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(runner_service, fs::temp_directory_path(),
	                                     {runner.string(), "serve"}, &error));
	service.CloseStdioProcessInput(runner_service);
	run_runner("start");
	struct StopGuard
	{
		const fs::path& runner;
		uam::platform::StdioProcessPlatformFields& runner_service;
		~StopGuard()
		{
			uam::platform::StdioProcessPlatformFields process;
			std::string ignored;
			auto& service = PlatformServicesFactory::Instance().process_service;
			if (service.StartStdioProcess(process, fs::temp_directory_path(),
			                              {runner.string(), "stop"}, &ignored))
			{
				service.CloseStdioProcessInput(process);
				int exit_code = -1;
				for (int attempt = 0; attempt < 500 &&
				     !service.PollStdioProcessExited(process, &exit_code); ++attempt)
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				service.CloseStdioProcessHandles(process);
			}
			service.CloseStdioProcessHandles(runner_service);
		}
	} stop_guard{runner, runner_service};

	uam::remote::RunnerClient first(service, {runner.string(), "bridge"});
	uam::remote::RunnerClient second(service, {runner.string(), "bridge"});
	UAM_ASSERT(first.Connect(&error));
	UAM_ASSERT(second.Connect(&error));
	const std::vector<std::string> waiting_command = {
	    "cmd.exe", "/d", "/v:on", "/s", "/c",
	    "set /p value=& <nul set /p =!value!"};
	UAM_ASSERT(first.StartProcess(
	    "windows-chat-a", fs::temp_directory_path(), waiting_command, {}, &error, false,
	    "windows-chat-a-token"));
	UAM_ASSERT(second.StartProcess(
	    "windows-chat-b", fs::temp_directory_path(),
	    {"cmd.exe", "/d", "/s", "/c", "<nul set /p =concurrent"}, {}, &error));
	auto poll_until_exit = [&](const std::string& session_id)
	{
		std::string output;
		bool exited = false;
		for (int attempt = 0; attempt < 500 && !exited; ++attempt)
		{
			uam::remote::ProcessPollResult poll;
			UAM_ASSERT(second.PollProcess(session_id, poll, &error));
			output += poll.standard_output;
			UAM_ASSERT(second.AcknowledgeProcessOutput(session_id, poll, &error));
			exited = !poll.running;
			if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		UAM_ASSERT(exited);
		return output;
	};
	UAM_ASSERT_EQ(poll_until_exit("windows-chat-b"), std::string("concurrent"));
	UAM_ASSERT(second.RemoveProcess("windows-chat-b", &error));
	first.Disconnect();
	UAM_ASSERT(second.StartProcess(
	    "windows-chat-a", fs::temp_directory_path(), waiting_command, {}, &error, true,
	    "windows-chat-a-token"));
	uam::remote::ProcessPollResult attached;
	UAM_ASSERT(second.PollProcess("windows-chat-a", attached, &error));
	UAM_ASSERT(attached.running);
	UAM_ASSERT(second.WriteProcess("windows-chat-a", "reconnected\r\n", &error));
	UAM_ASSERT(second.CloseProcessInput("windows-chat-a", &error));
	UAM_ASSERT_EQ(poll_until_exit("windows-chat-a"), std::string("reconnected"));
	UAM_ASSERT(second.RemoveProcess("windows-chat-a", &error));
	run_runner("stop");
	int runner_service_exit = -1;
	for (int attempt = 0; attempt < 500 &&
	     !service.PollStdioProcessExited(runner_service, &runner_service_exit); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT_EQ(runner_service_exit, 0);
	second.Disconnect();
}
#endif

#if defined(__APPLE__)
UAM_TEST(RemoteGitInitialCommitDoesNotRequireAnExistingIndexFile)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-git0");
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	const fs::path socket = temp.root / "r.sock";
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields runner_service;
	std::string error;
	UAM_ASSERT(process_service.StartStdioProcess(
	    runner_service, temp.root,
	    {runner.string(), "serve", "--socket", socket.string()}, &error));
	struct ServiceGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ServiceGuard()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	} service_guard{runner_service};
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));

	const fs::path fake_ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    fake_ssh,
	    "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge --socket \"$UAM_TEST_SOCKET\"\n"));
	fs::permissions(fake_ssh, fs::perms::owner_read | fs::perms::owner_write |
	                              fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	ScopedEnvVar scoped_path("PATH", temp.root.string() + ":" + inherited_path);
	ScopedEnvVar scoped_runner("UAM_TEST_RUNNER", runner.string());
	ScopedEnvVar scoped_socket("UAM_TEST_SOCKET", socket.string());

	const fs::path repository = temp.root / "repository";
	fs::create_directories(repository);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repository.string())));
	UAM_ASSERT(RunGitForTest(repository, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repository, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repository / "first.txt", "first\n"));
	UAM_ASSERT(!fs::exists(repository / ".git" / "index"));

	uam::AppState app;
	ExecutionHost host;
	host.id = "remote-initial-git";
	host.label = "Remote initial Git";
	host.ssh_alias = "test-host";
	host.platform = "linux";
	host.architecture = "arm64";
	host.runner_version = std::string(uam::constants::kAppVersion).substr(1);
	host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	host.runner_status = "ready";
	app.settings.execution_hosts.push_back(host);
	ChatSession chat;
	chat.id = "remote-initial-git-chat";
	chat.execution_host_id = host.id;
	chat.workspace_directory = repository.string();
	const uam::VcsCommitResult committed = uam::VcsCommitService().Commit(
	    app, chat, uam::VcsType::Git, "Initial remote commit", {"first.txt"});
	if (!committed.ok) throw TestFailure(committed.error);
	UAM_ASSERT(fs::exists(repository / ".git" / "index"));
	UAM_ASSERT(RunGitForTest(repository, "log --oneline --grep 'Initial remote commit'"));
}

UAM_TEST(RemoteRunnerProxyUsesTheConfiguredVersionAndAttachOnlyNeverStartsOrDuplicates)
{
	TempDir temp("uam-runner-proxy-reattach");
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	const fs::path socket = temp.root / "runner.sock";
	const std::string configured_version =
	    std::string(uam::constants::kAppVersion).substr(1);
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields runner_service;
	std::string error;
	UAM_ASSERT(process_service.StartStdioProcess(
	    runner_service, temp.root,
	    {runner.string(), "serve", "--socket", socket.string()}, &error));
	struct ServiceGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ServiceGuard()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	} service_guard{runner_service};
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));

	const fs::path fake_ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    fake_ssh,
	    "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge --socket \"$UAM_TEST_SOCKET\"\n"));
	fs::permissions(fake_ssh, fs::perms::owner_read | fs::perms::owner_write |
	                              fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	const std::vector<std::pair<std::string, std::string>> proxy_environment = {
	    {"PATH", temp.root.string() + ":" + inherited_path},
	    {"UAM_TEST_RUNNER", runner.string()}, {"UAM_TEST_SOCKET", socket.string()}};
	const auto run_proxy = [&](const std::string& spec, std::string& output)
	{
		uam::platform::StdioProcessPlatformFields proxy;
		std::string error;
		auto environment = proxy_environment;
		environment.emplace_back(uam::remote::kRemoteProcessSpecEnvironment, spec);
		UAM_ASSERT(process_service.StartStdioProcess(
		    proxy, temp.root,
		    {runner.string(), "proxy", "--alias", "test-host", "--platform", "linux",
		     "--version", configured_version},
		    &error, environment));
		std::array<char, 256> buffer{};
		int exit_code = -1;
		bool source_exit_acknowledged = false;
		for (int attempt = 0; attempt < 500; ++attempt)
		{
			const std::ptrdiff_t read = process_service.ReadStdioProcessStdout(
			    proxy, buffer.data(), buffer.size(), &error);
			if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
			for (std::size_t marker = output.find(uam::remote::kRemoteOutputMarkerPrefix);
			     marker != std::string::npos;
			     marker = output.find(uam::remote::kRemoteOutputMarkerPrefix))
			{
				const std::size_t end = output.find('\n', marker);
				if (end == std::string::npos) break;
				const std::string ack = std::string(uam::remote::kRemoteOutputAckPrefix) +
				    output.substr(marker + uam::remote::kRemoteOutputMarkerPrefix.size(),
				                  end - marker - uam::remote::kRemoteOutputMarkerPrefix.size()) + "\n";
				UAM_ASSERT(process_service.WriteToStdioProcess(
				    proxy, ack.data(), ack.size(), &error));
				output.erase(marker, end - marker + 1);
			}
			const std::size_t source_exit = output.find(uam::remote::kRemoteSourceExitPrefix);
			if (!source_exit_acknowledged && source_exit != std::string::npos)
			{
				const std::size_t end = output.find('\n', source_exit);
				if (end != std::string::npos)
				{
					const std::string ack = std::string(uam::remote::kRemoteSourceExitAckPrefix) +
					    output.substr(source_exit + uam::remote::kRemoteSourceExitPrefix.size(),
					                  end - source_exit - uam::remote::kRemoteSourceExitPrefix.size()) + "\n";
					UAM_ASSERT(process_service.WriteToStdioProcess(
					    proxy, ack.data(), ack.size(), &error));
					source_exit_acknowledged = true;
				}
			}
			if (process_service.PollStdioProcessExited(proxy, &exit_code)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		process_service.CloseStdioProcessHandles(proxy);
		return exit_code;
	};

	const fs::path missing_marker = temp.root / "missing-started";
	std::string output;
	UAM_ASSERT_EQ(run_proxy(uam::remote::BuildProcessProxySpec(
	                            "missing-session", temp.root,
	                            {"/usr/bin/touch", missing_marker.string()}, {}, true),
	                        output),
	              70);
	UAM_ASSERT(!fs::exists(missing_marker));

	const fs::path starts = temp.root / "starts";
	const std::vector<std::string> command = {
	    "/bin/sh", "-c",
	    "printf x >> '" + starts.string() + "'; cat >/dev/null; printf recovered"};
	uam::remote::RunnerClient client(
	    process_service, {runner.string(), "bridge", "--socket", socket.string()},
	    configured_version);
	UAM_ASSERT(client.Connect(&error));
	const std::string existing_token = "existing-process-token";
	UAM_ASSERT(client.StartProcess("existing-session", temp.root, command, {}, &error,
	                               false, existing_token));
	for (int attempt = 0; attempt < 100 && !fs::exists(starts); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(starts));
	UAM_ASSERT(client.CloseProcessInput("existing-session", &error));
	client.Disconnect();

	output.clear();
	UAM_ASSERT_EQ(run_proxy(uam::remote::BuildProcessProxySpec(
	                            "existing-session", temp.root, command, {}, false,
	                            existing_token),
	                        output),
	              70);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(starts), std::string("x"));

	output.clear();
	UAM_ASSERT_EQ(run_proxy(uam::remote::BuildProcessProxySpec(
	                            "existing-session", temp.root, command, {}, true,
	                            existing_token),
	                        output),
	              0);
	UAM_ASSERT(output.starts_with(
	    std::string(R"({"jsonrpc":"2.0","method":"uam/remoteAttached"})") +
	    "\nrecovered"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(starts), std::string("x"));

	const std::vector<std::string> rewind_command = {
	    "/bin/sh", "-c", "printf 'batch-one\\nbatch-two\\n'"};
	UAM_ASSERT(client.Connect(&error));
	const std::string rewind_token = "rewind-process-token";
	UAM_ASSERT(client.StartProcess(
	    "rewind-session", temp.root, rewind_command, {}, &error, false, rewind_token));
	uam::remote::ProcessPollResult committed;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		UAM_ASSERT(client.PollProcess("rewind-session", committed, &error));
		if (!committed.standard_output.empty() && !committed.running) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(committed.standard_output, std::string("batch-one\nbatch-two\n"));
	UAM_ASSERT(client.AcknowledgeProcessOutput("rewind-session", committed, &error));
	uam::remote::ProcessPollResult stale_ack;
	stale_ack.stdout_cursor = std::string("batch-one\n").size();
	stale_ack.acknowledgement_required = true;
	UAM_ASSERT(client.AcknowledgeProcessOutput("rewind-session", stale_ack, &error));
	client.Disconnect();

	output.clear();
	UAM_ASSERT_EQ(run_proxy(uam::remote::BuildProcessProxySpec(
	                            "rewind-session", temp.root, rewind_command, {}, true, rewind_token,
	                            std::string("batch-one\n").size(), 0),
	                        output),
	              0);
	const std::string expected_replayed_output = "batch-two\n";
	if (output.find(expected_replayed_output) == std::string::npos ||
	    output.find(std::string(uam::remote::kRemoteSourceExitPrefix) +
	                rewind_token + " 0\n") == std::string::npos)
		throw TestFailure("Unexpected replayed proxy output: " + nlohmann::json(output).dump());

	const std::vector<std::string> source_exit_command = {"/bin/sh", "-c", "exit 70"};
	UAM_ASSERT(client.Connect(&error));
	UAM_ASSERT(client.StartProcess(
	    "source-exit-session", temp.root, source_exit_command, {}, &error, false,
	    "source-exit-token"));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	output.clear();
	const std::string source_exit_token = "source-exit-token";
	UAM_ASSERT_EQ(run_proxy(uam::remote::BuildProcessProxySpec(
	                            "source-exit-session", temp.root,
	                            source_exit_command, {}, true, source_exit_token),
	                        output),
	              70);
	UAM_ASSERT(output.find(R"({"jsonrpc":"2.0","method":"uam/remoteAttached"})") !=
	           std::string::npos);
	UAM_ASSERT(output.find(std::string(uam::remote::kRemoteSourceExitPrefix) +
	                       source_exit_token + " 70\n") != std::string::npos);
}

UAM_TEST(RemoteRunnerProxyRelaysRealProviderStdioWithoutBlockingTheAppTransport)
{
	TempDir temp("uam-runner-proxy");
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	const fs::path socket = temp.root / "runner.sock";
	auto& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields runner_service;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(
	    runner_service, temp.root,
	    {runner.string(), "serve", "--socket", socket.string()}, &error));
	struct ServiceGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ServiceGuard()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	} service_guard{runner_service};
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));

	const fs::path fake_ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    fake_ssh,
	    "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge --socket \"$UAM_TEST_SOCKET\"\n"));
	fs::permissions(fake_ssh, fs::perms::owner_read | fs::perms::owner_write |
	                              fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	const std::string runner_version = std::string(uam::constants::kAppVersion).substr(1);
	const std::string delivery_token = "test-delivery-token";
	const std::string spec = uam::remote::BuildProcessProxySpec(
	    "acp-proxy-test", temp.root,
	    {"/bin/sh", "-c", "while IFS= read -r line; do printf 'reply:%s\\n' \"$line\"; done"},
	    {}, false, delivery_token);
	uam::platform::StdioProcessPlatformFields proxy;
	UAM_ASSERT(service.StartStdioProcess(
	    proxy, temp.root, {runner.string(), "proxy", "--alias", "test-host",
	                       "--platform", "linux", "--version", runner_version}, &error,
	    {{uam::remote::kRemoteProcessSpecEnvironment, spec},
	     {"PATH", temp.root.string() + ":" + inherited_path},
	     {"UAM_TEST_RUNNER", runner.string()}, {"UAM_TEST_SOCKET", socket.string()}}));

	std::string output;
	std::array<char, 256> buffer{};
	const auto acknowledge_output = [&]
	{
		const std::string marker = std::string(uam::remote::kRemoteOutputMarkerPrefix) +
		                           delivery_token + " ";
		for (std::size_t start = output.find(marker); start != std::string::npos;
		     start = output.find(marker))
		{
			const std::size_t end = output.find('\n', start);
			if (end == std::string::npos) break;
			const std::string ack = std::string(uam::remote::kRemoteOutputAckPrefix) +
			                        delivery_token + " " +
			                        output.substr(start + marker.size(), end - start - marker.size()) +
			                        "\n";
			UAM_ASSERT(service.WriteToStdioProcess(proxy, ack.data(), ack.size(), &error));
			output.erase(start, end - start + 1);
		}
	};
	for (int message = 0; message < 16; ++message)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(message));
		const std::string input = "line-" + std::to_string(message) + "\n";
		UAM_ASSERT(service.WriteToStdioProcess(proxy, input.data(), input.size(), &error));
		const std::string expected = "reply:line-" + std::to_string(message) + "\n";
		for (int attempt = 0; attempt < 300 && output.find(expected) == std::string::npos;
		     ++attempt)
		{
			const std::ptrdiff_t read = service.ReadStdioProcessStdout(
			    proxy, buffer.data(), buffer.size(), &error);
			if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
			acknowledge_output();
			if (output.find(expected) == std::string::npos)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (output.find(expected) == std::string::npos)
		{
			std::string diagnostics;
			const std::ptrdiff_t diagnostic_read = service.ReadStdioProcessStderr(
			    proxy, buffer.data(), buffer.size(), &error);
			if (diagnostic_read > 0)
				diagnostics.assign(buffer.data(), static_cast<std::size_t>(diagnostic_read));
			int early_exit_code = -1;
			const bool exited = service.PollStdioProcessExited(proxy, &early_exit_code);
			throw TestFailure("Missing relayed response " + expected + " after: " + output +
			                  " stderr: " + diagnostics + " exited=" +
			                  (exited ? std::to_string(early_exit_code) : "false"));
		}
	}
	service.CloseStdioProcessInput(proxy);

	int exit_code = -1;
	for (int attempt = 0; attempt < 500; ++attempt)
	{
		const std::ptrdiff_t read = service.ReadStdioProcessStdout(
		    proxy, buffer.data(), buffer.size(), &error);
		if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
		acknowledge_output();
		if (service.PollStdioProcessExited(proxy, &exit_code)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	service.CloseStdioProcessHandles(proxy);
	UAM_ASSERT_EQ(exit_code, 0);
	for (int message = 0; message < 16; ++message)
		UAM_ASSERT(output.find("reply:line-" + std::to_string(message) + "\n") !=
		           std::string::npos);

	// Closing the local proxy transport is a detach, not authority to close the
	// helper-owned provider stdin. A fresh bridge must reach the same process.
	uam::remote::RunnerClient reattached(
	    service, {runner.string(), "bridge", "--socket", socket.string()}, runner_version);
	reattached.SetProcessControlToken("acp-proxy-test", delivery_token);
	UAM_ASSERT(reattached.Connect(&error));
	uam::remote::ProcessPollResult still_running;
	UAM_ASSERT(reattached.PollProcess("acp-proxy-test", still_running, &error));
	UAM_ASSERT(still_running.running);
	UAM_ASSERT(reattached.WriteProcess("acp-proxy-test", "after-reconnect\n", &error));
	UAM_ASSERT(reattached.CloseProcessInput("acp-proxy-test", &error));
	std::string recovered_output;
	for (int attempt = 0; attempt < 300; ++attempt)
	{
		uam::remote::ProcessPollResult polled;
		UAM_ASSERT(reattached.PollProcess("acp-proxy-test", polled, &error));
		recovered_output += polled.standard_output;
		UAM_ASSERT(reattached.AcknowledgeProcessOutput("acp-proxy-test", polled, &error));
		if (!polled.running) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(recovered_output.find("reply:after-reconnect\n") != std::string::npos);
	UAM_ASSERT(reattached.RemoveProcess("acp-proxy-test", &error));
}

UAM_TEST(RemoteRunnerProxyStopsAndRemovesTheProviderOnlyOnTheExplicitControlLine)
{
	TempDir temp("uam-runner-proxy-stop");
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	const fs::path fake_ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    fake_ssh, "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	fs::permissions(fake_ssh, fs::perms::owner_read | fs::perms::owner_write |
	                              fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	const std::string runner_version = std::string(uam::constants::kAppVersion).substr(1);
	const std::string spec = uam::remote::BuildProcessProxySpec(
	    "acp-proxy-stop", temp.root,
	    {"/bin/sh", "-c", "IFS= read -r line; printf 'unexpected:%s\\n' \"$line\"; sleep 10"},
	    {}, false, "proxy-stop-token");
	uam::platform::StdioProcessPlatformFields proxy;
	std::string error;
	auto& service = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(service.StartStdioProcess(
	    proxy, temp.root, {runner.string(), "proxy", "--alias", "test-host",
	                       "--platform", "linux", "--version", runner_version}, &error,
	    {{uam::remote::kRemoteProcessSpecEnvironment, spec},
	     {"PATH", temp.root.string() + ":" + inherited_path},
	     {"UAM_TEST_RUNNER", runner.string()}}));
	UAM_ASSERT(service.WriteToStdioProcess(
	    proxy, uam::remote::kRemoteStopControlLine.data(),
	    uam::remote::kRemoteStopControlLine.size(), &error));
	int exit_code = -1;
	for (int attempt = 0; attempt < 300 &&
	     !service.PollStdioProcessExited(proxy, &exit_code); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	std::array<char, 256> buffer{};
	const std::ptrdiff_t read = service.ReadStdioProcessStdout(
	    proxy, buffer.data(), buffer.size(), &error);
	const std::ptrdiff_t diagnostic_read = service.ReadStdioProcessStderr(
	    proxy, buffer.data(), buffer.size(), &error);
	const std::string diagnostics = diagnostic_read > 0
	    ? std::string(buffer.data(), static_cast<std::size_t>(diagnostic_read)) : std::string{};
	service.CloseStdioProcessHandles(proxy);
	if (exit_code != 0) throw TestFailure("Remote stop proxy exit=" + std::to_string(exit_code) +
	    " stderr=" + diagnostics + " transport=" + error);
	UAM_ASSERT(read <= 0);
}

UAM_TEST(RemoteRunnerMcpShimRelaysTheActualUamControlProtocolOverTheSharedService)
{
	TempDir temp("uam-runner-mcp-relay");
	const fs::path runner =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()
	        .parent_path() / "uam-runner";
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "remote-control-relay";
	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.uam_control_enabled = true;
	app.chats.push_back(std::move(chat));
	auto acp_session = std::make_unique<uam::AcpSessionState>();
	acp_session->chat_id = app.chats.front().id;
	acp_session->provider_id = app.chats.front().provider_id;
	acp_session->running = true;
	uam::AcpSessionState* raw_session = acp_session.get();
	app.acp_sessions.push_back(std::move(acp_session));
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	nlohmann::json setup{{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(
	    app, *raw_session, app.chats.front(), "session/new", setup, &error));
	const fs::path in_progress_request =
	    app.uam_control_capabilities.front().directory / "requests" / "pending.json.tmp.1";
	UAM_ASSERT(uam::io::WriteTextFile(in_progress_request, "partial"));
	UAM_ASSERT(!uam::UamControlService::ProcessPendingRequests(app));
	UAM_ASSERT(fs::exists(in_progress_request));
	uam::paths::RemoveFileNoThrow(in_progress_request);
	const nlohmann::json& local_server = setup["params"]["mcpServers"][0];
	std::vector<std::string> local_argv = {local_server.value("command", "")};
	local_argv.push_back("--uam-test-control-mcp");
	std::vector<std::pair<std::string, std::string>> local_environment;
	for (const nlohmann::json& entry : local_server["env"])
		local_environment.emplace_back(entry.value("name", ""), entry.value("value", ""));
	const fs::path socket = temp.root / "runner.sock";
	uam::platform::StdioProcessPlatformFields service_process;
	auto& service = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(service.StartStdioProcess(
	    service_process, temp.root,
	    {runner.string(), "serve", "--socket", socket.string()}, &error));
	struct ServiceGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ServiceGuard()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(process);
		}
	} guard{service_process};
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));

	const fs::path fake_ssh = temp.root / "ssh";
	UAM_ASSERT(uam::io::WriteTextFile(
	    fake_ssh, "#!/bin/sh\nexec \"$UAM_TEST_RUNNER\" bridge --socket \"$UAM_TEST_SOCKET\"\n"));
	fs::permissions(fake_ssh, fs::perms::owner_read | fs::perms::owner_write |
	                              fs::perms::owner_exec);
	const std::string inherited_path =
	    uam::env::GetNonEmptyString("PATH").value_or("/usr/bin:/bin");
	const std::string runner_version = std::string(uam::constants::kAppVersion).substr(1);
	const std::string process_spec = uam::remote::BuildProcessProxySpec(
	    "mcp-provider", temp.root, {"/bin/sh", "-c", "sleep 30"}, {}, false,
	    "mcp-provider-token");
	uam::platform::StdioProcessPlatformFields proxy;
	UAM_ASSERT(service.StartStdioProcess(
	    proxy, temp.root, {runner.string(), "proxy", "--alias", "test-host",
	                       "--platform", "linux", "--version", runner_version}, &error,
	    {{uam::remote::kRemoteProcessSpecEnvironment, process_spec},
	     {"PATH", temp.root.string() + ":" + inherited_path},
	     {"UAM_TEST_RUNNER", runner.string()}, {"UAM_TEST_SOCKET", socket.string()}}));
	const std::string channel_id = "control-relay-test";
	const std::string control_line = uam::remote::BuildRemoteMcpControlLine(
	    channel_id, temp.root, local_argv, local_environment);
	UAM_ASSERT(service.WriteToStdioProcess(proxy, control_line.data(), control_line.size(), &error));

	uam::platform::StdioProcessPlatformFields shim;
	UAM_ASSERT(service.StartStdioProcess(
	    shim, temp.root,
	    {runner.string(), "mcp", "--channel", channel_id, "--socket", socket.string()},
	    &error));
	const std::string initialize =
	    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})" "\n";
	UAM_ASSERT(service.WriteToStdioProcess(
	    shim, initialize.data(), initialize.size(), &error));
	std::string output;
	std::array<char, 256> buffer{};
	for (int attempt = 0; attempt < 500 && output.find("uam-control") == std::string::npos;
	     ++attempt)
	{
		const std::ptrdiff_t read = service.ReadStdioProcessStdout(
		    shim, buffer.data(), buffer.size(), &error);
		if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
		if (output.find("uam-control") == std::string::npos)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(output.find("uam-control") != std::string::npos);
	const std::string goal_get =
	    R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"goal_get","arguments":{}}})" "\n";
	UAM_ASSERT(service.WriteToStdioProcess(shim, goal_get.data(), goal_get.size(), &error));
	const auto has_goal_response = [&output]
	{
		const std::size_t response = output.find("\"id\":2");
		return response != std::string::npos &&
		       output.find("\"isError\":false", response) != std::string::npos;
	};
	for (int attempt = 0; attempt < 1200 && !has_goal_response(); ++attempt)
	{
		(void)uam::UamControlService::ProcessPendingRequests(app);
		const std::ptrdiff_t read = service.ReadStdioProcessStdout(
		    shim, buffer.data(), buffer.size(), &error);
		if (read > 0) output.append(buffer.data(), static_cast<std::size_t>(read));
		if (!has_goal_response()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(has_goal_response());
	// A relay must drain the child's final response after observing its exit.
	const std::string final_response =
	    "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":\"" + std::string(128 * 1024 + 7, 'x') + "\"}\n";
	const fs::path final_response_path = temp.root / "final-response.json";
	UAM_ASSERT(uam::io::WriteTextFile(final_response_path, final_response));
	const std::string final_control_line = uam::remote::BuildRemoteMcpControlLine(
	    channel_id, temp.root, {"/bin/sh", "-c", "exec cat \"$1\"", "mcp-final-response",
	                           final_response_path.string()}, {});
	output.clear();
	UAM_ASSERT(service.WriteToStdioProcess(
	    proxy, final_control_line.data(), final_control_line.size(), &error));
	std::array<char, 16 * 1024> final_buffer{};
	for (int attempt = 0; attempt < 500 && output.find(final_response) == std::string::npos;
	     ++attempt)
	{
		const std::ptrdiff_t read = service.ReadStdioProcessStdout(
		    shim, final_buffer.data(), final_buffer.size(), &error);
		if (read > 0) output.append(final_buffer.data(), static_cast<std::size_t>(read));
		if (output.find(final_response) == std::string::npos)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(output.find(final_response) != std::string::npos);
	// Pipe backpressure must retain channel input until the asynchronous writer drains.
	const fs::path input_release = temp.root / "release-input";
	const fs::path input_ready = temp.root / "input-ready";
	const fs::path received_input = temp.root / "received-input";
	const std::string blocked_input(512 * 1024, 'q');
	const std::string blocked_control_line = uam::remote::BuildRemoteMcpControlLine(
	    channel_id, temp.root,
	    {"/bin/sh", "-c",
	     ": > \"$3\"; while [ ! -f \"$1\" ]; do sleep 0.01; done; exec cat > \"$2\"",
	     "mcp-blocked-input", input_release.string(), received_input.string(),
	     input_ready.string()}, {});
	UAM_ASSERT(service.WriteToStdioProcess(
	    proxy, blocked_control_line.data(), blocked_control_line.size(), &error));
	for (int attempt = 0; attempt < 500 && !fs::exists(input_ready); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(input_ready));
	uam::remote::RunnerClient observer(
	    service, {runner.string(), "bridge", "--socket", socket.string()}, runner_version);
	UAM_ASSERT(observer.OpenChannel(channel_id, &error, true));
	std::string retained_input;
	std::uintmax_t initial_input_cursor = 0;
	UAM_ASSERT(observer.PollChannel(
	    channel_id, "remoteToDesktop", retained_input, &error, &initial_input_cursor));
	UAM_ASSERT(retained_input.empty());
	UAM_ASSERT(observer.WriteChannel(channel_id, "remoteToDesktop",
	    std::string_view(blocked_input).substr(0, 256 * 1024), &error));
	UAM_ASSERT(observer.WriteChannel(channel_id, "remoteToDesktop",
	    std::string_view(blocked_input).substr(256 * 1024), &error));
	std::uintmax_t observed_input_cursor = initial_input_cursor;
	bool acknowledged_while_blocked = false;
	const std::chrono::steady_clock::time_point blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (std::chrono::steady_clock::now() < blocked_deadline && !acknowledged_while_blocked)
	{
		UAM_ASSERT(observer.OpenChannel(channel_id, &error, true));
		// The relay may advance the acknowledged cursor between open and poll.
		if (!observer.PollChannel(
		        channel_id, "remoteToDesktop", retained_input, &error, &observed_input_cursor))
			continue;
		acknowledged_while_blocked = retained_input.empty() &&
		    observed_input_cursor == initial_input_cursor + blocked_input.size();
		if (!acknowledged_while_blocked)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool retained_while_blocked = !retained_input.empty();
	UAM_ASSERT(uam::io::WriteTextFile(input_release, "release"));
	bool input_delivered_and_acknowledged = false;
	const std::chrono::steady_clock::time_point delivered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < delivered_deadline && !input_delivered_and_acknowledged)
	{
		UAM_ASSERT(observer.OpenChannel(channel_id, &error, true));
		if (!observer.PollChannel(
		        channel_id, "remoteToDesktop", retained_input, &error, &observed_input_cursor))
			continue;
		input_delivered_and_acknowledged = retained_input.empty() &&
		    observed_input_cursor == initial_input_cursor + blocked_input.size() &&
		    ReadFile(received_input) == blocked_input;
		if (!input_delivered_and_acknowledged)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(!acknowledged_while_blocked);
	UAM_ASSERT(retained_while_blocked);
	UAM_ASSERT(input_delivered_and_acknowledged);
	UAM_ASSERT(service.WriteToStdioProcess(
	    proxy, uam::remote::kRemoteStopControlLine.data(),
	    uam::remote::kRemoteStopControlLine.size(), &error));
	service.CloseStdioProcessInput(shim);
	service.StopStdioProcess(shim, true);
	service.CloseStdioProcessHandles(shim);
	service.StopStdioProcess(proxy, true);
	service.CloseStdioProcessHandles(proxy);
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(RemoteRunnerServiceSupportsConcurrentChatsAndBridgeReconnects)
{
	TempDir temp("uam-runner-service");
	const fs::path test_executable =
	    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	const fs::path runner = test_executable.parent_path() / "uam-runner";
	const fs::path socket = temp.root / "runner.sock";
	uam::platform::StdioProcessPlatformFields service;
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    service, temp.root, {runner.string(), "serve", "--socket", socket.string()}, &error));
	struct ServiceGuard
	{
		uam::platform::StdioProcessPlatformFields& process;
		~ServiceGuard()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(process, true);
		}
	} guard{service};
	for (int attempt = 0; attempt < 100 && !fs::exists(socket); ++attempt)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(fs::exists(socket));

	uam::remote::RunnerClient first(
	    PlatformServicesFactory::Instance().process_service,
	    {runner.string(), "bridge", "--socket", socket.string()});
	UAM_ASSERT(first.Connect(&error));
	UAM_ASSERT(first.StartProcess(
	    "persistent-session", temp.root,
	    {"/bin/sh", "-c", "sleep 0.1; /usr/bin/yes x | /usr/bin/head -c 524288; /usr/bin/printf persistent-ok"}, {}, &error,
	    false, "persistent-process-token"));

	uam::remote::RunnerClient simultaneous(
	    PlatformServicesFactory::Instance().process_service,
	    {runner.string(), "bridge", "--socket", socket.string()});
	UAM_ASSERT(simultaneous.Connect(&error));
	UAM_ASSERT(simultaneous.StartProcess(
	    "parallel-session", temp.root, {"/usr/bin/printf", "parallel-ok"}, {}, &error));
	std::string parallel_output;
	bool parallel_exited = false;
	for (int attempt = 0; attempt < 100 && !parallel_exited; ++attempt)
	{
		uam::remote::ProcessPollResult polled;
		UAM_ASSERT(simultaneous.PollProcess("parallel-session", polled, &error));
		parallel_output += polled.standard_output;
		UAM_ASSERT(simultaneous.AcknowledgeProcessOutput("parallel-session", polled,
		                                                  &error));
		parallel_exited = !polled.running;
		if (!parallel_exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(parallel_exited);
	UAM_ASSERT_EQ(parallel_output, std::string("parallel-ok"));
	UAM_ASSERT(simultaneous.RemoveProcess("parallel-session", &error));
	first.Disconnect();
	simultaneous.Disconnect();
	UAM_ASSERT_EQ(uam::remote::StopRunnerService(socket), 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(350));

	uam::remote::RunnerClient second(
	    PlatformServicesFactory::Instance().process_service,
	    {runner.string(), "bridge", "--socket", socket.string()});
	second.SetProcessControlToken("persistent-session", "persistent-process-token");
	UAM_ASSERT(second.Connect(&error));
	std::string output;
	bool exited = false;
	for (int attempt = 0; attempt < 100 && !exited; ++attempt)
	{
		uam::remote::ProcessPollResult polled;
		UAM_ASSERT(second.PollProcess("persistent-session", polled, &error));
		output += polled.standard_output;
		UAM_ASSERT(second.AcknowledgeProcessOutput("persistent-session", polled,
		                                            &error));
		exited = !polled.running;
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(output.size(), static_cast<std::size_t>(524288 + 13));
	UAM_ASSERT(output.ends_with("persistent-ok"));
	UAM_ASSERT(second.RemoveProcess("persistent-session", &error));
	second.Disconnect();
	UAM_ASSERT_EQ(uam::remote::StopRunnerService(socket), 0);
}
#endif
