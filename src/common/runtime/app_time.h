#pragma once

#include <chrono>

/// <summary>
/// Returns the number of seconds elapsed since the first call to this function.
/// Provides a stable monotonic clock for app polling.
/// Thread-safe: the epoch is captured on first call via a local static.
/// </summary>
inline double GetAppTimeSeconds()
{
	static const auto kEpoch = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(now - kEpoch).count();
}
