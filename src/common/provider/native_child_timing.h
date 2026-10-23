#pragma once

#include "common/utils/nlohmann_json_utils.h"

#include <cstdint>
#include <optional>

namespace uam::native_agents
{

// Optional child-local wire duration, shared by native adapters. Missing or
// malformed evidence stays unknown, including values unsafe in the frontend.
inline std::optional<std::int64_t> ReportedDurationMilliseconds(const nlohmann::json& object, std::string_view key)
{
	const auto* duration = nlohmann_json::FindField(object, key);
	if (duration == nullptr || !duration->is_number_integer()) return std::nullopt;
	constexpr std::int64_t max_clock = 8'640'000'000'000'000LL;
	if (duration->is_number_unsigned())
	{
		const auto value = duration->get<std::uint64_t>();
		return value <= static_cast<std::uint64_t>(max_clock) ? std::optional<std::int64_t>(static_cast<std::int64_t>(value)) : std::nullopt;
	}
	const auto value = duration->get<std::int64_t>();
	return value >= 0 && value <= max_clock ? std::optional<std::int64_t>(value) : std::nullopt;
}

}
