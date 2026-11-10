#include "remote/runner_bootstrap.h"
#include "remote/runner_client.h"
#include "remote/runner_state.h"
#include "common/utils/base64.h"
#include "test_harness.h"
#include <future>

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
	const auto request = [&](nlohmann::json value)
	{
		value["id"] = "drained";
		value["sessionId"] = "drained-session";
		value["controlToken"] = "drained-token";
		return state.HandleProcessRequest(value);
	};
#if defined(_WIN32)
	const nlohmann::json argv = nlohmann::json::array({"cmd.exe", "/d", "/v:on", "/s", "/c", "set /p value=& <nul set /p =!value!"});
#else
	const nlohmann::json argv = nlohmann::json::array({"/bin/sh", "-c", "IFS= read -r value; printf %s \"$value\""});
#endif
	UAM_ASSERT(request({{"type", "process.start"}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", argv}}).value("ok", false));
	UAM_ASSERT(state.HasManagedProcesses());
	UAM_ASSERT(request({{"type", "process.write"}, {"dataBase64", uam::base64::Encode("done\n")}}).value("ok", false));
	std::string output;
	bool exited = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!exited && std::chrono::steady_clock::now() < deadline)
	{
		const nlohmann::json polled = request({{"type", "process.poll"}, {"acknowledgedOutput", true}});
		UAM_ASSERT(polled.value("ok", false));
		const nlohmann::json& result = polled["result"];
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(result.value("stdoutBase64", ""), chunk));
		output += chunk;
		UAM_ASSERT(request({{"type", "process.ack"}, {"stdoutCursor", result["stdoutCursor"]}, {"stderrCursor", result["stderrCursor"]}}).value("ok", false));
		exited = !result.value("running", true);
		if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(uam::strings::Trim(output), std::string("done"));
	UAM_ASSERT(!state.HasManagedProcesses());
}

#if !defined(_WIN32)
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
	UAM_ASSERT(request({{"type", "process.start"}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", nlohmann::json::array({"/bin/sh", "-c", "awk 'BEGIN{for(i=0;i<500000;i++)printf \"%07d\\n\",i}'; IFS= read -r reply"})}}).value("ok", false));
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
	const fs::path spool = RunnerStateRetirementTestAccess::SpoolDirectory(state) / "reclaim.stdout";
	UAM_ASSERT_EQ(fs::file_size(spool), total);
	UAM_ASSERT(request({{"type", "process.ack"}, {"stdoutCursor", acknowledged}, {"stderrCursor", 0u}}).value("ok", false));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (fs::file_size(spool) == total && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT_EQ(fs::file_size(spool), total - acknowledged); // Exactly the unacknowledged tail remains.
	const nlohmann::json replay = poll(acknowledged);
	UAM_ASSERT(replay.value("ok", false));
	std::string replayed;
	UAM_ASSERT(uam::base64::Decode(replay["result"].value("stdoutBase64", ""), replayed));
	UAM_ASSERT(replayed.starts_with("0400000\n"));
	UAM_ASSERT_EQ(replay["result"].value("stdoutCursor", std::uintmax_t{0}), acknowledged + replayed.size());
	// A cursor below the reclaimed prefix keeps failing the existing way, never returning other bytes.
	const nlohmann::json stale = poll(acknowledged - 1);
	UAM_ASSERT(!stale.value("ok", false));
	UAM_ASSERT_EQ(stale["error"].value("code", ""), std::string("invalid_request"));
	UAM_ASSERT(request({{"type", "process.stop"}}).value("ok", false));
}
#endif

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
	UAM_ASSERT(windows.find("Stop-Process") != std::string::npos);
	UAM_ASSERT(windows.find("ParentProcessId=") != std::string::npos);
	// The forced stop needs a stop refusal, no other process from that exact executable and no children, checked per process.
	UAM_ASSERT(windows.find("-not $stopped -and -not $shared -and $sameRoot") != std::string::npos);
	UAM_ASSERT(windows.find("Where-Object { $_.CommandLine -notmatch") != std::string::npos);
	const std::size_t child_check = windows.find("Get-UamChildren $helper.ProcessId");
	const std::size_t forced = windows.find("Stop-Process -Id $helper.ProcessId");
	UAM_ASSERT(child_check != std::string::npos && forced != std::string::npos && child_check < forced);
	UAM_ASSERT(windows.find("Stop-Process -Name") == std::string::npos);
	UAM_ASSERT(windows.find("uam-runner-spool-*") != std::string::npos);

	const std::string linux_script = uam::remote::LinuxHelperRetirementScript();
	UAM_ASSERT(linux_script.find("readlink \"/proc/$1/exe\"") != std::string::npos);
	UAM_ASSERT(linux_script.find("uam_owned") != std::string::npos);
	UAM_ASSERT(linux_script.find("test \"$uam_ppid\" = \"$1\" && test \"$uam_state\" != Z") != std::string::npos);
	// Termination re-verifies identity, start time and children immediately before the signal.
	const std::size_t reverified = linux_script.find("uam_born\" = \"$uam_helper_born\" && ! uam_has_child \"$uam_pid\"");
	const std::size_t signal = linux_script.find("kill -TERM \"$uam_pid\"");
	UAM_ASSERT(reverified != std::string::npos && signal != std::string::npos && reverified < signal);
	UAM_ASSERT(linux_script.find("pkill") == std::string::npos && linux_script.find("killall") == std::string::npos);
	UAM_ASSERT(linux_script.find("uam-runner-spool-*") != std::string::npos);
#if !defined(_WIN32)
	TempDir temp("uam-retirement-script");
	const fs::path script = temp.root / "retire.sh";
	UAM_ASSERT(uam::io::WriteTextFile(script, "uam_root=/none; uam_version=1; uam_endpoint=uam-1-p3\n" + linux_script));
	const ProcessExecutionResult syntax = PlatformServicesFactory::Instance().process_service.ExecuteCommand("/bin/sh -n \"" + script.string() + "\" 2>&1", 10000);
	UAM_ASSERT_EQ(syntax.exit_code, 0);
#endif
}

#if defined(__linux__)
UAM_TEST(LinuxRetirementScriptStopsIdleHelpersAndKeepsBusyOnes)
{
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path built = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::exists(built));
	TempDir temp("uam-retirement-linux");
	const fs::path root = fs::canonical(temp.root);
	const auto install = [&](const std::string& version)
	{
		const fs::path executable = root / version / "uam-runner";
		fs::create_directories(executable.parent_path());
		fs::copy_file(built, executable);
		return executable;
	};
	const fs::path idle = install("4.11.0"), busy = install("5.0.0");
	const fs::path idle_socket = root / "uam-4.11.0-p3.sock", busy_socket = root / "uam-5.0.0-p3.sock";
	UAM_ASSERT_EQ(service.ExecuteCommand("\"" + idle.string() + "\" start --socket \"" + idle_socket.string() + "\"", 30000).exit_code, 0);
	UAM_ASSERT_EQ(service.ExecuteCommand("\"" + busy.string() + "\" start --socket \"" + busy_socket.string() + "\"", 30000).exit_code, 0);
	uam::remote::RunnerClient busy_client(service, {busy.string(), "bridge", "--socket", busy_socket.string()});
	std::string error;
	UAM_ASSERT(busy_client.Connect(&error));
	UAM_ASSERT(busy_client.StartProcess("retire-busy", root, {"/bin/sh", "-c", "IFS= read -r value"}, {}, &error));
	const std::string script = "uam_root='" + root.string() + "'; uam_version=5.1.0; uam_endpoint=uam-5.1.0-p3\n" + uam::remote::LinuxHelperRetirementScript();
	UAM_ASSERT(uam::io::WriteTextFile(root / "retire.sh", script));
	const ProcessExecutionResult retired = service.ExecuteCommand("/bin/sh \"" + (root / "retire.sh").string() + "\"", 90000);
	(void)busy_client.CloseProcessInput("retire-busy", &error);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	(void)busy_client.RemoveProcess("retire-busy", &error);
	busy_client.Disconnect();
	(void)service.ExecuteCommand("\"" + busy.string() + "\" stop --socket \"" + busy_socket.string() + "\"", 30000);
	UAM_ASSERT(retired.output.find("UAM_HELPER_RETIRED:" + idle.string()) != std::string::npos);
	UAM_ASSERT(retired.output.find("UAM_HELPER_KEPT_CHILDREN:" + busy.string()) != std::string::npos);
	uam::remote::RunnerClient stopped_client(service, {idle.string(), "bridge", "--socket", idle_socket.string()});
	UAM_ASSERT(!stopped_client.Connect(&error));
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
