#pragma once

#include <string_view>

namespace uam::remote
{
	int RunRunnerService(std::string_view runner_version);
	int StartRunnerService(std::string_view runner_version);
	int StopRunnerService(std::string_view runner_version);
	int RunRunnerBridge(std::string_view runner_version);
}
