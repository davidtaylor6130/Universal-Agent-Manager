#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/string_utils.h"

#include <filesystem>
#include <string>

namespace uam::platform
{
	/// Validate a custom explorer as a native application, never a shell command.
	inline bool IsFileExplorerApplication(const std::string& application, bool windows)
	{
		if (application.empty()) return true;
		if (application.size() > 4096 || application.find_first_of("\r\n") != std::string::npos || application.find('\0') != std::string::npos) return false;
		const std::filesystem::path path = uam::paths::PathFromUtf8(application);
		if (!path.is_absolute()) return false;
		const std::string extension = uam::strings::ToLowerAscii(uam::paths::Utf8PathString(path.extension()));
		std::error_code error;
		return windows ? extension == ".exe" && std::filesystem::is_regular_file(path, error)
		               : extension == ".app" && std::filesystem::is_directory(path, error);
	}
}
