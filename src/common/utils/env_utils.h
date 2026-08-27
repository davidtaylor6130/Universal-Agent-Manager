#pragma once

#include "common/utils/string_utils.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace uam::env
{
	inline bool IsVariableName(std::string_view value)
	{
		if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_'))
		{
			return false;
		}
		for (const char c : value)
		{
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
		}
		return true;
	}

	inline std::optional<std::string> GetNonEmptyString(const char* name)
	{
		if (name == nullptr || name[0] == '\0')
		{
			return std::nullopt;
		}

		const char* value = std::getenv(name);
		if (value == nullptr || value[0] == '\0')
		{
			return std::nullopt;
		}

		return std::string(value);
	}

	inline std::optional<std::string> GetTrimmedString(const char* name)
	{
		const std::optional<std::string> value = GetNonEmptyString(name);
		if (!value)
		{
			return std::nullopt;
		}

		const std::string trimmed = uam::strings::Trim(*value);
		if (trimmed.empty())
		{
			return std::nullopt;
		}
		return trimmed;
	}

	inline std::optional<std::filesystem::path> GetTrimmedPath(const char* name)
	{
		const std::optional<std::string> value = GetTrimmedString(name);
		if (!value)
		{
			return std::nullopt;
		}
		return std::filesystem::path(*value);
	}

	inline std::optional<std::filesystem::path> GetWindowsHomeDrivePath()
	{
		const std::optional<std::string> home_drive = GetTrimmedString("HOMEDRIVE");
		const std::optional<std::string> home_path = GetTrimmedString("HOMEPATH");
		if (!home_drive || !home_path)
		{
			return std::nullopt;
		}
		return std::filesystem::path(*home_drive + *home_path);
	}

	inline std::optional<std::filesystem::path> GetUserHomePath()
	{
#if defined(_WIN32)
		if (const std::optional<std::filesystem::path> user_profile = GetTrimmedPath("USERPROFILE"))
		{
			return user_profile;
		}

		if (const std::optional<std::filesystem::path> home_drive_path = GetWindowsHomeDrivePath())
		{
			return home_drive_path;
		}
#endif

		return GetTrimmedPath("HOME");
	}
} // namespace uam::env
