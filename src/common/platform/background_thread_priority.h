#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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

	/// <summary>
	/// Priority of one worker that runs below the UI poll thread until a barrier blocks on it (#526).
	/// A lowered worker gets no boost from a future or join wait, so a thread about to block on the
	/// worker (a force, flush, synchronous-save or shutdown barrier) calls RaiseForBarrier first: the
	/// worker returns to the priority it started at (macOS through a QoS override, Windows through its
	/// thread handle, Linux best effort because raising niceness needs privilege). A raise before the
	/// worker starts keeps it from lowering at all. Each object belongs to exactly one worker.
	/// </summary>
	class BarrierRaisableWorkerPriority
	{
	public:
		BarrierRaisableWorkerPriority() = default;
		BarrierRaisableWorkerPriority(const BarrierRaisableWorkerPriority&) = delete;
		BarrierRaisableWorkerPriority& operator=(const BarrierRaisableWorkerPriority&) = delete;
		~BarrierRaisableWorkerPriority();

		/// <summary>Runs first on the worker thread: records its identity, then lowers it unless already raised.</summary>
		void EnterWorker() noexcept;
		/// <summary>Runs last on the worker thread; later raises are no-ops and any raise is released.</summary>
		void LeaveWorker() noexcept;
		/// <summary>Any thread, before it blocks on this worker. Idempotent.</summary>
		void RaiseForBarrier() noexcept;
		/// <summary>True once the worker has left; such an object can be dropped.</summary>
		bool Finished() const noexcept;
		/// <summary>True while the worker runs lowered (entered, not raised, not left).</summary>
		bool RunningLowered() const noexcept;

	private:
		mutable std::mutex m_mutex;
		bool m_entered = false;
		bool m_left = false;
		bool m_lowered = false;
		bool m_raised = false;
		std::uintptr_t m_thread = 0;
		std::intptr_t m_original_priority = 0;
		void* m_raise_token = nullptr;
	};

	/// <summary>Starts a worker that runs below UI priority until `priority` is raised for a barrier.</summary>
	template <typename Work> std::thread StartBarrierRaisableBackgroundThread(std::shared_ptr<BarrierRaisableWorkerPriority> priority, Work&& work)
	{
		return std::thread([priority = std::move(priority), work = std::forward<Work>(work)]() mutable
		{
			priority->EnterWorker();
			struct Leave
			{
				BarrierRaisableWorkerPriority& priority;
				~Leave() { priority.LeaveWorker(); }
			} leave{*priority};
			work();
		});
	}
} // namespace uam::platform
