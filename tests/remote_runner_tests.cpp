#include "test_harness.h"
#include "app/uam_control_service.h"
#include "remote/runner_service_posix.h"

using namespace uam_test;

namespace
{
	constexpr std::string_view kProcessControlToken = "test-process-control-token";
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

UAM_TEST(RemoteRunnerProcessProxySpecStaysOffTheCommandLineAndReconnectCanAttach)
{
	const std::string encoded = uam::remote::BuildProcessProxySpec(
	    "acp-chat-1", fs::temp_directory_path(), {"provider", "--stdio"},
	    {{"PATH", "/usr/bin"}}, true, "delivery-token", 123, 45);
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(encoded, decoded));
	const nlohmann::json spec = nlohmann::json::parse(decoded);
	UAM_ASSERT_EQ(spec.value("sessionId", ""), std::string("acp-chat-1"));
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
	     {"cwd", fs::temp_directory_path().string()}, {"argv", arguments}},
	    "test-version", &state);
	UAM_ASSERT(started.value("ok", false));
	const nlohmann::json attached = uam::remote::HandleRunnerRequest(
	    {{"id", "attach"}, {"type", "process.start"}, {"sessionId", "acp-chat-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", fs::temp_directory_path().string()}, {"argv", arguments},
	     {"attachIfExists", true}},
	    "test-version", &state);
	UAM_ASSERT(attached.value("ok", false));
	UAM_ASSERT(attached["result"].value("attached", false));
	const nlohmann::json conflict = uam::remote::HandleRunnerRequest(
	    {{"id", "conflict"}, {"type", "process.start"}, {"sessionId", "acp-chat-1"},
	     {"controlToken", kProcessControlToken},
	     {"cwd", fs::temp_directory_path().string()},
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
	const fs::path cwd = "/tmp";
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
	UAM_ASSERT_EQ(spec.value("cwd", ""), cwd.string());
	UAM_ASSERT_EQ(spec["argv"].get<std::vector<std::string>>(), provider_argv);
	UAM_ASSERT(spec["environment"].empty());

	const std::vector<std::string> windows_ssh =
	    uam::remote::BuildRemoteTerminalSshArgv(
	        "windows-lab", "windows", "4.5.7", fs::path("C:\\Work\\Project"),
	        {"opencode.cmd", "--help"});
	UAM_ASSERT(!windows_ssh.empty());
	UAM_ASSERT(windows_ssh.back().starts_with("powershell.exe "));
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
	UAM_ASSERT(decoded_commands.find("Move-Item -LiteralPath $backup -Destination $installed") !=
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
	                                 {runner.string(), "bridge-direct"});
	std::string error;
	UAM_ASSERT(client.Connect(&error));
#if defined(__APPLE__)
	const std::vector<std::string> arguments = {"/usr/bin/printf", "bridge-ok"};
#elif defined(_WIN32)
	const std::vector<std::string> arguments = {"cmd.exe", "/d", "/s", "/c", "<nul set /p =bridge-ok"};
#endif
	UAM_ASSERT(client.StartProcess("bridge-session", fs::temp_directory_path(), arguments, {},
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
	TempDir upload("uam-runner-upload");
	const fs::path uploaded = upload.root / "nested" / "attachment.txt";
	UAM_ASSERT(client.UploadFile("upload-1", uploaded, "attachment-ok", &error));
	std::string uploaded_bytes;
	UAM_ASSERT(uam::io::TryReadBinaryFile(uploaded, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	const fs::path copied = upload.root / "nested" / "attachment-copy.txt";
	UAM_ASSERT(client.CopyFile("copy-1", uploaded, copied, false, &error));
	UAM_ASSERT(uam::io::TryReadBinaryFile(copied, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	UAM_ASSERT(!client.CopyFile("copy-2", uploaded, copied, false, &error));
	UAM_ASSERT(client.CopyFile("copy-3", uploaded, copied, true, &error));
	UAM_ASSERT(!client.UploadFile("upload-2", uploaded, "must-not-overwrite", &error));
	UAM_ASSERT(uam::io::TryReadBinaryFile(uploaded, uploaded_bytes));
	UAM_ASSERT_EQ(uploaded_bytes, std::string("attachment-ok"));
	uam::remote::DirectoryListing listing;
	UAM_ASSERT(client.ListDirectories(upload.root, listing, &error));
	UAM_ASSERT_EQ(listing.directory, upload.root.string());
	UAM_ASSERT_EQ(listing.directories.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(listing.directories.front().first, std::string("nested"));
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
	service.CloseStdioProcessHandles(proxy);
	UAM_ASSERT_EQ(exit_code, 0);
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
