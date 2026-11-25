#pragma once

#include <string>
#include <vector>

namespace uam::cef
{
	std::string MacOsWebAppShortcutCrashDisabledFeatures();

	// Test-only opt-in for Chromium's remote debugging switches. Only the exact value "1" enables it; scripts/perf_driven.py sets it.
	inline constexpr const char* kTestRemoteDebuggingEnv = "UAM_TEST_ALLOW_REMOTE_DEBUGGING";

	// Command-line switches (names without leading dashes) that CEF must not see. Remote debugging switches are dropped unless
	// the test variable is "1"; remote-debugging-address is always dropped so an allowed port stays on Chromium's 127.0.0.1 default.
	std::vector<std::string> RemoteDebuggingSwitchesToDrop(const std::vector<std::string>& switch_names, const char* test_env_value);
}
