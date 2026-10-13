#include "remote/runner_state.h"
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
