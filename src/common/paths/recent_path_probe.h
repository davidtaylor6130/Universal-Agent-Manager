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
	/// <summary>The display readers that share the memo; each has its own key space and probe count.</summary>
	enum class RecentPathProbeKind { CatalogWorkspace, WorkspaceOwnership };

	/// <summary>Number of disk probes RecentPathProbe ran for a reader, so tests can prove which pushes probe.</summary>
	inline std::atomic<std::size_t>& RecentPathProbeCount(RecentPathProbeKind kind)
	{
		static std::atomic<std::size_t> counts[2];
		return counts[static_cast<std::size_t>(kind)];
	}

	/// <summary>Clock the memo reads, so tests can step its expiry; nullptr restores the real clock.</summary>
	inline std::chrono::steady_clock::time_point (*&RecentPathProbeClock())()
	{
		static std::chrono::steady_clock::time_point (*clock)() = &std::chrono::steady_clock::now;
		return clock;
	}

	inline void SetRecentPathProbeClockForTests(std::chrono::steady_clock::time_point (*clock)())
	{
		RecentPathProbeClock() = clock != nullptr ? clock : &std::chrono::steady_clock::now;
	}

	/// <summary>
	/// Reuses a disk-probing path result (a canonical path, an ownership key) for two to three seconds per key.
	/// Only for display code that runs on every state push: there each scope otherwise costs a realpath on the UI thread.
	/// The probes still run on that thread when an entry expires; expiry is spread per key so one push does not
	/// repeat every probe at once. A symlink retargeted, or a folder created, inside the window is shown under its
	/// previous resolution until the entry expires; code that writes, launches or decides ownership must keep probing directly.
	/// </summary>
	inline std::string RecentPathProbe(RecentPathProbeKind kind, const std::string& path, const std::function<std::string()>& probe)
	{
		const std::string key = (kind == RecentPathProbeKind::CatalogWorkspace ? "catalog\n" : "ownership\n") + path;
		struct Entry
		{
			std::chrono::steady_clock::time_point probed_at;
			std::string value;
		};
		static std::mutex mutex;
		static std::unordered_map<std::string, Entry> cache;
		const std::chrono::steady_clock::time_point now = RecentPathProbeClock()();
		// Entries written by one push would otherwise all expire in the same later push.
		const std::chrono::milliseconds lifetime{2000 + static_cast<long long>(std::hash<std::string>{}(key) % 1000)};
		{
			const std::lock_guard<std::mutex> lock(mutex);
			if (const auto found = cache.find(key); found != cache.end() && now - found->second.probed_at < lifetime)
				return found->second.value;
		}
		std::string value = probe();
		++RecentPathProbeCount(kind);
		const std::lock_guard<std::mutex> lock(mutex);
		if (cache.size() >= 4096) cache.clear(); // ponytail: wholesale reset bounds the map; LRU only if scope counts grow.
		cache[key] = Entry{now, value};
		return value;
	}
} // namespace uam::paths
