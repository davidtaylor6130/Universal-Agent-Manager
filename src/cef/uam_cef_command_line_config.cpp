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

std::string AddFeatureToList(const std::string& feature_list, const std::string& feature)
{
	std::string_view rest(feature_list);
	while (!rest.empty())
	{
		const size_t comma = rest.find(',');
		std::string_view entry = rest.substr(0, comma);
		rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
		// Chromium's entry form is [*]Name[<Trial][:param/value...], with surrounding whitespace ignored.
		const size_t first = entry.find_first_not_of(" \t");
		entry = first == std::string_view::npos ? std::string_view() : entry.substr(first);
		if (!entry.empty() && entry.front() == '*')
		{
			entry.remove_prefix(1);
		}
		entry = entry.substr(0, entry.find_first_of("<: \t"));
		if (entry == std::string_view(feature))
		{
			return feature_list;
		}
	}
	if (feature_list.find_first_not_of(" \t,") == std::string::npos)
	{
		return feature;
	}
	return feature_list.back() == ',' ? feature_list + feature : feature_list + "," + feature;
}

} // namespace uam::cef
