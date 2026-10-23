#pragma once
#include <optional>
#include <string>
#include <vector>

namespace uam::platform
{
	/// Reserved for a supervised GUI's explicit restart after normal shutdown.
	inline constexpr int kApplicationRestartExitCode = 75;

	/// Accept one restart request only in a GUI child attached to its supervisor.
	/// Returns false in service/subprocess/unsupervised contexts or after acceptance.
	bool RequestApplicationRestart();
	bool ApplicationRestartRequested();

	/// Run the GUI in a child process so fatal crashes can be recovered outside it.
	/// CEF children and service invocations bypass this entry point in main.
	std::optional<int> RunAppCrashSupervisor(const std::vector<std::string>& arguments);
}
