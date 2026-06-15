#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace uam::codex
{
	inline constexpr auto kReasoningEfforts = std::to_array<std::string_view>({
	    "none",
	    "minimal",
	    "low",
	    "medium",
	    "high",
	    "xhigh",
	});

	inline constexpr auto kServiceTiers = std::to_array<std::string_view>({
	    "fast",
	    "flex",
	});

	template <std::size_t N> inline bool IsAllowedOption(std::string_view value, const std::array<std::string_view, N>& allowed_values)
	{
		return uam::ranges::Contains(allowed_values, value);
	}

	template <std::size_t N> inline std::string NormalizeAllowedOption(std::string_view value, const std::array<std::string_view, N>& allowed_values)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(value);
		const std::optional<std::string_view> allowed_value = uam::strings::FindEqualIgnoreCase(allowed_values, trimmed);
		return allowed_value ? std::string(*allowed_value) : std::string();
	}

	inline bool IsReasoningEffort(std::string_view value)
	{
		return IsAllowedOption(value, kReasoningEfforts);
	}

	inline std::string NormalizeReasoningEffort(std::string_view value)
	{
		return NormalizeAllowedOption(value, kReasoningEfforts);
	}

	inline bool IsServiceTier(std::string_view value)
	{
		return IsAllowedOption(value, kServiceTiers);
	}

	inline std::string NormalizeServiceTier(std::string_view value)
	{
		return NormalizeAllowedOption(value, kServiceTiers);
	}
} // namespace uam::codex
