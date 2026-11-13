#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uam::computer_use
{
	/// Returns the installed helper, copying the embedded one only when its receipt no longer matches.
	/// The copy runs in a detached installer process (`RunComputerUseInstall`) so it survives its caller.
	std::filesystem::path PrepareStandaloneComputerUse(const std::filesystem::path& embedded_app, std::string* error);
	bool IsComputerUseInstallInvocation(const std::vector<std::string>& arguments);
	/// Installer process entry: installs the bundle it runs from. Returns the process exit code.
	int RunComputerUseInstall();
}
