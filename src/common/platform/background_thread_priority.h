#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace uam::platform
{
	/// <summary>
	/// Moves the calling worker below the UI poll thread so save, setup and preparation work
	/// cannot starve the poll loop on a loaded host (#647). Weighted priority, not strict:
	/// macOS QoS overrides a lower thread holding a mutex the UI waits on, CFS and the Windows
	/// balance-set manager never starve a lower thread, so shared locks cannot invert indefinitely.
	/// </summary>
	void LowerCurrentThreadToBackgroundPriority() noexcept;

	/// <summary>
	/// Test hook: keeps a finished worker's callable, and so its captures, alive this many
	/// milliseconds before thread exit, proving results never depend on thread teardown (#647).
	/// Must stay 0 outside tests; never set it around code that joins these threads.
	/// </summary>
	inline std::atomic<int> g_background_thread_exit_delay_ms_for_tests{0};

	/// <summary>Starts a detached-style worker thread that runs below UI priority.</summary>
	template <typename Work> std::thread StartBackgroundThread(Work&& work)
	{
		return std::thread([work = std::forward<Work>(work)]() mutable
		{
			LowerCurrentThreadToBackgroundPriority();
			work();
			if (const int delay = g_background_thread_exit_delay_ms_for_tests.load(std::memory_order_relaxed); delay > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(delay));
		});
	}
} // namespace uam::platform
