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

	// Chromium feature that runs the Windows network service in an AppContainer at low integrity. Chromium leaves it off by
	// default (Chrome turns it on only through a field trial), so UAM enables it in the Windows browser process.
	inline constexpr const char* kNetworkServiceSandboxFeature = "NetworkServiceSandbox";

	// Adds a feature to a comma-separated Chromium feature list such as an --enable-features value, keeping existing entries.
	// Returns the list unchanged when an entry already names the feature, with or without a "*" prefix or a trial/parameter suffix.
	std::string AddFeatureToList(const std::string& feature_list, const std::string& feature);
}
