#pragma once

#include <cstdint>
#include <optional>

namespace uam::platform
{
	/// <summary>
	/// One reading of how busy a computer is. Every figure is optional: a figure this platform or an older
	/// helper cannot report stays empty and means "unknown", never zero or idle.
	/// </summary>
	struct HostLoad
	{
		/// Logical processors online.
		std::optional<std::uint32_t> cores;
		/// One-minute run-queue load average (macOS and Linux).
		std::optional<double> load_average_1m;
		/// Share of all processors busy since the previous reading, 0 to 100 (Windows, which has no load average).
		std::optional<double> cpu_percent;
		std::optional<std::uint64_t> memory_available_bytes;
		std::optional<std::uint64_t> memory_total_bytes;
		/// Live processes the UAM helper on that computer is running for every desktop connected to it.
		/// The local reading leaves this empty; the app counts its own processes.
		std::optional<std::uint32_t> runner_processes;

		bool operator==(const HostLoad&) const = default;
	};

	/// <summary>
	/// Reads this computer's processor count, load (load average, or CPU use on Windows) and memory.
	/// Cheap enough for a background thread every few seconds; never call it on the UI thread, because the
	/// first Windows reading waits a quarter second to measure CPU use.
	/// </summary>
	HostLoad ReadCurrentHostLoad() noexcept;
}
