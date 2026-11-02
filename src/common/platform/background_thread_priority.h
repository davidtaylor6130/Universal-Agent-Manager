#pragma once

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

	/// <summary>Starts a detached-style worker thread that runs below UI priority.</summary>
	template <typename Work> std::thread StartBackgroundThread(Work&& work)
	{
		return std::thread([work = std::forward<Work>(work)]() mutable
		{
			LowerCurrentThreadToBackgroundPriority();
			work();
		});
	}
} // namespace uam::platform
