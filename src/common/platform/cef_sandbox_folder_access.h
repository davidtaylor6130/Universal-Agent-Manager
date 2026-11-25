#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::platform
{
	/// Environment variable through which UAM's own helper launches name their working directory on Windows. CEF's
	/// bootstrap.exe moves every non-CEF process into the executable's folder before UAM's code runs; the entry point
	/// returns to this directory and removes the variable so it never reaches grandchildren.
	inline constexpr char kLaunchWorkingDirectoryVariable[] = "UAM_LAUNCH_WORKING_DIRECTORY";

#if defined(_WIN32)
	/// True on Windows: CefInitialize must receive the sandbox information that CEF's bootstrap.exe passes in.
	inline constexpr bool kCefSandboxInfoRequired = true;

	/// <summary>
	/// True when Chromium started this process as a CEF subprocess: an unquoted --type= switch before any "--"
	/// terminator. Pure string inspection, so a sandboxed subprocess can call it before loading any library.
	/// </summary>
	bool IsCefSubprocessCommandLine(std::wstring_view command_line);

	/// <summary>
	/// Adds kLaunchWorkingDirectoryVariable to a child's environment when the child is UAM's own executable and a
	/// working directory was chosen. Every Windows launcher that can start UAM itself calls this.
	/// </summary>
	void AddLaunchWorkingDirectory(std::vector<std::pair<std::string, std::string>>& environment, std::string_view child_executable_utf8,
	    const std::filesystem::path& working_directory, const std::filesystem::path& current_executable);

	/// AddLaunchWorkingDirectory against this process's own executable.
	void AddLaunchWorkingDirectory(std::vector<std::pair<std::string, std::string>>& environment, std::string_view child_executable_utf8,
	    const std::filesystem::path& working_directory);

	/// Which parts of the LPAC folder grant an access control list already gives ALL RESTRICTED APPLICATION PACKAGES.
	struct CefSandboxFolderGrant
	{
		bool folder = false;     // read and execute on the folder itself
		bool files = false;      // inherited by files (object inherit)
		bool subfolders = false; // inherited by subfolders such as locales (container inherit)

		bool Complete() const
		{
			return folder && files && subfolders;
		}
	};

	/// Evaluates a folder DACL (PACL) for the S-1-15-2-2 grant, counting entries split across folder-only and
	/// inherit-only entries, as Program Files uses. A null DACL grants everything.
	CefSandboxFolderGrant EvaluateCefSandboxFolderGrant(const void* dacl);

	/// <summary>
	/// Ensures the folder holding CEF's binaries grants read and execute to ALL RESTRICTED APPLICATION PACKAGES
	/// (S-1-15-2-2), inherited by its files and folders. Chromium's AppContainer and LPAC sandboxed processes need this to load
	/// CEF; ZIP extraction does not keep it. UAM adds it only to a folder that holds libcef.dll and
	/// universal_agent_manager.dll and is not the user's profile, Desktop, Documents or Downloads folder. Returns
	/// false with a reason when the grant is missing and was not added.
	/// </summary>
	bool EnsureCefSandboxFolderAccess(const std::filesystem::path& folder, std::string& error);
#else
	inline constexpr bool kCefSandboxInfoRequired = false;

	/// No folder grant is needed outside Windows.
	inline bool EnsureCefSandboxFolderAccess(const std::filesystem::path&, std::string&)
	{
		return true;
	}
#endif
}
