#include "cef/uam_cef_command_line_config.h"

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

} // namespace uam::cef
