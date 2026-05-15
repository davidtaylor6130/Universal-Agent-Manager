#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace uam::codex
{
	inline constexpr auto kUuidHyphenPositions = std::to_array<std::size_t>({8, 13, 18, 23});
	inline constexpr std::string_view kUuidUrnPrefix = "urn:uuid:";
	inline constexpr auto kInvalidThreadIdErrorMarkers = std::to_array<std::string_view>({"invalid thread id", "urn:uuid"});

	inline bool IsUuidHex(char ch)
	{
		return uam::strings::IsAsciiHexDigit(static_cast<unsigned char>(ch));
	}

	inline bool IsUuidHyphenPosition(std::size_t index)
	{
		return uam::ranges::Contains(kUuidHyphenPositions, index);
	}

	inline bool IsCanonicalUuid(std::string_view value)
	{
		if (value.size() != 36)
		{
			return false;
		}

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			if (IsUuidHyphenPosition(i))
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
		if (uam::strings::StartsWith(value, kUuidUrnPrefix))
		{
			value.remove_prefix(kUuidUrnPrefix.size());
		}
		return IsCanonicalUuid(value);
	}

	inline std::string ValidThreadIdOrEmpty(std::string_view value)
	{
		const std::string trimmed = uam::strings::Trim(value);
		return IsValidThreadId(trimmed) ? trimmed : std::string{};
	}

	inline bool ErrorLooksLikeInvalidThreadId(std::string_view message)
	{
		return uam::strings::ContainsAnyCaseInsensitive(message, kInvalidThreadIdErrorMarkers);
	}
} // namespace uam::codex
