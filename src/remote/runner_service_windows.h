#pragma once

#include <string_view>

namespace uam::remote
{
	int RunRunnerService(std::string_view runner_version);
	int StartRunnerService(std::string_view runner_version);
	int StopRunnerService();
	int RunRunnerBridge();
}
