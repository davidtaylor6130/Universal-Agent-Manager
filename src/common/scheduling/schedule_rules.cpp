#include "common/scheduling/schedule_rules.h"

#include <algorithm>
#include <ctime>
#include <limits>

namespace uam::scheduling
{
	namespace
	{
		constexpr std::int64_t kDayMs = 24 * 60 * kMinuteMs;
		/// Due weekly slots counted one by one before the rest are skipped in one step.
		constexpr int kMaxCountedSlots = 1000;

		std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor)
		{
			return value / divisor - (value % divisor != 0 && (value < 0) != (divisor < 0) ? 1 : 0);
		}

		/// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
		std::int64_t DaysFromCivil(int year, int month, int day)
		{
			const int y = year - (month <= 2 ? 1 : 0);
			const int era = (y >= 0 ? y : y - 399) / 400;
			const int yoe = y - era * 400;
			const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
			const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
			return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
		}

		void CivilFromDays(std::int64_t days, int& year, int& month, int& day)
		{
			days += 719468;
			const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
			const int doe = static_cast<int>(days - era * 146097);
			const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
			const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
			const int mp = (5 * doy + 2) / 153;
			day = doy - (153 * mp + 2) / 5 + 1;
			month = mp + (mp < 10 ? 3 : -9);
			year = static_cast<int>(yoe + era * 400) + (month <= 2 ? 1 : 0);
		}

		int WeekdayFromDays(std::int64_t days)
		{
			// 1970-01-01 was a Thursday.
			return static_cast<int>(((days % 7) + 7 + 4) % 7);
		}

		LocalTime FromTm(const std::tm& value)
		{
			return {.year = value.tm_year + 1900, .month = value.tm_mon + 1, .day = value.tm_mday, .hour = value.tm_hour, .minute = value.tm_min, .weekday = value.tm_wday};
		}
	} // namespace

	const LocalClock& SystemLocalClock()
	{
		static const LocalClock clock{
		    .to_local = [](std::int64_t epoch_ms)
		    {
			    const std::time_t seconds = static_cast<std::time_t>(FloorDiv(epoch_ms, 1000));
			    std::tm local{};
#if defined(_WIN32)
			    localtime_s(&local, &seconds);
#else
			    localtime_r(&seconds, &local);
#endif
			    return FromTm(local);
		    },
		    .from_local = [](int year, int month, int day, int hour, int minute)
		    {
			    std::tm local{};
			    local.tm_year = year - 1900;
			    local.tm_mon = month - 1;
			    local.tm_mday = day;
			    local.tm_hour = hour;
			    local.tm_min = minute;
			    // The C library decides whether daylight saving applies, including in the skipped and repeated hours.
			    local.tm_isdst = -1;
			    std::tm copy = local;
			    std::time_t seconds = std::mktime(&copy);
			    // glibc moves a skipped time forward; the BSD-derived C library on macOS can refuse it instead.
			    if (seconds == static_cast<std::time_t>(-1))
			    {
				    copy = local;
				    copy.tm_isdst = 0;
				    seconds = std::mktime(&copy);
			    }
			    return seconds == static_cast<std::time_t>(-1) ? std::int64_t{-1} : static_cast<std::int64_t>(seconds) * 1000;
		    },
		};
		return clock;
	}

	LocalClock FixedOffsetClock(int offset_minutes)
	{
		const std::int64_t offset_ms = static_cast<std::int64_t>(offset_minutes) * kMinuteMs;
		return {
		    .to_local = [offset_ms](std::int64_t epoch_ms)
		    {
			    const std::int64_t local_ms = epoch_ms + offset_ms;
			    const std::int64_t days = FloorDiv(local_ms, kDayMs);
			    const std::int64_t minute_of_day = (local_ms - days * kDayMs) / kMinuteMs;
			    LocalTime time;
			    CivilFromDays(days, time.year, time.month, time.day);
			    time.hour = static_cast<int>(minute_of_day / 60);
			    time.minute = static_cast<int>(minute_of_day % 60);
			    time.weekday = WeekdayFromDays(days);
			    return time;
		    },
		    .from_local = [offset_ms](int year, int month, int day, int hour, int minute)
		    { return DaysFromCivil(year, month, day) * kDayMs + (static_cast<std::int64_t>(hour) * 60 + minute) * kMinuteMs - offset_ms; },
		};
	}

	std::optional<Trigger> ParseTrigger(const nlohmann::json& value, std::string* error_out)
	{
		const auto fail = [&](std::string message) -> std::optional<Trigger>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()) return fail("Choose when the schedule runs.");
		const std::string kind = value["kind"].get<std::string>();
		Trigger trigger;
		if (kind == "interval")
		{
			const auto minutes = value.find("minutes");
			if (minutes == value.end() || !minutes->is_number_integer() || minutes->get<std::int64_t>() < kMinIntervalMinutes || minutes->get<std::int64_t>() > kMaxIntervalMinutes)
				return fail("Choose an interval from 15 minutes to 7 days.");
			trigger.kind = TriggerKind::Interval;
			trigger.interval_minutes = minutes->get<int>();
			return trigger;
		}
		if (kind != "weekly") return fail("Choose when the schedule runs.");
		trigger.kind = TriggerKind::Weekly;
		const auto days = value.find("days");
		if (days == value.end() || !days->is_array() || days->empty()) return fail("Choose at least one day.");
		for (const nlohmann::json& day : *days)
		{
			if (!day.is_number_integer() || day.get<std::int64_t>() < 0 || day.get<std::int64_t>() > 6) return fail("Choose at least one day.");
			trigger.days[static_cast<std::size_t>(day.get<int>())] = true;
		}
		const auto time = value.find("time");
		const std::string text = time != value.end() && time->is_string() ? time->get<std::string>() : std::string{};
		const auto digit = [&](std::size_t index) { return text[index] >= '0' && text[index] <= '9'; };
		if (text.size() != 5 || text[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4)) return fail("Enter a time as HH:MM.");
		trigger.hour = (text[0] - '0') * 10 + (text[1] - '0');
		trigger.minute = (text[3] - '0') * 10 + (text[4] - '0');
		if (trigger.hour > 23 || trigger.minute > 59) return fail("Enter a time as HH:MM.");
		return trigger;
	}

	nlohmann::json TriggerJson(const Trigger& trigger)
	{
		if (trigger.kind == TriggerKind::Interval) return {{"kind", "interval"}, {"minutes", trigger.interval_minutes}};
		nlohmann::json days = nlohmann::json::array();
		for (int day = 0; day < 7; ++day)
			if (trigger.days[static_cast<std::size_t>(day)]) days.push_back(day);
		const std::string time = std::string(1, static_cast<char>('0' + trigger.hour / 10)) + static_cast<char>('0' + trigger.hour % 10) + ":" +
		    static_cast<char>('0' + trigger.minute / 10) + static_cast<char>('0' + trigger.minute % 10);
		return {{"kind", "weekly"}, {"days", std::move(days)}, {"time", time}};
	}

	std::optional<MissedPolicy> ParseMissedPolicy(std::string_view value)
	{
		if (value == "skip") return MissedPolicy::Skip;
		if (value == "runOnce") return MissedPolicy::RunOnce;
		return std::nullopt;
	}

	std::string MissedPolicyName(MissedPolicy policy)
	{
		return policy == MissedPolicy::RunOnce ? "runOnce" : "skip";
	}

	std::int64_t NextRunAfter(const Trigger& trigger, std::int64_t anchor_ms, std::int64_t after_ms, const LocalClock& clock)
	{
		if (trigger.kind == TriggerKind::Interval)
		{
			const std::int64_t interval_ms = std::max<std::int64_t>(1, trigger.interval_minutes) * kMinuteMs;
			if (after_ms < anchor_ms) return anchor_ms;
			return anchor_ms + (FloorDiv(after_ms - anchor_ms, interval_ms) + 1) * interval_ms;
		}
		const LocalTime now = clock.to_local(after_ms);
		const std::int64_t today = DaysFromCivil(now.year, now.month, now.day);
		// Eight days reaches the same weekday next week even when today's time has passed.
		for (int offset = 0; offset <= 8; ++offset)
		{
			const int weekday = (now.weekday + offset) % 7;
			if (!trigger.days[static_cast<std::size_t>(weekday)]) continue;
			// Today counts only before its time; this is also what keeps the repeated hour from running twice.
			if (offset == 0 && (now.hour > trigger.hour || (now.hour == trigger.hour && now.minute >= trigger.minute))) continue;
			int year = 0, month = 0, day = 0;
			CivilFromDays(today + offset, year, month, day);
			std::int64_t candidate = clock.from_local(year, month, day, trigger.hour, trigger.minute);
			// A time the clock cannot express (skipped when clocks go forward) runs at the next hour instead of not at all.
			if (candidate < 0 && trigger.hour < 23) candidate = clock.from_local(year, month, day, trigger.hour + 1, trigger.minute);
			if (candidate > after_ms) return candidate;
		}
		return std::numeric_limits<std::int64_t>::max();
	}

	DueDecision Decide(const Trigger& trigger, MissedPolicy policy, std::int64_t anchor_ms, std::int64_t next_run_ms, std::int64_t now_ms, const LocalClock& clock)
	{
		DueDecision decision{.next_run_ms = next_run_ms};
		if (now_ms < next_run_ms) return decision;
		int due = 0;
		std::int64_t latest = next_run_ms;
		std::int64_t next = next_run_ms;
		if (trigger.kind == TriggerKind::Interval)
		{
			const std::int64_t interval_ms = std::max<std::int64_t>(1, trigger.interval_minutes) * kMinuteMs;
			const std::int64_t count = FloorDiv(now_ms - next_run_ms, interval_ms) + 1;
			due = static_cast<int>(std::min<std::int64_t>(count, std::numeric_limits<int>::max()));
			latest = next_run_ms + (count - 1) * interval_ms;
			next = latest + interval_ms;
		}
		else
		{
			while (next <= now_ms && due < kMaxCountedSlots)
			{
				latest = next;
				++due;
				next = NextRunAfter(trigger, anchor_ms, next, clock);
			}
			// A very long absence: the remaining slots are skipped in one step.
			if (next <= now_ms) next = NextRunAfter(trigger, anchor_ms, now_ms, clock);
		}
		decision.next_run_ms = next;
		const bool on_time = now_ms - latest <= kOnTimeGraceMs;
		if (on_time || policy == MissedPolicy::RunOnce)
		{
			decision.run = true;
			decision.slot_ms = latest;
			decision.skipped = due - 1;
		}
		else decision.skipped = due;
		return decision;
	}
} // namespace uam::scheduling
