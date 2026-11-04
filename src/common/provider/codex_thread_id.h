#pragma once

#include "common/utils/uuid.h"
#include "common/utils/string_utils.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace uam::codex
{
	inline constexpr std::string_view kUuidUrnPrefix = "urn:uuid:";
	inline constexpr auto kInvalidThreadIdErrorMarkers = std::to_array<std::string_view>({"invalid thread id", "no rollout found for thread id", "urn:uuid"});

	inline bool IsValidThreadId(std::string_view value)
	{
		if (uam::strings::StartsWith(value, kUuidUrnPrefix))
		{
			value.remove_prefix(kUuidUrnPrefix.size());
		}
		return uam::uuid::IsCanonicalUuid(value);
	}

	inline std::string ValidThreadIdOrEmpty(std::string_view value)
	{
		const std::string trimmed = uam::strings::Trim(value);
		return IsValidThreadId(trimmed) ? trimmed : std::string{};
	}

	/// <summary>Compares native UUID and URN aliases without changing their returned identity.</summary>
	inline std::string ThreadIdKey(std::string_view value)
	{
		std::string key = ValidThreadIdOrEmpty(value);
		if (key.starts_with(kUuidUrnPrefix)) key.erase(0, kUuidUrnPrefix.size());
		return uam::strings::ToLowerAscii(key);
	}

	inline bool ErrorLooksLikeInvalidThreadId(std::string_view message)
	{
		return uam::strings::ContainsAnyCaseInsensitive(message, kInvalidThreadIdErrorMarkers);
	}
} // namespace uam::codex
