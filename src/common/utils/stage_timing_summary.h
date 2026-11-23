#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uam::diagnostics
{

struct StageTimingSummaryOptions
{
	/// Samples at or above this count as slow; a window without one writes no summary.
	std::chrono::milliseconds summary_threshold{50};
	/// Samples at or above this are also written individually, at most once per individual_interval.
	std::chrono::milliseconds individual_threshold{250};
	std::chrono::seconds individual_interval{5};
	std::chrono::seconds window{60};
};

/// <summary>Aggregates repeated timings of a staged loop (for example PollTick) into one periodic
/// p50/p95/max summary per stage, plus an individual line only for a sample above a higher threshold.
/// Memory is constant: each stage keeps a fixed millisecond histogram, not its samples.</summary>
class StageTimingSummary
{
public:
	using Clock = std::chrono::steady_clock;
	using Options = StageTimingSummaryOptions;

	StageTimingSummary(std::string name, std::vector<std::string> stage_names, Options options = {})
	    : m_name(std::move(name)), m_stageNames(std::move(stage_names)), m_options(options), m_stages(m_stageNames.size())
	{
	}

	/// <summary>Records one sample; stage durations follow the constructor's stage order. Returns the
	/// lines to log (an individual slow sample and/or a finished window's summary), usually none.</summary>
	std::vector<std::string> Record(Clock::time_point now, std::chrono::milliseconds total, const std::vector<std::chrono::milliseconds>& stages)
	{
		std::vector<std::string> lines;
		if (!m_windowStart) m_windowStart = now;
		else if (now - *m_windowStart >= m_options.window)
		{
			if (std::optional<std::string> summary = Summary(now)) lines.push_back(std::move(*summary));
			Reset(now);
		}
		++m_samples;
		if (total >= m_options.summary_threshold) ++m_slowSamples;
		m_total.Add(total.count());
		for (std::size_t index = 0; index < m_stages.size() && index < stages.size(); ++index) m_stages[index].Add(stages[index].count());
		if (total >= m_options.individual_threshold &&
		    (!m_lastIndividual || now - *m_lastIndividual >= m_options.individual_interval))
		{
			std::string line = "[performance] " + m_name + " took " + std::to_string(total.count()) + " ms; stages=";
			for (std::size_t index = 0; index < m_stageNames.size() && index < stages.size(); ++index)
				line += (index == 0 ? "" : ",") + m_stageNames[index] + ":" + std::to_string(stages[index].count());
			lines.push_back(line + " ms.");
			m_lastIndividual = now;
		}
		return lines;
	}

	/// <summary>Writes the current partial window's summary (for example at shutdown) and starts a new one.</summary>
	std::optional<std::string> Flush(Clock::time_point now)
	{
		std::optional<std::string> summary = Summary(now);
		Reset(now);
		return summary;
	}

private:
	// Upper bounds in milliseconds; the last bucket holds everything slower.
	static constexpr std::array<std::int64_t, 32> kBounds{0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 80, 100, 125, 150, 200, 250, 300, 400, 500, 750, 1000, 1500, 2000, 3000, 5000};

	struct Histogram
	{
		std::array<std::uint32_t, kBounds.size() + 1> counts{};
		std::uint64_t samples = 0;
		std::int64_t max = 0;

		void Add(std::int64_t milliseconds)
		{
			const auto bucket = std::lower_bound(kBounds.begin(), kBounds.end(), milliseconds);
			++counts[static_cast<std::size_t>(bucket - kBounds.begin())];
			++samples;
			max = std::max(max, milliseconds);
		}

		/// Reports the bucket's upper bound (never above the observed maximum).
		std::int64_t Percentile(double fraction) const
		{
			if (samples == 0) return 0;
			const std::uint64_t rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(samples) - 1e-9)));
			std::uint64_t seen = 0;
			for (std::size_t index = 0; index < counts.size(); ++index)
			{
				seen += counts[index];
				if (seen >= rank) return index < kBounds.size() ? std::min(kBounds[index], max) : max;
			}
			return max;
		}

		std::string Describe() const
		{
			return std::to_string(Percentile(0.5)) + "/" + std::to_string(Percentile(0.95)) + "/" + std::to_string(max);
		}
	};

	std::optional<std::string> Summary(Clock::time_point now) const
	{
		if (m_samples == 0 || m_slowSamples == 0 || !m_windowStart) return std::nullopt;
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - *m_windowStart).count();
		std::string line = "[performance] " + m_name + " summary over " + std::to_string(seconds) + " s: samples=" + std::to_string(m_samples) +
		    " slow(>=" + std::to_string(m_options.summary_threshold.count()) + "ms)=" + std::to_string(m_slowSamples) +
		    "; p50/p95/max ms total=" + m_total.Describe() + " stages=";
		bool first = true;
		for (std::size_t index = 0; index < m_stages.size(); ++index)
		{
			// Stages that never took a whole millisecond add nothing a reader can act on.
			if (m_stages[index].max == 0) continue;
			line += (first ? "" : ",") + m_stageNames[index] + ":" + m_stages[index].Describe();
			first = false;
		}
		if (first) line += "none";
		return line + ".";
	}

	void Reset(Clock::time_point now)
	{
		m_windowStart = now;
		m_samples = 0;
		m_slowSamples = 0;
		m_total = {};
		std::fill(m_stages.begin(), m_stages.end(), Histogram{});
	}

	std::string m_name;
	std::vector<std::string> m_stageNames;
	Options m_options;
	std::optional<Clock::time_point> m_windowStart;
	std::optional<Clock::time_point> m_lastIndividual;
	std::uint64_t m_samples = 0;
	std::uint64_t m_slowSamples = 0;
	Histogram m_total;
	std::vector<Histogram> m_stages;
};

} // namespace uam::diagnostics
