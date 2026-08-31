#pragma once

#include <filesystem>
#include <string_view>

namespace uam::remote
{
	int RunRunnerService(const std::filesystem::path& socket_path,
	                     std::string_view runner_version);
	int StartRunnerService(const std::filesystem::path& socket_path,
	                       std::string_view runner_version);
	int StopRunnerService(const std::filesystem::path& socket_path);
	int RunRunnerBridge(const std::filesystem::path& socket_path);
}
