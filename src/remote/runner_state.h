#pragma once

#include "common/platform/platform_state_fields.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uam::remote
{
	class RunnerState
	{
	  public:
		RunnerState();
		~RunnerState();
		RunnerState(const RunnerState&) = delete;
		RunnerState& operator=(const RunnerState&) = delete;

		nlohmann::json HandleProcessRequest(const nlohmann::json& request);
		bool HasManagedProcesses();

		// Kept public only so the translation unit's drain helpers stay small; it is
		// never exposed by the protocol.
		struct Process
		{
			struct InputDelivery
			{
				std::uint64_t digest = 0;
				std::size_t size = 0;
				std::uint64_t sequence = 0;
			};
			uam::platform::StdioProcessPlatformFields fields;
			std::filesystem::path stdout_spool;
			std::filesystem::path stderr_spool;
			std::filesystem::path working_directory;
			std::vector<std::string> arguments;
			std::vector<std::pair<std::string, std::string>> environment;
			std::string control_token;
			std::uintmax_t stdout_offset = 0;
			std::uintmax_t stderr_offset = 0;
			std::uint64_t input_sequence = 0;
			std::uint64_t input_digest = 0;
			std::size_t input_size = 0;
			std::unordered_map<std::string, InputDelivery> input_deliveries;
			std::deque<std::string> input_delivery_order;
			bool transient_lease = false;
			std::int64_t lease_duration_ms = 0;
			std::atomic<std::int64_t> lease_deadline_ms{0};
			std::mutex mutex;
			std::jthread drainer;
			std::atomic<bool> ready{false};
			std::atomic<bool> exited{false};
			std::atomic<int> exit_code{-1};
			std::string spool_error;
		};
		struct ChannelBuffer
		{
			std::string bytes;
			std::uintmax_t base_cursor = 0;
			std::uint64_t write_sequence = 0;
			std::uint64_t write_digest = 0;
			std::size_t write_size = 0;
		};
		struct Channel
		{
			ChannelBuffer remote_to_desktop;
			ChannelBuffer desktop_to_remote;
		};
		struct Upload
		{
			enum class State
			{
				Starting,
				Active,
				Finishing,
				Finished
			};

			std::mutex mutex;
			std::filesystem::path target;
			std::filesystem::path temporary;
			std::uintmax_t expected_size = 0;
			std::uintmax_t received_size = 0;
			std::uint64_t digest = 0;
			std::string expected_digest;
			State state = State::Starting;
		};

	  private:
		void SweepExpiredTransientProcesses();
		std::mutex m_stateMutex;
		std::filesystem::path m_spoolDirectory;
		std::unordered_map<std::string, std::shared_ptr<Process>> m_processes;
		std::unordered_map<std::string, Channel> m_channels;
		std::unordered_map<std::string, std::shared_ptr<Upload>> m_uploads;
	};
}
