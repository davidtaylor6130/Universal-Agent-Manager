#pragma once

#include "common/platform/platform_state_fields.h"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <atomic>
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

		// Kept public only so the translation unit's drain helpers stay small; it is
		// never exposed by the protocol.
		struct Process
		{
			uam::platform::StdioProcessPlatformFields fields;
			std::filesystem::path stdout_spool;
			std::filesystem::path stderr_spool;
			std::filesystem::path working_directory;
			std::vector<std::string> arguments;
			std::vector<std::pair<std::string, std::string>> environment;
			std::uintmax_t stdout_offset = 0;
			std::uintmax_t stderr_offset = 0;
			std::mutex mutex;
			std::jthread drainer;
			std::atomic<bool> exited{false};
			std::atomic<int> exit_code{-1};
			std::string spool_error;
		};
		struct Channel
		{
			std::string remote_to_desktop;
			std::string desktop_to_remote;
		};

	  private:
		std::mutex m_stateMutex;
		std::filesystem::path m_spoolDirectory;
		std::unordered_map<std::string, std::unique_ptr<Process>> m_processes;
		std::unordered_map<std::string, Channel> m_channels;
	};
}
