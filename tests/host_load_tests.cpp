#include "test_harness.h"

#include "common/runtime/host_load_sampler.h"
#include "remote/host_load_protocol.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace uam_test;

namespace
{
	nlohmann::json Hello(uam::remote::RunnerState* state)
	{
		return uam::remote::HandleRunnerRequest({{"id", "hello"}, {"type", "hello"}, {"protocolVersion", uam::remote::kRunnerProtocolVersion}, {"nonce", "host-load"}}, "5.2.0", state);
	}

	nlohmann::json HostLoadRequest(uam::remote::RunnerState* state)
	{
		return uam::remote::HandleRunnerRequest({{"id", "load"}, {"type", "host.load"}}, "5.2.0", state);
	}

	std::uint32_t RunnerProcesses(uam::remote::RunnerState& state)
	{
		const nlohmann::json response = HostLoadRequest(&state);
		UAM_ASSERT(response.value("ok", false));
		uam::platform::HostLoad load;
		UAM_ASSERT(uam::remote::HostLoadFromJson(response["result"], load));
		UAM_ASSERT(load.runner_processes.has_value());
		return *load.runner_processes;
	}

	bool WaitFor(const std::function<bool()>& condition, std::chrono::milliseconds limit = std::chrono::seconds(10))
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + limit;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	uam::platform::HostLoad SampleLoad(std::uint32_t processes)
	{
		uam::platform::HostLoad load;
		load.cores = 8;
		load.load_average_1m = 2.5;
		load.memory_available_bytes = 4ull << 30U;
		load.memory_total_bytes = 16ull << 30U;
		load.runner_processes = processes;
		return load;
	}
}

UAM_TEST(HostLoadWireFormatKeepsUnknownFiguresUnknownAndRejectsDamagedReplies)
{
	using uam::remote::HostLoadFromJson;
	using uam::remote::HostLoadToJson;
	uam::platform::HostLoad parsed;
	UAM_ASSERT(HostLoadFromJson(HostLoadToJson(SampleLoad(3)), parsed));
	UAM_ASSERT(parsed == SampleLoad(3));

	// A figure the helper could not read stays unknown; it never becomes zero.
	const nlohmann::json partial = {{"version", 1}, {"cores", 4}, {"loadAverage1m", nullptr}};
	UAM_ASSERT(HostLoadFromJson(partial, parsed));
	UAM_ASSERT_EQ(*parsed.cores, std::uint32_t{4});
	UAM_ASSERT(!parsed.load_average_1m && !parsed.cpu_percent && !parsed.memory_available_bytes && !parsed.runner_processes);

	// A later version may add fields; this reader keeps the ones it knows.
	UAM_ASSERT(HostLoadFromJson({{"version", 2}, {"cores", 2}, {"gpuPercent", 40}}, parsed));
	UAM_ASSERT_EQ(*parsed.cores, std::uint32_t{2});

	for (const nlohmann::json& damaged : {nlohmann::json::array(), nlohmann::json{{"cores", 2}}, nlohmann::json{{"version", 0}},
	         nlohmann::json{{"version", 1}, {"cores", -1}}, nlohmann::json{{"version", 1}, {"cores", "8"}},
	         nlohmann::json{{"version", 1}, {"cpuPercent", 140.0}}, nlohmann::json{{"version", 1}, {"loadAverage1m", -0.5}},
	         nlohmann::json{{"version", 1}, {"runnerProcesses", 1.5}}, nlohmann::json{{"version", 1}, {"memoryTotalBytes", true}}})
	{
		parsed = SampleLoad(1);
		UAM_ASSERT(!HostLoadFromJson(damaged, parsed));
		UAM_ASSERT(parsed == uam::platform::HostLoad{});
	}

	// Zero processors is not a reading, and available memory never exceeds the total.
	UAM_ASSERT(HostLoadFromJson({{"version", 1}, {"cores", 0}, {"memoryAvailableBytes", 10}, {"memoryTotalBytes", 4}}, parsed));
	UAM_ASSERT(!parsed.cores);
	UAM_ASSERT_EQ(*parsed.memory_available_bytes, std::uint64_t{4});
}

UAM_TEST(RunnerHostLoadIsVersionedAndCountsLiveProcessesForEveryDesktop)
{
	TempDir temp("uam-host-load");
	uam::remote::RunnerState state;
	UAM_ASSERT_EQ(Hello(&state)["capabilities"].value("hostLoad", 0), uam::remote::kHostLoadVersion);
	// Without process state the helper offers no load at all, so a caller treats its load as unknown.
	UAM_ASSERT_EQ(Hello(nullptr)["capabilities"].value("hostLoad", -1), 0);
	const nlohmann::json refused = HostLoadRequest(nullptr);
	UAM_ASSERT(!refused.value("ok", true));
	UAM_ASSERT_EQ(refused["error"].value("code", std::string()), std::string("unsupported_request"));

	const nlohmann::json idle = HostLoadRequest(&state);
	UAM_ASSERT(idle.value("ok", false));
	UAM_ASSERT_EQ(idle.value("type", std::string()), std::string("host.load"));
	uam::platform::HostLoad load;
	UAM_ASSERT(uam::remote::HostLoadFromJson(idle["result"], load));
	UAM_ASSERT(load.cores && *load.cores > 0);
	UAM_ASSERT(load.memory_total_bytes && *load.memory_total_bytes > 0);
	UAM_ASSERT(load.memory_available_bytes && *load.memory_available_bytes <= *load.memory_total_bytes);
	// Every platform reports one load figure: a load average, or CPU use where there is none.
	UAM_ASSERT(load.load_average_1m.has_value() != load.cpu_percent.has_value());
	UAM_ASSERT_EQ(*load.runner_processes, std::uint32_t{0});

	// Two desktops share one helper service; both of their processes count.
	for (const std::string session : {"acp-first-desktop", "acp-second-desktop"})
	{
		const nlohmann::json started = state.HandleProcessRequest({{"id", "start-" + session}, {"type", "process.start"}, {"sessionId", session},
		    {"controlToken", "token-" + session}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", StdioCatArgv()},
		    {"environment", nlohmann::json::object()}});
		UAM_ASSERT(started.value("ok", false));
	}
	UAM_ASSERT_EQ(RunnerProcesses(state), std::uint32_t{2});

	UAM_ASSERT(state.HandleProcessRequest({{"id", "stop"}, {"type", "process.stop"}, {"sessionId", "acp-first-desktop"}, {"controlToken", "token-acp-first-desktop"}}).value("ok", false));
	UAM_ASSERT(WaitFor([&] { return RunnerProcesses(state) == 1; }));
	UAM_ASSERT(state.HandleProcessRequest({{"id", "stop"}, {"type", "process.stop"}, {"sessionId", "acp-second-desktop"}, {"controlToken", "token-acp-second-desktop"}}).value("ok", false));
	UAM_ASSERT(WaitFor([&] { return RunnerProcesses(state) == 0; }));
}

UAM_TEST(HostLoadSamplerReadsOffTheCallerThreadAndRefreshesWithinTheInterval)
{
	uam::host_load::Sampler sampler(std::chrono::milliseconds(20), std::chrono::seconds(30), std::chrono::seconds(30));
	const std::thread::id caller = std::this_thread::get_id();
	std::atomic<int> reads{0};
	std::atomic<bool> read_on_caller{false};
	std::atomic<bool> release{false};
	sampler.Want("host-a", "identity-1", [&]
	{
		return uam::host_load::Reader([&](std::stop_token stop)
		{
			if (std::this_thread::get_id() == caller) read_on_caller = true;
			// The first read blocks until released: Want, Latest and Poll must not wait for it.
			while (!release.load() && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const int count = ++reads;
			return uam::host_load::Reading{uam::host_load::SampleStatus::Known, SampleLoad(static_cast<std::uint32_t>(count))};
		});
	});
	const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
	UAM_ASSERT(sampler.Latest("host-a").has_value());
	UAM_ASSERT(sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Pending);
	(void)sampler.Poll();
	sampler.Want("host-a", "identity-1", {});
	UAM_ASSERT(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
	release = true;
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Known; }));
	UAM_ASSERT(sampler.Poll());
	// Repeated reads keep the figure current; each new figure is reported once.
	UAM_ASSERT(WaitFor([&] { return reads.load() >= 3; }));
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->load.runner_processes.value_or(0) >= 3; }));
	UAM_ASSERT(!read_on_caller.load());
	UAM_ASSERT(sampler.Poll());
	UAM_ASSERT(!sampler.Latest("missing").has_value());
}

UAM_TEST(HostLoadSamplerNeverReportsAFailedOrOldReadingAsIdle)
{
	uam::host_load::Sampler sampler(std::chrono::milliseconds(10), std::chrono::seconds(30), std::chrono::milliseconds(150));
	std::atomic<int> mode{0};
	sampler.Want("host-a", "identity-1", [&]
	{
		return uam::host_load::Reader([&](std::stop_token stop)
		{
			if (mode.load() == 0) return uam::host_load::Reading{uam::host_load::SampleStatus::Known, SampleLoad(4)};
			if (mode.load() == 1) return uam::host_load::Reading{};
			// A read that hangs leaves the last figure to age.
			while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return uam::host_load::Reading{};
		});
	});
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Known; }));
	mode = 1;
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Unreachable; }));
	UAM_ASSERT(sampler.Latest("host-a")->load == uam::platform::HostLoad{});
	mode = 0;
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Known; }));
	mode = 2;
	(void)sampler.Poll();
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Stale; }));
	UAM_ASSERT(sampler.Latest("host-a")->load == uam::platform::HostLoad{});
	// Turning stale publishes once.
	UAM_ASSERT(sampler.Poll());
	UAM_ASSERT(!sampler.Poll());
	// The destructor stops the hung read and joins its thread.
}

UAM_TEST(HostLoadSamplerStopsAskingAnOlderHelperAndRestartsWhenTheHostChanges)
{
	uam::host_load::Sampler sampler(std::chrono::milliseconds(5), std::chrono::seconds(30), std::chrono::seconds(30));
	std::atomic<int> old_reads{0};
	std::atomic<bool> old_reader_closed_on_caller{false};
	const std::thread::id caller = std::this_thread::get_id();
	struct Connection
	{
		std::thread::id caller;
		std::atomic<bool>* closed_on_caller;
		~Connection() { if (std::this_thread::get_id() == caller) *closed_on_caller = true; }
	};
	sampler.Want("host-a", "helper-5.1", [&]
	{
		const std::shared_ptr<Connection> connection = std::make_shared<Connection>(Connection{caller, &old_reader_closed_on_caller});
		return uam::host_load::Reader([&, connection](std::stop_token)
		{
			++old_reads;
			return uam::host_load::Reading{uam::host_load::SampleStatus::Unsupported, {}};
		});
	});
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Unsupported; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	sampler.Want("host-a", "helper-5.1", {});
	(void)sampler.Poll();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	UAM_ASSERT_EQ(old_reads.load(), 1);
	UAM_ASSERT(sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Unsupported);

	// An updated helper is another identity: the old answer is discarded and the new helper is asked.
	sampler.Want("host-a", "helper-5.2", []
	{
		return uam::host_load::Reader([](std::stop_token) { return uam::host_load::Reading{uam::host_load::SampleStatus::Known, SampleLoad(1)}; });
	});
	UAM_ASSERT(sampler.Latest("host-a")->status != uam::host_load::SampleStatus::Unsupported);
	UAM_ASSERT(WaitFor([&] { return sampler.Latest("host-a")->status == uam::host_load::SampleStatus::Known; }));
	UAM_ASSERT(WaitFor([&] { (void)sampler.Poll(); return sampler.Keys() == std::vector<std::string>{"host-a"}; }));
	UAM_ASSERT(!old_reader_closed_on_caller.load());
}

UAM_TEST(HostLoadSamplerDropsAHostNobodyWantsAfterTheLingerTime)
{
	uam::host_load::Sampler sampler(std::chrono::milliseconds(5), std::chrono::milliseconds(60), std::chrono::seconds(30));
	std::atomic<int> reads{0};
	sampler.Want("host-a", "identity-1", [&]
	{
		return uam::host_load::Reader([&](std::stop_token) { ++reads; return uam::host_load::Reading{uam::host_load::SampleStatus::Known, SampleLoad(0)}; });
	});
	UAM_ASSERT(WaitFor([&] { return reads.load() > 0; }));
	UAM_ASSERT(WaitFor([&] { (void)sampler.Poll(); return sampler.Keys().empty(); }));
	UAM_ASSERT(!sampler.Latest("host-a").has_value());
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const int settled = reads.load();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	UAM_ASSERT_EQ(reads.load(), settled);
	(void)sampler.Poll();
}
