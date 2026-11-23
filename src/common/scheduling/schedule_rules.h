#pragma once

// Wall-clock schedule rules shared by automation schedules and chat self-wakes: trigger parsing,
// the next run after a moment, and which due slots a missed-run policy runs. Pure functions with
// no I/O; local time is supplied by a LocalClock so tests do not depend on the machine's zone.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace uam::scheduling
{
	inline constexpr std::int64_t kMinuteMs = 60 * 1000;
	/// Shortest and longest interval an automation schedule may use.
	inline constexpr int kMinIntervalMinutes = 15;
	inline constexpr int kMaxIntervalMinutes = 7 * 24 * 60;
	/// A slot noticed this long after its time counts as missed (UAM was closed or the computer slept).
	inline constexpr std::int64_t kOnTimeGraceMs = 5 * kMinuteMs;

	/// Calendar fields in the user's local time zone.
	struct LocalTime
	{
		int year = 1970;
		int month = 1; // 1-12
		int day = 1;   // 1-31
		int hour = 0;
		int minute = 0;
		int weekday = 4; // 0 = Sunday
	};

	/// Converts between epoch milliseconds and local calendar time. The default uses the operating system's zone,
	/// including its daylight-saving rules; tests supply a fixed one.
	struct LocalClock
	{
		std::function<LocalTime(std::int64_t epoch_ms)> to_local;
		/// Epoch milliseconds of a local date and time. A time skipped by a daylight-saving change resolves to a
		/// moment just after the change; a repeated time resolves to either instance.
		std::function<std::int64_t(int year, int month, int day, int hour, int minute)> from_local;
	};
	const LocalClock& SystemLocalClock();
	/// A zone with a fixed offset from UTC and no daylight saving.
	LocalClock FixedOffsetClock(int offset_minutes);

	enum class TriggerKind
	{
		Interval,
		Weekly,
	};

	struct Trigger
	{
		TriggerKind kind = TriggerKind::Interval;
		int interval_minutes = 60;
		/// Weekly: index 0 is Sunday.
		std::array<bool, 7> days{};
		int hour = 9;
		int minute = 0;
	};

	enum class MissedPolicy
	{
		Skip,
		RunOnce,
	};

	/// Parses {"kind":"interval","minutes":N} or {"kind":"weekly","days":[0..6],"time":"HH:MM"}. Returns a
	/// one-line reason for the user when the value is not a valid trigger.
	std::optional<Trigger> ParseTrigger(const nlohmann::json& value, std::string* error_out = nullptr);
	nlohmann::json TriggerJson(const Trigger& trigger);
	std::optional<MissedPolicy> ParseMissedPolicy(std::string_view value);
	std::string MissedPolicyName(MissedPolicy policy);

	/// The first run strictly after `after_ms`. An interval counts from `anchor_ms`; a weekly time runs at most once
	/// per local day, so the repeated hour when clocks go back does not run it twice.
	std::int64_t NextRunAfter(const Trigger& trigger, std::int64_t anchor_ms, std::int64_t after_ms, const LocalClock& clock);

	struct DueDecision
	{
		/// True when this poll starts one run; never more than one per poll.
		bool run = false;
		/// Slots that were due and are not run.
		int skipped = 0;
		/// The slot the run stands for, when it runs.
		std::int64_t slot_ms = 0;
		/// The next slot after now, to save before the run starts.
		std::int64_t next_run_ms = 0;
	};

	/// Decides what one poll does with a schedule whose next slot is `next_run_ms`. A slot noticed within the
	/// on-time grace runs; older ones are missed, and the policy either skips them or runs once for all of them.
	DueDecision Decide(const Trigger& trigger, MissedPolicy policy, std::int64_t anchor_ms, std::int64_t next_run_ms, std::int64_t now_ms, const LocalClock& clock);
} // namespace uam::scheduling
