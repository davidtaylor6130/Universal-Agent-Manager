#include "cef/uam_cef_command_line_config.h"

namespace uam::cef
{

std::string MacOsWebAppShortcutCrashDisabledFeatures()
{
	return
		"WebAppEnableShortcuts,"
		"DesktopPWADeterminedInstalledByOsIntegration,"
		"WebAppSystemMediaControlsWin,"
		"WebAppEnableOsIntegrationSubManagers,"
		"DesktopPWAsRunOnOsLogin,"
		"DesktopPWAsWithoutExtensions,"
		"DesktopPWAsSubApps,"
		"WebAppEnableLinkCapturing,"
		"WebAppUniversalInstall";
}

} // namespace uam::cef
