#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace uam::codex
{
	inline bool IsUuidHex(const char ch)
	{
		return (ch >= '0' && ch <= '9') ||
		       (ch >= 'a' && ch <= 'f') ||
		       (ch >= 'A' && ch <= 'F');
	}

	inline bool IsCanonicalUuid(std::string_view value)
	{
		if (value.size() != 36)
		{
			return false;
		}

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const bool hyphen_position = i == 8 || i == 13 || i == 18 || i == 23;
			if (hyphen_position)
			{
				if (value[i] != '-')
				{
					return false;
				}
			}
			else if (!IsUuidHex(value[i]))
			{
				return false;
			}
		}

		return true;
	}

	inline bool IsValidThreadId(std::string_view value)
	{
		constexpr std::string_view urn_prefix = "urn:uuid:";
		if (value.rfind(urn_prefix, 0) == 0)
		{
			value.remove_prefix(urn_prefix.size());
		}
		return IsCanonicalUuid(value);
	}

	inline std::string TrimAsciiWhitespace(const std::string& value)
	{
		const std::size_t start = value.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
		{
			return "";
		}
		const std::size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	inline std::string ValidThreadIdOrEmpty(const std::string& value)
	{
		const std::string trimmed = TrimAsciiWhitespace(value);
		return IsValidThreadId(trimmed) ? trimmed : std::string{};
	}

	inline bool ErrorLooksLikeInvalidThreadId(const std::string& message)
	{
		std::string lowered;
		lowered.reserve(message.size());
		for (const unsigned char ch : message)
		{
			lowered.push_back(static_cast<char>(std::tolower(ch)));
		}

		return lowered.find("invalid thread id") != std::string::npos ||
		       lowered.find("urn:uuid") != std::string::npos;
	}
} // namespace uam::codex
