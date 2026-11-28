#pragma once

#include "common/models/app_models.h"

#include <algorithm>
#include <cstdint>

#include <nlohmann/json.hpp>

// Settings for host admission (see src/common/runtime/host_admission.h): the local computer's own slot limit,
// the limit for remote computers without a routing slot count, the background budget and the load thresholds.
namespace uam::host_admission_config
{
	inline constexpr int kMaxSlotLimit = 256;
	/// A small budget by design (the default is 2); a busy machine is protected by the load limits too.
	inline constexpr int kMaxBackgroundBudget = 4;
	inline constexpr int kMaxHardCpuSeconds = 300;

	/// <summary>The local default: every processor but two, so the interface and the system keep room. At least one;
	/// when the processor count is unknown (reported as 0), a conservative four.</summary>
	inline int AutomaticLocalSlotLimit(unsigned cores)
	{
		if (cores == 0) return 4;
		return cores > 3 ? static_cast<int>(std::min<unsigned>(cores - 2, kMaxSlotLimit)) : 1;
	}

	inline int LocalSlotLimit(const HostAdmissionSettings& settings, unsigned cores)
	{
		return settings.local_slot_limit > 0 ? settings.local_slot_limit : AutomaticLocalSlotLimit(cores);
	}

	/// <summary>Hard limits must be at least as strict as soft ones: more CPU, less free memory.</summary>
	inline bool IsValid(const HostAdmissionSettings& settings)
	{
		return settings.local_slot_limit >= 0 && settings.local_slot_limit <= kMaxSlotLimit &&
		    settings.remote_slot_limit >= 1 && settings.remote_slot_limit <= kMaxSlotLimit &&
		    settings.background_budget >= 1 && settings.background_budget <= kMaxBackgroundBudget &&
		    settings.soft_cpu_percent >= 10 && settings.soft_cpu_percent <= 100 &&
		    settings.hard_cpu_percent >= settings.soft_cpu_percent && settings.hard_cpu_percent <= 100 &&
		    settings.hard_cpu_seconds >= 1 && settings.hard_cpu_seconds <= kMaxHardCpuSeconds &&
		    settings.soft_free_memory_percent >= 1 && settings.soft_free_memory_percent <= 90 &&
		    settings.hard_free_memory_percent >= 1 && settings.hard_free_memory_percent <= settings.soft_free_memory_percent;
	}

	/// <summary>Clamps each value into range; thresholds that contradict each other fall back to the defaults.</summary>
	inline void Normalize(HostAdmissionSettings& settings)
	{
		const HostAdmissionSettings defaults;
		settings.local_slot_limit = std::clamp(settings.local_slot_limit, 0, kMaxSlotLimit);
		settings.remote_slot_limit = std::clamp(settings.remote_slot_limit, 1, kMaxSlotLimit);
		settings.background_budget = std::clamp(settings.background_budget, 1, kMaxBackgroundBudget);
		settings.hard_cpu_seconds = std::clamp(settings.hard_cpu_seconds, 1, kMaxHardCpuSeconds);
		if (!IsValid(settings))
		{
			settings.soft_cpu_percent = defaults.soft_cpu_percent;
			settings.hard_cpu_percent = defaults.hard_cpu_percent;
			settings.soft_free_memory_percent = defaults.soft_free_memory_percent;
			settings.hard_free_memory_percent = defaults.hard_free_memory_percent;
		}
	}

	inline nlohmann::json Serialize(const HostAdmissionSettings& settings)
	{
		return {{"localSlotLimit", settings.local_slot_limit}, {"remoteSlotLimit", settings.remote_slot_limit}, {"backgroundBudget", settings.background_budget},
		    {"softCpuPercent", settings.soft_cpu_percent}, {"softFreeMemoryPercent", settings.soft_free_memory_percent},
		    {"hardCpuPercent", settings.hard_cpu_percent}, {"hardCpuSeconds", settings.hard_cpu_seconds}, {"hardFreeMemoryPercent", settings.hard_free_memory_percent}};
	}

	/// <summary>Reads settings without normalizing; a missing or non-integer field keeps its default.</summary>
	inline HostAdmissionSettings Parse(const nlohmann::json& value)
	{
		HostAdmissionSettings settings;
		if (!value.is_object()) return settings;
		const auto read = [&value](const char* key, int& target)
		{
			const nlohmann::json::const_iterator found = value.find(key);
			if (found != value.end() && found->is_number_integer()) target = static_cast<int>(std::clamp<std::int64_t>(found->get<std::int64_t>(), -1, 100000));
		};
		read("localSlotLimit", settings.local_slot_limit);
		read("remoteSlotLimit", settings.remote_slot_limit);
		read("backgroundBudget", settings.background_budget);
		read("softCpuPercent", settings.soft_cpu_percent);
		read("softFreeMemoryPercent", settings.soft_free_memory_percent);
		read("hardCpuPercent", settings.hard_cpu_percent);
		read("hardCpuSeconds", settings.hard_cpu_seconds);
		read("hardFreeMemoryPercent", settings.hard_free_memory_percent);
		return settings;
	}
}
