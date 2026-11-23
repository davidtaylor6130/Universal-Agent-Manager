#pragma once

#include <filesystem>
#include <string>

namespace uam::platform
{
	/// Environment variable through which UAM's own helper launches name their working directory on Windows. CEF's
	/// bootstrap.exe moves every non-CEF process into the executable's folder before UAM's code runs; the entry point
	/// returns to this directory and removes the variable so it never reaches grandchildren.
	inline constexpr char kLaunchWorkingDirectoryVariable[] = "UAM_LAUNCH_WORKING_DIRECTORY";

#if defined(_WIN32)
	/// <summary>
	/// Ensures the folder holding CEF's binaries grants read and execute to ALL RESTRICTED APPLICATION PACKAGES
	/// (S-1-15-2-2), inherited by its files and folders. Chromium's LPAC-sandboxed processes need this to load
	/// CEF; ZIP extraction does not keep it. Returns false with a reason when the grant is missing and could not
	/// be added.
	/// </summary>
	bool EnsureCefSandboxFolderAccess(const std::filesystem::path& folder, std::string& error);
#else
	/// No folder grant is needed outside Windows.
	inline bool EnsureCefSandboxFolderAccess(const std::filesystem::path&, std::string&)
	{
		return true;
	}
#endif
}
