#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace uam::time
{
	inline constexpr const char* kIsoUtcTimestampFormat = "%Y-%m-%dT%H:%M:%S.000Z";
	inline constexpr const char* kLocalDisplayTimestampFormat = "%Y-%m-%d %H:%M:%S";

	namespace detail
	{
		inline std::time_t SystemTimeNow()
		{
			return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		}

		template <typename Clock, typename Duration> inline std::int64_t EpochCountNow()
		{
			const auto elapsed = Clock::now().time_since_epoch();
			return std::chrono::duration_cast<Duration>(elapsed).count();
		}

		inline std::tm LocalTimeSnapshot(const std::time_t value)
		{
			std::tm tm_snapshot{};
#if defined(_WIN32)
			localtime_s(&tm_snapshot, &value);
#else
			localtime_r(&value, &tm_snapshot);
#endif
			return tm_snapshot;
		}

		inline std::tm UtcTimeSnapshot(const std::time_t value)
		{
			std::tm tm_snapshot{};
#if defined(_WIN32)
			gmtime_s(&tm_snapshot, &value);
#else
			gmtime_r(&value, &tm_snapshot);
#endif
			return tm_snapshot;
		}

		inline std::string FormatTimestamp(const std::tm& tm_snapshot, const char* format)
		{
			std::ostringstream out;
			out << std::put_time(&tm_snapshot, format);
			return out.str();
		}
	} // namespace detail

	inline std::string LocalTimestampNow(const char* format)
	{
		return detail::FormatTimestamp(detail::LocalTimeSnapshot(detail::SystemTimeNow()), format);
	}

	inline std::string UtcTimestampNow(const char* format)
	{
		return detail::FormatTimestamp(detail::UtcTimeSnapshot(detail::SystemTimeNow()), format);
	}

	inline std::string IsoUtcTimestampNow()
	{
		return UtcTimestampNow(kIsoUtcTimestampFormat);
	}

	inline std::int64_t SystemEpochMillisecondsNow()
	{
		return detail::EpochCountNow<std::chrono::system_clock, std::chrono::milliseconds>();
	}

	inline std::int64_t SystemEpochMicrosecondsNow()
	{
		return detail::EpochCountNow<std::chrono::system_clock, std::chrono::microseconds>();
	}

	inline std::string SystemEpochMicrosecondsTokenNow()
	{
		return std::to_string(SystemEpochMicrosecondsNow());
	}

	inline std::int64_t SteadyEpochNanosecondsNow()
	{
		return detail::EpochCountNow<std::chrono::steady_clock, std::chrono::nanoseconds>();
	}

	inline std::string SteadyEpochNanosecondsTokenNow()
	{
		return std::to_string(SteadyEpochNanosecondsNow());
	}

	inline std::string TimestampNow()
	{
		return LocalTimestampNow(kLocalDisplayTimestampFormat);
	}

	inline std::int64_t TimestampNowSec()
	{
		return detail::SystemTimeNow();
	}
} // namespace uam::time
