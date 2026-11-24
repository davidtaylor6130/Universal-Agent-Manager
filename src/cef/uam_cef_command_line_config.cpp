#include "cef/uam_cef_command_line_config.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace uam::cef
{

std::string MacOsWebAppShortcutCrashDisabledFeatures()
{
	return
		"WebAppEnableShortcuts,"
		"WebAppShortcuts,"
		"WebAppShortcutCopier,"
		"DesktopPWADeterminedInstalledByOsIntegration,"
		"WebAppSystemMediaControlsWin,"
		"WebAppEnableOsIntegrationSubManagers,"
		"DesktopPWAsRunOnOsLogin,"
		"DesktopPWAsWithoutExtensions,"
		"DesktopPWAsSubApps,"
		"WebAppEnableLinkCapturing,"
		"WebAppUniversalInstall";
}

namespace
{
	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}
} // namespace

std::vector<std::string> RemoteDebuggingSwitchesToDrop(const std::vector<std::string>& switch_names, const char* test_env_value)
{
	const bool test_run = test_env_value != nullptr && std::string_view(test_env_value) == "1";
	std::vector<std::string> dropped;
	for (const std::string& name : switch_names)
	{
		const std::string lower = LowerAscii(name);
		if (lower == "remote-debugging-address")
		{
			dropped.push_back(name);
		}
		else if (!test_run && (lower.rfind("remote-debugging-", 0) == 0 || lower == "remote-allow-origins"))
		{
			dropped.push_back(name);
		}
	}
	return dropped;
}

} // namespace uam::cef
