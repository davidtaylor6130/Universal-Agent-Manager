#pragma once

#include "common/platform/host_load.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace uam::remote
{
	/// <summary>
	/// Version of the `host.load` request, advertised as the `hostLoad` capability in `hello`. A helper without
	/// the capability (older helpers, or a context without process state) has unknown load, which callers must
	/// never treat as idle. A newer version may add fields; readers ignore fields they do not know.
	/// </summary>
	inline constexpr int kHostLoadVersion = 1;

	/// <summary>Wire form of a `host.load` result. A figure the helper could not read is sent as null.</summary>
	inline nlohmann::json HostLoadToJson(const uam::platform::HostLoad& load)
	{
		const auto optional = [](const auto& value) { return value ? nlohmann::json(*value) : nlohmann::json(nullptr); };
		return {{"version", kHostLoadVersion}, {"cores", optional(load.cores)}, {"loadAverage1m", optional(load.load_average_1m)},
		    {"cpuPercent", optional(load.cpu_percent)}, {"memoryAvailableBytes", optional(load.memory_available_bytes)},
		    {"memoryTotalBytes", optional(load.memory_total_bytes)}, {"runnerProcesses", optional(load.runner_processes)}};
	}

	/// <summary>
	/// Reads a `host.load` result. A missing or null figure stays unknown; a present figure of the wrong type or
	/// outside a plausible range rejects the whole result, so a damaged reply never reads as a quiet machine.
	/// </summary>
	inline bool HostLoadFromJson(const nlohmann::json& result, uam::platform::HostLoad& load)
	{
		load = {};
		if (!result.is_object() || !result.contains("version") || !result["version"].is_number_integer() || result["version"].get<std::int64_t>() < 1) return false;
		const auto read_count = [&result](const char* key, auto& target, std::uint64_t limit) -> bool
		{
			const nlohmann::json::const_iterator found = result.find(key);
			if (found == result.end() || found->is_null()) return true;
			if (!found->is_number_unsigned() && !(found->is_number_integer() && found->get<std::int64_t>() >= 0)) return false;
			const std::uint64_t value = found->get<std::uint64_t>();
			if (value > limit) return false;
			target = static_cast<typename std::remove_reference_t<decltype(target)>::value_type>(value);
			return true;
		};
		const auto read_ratio = [&result](const char* key, std::optional<double>& target, double limit) -> bool
		{
			const nlohmann::json::const_iterator found = result.find(key);
			if (found == result.end() || found->is_null()) return true;
			if (!found->is_number()) return false;
			const double value = found->get<double>();
			if (!std::isfinite(value) || value < 0.0 || value > limit) return false;
			target = value;
			return true;
		};
		constexpr std::uint64_t kMaxBytes = std::numeric_limits<std::uint64_t>::max();
		if (!read_count("cores", load.cores, 1u << 20U) || !read_count("memoryAvailableBytes", load.memory_available_bytes, kMaxBytes) ||
		    !read_count("memoryTotalBytes", load.memory_total_bytes, kMaxBytes) || !read_count("runnerProcesses", load.runner_processes, 1u << 24U) ||
		    !read_ratio("loadAverage1m", load.load_average_1m, 1.0e6) || !read_ratio("cpuPercent", load.cpu_percent, 100.0))
		{
			load = {};
			return false;
		}
		if (load.cores && *load.cores == 0) load.cores.reset();
		if (load.memory_available_bytes && load.memory_total_bytes && *load.memory_available_bytes > *load.memory_total_bytes)
			load.memory_available_bytes = load.memory_total_bytes;
		return true;
	}
}
