#pragma once

#include <string>
#include <algorithm>
#include <cctype>

namespace uam::strings
{
	inline std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	inline bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
	{
		if (needle.empty()) return true;
		auto it = std::search(
			haystack.begin(), haystack.end(),
			needle.begin(), needle.end(),
			[](unsigned char ch1, unsigned char ch2) {
				return std::tolower(ch1) == std::tolower(ch2);
			}
		);
		return it != haystack.end();
	}
}
