#pragma once

#include "common/utils/time_utils.h"

#include <array>
#include <optional>
#include <string_view>

namespace uam::time
{
	enum class HistoryTimestampOrder
	{
		Before,
		Equal,
		After,
		Unknown
	};

	enum class HistoryTimestampProvenance
	{
		Unknown,
		ExplicitOffset,
		AssumedCurrentLocalZone
	};

	struct HistoryTimestampComparison
	{
		HistoryTimestampOrder order = HistoryTimestampOrder::Unknown;
		HistoryTimestampProvenance provenance = HistoryTimestampProvenance::Unknown;
	};

	namespace history_timestamp_detail
	{
		struct Instant
		{
			std::int64_t seconds = 0;
			std::int32_t nanoseconds = 0;
			bool assumed_local = false;
		};

		struct Civil
		{
			int year, month, day, hour, minute, second;
		};

		inline int Decimal(std::string_view text, std::size_t offset, std::size_t length)
		{
			int result = 0;
			for (std::size_t index = offset; index < offset + length; ++index)
			{
				if (text[index] < '0' || text[index] > '9')
				{
					return -1;
				}
				result = result * 10 + text[index] - '0';
			}
			return result;
		}

		inline std::optional<Civil> ParseCivil(std::string_view text)
		{
			if (text.size() < 19 || text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':')
			{
				return std::nullopt;
			}
			const Civil value{Decimal(text, 0, 4), Decimal(text, 5, 2), Decimal(text, 8, 2), Decimal(text, 11, 2), Decimal(text, 14, 2), Decimal(text, 17, 2)};
			if (value.year < 1970 || value.month < 1 || value.day < 1 || value.hour < 0 || value.hour > 23 || value.minute < 0 || value.minute > 59 || value.second < 0 || value.second > 59)
			{
				return std::nullopt;
			}
			const std::chrono::year_month_day date{std::chrono::year{value.year}, std::chrono::month{static_cast<unsigned>(value.month)}, std::chrono::day{static_cast<unsigned>(value.day)}};
			if (!date.ok())
			{
				return std::nullopt;
			}
			return value;
		}

		inline bool SameCivil(const std::tm& observed, const Civil& expected)
		{
			return observed.tm_year + 1900 == expected.year && observed.tm_mon + 1 == expected.month && observed.tm_mday == expected.day && observed.tm_hour == expected.hour && observed.tm_min == expected.minute && observed.tm_sec == expected.second;
		}

		inline std::optional<Instant> Parse(std::string_view text)
		{
			const auto civil = ParseCivil(text);
			if (!civil)
			{
				return std::nullopt;
			}
			if (text.size() == 19 && text[10] == ' ')
			{
				// Compatibility only: old records contain no original offset/zone.
				// Round trips reject normalized gaps and ambiguities exposed by the
				// CRT DST candidates. They cannot certify an unchanged historical
				// zone or every political offset fold with the same DST flag.
				std::array<std::time_t, 3> instants{};
				std::size_t count = 0;
				for (const int daylight : std::array<int, 3>{-1, 0, 1})
				{
					std::tm candidate{};
					candidate.tm_year = civil->year - 1900;
					candidate.tm_mon = civil->month - 1;
					candidate.tm_mday = civil->day;
					candidate.tm_hour = civil->hour;
					candidate.tm_min = civil->minute;
					candidate.tm_sec = civil->second;
					candidate.tm_isdst = daylight;
					const std::time_t epoch = std::mktime(&candidate);
					if (epoch == static_cast<std::time_t>(-1) || !SameCivil(detail::LocalTimeSnapshot(epoch), *civil))
					{
						continue;
					}
					bool duplicate = false;
					for (std::size_t index = 0; index < count; ++index)
					{
						duplicate = duplicate || instants[index] == epoch;
					}
					if (!duplicate)
					{
						instants[count++] = epoch;
					}
				}
				if (count != 1)
				{
					return std::nullopt;
				}
				return Instant{static_cast<std::int64_t>(instants[0]), 0, true};
			}
			if (text.size() < 20 || text[10] != 'T')
			{
				return std::nullopt;
			}
			std::size_t cursor = 19;
			std::int32_t fraction = 0;
			if (text[cursor] == '.')
			{
				const std::size_t start = ++cursor;
				while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9')
				{
					if (cursor - start == 9)
					{
						return std::nullopt;
					}
					fraction = fraction * 10 + text[cursor++] - '0';
				}
				if (cursor == start)
				{
					return std::nullopt;
				}
				for (std::size_t digits = cursor - start; digits < 9; ++digits)
				{
					fraction *= 10;
				}
			}
			int offset_minutes = 0;
			if (cursor + 1 == text.size() && text[cursor] == 'Z')
			{
			}
			else if (cursor + 6 == text.size() && (text[cursor] == '+' || text[cursor] == '-') && text[cursor + 3] == ':')
			{
				const int hours = Decimal(text, cursor + 1, 2), minutes = Decimal(text, cursor + 4, 2);
				if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 || (text[cursor] == '-' && hours == 0 && minutes == 0))
				{
					return std::nullopt;
				}
				offset_minutes = (hours * 60 + minutes) * (text[cursor] == '+' ? 1 : -1);
			}
			else
			{
				return std::nullopt;
			}
			const std::chrono::year_month_day date{std::chrono::year{civil->year}, std::chrono::month{static_cast<unsigned>(civil->month)}, std::chrono::day{static_cast<unsigned>(civil->day)}};
			const std::int64_t seconds = std::chrono::sys_days{date}.time_since_epoch().count() * 86400LL + civil->hour * 3600LL + civil->minute * 60LL + civil->second - offset_minutes * 60LL;
			if (seconds < 0)
			{
				return std::nullopt;
			}
			return Instant{seconds, fraction, false};
		}
	}

	/// Legacy local stamps assume the current controller zone; they are not UTC
	/// or proof of their historical zone. Unknown never grants chronology order.
	inline HistoryTimestampComparison CompareHistoryTimestamps(std::string_view lhs, std::string_view rhs)
	{
		const auto left = history_timestamp_detail::Parse(lhs), right = history_timestamp_detail::Parse(rhs);
		if (!left || !right)
		{
			return {};
		}
		const auto provenance = left->assumed_local || right->assumed_local ? HistoryTimestampProvenance::AssumedCurrentLocalZone : HistoryTimestampProvenance::ExplicitOffset;
		if (left->seconds == right->seconds && left->nanoseconds == right->nanoseconds)
		{
			return {HistoryTimestampOrder::Equal, provenance};
		}
		const bool before = left->seconds < right->seconds || (left->seconds == right->seconds && left->nanoseconds < right->nanoseconds);
		return {before ? HistoryTimestampOrder::Before : HistoryTimestampOrder::After, provenance};
	}
}
