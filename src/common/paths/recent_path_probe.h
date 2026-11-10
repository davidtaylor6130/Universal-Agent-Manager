#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace uam::paths
{
	/// <summary>Number of disk probes RecentPathProbe ran, so tests can prove a repeated state push performs none.</summary>
	inline std::atomic<std::size_t>& RecentPathProbeCount()
	{
		static std::atomic<std::size_t> count{0};
		return count;
	}

	/// <summary>
	/// Reuses a disk-probing path result (a canonical path, an ownership key) for two seconds per key.
	/// Only for display code that runs on every state push: there each scope otherwise costs a realpath on the UI thread.
	/// A symlink retargeted in that window is shown under its previous target until the next push after it;
	/// code that writes, launches or decides ownership must keep probing directly.
	/// </summary>
	inline std::string RecentPathProbe(const std::string& key, const std::function<std::string()>& probe)
	{
		struct Entry
		{
			std::chrono::steady_clock::time_point probed_at;
			std::string value;
		};
		static std::mutex mutex;
		static std::unordered_map<std::string, Entry> cache;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		{
			const std::lock_guard<std::mutex> lock(mutex);
			if (const auto found = cache.find(key); found != cache.end() && now - found->second.probed_at < std::chrono::seconds(2))
				return found->second.value;
		}
		std::string value = probe();
		++RecentPathProbeCount();
		const std::lock_guard<std::mutex> lock(mutex);
		if (cache.size() >= 4096) cache.clear(); // ponytail: wholesale reset bounds the map; LRU only if scope counts grow.
		cache[key] = Entry{now, value};
		return value;
	}
} // namespace uam::paths
