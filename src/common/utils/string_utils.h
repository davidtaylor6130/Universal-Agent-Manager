#pragma once

#include <string>

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
}
