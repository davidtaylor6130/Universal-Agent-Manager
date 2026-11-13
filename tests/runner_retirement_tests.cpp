#include "remote/runner_bootstrap.h"
#include "remote/runner_client.h"
#include "remote/runner_state.h"
#include "common/utils/base64.h"
#include "test_harness.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <optional>

using namespace uam_test;
namespace uam::remote
{
	struct RunnerStateRetirementTestAccess
	{
		static std::shared_ptr<RunnerState::Process> Add(RunnerState& state, const fs::path& directory, bool transient = true, std::int64_t deadline = 0)
		{
			std::shared_ptr<RunnerState::Process> process = std::make_shared<RunnerState::Process>();
			process->transient_lease = transient;
			process->lease_deadline_ms = deadline;
			process->control_token = "owned-token";
			process->stdout_spool = directory / "stdout";
			process->stderr_spool = directory / "stderr";
			std::scoped_lock lock(state.m_stateMutex);
			state.m_processes.emplace("owned-worker", process);
			state.m_textWorkers.emplace("owned-worker", RunnerState::TextWorker{directory, "codex", 0});
			return process;
		}
		static std::shared_ptr<RunnerState::Process> AddRecord(RunnerState& state, const fs::path& directory)
		{
			std::shared_ptr<RunnerState::Process> process = std::make_shared<RunnerState::Process>();
			process->control_token = "owned-token";
			process->stdout_spool = directory / "stdout";
			process->stderr_spool = directory / "stderr";
			std::scoped_lock lock(state.m_stateMutex);
			state.m_processes.emplace("owned-record", process);
			return process;
		}
		static fs::path SpoolDirectory(RunnerState& state) { return state.m_spoolDirectory; }
		static bool Retiring(RunnerState& state)
		{
			std::scoped_lock lock(state.m_stateMutex);
			return state.m_retiringProcesses.contains("owned-worker");
		}
	};
}
using uam::remote::RunnerState;
using uam::remote::RunnerStateRetirementTestAccess;

UAM_TEST(RemoteExpiredWorkerRetiresOnFirstOwnershipQuery)
{
	TempDir temp("uam-retirement-first");
	RunnerState state;
	const fs::path directory = temp.root / "worker";
	fs::create_directory(directory);
	RunnerStateRetirementTestAccess::Add(state, directory);
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(!fs::exists(directory));
#if defined(__linux__)
	const fs::path deferred_directory = temp.root / "deferred-worker";
	fs::create_directory(deferred_directory);
	const std::shared_ptr<RunnerState::Process> deferred = RunnerStateRetirementTestAccess::Add(state, deferred_directory);
	deferred->fields.deferred_reap_complete = std::make_shared<std::atomic<bool>>(false);
	deferred->exited = true;
	UAM_ASSERT(state.HasManagedProcesses());
	UAM_ASSERT(fs::exists(deferred_directory));
	deferred->fields.deferred_reap_complete->store(true, std::memory_order_release);
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(!fs::exists(deferred_directory));
#endif
}

UAM_TEST(RemoteConcurrentRetirementRemainsOwnedWithoutWaiting)
{
	TempDir temp("uam-retirement-concurrent");
	RunnerState state;
	const fs::path directory = temp.root / "worker";
	fs::create_directory(directory);
	const std::shared_ptr<RunnerState::Process> process = RunnerStateRetirementTestAccess::Add(state, directory);
	std::unique_lock cleanup_hold(process->mutex);
	std::future<bool> first = std::async(std::launch::async, [&] { return state.HasManagedProcesses(); });
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!RunnerStateRetirementTestAccess::Retiring(state) && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
	const bool retiring = RunnerStateRetirementTestAccess::Retiring(state);
	std::future<bool> concurrent = std::async(std::launch::async, [&] { return state.HasManagedProcesses(); });
	const bool immediate = concurrent.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
	const bool worker_preserved = fs::exists(directory);
	const nlohmann::json denied = state.HandleProcessRequest({{"type", "process.poll"}, {"sessionId", "owned-worker"}, {"controlToken", "foreign"}});
	const nlohmann::json owned = state.HandleProcessRequest({{"type", "process.poll"}, {"sessionId", "owned-worker"}, {"controlToken", "owned-token"}});
	const nlohmann::json restart = state.HandleProcessRequest({{"type", "process.start"}, {"sessionId", "owned-worker"}, {"controlToken", "owned-token"}, {"cwd", temp.root.string()}, {"argv", nlohmann::json::array({"private-fixture-never-launched"})}});
	const bool first_immediate = first.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready;
	cleanup_hold.unlock();
	UAM_ASSERT(worker_preserved);
	UAM_ASSERT_EQ(denied["error"].value("code", ""), std::string("unauthorized"));
	UAM_ASSERT_EQ(owned["error"].value("code", ""), std::string("process_retiring"));
	UAM_ASSERT_EQ(restart["error"].value("code", ""), std::string("process_retiring"));
	UAM_ASSERT(retiring);
	UAM_ASSERT(immediate);
	UAM_ASSERT(first_immediate);
	UAM_ASSERT(concurrent.get());
	UAM_ASSERT(first.get());
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(!fs::exists(directory));
}

UAM_TEST(RemoteUnexpiredAndPersistentOwnershipPreservesWorker)
{
	for (bool transient : {false, true})
	{
		TempDir temp("uam-retirement-preserve");
		RunnerState state;
		const fs::path directory = temp.root / "worker";
		fs::create_directory(directory);
		RunnerStateRetirementTestAccess::Add(state, directory, transient, INT64_MAX);
		UAM_ASSERT(state.HasManagedProcesses());
		UAM_ASSERT(fs::exists(directory));
	}
}

UAM_TEST(RemoteExitedRecordStopsHoldingHelperOnceDelivered)
{
	TempDir temp("uam-retirement-exited");
	RunnerState state;
	const std::shared_ptr<RunnerState::Process> process = RunnerStateRetirementTestAccess::AddRecord(state, temp.root);
	UAM_ASSERT(uam::io::WriteTextFile(process->stdout_spool, "hello"));
	UAM_ASSERT(uam::io::WriteTextFile(process->stderr_spool, ""));
	UAM_ASSERT(state.HasManagedProcesses()); // A running process keeps its helper.
	process->exited = true;
	UAM_ASSERT(state.HasManagedProcesses()); // Its exit has not reached a client.
	process->exit_reported = true;
	UAM_ASSERT(state.HasManagedProcesses()); // Its output has not been acknowledged.
	process->stdout_offset = 5;
	UAM_ASSERT(!state.HasManagedProcesses());
	process->stdout_offset = 0;
	process->spool_error = "Remote process output could not be spooled.";
	UAM_ASSERT(!state.HasManagedProcesses()); // Polling can only report this failure, so nothing more is deliverable.
}

UAM_TEST(RemoteDrainedProcessAllowsShutdownWhileLiveProcessBlocksIt)
{
	TempDir temp("uam-retirement-drained");
	RunnerState state;
	const auto request = [](nlohmann::json value)
	{
		value["id"] = "drained";
		value["sessionId"] = "drained-session";
		value["controlToken"] = "drained-token";
		return value;
	};
#if defined(_WIN32)
	const nlohmann::json argv = nlohmann::json::array({"cmd.exe", "/d", "/v:on", "/s", "/c", "set /p value=& <nul set /p =!value!"});
#else
	const nlohmann::json argv = nlohmann::json::array({"/bin/sh", "-c", "IFS= read -r value; printf %s \"$value\""});
#endif
	UAM_ASSERT(state.HandleProcessRequest(request({{"type", "process.start"}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", argv}})).value("ok", false));
	UAM_ASSERT(state.HasManagedProcesses());
	UAM_ASSERT(state.HandleProcessRequest(request({{"type", "process.write"}, {"dataBase64", uam::base64::Encode("done\n")}})).value("ok", false));
	std::string output;
	nlohmann::json poll_request = request({{"type", "process.poll"}, {"acknowledgedOutput", true}});
	nlohmann::json polled;
	bool exited = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!exited && std::chrono::steady_clock::now() < deadline)
	{
		polled = state.HandleProcessRequest(poll_request);
		UAM_ASSERT(polled.value("ok", false));
		const nlohmann::json& result = polled["result"];
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(result.value("stdoutBase64", ""), chunk));
		output += chunk;
		UAM_ASSERT(state.HandleProcessRequest(request({{"type", "process.ack"}, {"stdoutCursor", result["stdoutCursor"]}, {"stderrCursor", result["stderrCursor"]}})).value("ok", false));
		exited = !result.value("running", true);
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(uam::strings::Trim(output), std::string("done"));
	// The exit is owed until a response carrying it was written to the client.
	UAM_ASSERT(state.HasManagedProcesses());
	state.ConfirmResponseDelivered(request({{"type", "process.poll"}}), {{"ok", true}, {"result", {{"running", true}, {"exitCode", nullptr}}}});
	UAM_ASSERT(state.HasManagedProcesses());
	nlohmann::json foreign = poll_request;
	foreign["controlToken"] = "foreign";
	state.ConfirmResponseDelivered(foreign, polled);
	UAM_ASSERT(state.HasManagedProcesses());
	state.ConfirmResponseDelivered(poll_request, polled);
	UAM_ASSERT(!state.HasManagedProcesses());
}

UAM_TEST(RemoteAcknowledgedSpoolIsReclaimedProgressivelyAndReplayStaysCorrect)
{
	TempDir temp("uam-retirement-reclaim");
	RunnerState state;
	const auto request = [&](nlohmann::json value)
	{
		value["id"] = "reclaim";
		value["sessionId"] = "reclaim";
		value["controlToken"] = "reclaim-token";
		return state.HandleProcessRequest(value);
	};
	// 8 bytes per line makes every cursor identifiable; the final read keeps the process running while the spool is checked.
	const std::uintmax_t total = 4000000, acknowledged = 3200000;
	std::string lines;
	lines.reserve(total);
	for (int index = 0; index < 500000; ++index)
	{
		char line[16];
		std::snprintf(line, sizeof(line), "%07d\n", index);
		lines += line;
	}
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "lines.txt", lines));
#if defined(_WIN32)
	const nlohmann::json argv = nlohmann::json::array({"cmd.exe", "/d", "/s", "/c", "type lines.txt & set /p reply="});
#else
	const nlohmann::json argv = nlohmann::json::array({"/bin/sh", "-c", "cat lines.txt; IFS= read -r reply"});
#endif
	UAM_ASSERT(request({{"type", "process.start"}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", argv}}).value("ok", false));
	const auto poll = [&](std::uintmax_t cursor) { return request({{"type", "process.poll"}, {"acknowledgedOutput", true}, {"stdoutCursor", cursor}, {"stderrCursor", 0u}}); };
	std::string read_back;
	const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (read_back.size() < total && std::chrono::steady_clock::now() < read_deadline)
	{
		const nlohmann::json polled = poll(read_back.size());
		UAM_ASSERT(polled.value("ok", false));
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(polled["result"].value("stdoutBase64", ""), chunk));
		read_back += chunk;
		if (chunk.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(read_back.size(), std::size_t(total));
	UAM_ASSERT(read_back == lines);
	const fs::path spool = RunnerStateRetirementTestAccess::SpoolDirectory(state) / "reclaim.stdout";
	UAM_ASSERT_EQ(fs::file_size(spool), total);
	UAM_ASSERT(request({{"type", "process.ack"}, {"stdoutCursor", acknowledged}, {"stderrCursor", 0u}}).value("ok", false));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (fs::file_size(spool) == total && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT_EQ(fs::file_size(spool), total - acknowledged); // Exactly the unacknowledged tail remains (MoveFileExW on Windows).
	const nlohmann::json replay = poll(acknowledged);
	UAM_ASSERT(replay.value("ok", false));
	std::string replayed;
	UAM_ASSERT(uam::base64::Decode(replay["result"].value("stdoutBase64", ""), replayed));
	UAM_ASSERT(replayed.starts_with("0400000\n"));
	UAM_ASSERT(lines.compare(acknowledged, replayed.size(), replayed) == 0);
	UAM_ASSERT_EQ(replay["result"].value("stdoutCursor", std::uintmax_t{0}), acknowledged + replayed.size());
	// A cursor below the reclaimed prefix keeps failing the existing way, never returning other bytes.
	const nlohmann::json stale = poll(acknowledged - 1);
	UAM_ASSERT(!stale.value("ok", false));
	UAM_ASSERT_EQ(stale["error"].value("code", ""), std::string("invalid_request"));
	UAM_ASSERT(request({{"type", "process.stop"}}).value("ok", false));
}

UAM_TEST(RemoteOrphanedSpoolsAreRemovedOnlyWhenTheirOwnerIsGone)
{
	TempDir temp("uam-retirement-orphans");
#if !defined(_WIN32)
	ScopedEnvVar tmpdir("TMPDIR", temp.root.string());
#endif
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	RunnerState live;
	const fs::path live_spool = RunnerStateRetirementTestAccess::SpoolDirectory(live);
	UAM_ASSERT(!live_spool.empty());
	const fs::path parent = live_spool.parent_path();
	const std::string suffix = service.GenerateUuid();
	const fs::path orphan = parent / ("uam-runner-spool-orphan-" + suffix);
	const fs::path legacy = parent / ("uam-runner-spool-legacy-" + suffix);
	const fs::path unrelated = parent / ("uam-runner-other-" + suffix);
	for (const fs::path& directory : {orphan, legacy, unrelated})
	{
		fs::create_directory(directory);
		UAM_ASSERT(uam::io::WriteTextFile(directory / "session.stdout", "kept bytes"));
	}
	UAM_ASSERT(uam::io::WriteTextFile(orphan / ".owner-lock", ""));
	UAM_ASSERT(uam::io::WriteTextFile(unrelated / ".owner-lock", ""));
	RunnerState starting;
	starting.RemoveOrphanedSpools();
	UAM_ASSERT(!fs::exists(orphan)); // Nobody holds its lock.
	UAM_ASSERT(fs::exists(live_spool / ".owner-lock")); // Another helper's held lock protects its spool.
	UAM_ASSERT(fs::exists(legacy / "session.stdout")); // An unmarked spool may belong to a live older helper.
	UAM_ASSERT(fs::exists(unrelated / "session.stdout")); // Only the spool name prefix is in scope.
	UAM_ASSERT(fs::exists(RunnerStateRetirementTestAccess::SpoolDirectory(starting)));
	std::error_code ignored;
	fs::remove_all(legacy, ignored);
	fs::remove_all(unrelated, ignored);
}

UAM_TEST(RemoteRetirementScriptsTerminateOnlyVerifiedIdleHelpersByPath)
{
	const std::string windows = uam::remote::WindowsHelperRetirementScript();
	// A forced stop needs a real refusal, a binary that predates --stop-contract, no other process from that executable,
	// and, per process, an unchanged creation time and no children.
	UAM_ASSERT(windows.find("$refused=$r -and $r.Code -eq 2 -and $r.Err -match 'still owns remote processes'") != std::string::npos);
	UAM_ASSERT(windows.find("-not $stopped -and $refused -and $sameRoot") != std::string::npos);
	UAM_ASSERT(windows.find("Invoke-UamHelper $path '--stop-contract'") != std::string::npos);
	UAM_ASSERT(windows.find("$c.Code -eq 2 -and -not $c.Out -and $c.Err.StartsWith('Usage: uam-runner ')") != std::string::npos);
	UAM_ASSERT(windows.find("Where-Object { $_.CommandLine -notmatch") != std::string::npos);
	const std::size_t creation = windows.find("$fresh.CreationDate -eq $helper.CreationDate -and @(Get-UamChildren $helper.ProcessId).Count -eq 0");
	const std::size_t forced = windows.find("Stop-Process -Id $helper.ProcessId");
	UAM_ASSERT(creation != std::string::npos && forced != std::string::npos && creation < forced);
	UAM_ASSERT(windows.find("Stop-Process -Name") == std::string::npos);
	const std::string spools = uam::remote::WindowsHelperSpoolCleanupScript();
	UAM_ASSERT(spools.find("if (-not $process.ExecutablePath) { $uamKnown=$false }") != std::string::npos);
	UAM_ASSERT(spools.find("[IO.File]::Open($lock,'Open','ReadWrite','None').Dispose() } catch { continue }") != std::string::npos);

	const std::string linux_script = uam::remote::LinuxHelperRetirementScript();
	UAM_ASSERT(linux_script.find("readlink \"/proc/$1/exe\"") != std::string::npos);
	UAM_ASSERT(linux_script.find("test \"$uam_ppid\" = \"$1\" && test \"$uam_state\" != Z") != std::string::npos);
	const std::size_t refusal = linux_script.find("2:*'still owns remote processes'*) ;;");
	const std::size_t contract = linux_script.find(":2:'Usage: uam-runner '*) ;;");
	const std::size_t reverified = linux_script.find("uam_born\" = \"$uam_helper_born\" && ! uam_has_child \"$uam_pid\"");
	const std::size_t signal = linux_script.find("kill -TERM \"$uam_pid\"");
	UAM_ASSERT(refusal != std::string::npos && contract != std::string::npos && reverified != std::string::npos && signal != std::string::npos);
	UAM_ASSERT(refusal < contract && contract < reverified && reverified < signal);
	UAM_ASSERT(linux_script.find("pkill") == std::string::npos && linux_script.find("killall") == std::string::npos);
	UAM_ASSERT(linux_script.find("rm -rf") == std::string::npos); // Linux retirement never deletes spools.
#if !defined(_WIN32)
	TempDir temp("uam-retirement-script");
	const fs::path script = temp.root / "retire.sh";
	UAM_ASSERT(uam::io::WriteTextFile(script, "uam_root=/none; uam_version=1; uam_endpoint=uam-1-p3; uam_assume_pre_fix=\n" + linux_script));
	const ProcessExecutionResult syntax = PlatformServicesFactory::Instance().process_service.ExecuteCommand("/bin/sh -n \"" + script.string() + "\" 2>&1", 10000);
	UAM_ASSERT_EQ(syntax.exit_code, 0);
#endif
}

UAM_TEST(RemoteRetirementCommandsStayUnderTheWindowsLimitOrAreSkipped)
{
	uam::remote::BootstrapPlan plan;
	plan.ssh_alias = "windows-lab";
	plan.version = "5.1.0";
	plan.nonce = std::string(64, 'n');
	const std::vector<uam::remote::BootstrapStep> defaults = uam::remote::BuildBootstrapRetirementSteps(plan, "windows");
	UAM_ASSERT_EQ(defaults.size(), std::size_t{3});
	for (const uam::remote::BootstrapStep& step : defaults) UAM_ASSERT(step.argv.back().size() < 8000);
	for (const std::size_t length : {std::size_t{120}, std::size_t{240}})
	{
		plan.runner_directory = std::string(length, 'd');
		const std::vector<uam::remote::BootstrapStep> steps = uam::remote::BuildBootstrapRetirementSteps(plan, "windows");
		UAM_ASSERT(steps.empty() || steps.size() == 3);
		for (const uam::remote::BootstrapStep& step : steps) UAM_ASSERT(step.argv.back().size() < 8000);
	}
	UAM_ASSERT_EQ(uam::remote::BuildBootstrapRetirementSteps(plan, "linux").size(), std::size_t{2});
	UAM_ASSERT(uam::remote::BuildBootstrapRetirementSteps(plan, "linux").back().argv.back().find("uam_nonce='" + plan.nonce + "'; uam_keep_versions=2;") != std::string::npos);
	UAM_ASSERT(uam::remote::BuildBootstrapRetirementSteps(plan, "linux").front().argv.back().find("uam_assume_pre_fix=;") != std::string::npos);
}

UAM_TEST(RemoteRunnerAnswersTheStopContract)
{
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
#if defined(_WIN32)
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner.exe";
#else
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
#endif
	UAM_ASSERT(fs::exists(runner));
	const ProcessExecutionResult answered = service.ExecuteCommand("\"" + runner.string() + "\" --stop-contract", 30000);
	UAM_ASSERT_EQ(answered.exit_code, 0);
	UAM_ASSERT_EQ(uam::strings::Trim(answered.output), std::string("1"));
	// Pins the real pre-fix probe pattern: a helper without --stop-contract treats it as an unknown flag.
#if defined(_WIN32)
	const std::string discard = "nul";
#else
	const std::string discard = "/dev/null";
#endif
	const std::string unknown = "\"" + runner.string() + "\" --no-such-flag";
	const ProcessExecutionResult standard_output = service.ExecuteCommand(unknown + " 2>" + discard, 30000);
	const ProcessExecutionResult standard_error = service.ExecuteCommand(unknown + " 2>&1 >" + discard, 30000);
	UAM_ASSERT_EQ(standard_output.exit_code, 2);
	UAM_ASSERT(standard_output.output.empty());
	UAM_ASSERT_EQ(standard_error.exit_code, 2);
	UAM_ASSERT(standard_error.output.starts_with("Usage: uam-runner "));
}

#if defined(__linux__)
UAM_TEST(LinuxRetirementHonoursOwedRefusalsAndTerminatesOnlyPreFixHelpers)
{
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path built = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::exists(built));
	TempDir temp("uam-retirement-linux");
	const fs::path root = fs::canonical(temp.root);
	const auto install = [&](const fs::path& base, const std::string& version)
	{
		const fs::path executable = base / version / "uam-runner";
		fs::create_directories(executable.parent_path());
		fs::copy_file(built, executable);
		return executable;
	};
	const fs::path owed = install(root, "5.0.0"), busy = install(root, "4.11.0");
	const fs::path owed_socket = root / "uam-5.0.0-p3.sock", busy_socket = root / "uam-4.11.0-p3.sock";
	const auto runner = [&](const fs::path& executable, const std::string& verb, const fs::path& socket)
	{ return service.ExecuteCommand("\"" + executable.string() + "\" " + verb + " --socket \"" + socket.string() + "\"", 30000).exit_code; };
	const auto connected = [&](const fs::path& executable, const fs::path& socket)
	{
		std::string ignored;
		uam::remote::RunnerClient probe(service, {executable.string(), "bridge", "--socket", socket.string()});
		return probe.Connect(&ignored);
	};
	struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } };
	std::optional<uam::remote::RunnerClient> busy_client;
	Cleanup cleanup{[&]
	{
		try {
		std::string ignored;
		if (busy_client) { (void)busy_client->CloseProcessInput("retire-busy", &ignored); std::this_thread::sleep_for(std::chrono::milliseconds(300)); (void)busy_client->RemoveProcess("retire-busy", &ignored); busy_client->Disconnect(); }
		(void)runner(busy, "stop", busy_socket);
		} catch (...) {}
	}};
	UAM_ASSERT_EQ(runner(owed, "start", owed_socket), 0);
	UAM_ASSERT_EQ(runner(busy, "start", busy_socket), 0);
	std::string error;
	{
		// A finished process whose output nobody read is owed work: this helper's refusal is truthful.
		uam::remote::RunnerClient owed_client(service, {owed.string(), "bridge", "--socket", owed_socket.string()});
		UAM_ASSERT(owed_client.Connect(&error));
		UAM_ASSERT(owed_client.StartProcess("owed", root, {"/bin/sh", "-c", "printf owed-output"}, {}, &error));
		std::this_thread::sleep_for(std::chrono::seconds(1));
		owed_client.Disconnect();
	}
	busy_client.emplace(service, std::vector<std::string>{busy.string(), "bridge", "--socket", busy_socket.string()});
	UAM_ASSERT(busy_client->Connect(&error));
	UAM_ASSERT(busy_client->StartProcess("retire-busy", root, {"/bin/sh", "-c", "IFS= read -r value"}, {}, &error));
	const auto retire = [&](const std::string& assume_pre_fix)
	{
		const std::string script = "uam_root='" + root.string() + "'; uam_version=5.1.0; uam_endpoint=uam-5.1.0-p3; uam_assume_pre_fix=" + assume_pre_fix + "\n" + uam::remote::LinuxHelperRetirementScript();
		UAM_ASSERT(uam::io::WriteTextFile(root / "retire.sh", script));
		return service.ExecuteCommand("/bin/sh \"" + (root / "retire.sh").string() + "\"", 120000).output;
	};
	const std::string honoured = retire("");
	UAM_ASSERT(honoured.find("UAM_HELPER_KEPT_BUSY:" + owed.string()) != std::string::npos);
	UAM_ASSERT(honoured.find("UAM_HELPER_KEPT_CHILDREN:" + busy.string()) != std::string::npos);
	UAM_ASSERT(honoured.find("UAM_HELPER_TERMINATED") == std::string::npos);
	UAM_ASSERT(connected(owed, owed_socket));
	// A pre-fix stand-in: the same refusal is untrustworthy, there is no child, so only this helper is terminated.
	const std::string forced = retire("1");
	UAM_ASSERT(forced.find("UAM_HELPER_TERMINATED:" + owed.string()) != std::string::npos);
	UAM_ASSERT(forced.find("UAM_HELPER_RETIRED:" + owed.string()) != std::string::npos);
	UAM_ASSERT(forced.find("UAM_HELPER_KEPT_CHILDREN:" + busy.string()) != std::string::npos);
	UAM_ASSERT(!connected(owed, owed_socket));
	UAM_ASSERT(connected(busy, busy_socket));
}
#endif

#if defined(_WIN32)
UAM_TEST(WindowsRetirementHonoursOwedRefusalsAndKeepsSpoolsWhileForeignHelpersRun)
{
	TempDir temp("uam-helper-owed");
	const fs::path root = fs::canonical(temp.root);
	ScopedEnvVar no_namespace("UAM_RUNNER_SERVICE_NAMESPACE", "");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path built = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner.exe";
	UAM_ASSERT(fs::exists(built));
	const auto install = [&](const fs::path& executable, const fs::path& source)
	{
		fs::create_directories(executable.parent_path());
		fs::copy_file(source, executable);
		return executable;
	};
	const fs::path owed = install(root / "helpers" / "5.0.0" / "uam-runner.exe", built);
	const fs::path foreign = install(root / "other" / "5.0.0" / "uam-runner.exe", built);
	// The current helper must report another version, or the owed helper would count as sharing its pipe; any small executable stands in.
	const fs::path current = install(root / "helpers" / "5.1.0" / "uam-runner.exe", fs::path(uam::env::GetNonEmptyString("SystemRoot").value_or("C:\\Windows")) / "System32" / "hostname.exe");
	int script_index = 0;
	const auto powershell = [&](const std::string& script)
	{
		const fs::path file = temp.root / ("script-" + std::to_string(script_index++) + ".ps1");
		if (!uam::io::WriteTextFile(file, script)) return ProcessExecutionResult{};
		return service.ExecuteCommand("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + file.string() + "\"", 180000);
	};
	const std::string variables = "$uamHelperRoot='" + root.string() + "'; $uamRunKey='HKCU:\\Software\\UAM-Tests\\" + service.GenerateUuid() + "\\Run'; $uamCurrent='" + current.string() + "'; ";
	const auto connected = [&](const fs::path& executable)
	{
		std::string ignored;
		uam::remote::RunnerClient probe(service, {executable.string(), "bridge"});
		return probe.Connect(&ignored);
	};
	std::vector<uam::platform::StdioProcessPlatformFields> services(2);
	struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } };
	Cleanup cleanup{[&]
	{
		try { for (auto& fields : services) { service.TerminateStdioProcess(fields, true); service.CloseStdioProcessHandles(fields); } } catch (...) {}
	}};
	const auto serve = [&](std::size_t index, const fs::path& executable)
	{
		std::string start_error;
		UAM_ASSERT(service.StartStdioProcess(services[index], fs::temp_directory_path(), {executable.string(), "serve"}, &start_error));
		service.CloseStdioProcessInput(services[index]);
		bool ready = false;
		for (int attempt = 0; attempt < 100 && !ready; ++attempt)
		{
			ready = connected(executable);
			if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		UAM_ASSERT(ready);
	};
	serve(0, owed);
	std::string error;
	{
		// A finished process whose output nobody read is owed work: this helper's refusal is truthful.
		uam::remote::RunnerClient owed_client(service, {owed.string(), "bridge"});
		UAM_ASSERT(owed_client.Connect(&error));
		UAM_ASSERT(owed_client.StartProcess("owed", fs::temp_directory_path(), {"cmd.exe", "/d", "/s", "/c", "echo owed-output"}, {}, &error));
		std::this_thread::sleep_for(std::chrono::seconds(1));
		owed_client.Disconnect();
	}
	const ProcessExecutionResult honoured = powershell(variables + "$uamAssumePreFix=$false; " + uam::remote::WindowsHelperRetirementScript());
	UAM_ASSERT(honoured.output.find("UAM_HELPER_KEPT_BUSY:" + owed.string()) != std::string::npos);
	UAM_ASSERT(honoured.output.find("UAM_HELPER_TERMINATED") == std::string::npos);
	UAM_ASSERT(honoured.output.find("UAM_HELPER_RETIREMENT_FAILED") == std::string::npos);
	UAM_ASSERT(connected(owed));
	// A pre-fix stand-in: the same refusal is untrustworthy and there is no child, so the helper is terminated.
	const ProcessExecutionResult forced = powershell(variables + "$uamAssumePreFix=$true; " + uam::remote::WindowsHelperRetirementScript());
	UAM_ASSERT(forced.output.find("UAM_HELPER_TERMINATED:" + owed.string()) != std::string::npos);
	UAM_ASSERT(forced.output.find("UAM_HELPER_RETIRED:" + owed.string()) != std::string::npos);
	UAM_ASSERT(!connected(owed));

	// The killed helper's spool stays while a helper outside this root runs, then goes; a held owner lock always keeps its spool.
	std::vector<fs::path> spools;
	for (const fs::directory_entry& entry : fs::directory_iterator(owed.parent_path()))
		if (entry.path().filename().string().starts_with("uam-runner-spool-")) spools.push_back(entry.path());
	UAM_ASSERT(!spools.empty());
	const fs::path held = root / "helpers" / "4.0.0" / "uam-runner-spool-held";
	fs::create_directories(held);
	std::ofstream held_lock(held / ".owner-lock");
	UAM_ASSERT(held_lock.good());
	serve(1, foreign);
	const ProcessExecutionResult kept = powershell(variables + uam::remote::WindowsHelperSpoolCleanupScript());
	UAM_ASSERT(kept.output.find("UAM_HELPER_SPOOLS_KEPT") != std::string::npos);
	for (const fs::path& spool : spools) UAM_ASSERT(fs::exists(spool));
	UAM_ASSERT_EQ(service.ExecuteCommand("\"" + foreign.string() + "\" stop", 30000).exit_code, 0);
	for (int attempt = 0; attempt < 100 && connected(foreign); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const ProcessExecutionResult swept = powershell(variables + uam::remote::WindowsHelperSpoolCleanupScript());
	UAM_ASSERT(swept.output.find("UAM_HELPER_RETIREMENT_FAILED") == std::string::npos);
	if (swept.output.find("UAM_HELPER_SPOOLS_KEPT") == std::string::npos) // Another test's helper may run elsewhere on this machine.
		for (const fs::path& spool : spools) UAM_ASSERT(!fs::exists(spool));
	UAM_ASSERT(fs::exists(held / ".owner-lock"));
}
#endif

namespace
{
	/// One helper root: the current version, older versions by age, a pending install, a live folder and things that are not ours.
	struct HelperVersionFixture
	{
		fs::path root;
		fs::path outside;
		std::string executable;
		explicit HelperVersionFixture(const fs::path& base, std::string name) : root(base / "helpers"), outside(base / "outside"), executable(std::move(name))
		{
			for (const char* version : {"5.1.0", "5.0.3", "5.0.2", "5.0.1", "5.0.0", "4.9.0", "4.8.0"})
				Write(root / version / executable);
			// Windows renames a folder before deleting it; a leftover from an interrupted pass is finished off.
			if (Windows()) Write(root / "5.0.4.uam-removing" / executable);
			Write(root / "5.1.0" / ("uam-runner.tmp-stale" + Suffix()));
			Write(root / "5.1.0" / ("uam-runner.tmp-current-nonce" + Suffix()));
			Write(root / "5.0.0" / ("uam-runner.tmp-old" + Suffix()));
			Write(root / "4.9.0" / "uam-runner.activation-pending");
			Write(root / "notes" / "keep.txt");
			Write(root / "uam-5.1.0-p3.sock");
			Write(outside / executable);
			Write(outside / "keep.txt");
			int age = 0;
			for (const char* version : {"5.1.0", "5.0.3", "5.0.2", "5.0.1", "5.0.0", "4.9.0", "4.8.0"})
				fs::last_write_time(root / version, fs::file_time_type::clock::now() - std::chrono::hours(24 * ++age));
		}
		bool Windows() const { return executable.ends_with(".exe"); }
		std::string Suffix() const { return Windows() ? ".exe" : ""; }
		static void Write(const fs::path& path)
		{
			fs::create_directories(path.parent_path());
			std::ofstream(path) << "x";
		}
		void Verify(const std::string& output) const
		{
			UAM_ASSERT(output.find("UAM_HELPER_PRUNE_FAILED") == std::string::npos);
			// Current, the two newest others, the live and pending folders, and everything not shaped like a helper version stay.
			for (const char* version : {"5.1.0", "5.0.3", "5.0.2", "4.9.0", "4.8.0"}) UAM_ASSERT(fs::exists(root / version / executable));
			UAM_ASSERT(output.find("UAM_HELPER_VERSION_KEPT_PENDING:4.9.0") != std::string::npos);
			UAM_ASSERT(output.find("UAM_HELPER_VERSION_KEPT_LIVE:4.8.0") != std::string::npos);
			UAM_ASSERT(fs::exists(root / "notes" / "keep.txt"));
			UAM_ASSERT(fs::exists(root / "uam-5.1.0-p3.sock"));
			UAM_ASSERT(fs::exists(outside / executable) && fs::exists(outside / "keep.txt"));
			// The oldest unused versions, an interrupted removal and abandoned uploads go; the current install's upload stays.
			UAM_ASSERT(!fs::exists(root / "5.0.1") && !fs::exists(root / "5.0.0"));
			if (Windows()) UAM_ASSERT(!fs::exists(root / "5.0.4.uam-removing"));
			UAM_ASSERT(output.find("UAM_HELPER_VERSION_REMOVED:5.0.1") != std::string::npos);
			UAM_ASSERT(!fs::exists(root / "5.1.0" / ("uam-runner.tmp-stale" + Suffix())));
			UAM_ASSERT(fs::exists(root / "5.1.0" / ("uam-runner.tmp-current-nonce" + Suffix())));
		}
	};
}

UAM_TEST(RemoteHelperVersionPruneScriptsKeepCurrentLiveAndPendingVersions)
{
	const std::string windows = uam::remote::WindowsHelperVersionPruneScript();
	UAM_ASSERT(windows.find("elseif ($process.Name -ieq 'uam-runner.exe') { $uamKnown=$false }") != std::string::npos);
	UAM_ASSERT(windows.find("[IO.Directory]::Move($folder.FullName,$gone)") < windows.find("[IO.Directory]::Delete($gone,$true)"));
	UAM_ASSERT(windows.find("Remove-Item") == std::string::npos); // Directory.Delete does not follow junctions inside a removed folder.
	const std::string linux_script = uam::remote::LinuxHelperVersionPruneScript();
	UAM_ASSERT(linux_script.find("for uam_link in exe cwd") != std::string::npos);
	UAM_ASSERT(linux_script.find("test ! -L \"$uam_name\"") != std::string::npos);
	UAM_ASSERT(linux_script.find("pkill") == std::string::npos && linux_script.find("kill ") == std::string::npos);
#if !defined(_WIN32)
	TempDir temp("uam-version-prune-syntax");
	const fs::path script = temp.root / "prune.sh";
	UAM_ASSERT(uam::io::WriteTextFile(script, "uam_root=/none; uam_version=1; uam_nonce=n; uam_keep_versions=2\n" + linux_script));
	const ProcessExecutionResult syntax = PlatformServicesFactory::Instance().process_service.ExecuteCommand("/bin/sh -n \"" + script.string() + "\" 2>&1", 10000);
	UAM_ASSERT_EQ(syntax.exit_code, 0);
#endif
}

#if defined(__linux__)
UAM_TEST(LinuxHelperVersionPruneRemovesOnlyOldUnusedVersions)
{
	TempDir temp("uam-version-prune-linux");
	const HelperVersionFixture fixture(fs::canonical(temp.root), "uam-runner");
	fs::create_directory_symlink(fixture.outside, fixture.root / "4.0.0");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields live;
	std::string error;
	// A process working inside 4.8.0 keeps that version even though it is old.
	UAM_ASSERT(service.StartStdioProcess(live, fixture.root / "4.8.0", {"/bin/sleep", "60"}, &error));
	struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } } cleanup{[&] { service.TerminateStdioProcess(live, true); service.CloseStdioProcessHandles(live); }};
	const std::string script = "uam_root='" + fixture.root.string() + "'; uam_version=5.1.0; uam_nonce=current-nonce; uam_keep_versions=2\n" + uam::remote::LinuxHelperVersionPruneScript();
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "prune.sh", script));
	const ProcessExecutionResult result = service.ExecuteCommand("/bin/sh \"" + (temp.root / "prune.sh").string() + "\"", 60000);
	UAM_ASSERT_EQ(result.exit_code, 0);
	fixture.Verify(result.output);
	UAM_ASSERT(fs::is_symlink(fixture.root / "4.0.0"));
	UAM_ASSERT(!fs::exists(fixture.root / "5.0.0" / "uam-runner.tmp-old"));
}
#endif

#if defined(_WIN32)
UAM_TEST(WindowsHelperVersionPruneRemovesOnlyOldUnusedVersions)
{
	TempDir temp("uam-version-prune-windows");
	const HelperVersionFixture fixture(fs::canonical(temp.root), "uam-runner.exe");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	// A process started from inside 4.8.0 keeps that version even though it is old.
	const fs::path system = fs::path(uam::env::GetNonEmptyString("SystemRoot").value_or("C:\\Windows")) / "System32";
	fs::copy_file(system / "ping.exe", fixture.root / "4.8.0" / "uam-live.exe");
	const ProcessExecutionResult junction = service.ExecuteCommand("cmd.exe /d /c mklink /J \"" + (fixture.root / "4.0.0").string() + "\" \"" + fixture.outside.string() + "\"", 30000);
	UAM_ASSERT_EQ(junction.exit_code, 0);
	uam::platform::StdioProcessPlatformFields live;
	std::string error;
	UAM_ASSERT(service.StartStdioProcess(live, temp.root, {(fixture.root / "4.8.0" / "uam-live.exe").string(), "-n", "60", "127.0.0.1"}, &error));
	struct Cleanup { std::function<void()> run; ~Cleanup() { run(); } } cleanup{[&] { service.TerminateStdioProcess(live, true); service.CloseStdioProcessHandles(live); }};
	const fs::path file = temp.root / "prune.ps1";
	UAM_ASSERT(uam::io::WriteTextFile(file, "$uamCurrent='" + (fixture.root / "5.1.0" / "uam-runner.exe").string() + "'; $uamNonce='current-nonce'; $uamKeepVersions=2; " +
	    uam::remote::WindowsHelperVersionPruneScript()));
	const ProcessExecutionResult result = service.ExecuteCommand("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + file.string() + "\"", 180000);
	UAM_ASSERT_EQ(result.exit_code, 0);
	if (result.output.find("UAM_HELPER_VERSIONS_KEPT") != std::string::npos) return; // Another unreadable helper runs on this machine: nothing may be removed.
	fixture.Verify(result.output);
	UAM_ASSERT(fs::exists(fixture.root / "4.0.0"));
}
#endif

UAM_TEST(RemoteFailedSpoolCleanupRetainsOwnership)
{
	TempDir temp("uam-retirement-failure");
	RunnerState state;
	const fs::path directory = temp.root / "worker";
	fs::create_directory(directory);
	RunnerStateRetirementTestAccess::Add(state, directory);
	fs::create_directory(directory / "stdout");
	UAM_ASSERT(uam::io::WriteTextFile(directory / "stdout" / "pending", "owned output"));
	UAM_ASSERT(state.HasManagedProcesses());
	UAM_ASSERT(RunnerStateRetirementTestAccess::Retiring(state));
	UAM_ASSERT(fs::exists(directory / "stdout" / "pending"));
	UAM_ASSERT(state.HasManagedProcesses());
	fs::remove(directory / "stdout" / "pending");
	UAM_ASSERT(!state.HasManagedProcesses());
	UAM_ASSERT(!fs::exists(directory));
}

#if defined(__linux__)
UAM_TEST(LinuxDeferredExitObservationSurvivesProcessTransfer)
{
	TempDir temp("uam-linux-reap-transfer");
	uam::platform::StdioProcessPlatformFields source;
	uam::platform::StdioProcessPlatformFields destination;
	const std::shared_ptr<std::atomic<bool>> completion = std::make_shared<std::atomic<bool>>(false);
	source.deferred_reap_complete = completion;
	uam::TransferStdioProcessFields(source, destination);
	UAM_ASSERT(!source.deferred_reap_complete);
	UAM_ASSERT(destination.deferred_reap_complete == completion);
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	int exit_code = -1;
	UAM_ASSERT(!service.PollStdioProcessExited(destination, &exit_code));
	std::string error;
	UAM_ASSERT(!service.StartStdioProcess(destination, temp.root, {"/bin/sh", "-c", "exit 0"}, &error));
	UAM_ASSERT(destination.deferred_reap_complete == completion);
	completion->store(true, std::memory_order_release);
	UAM_ASSERT(service.PollStdioProcessExited(destination, &exit_code));
	UAM_ASSERT(service.StartStdioProcess(destination, temp.root, {"/bin/sh", "-c", "exit 0"}, &error));
	UAM_ASSERT(!destination.deferred_reap_complete);
	service.StopStdioProcess(destination, false);
}
#endif
