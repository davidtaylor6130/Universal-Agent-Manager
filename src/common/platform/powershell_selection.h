#pragma once

#include <string>
#include <string_view>

namespace uam::powershell
{
	inline constexpr std::string_view kAuto = "auto";
	inline constexpr std::string_view kWindowsPowerShell = "windows-powershell";
	inline constexpr std::string_view kPowerShell7 = "pwsh";

	inline bool IsPreference(std::string_view value)
	{
		return value == kAuto || value == kWindowsPowerShell || value == kPowerShell7;
	}

	/// <summary>Pick the PowerShell executable for a Windows preference; empty with a user-facing error when the choice is missing.</summary>
	/// <remarks>Auto prefers built-in Windows PowerShell 5.1 because every UAM script targets it; PowerShell 7 is used only when selected or when 5.1 is absent.</remarks>
	inline std::string Select(std::string_view preference, bool windows_powershell_available, bool pwsh_available, std::string* error_out = nullptr)
	{
		const auto fail = [error_out](const char* message)
		{
			if (error_out != nullptr) *error_out = message;
			return std::string();
		};
		if (preference == kPowerShell7)
		{
			return pwsh_available ? std::string("pwsh.exe") : fail("PowerShell 7 (pwsh.exe) was not found. Install PowerShell 7 or choose Windows PowerShell in Settings > Editors.");
		}
		if (preference == kWindowsPowerShell)
		{
			return windows_powershell_available ? std::string("powershell.exe") : fail("Windows PowerShell (powershell.exe) was not found. Choose PowerShell 7 or Automatic in Settings > Editors.");
		}
		if (windows_powershell_available) return "powershell.exe";
		if (pwsh_available) return "pwsh.exe";
		return fail("No PowerShell was found. Windows PowerShell (powershell.exe) or PowerShell 7 (pwsh.exe) is required to open this terminal.");
	}
}
