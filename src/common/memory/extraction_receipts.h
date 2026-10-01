#pragma once
#include "common/utils/string_utils.h"
#include <algorithm>
#include <string>
#include <vector>

namespace uam::memory
{
	inline std::vector<std::string> ExistingExtractionKeys(const std::string& text)
	{
		std::vector<std::string> keys;
		constexpr std::string_view marker = "Extraction key: ";
		for (std::size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + marker.size()))
		{
			const std::size_t start = at + marker.size();
			const std::size_t end = text.find_first_of("\r\n", start);
			const std::string key = strings::Trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (!key.empty() && std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
		}
		return keys;
	}
}
