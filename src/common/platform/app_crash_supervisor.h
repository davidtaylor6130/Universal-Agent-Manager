#pragma once
#include <optional>
#include <string>
#include <vector>

namespace uam::platform
{
	/// Run the GUI in a child process so fatal crashes can be recovered outside it.
	/// CEF children and service invocations bypass this entry point in main.
	std::optional<int> RunAppCrashSupervisor(const std::vector<std::string>& arguments);
}
