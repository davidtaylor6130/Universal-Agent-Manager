#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace uam::platform
{
	inline constexpr const char* kMacParentDeathWatchdogArgument = "--uam-mac-parent-death-watchdog";
	std::optional<int> RunMacParentDeathWatchdogIfRequested(int argc, char* argv[]);
	bool InitializeMacApplication();
	bool BrowsePath(bool choose_directory, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr);
	bool OpenExternalUrl(const std::string& url, std::string* error_out = nullptr);
	bool OpenPath(const std::filesystem::path& path, std::string* error_out = nullptr);
	bool OpenPathWithApplication(const std::filesystem::path& path, const std::string& application_bundle_id, std::string* error_out = nullptr);
	bool RevealPath(const std::filesystem::path& path, std::string* error_out = nullptr);
}
