#pragma once

#include <string>
#include <string_view>

namespace uam::remote
{
	/// Enables byte-preserving CRT input/output for framed runner bridges.
	bool EnableRunnerBinaryStdio();
	/// The named pipe this user's service of that version listens on; empty when it cannot be derived.
	std::wstring RunnerServicePipeName(std::string_view runner_version);
	int RunRunnerService(std::string_view runner_version);
	int StartRunnerService(std::string_view runner_version);
	int StopRunnerService(std::string_view runner_version);
	int RunRunnerBridge(std::string_view runner_version);
}
