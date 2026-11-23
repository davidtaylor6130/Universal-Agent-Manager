#pragma once

#include "common/utils/io_utils.h"

#include <chrono>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace uam_test
{
	/// <summary>
	/// The clock a per-input or per-poll budget is charged in (#526); see docs/engineering/testing.md.
	/// Locally and in acceptance runs it is the wall clock, so the budget is the real responsiveness bound.
	/// On GitHub-hosted runners it is the CPU time of the calling thread: there, background save, setup and
	/// provider work from the same case can keep a few shared cores busy and preempt the polling thread
	/// for longer than any budget, which says nothing about the work the poll itself performs. A case that
	/// scales a budget from calibrated blocking work must time that calibration with this clock too.
	/// </summary>
	struct PollBudgetClock
	{
		static bool ChargesThreadCpuTime()
		{
			static const bool hosted = []
			{
				const char* value = std::getenv("GITHUB_ACTIONS");
				return value != nullptr && std::string_view(value) == "true";
			}();
			return hosted;
		}

		static std::chrono::nanoseconds Now()
		{
			if (!ChargesThreadCpuTime())
				return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
#if defined(_WIN32)
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
				return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
			const auto ticks = [](const FILETIME& time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
			return std::chrono::nanoseconds(static_cast<long long>((ticks(kernel) + ticks(user)) * 100ULL));
#else
			timespec now{};
			if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
				return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
			return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
#endif
		}
	};

	/// <summary>
	/// Times one budgeted call or poll on the calling thread with PollBudgetClock. Within() also fails when the
	/// call made a durable write on this thread while a case armed uam::io::t_durable_write_delay_ms_for_tests,
	/// so that guard holds whichever clock is charged.
	/// </summary>
	struct PollBudgetTimer
	{
		std::chrono::nanoseconds started = PollBudgetClock::Now();
		int delayed_durable_writes = uam::io::t_delayed_durable_writes_for_tests;

		std::chrono::nanoseconds Elapsed() const { return PollBudgetClock::Now() - started; }

		template <typename Rep, typename Period> bool Within(std::chrono::duration<Rep, Period> budget) const
		{
			return Elapsed() <= budget && uam::io::t_delayed_durable_writes_for_tests == delayed_durable_writes;
		}
	};
} // namespace uam_test
