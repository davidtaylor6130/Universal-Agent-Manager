#pragma once

#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace uam::time
{
	inline std::string TimestampNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &tt);
#else
		localtime_r(&tt, &tm_snapshot);
#endif
		std::ostringstream out;
		out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
		return out.str();
	}
}
