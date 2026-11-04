#pragma once

#include <filesystem>
#include <optional>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <pwd.h>
#include <unistd.h>
#include <vector>
#endif

namespace uam::platform
{
	/// <summary>Native CLI home semantics without mutating process environment.</summary>
	inline std::optional<std::filesystem::path> ResolveNativeUserHomePath(const std::string& home, const std::string& user_profile)
	{
#if defined(_WIN32)
		if (!user_profile.empty()) return std::filesystem::u8path(user_profile);
		wchar_t directory[MAX_PATH]{};
		if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, SHGFP_TYPE_CURRENT, directory))) return std::filesystem::path(directory);
#else
		(void)user_profile;
		if (!home.empty()) return std::filesystem::u8path(home);
		for (std::size_t size = 4096; size <= 64 * 1024; size *= 2)
		{
			std::vector<char> buffer(size);
			passwd entry{}; passwd* result = nullptr;
			const int status = getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &result);
			if (status == 0 && result != nullptr && entry.pw_dir != nullptr && *entry.pw_dir != '\0') return std::filesystem::path(entry.pw_dir);
			if (status != ERANGE) break;
		}
#endif
		return std::nullopt;
	}
}
