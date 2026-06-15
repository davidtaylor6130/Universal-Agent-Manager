#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/utils/string_utils.h"

namespace uam::ranges
{
	template <typename Range, typename Value> inline bool Contains(const Range& range, const Value& value)
	{
		return std::ranges::find(range, value) != std::ranges::end(range);
	}

	inline bool PushUniqueNonEmptyString(std::vector<std::string>& values, std::string_view value)
	{
		if (value.empty() || Contains(values, value))
		{
			return false;
		}

		values.emplace_back(value.data(), value.size());
		return true;
	}

	inline bool PushUniqueTrimmedNonEmptyString(std::vector<std::string>& values, std::string_view value)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(value);
		return PushUniqueNonEmptyString(values, trimmed);
	}

	inline bool InsertTrimmedNonEmptyString(std::unordered_set<std::string>& values, std::string_view value)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(value);
		if (trimmed.empty())
		{
			return false;
		}

		return values.emplace(trimmed.data(), trimmed.size()).second;
	}
} // namespace uam::ranges
