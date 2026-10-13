#pragma once

#include "common/usage/transcript_usage.h"

#include <map>
#include <tuple>
#include <vector>

namespace uam::usage
{
	using ModelKey = std::tuple<std::string, std::string, std::string>;
	using ProviderPeriodKey = std::pair<std::int64_t, std::string>;

	struct ModelPrices
	{
		std::optional<double> input_per_million;
		std::optional<double> cached_input_per_million;
		std::optional<double> cache_write_per_million;
		std::optional<double> output_per_million;
	};

	struct Totals
	{
		TokenCounts tokens;
		double reported_cost_usd = 0;
		double estimated_cost_usd = 0;
		double estimated_cache_savings_usd = 0;
		std::int64_t reported_cost_tokens = 0;
		std::int64_t estimated_cost_tokens = 0;
		std::int64_t unpriced_tokens = 0;
		std::size_t records = 0;
		std::size_t sessions = 0;
	};

	struct Summary
	{
		std::int64_t chart_start_ms = 0;
		std::int64_t chart_bucket_ms = 86400000;
		Totals total;
		std::map<std::string, Totals> providers;
		std::map<ModelKey, Totals> models;
		// UTC bucket starts; chart_bucket_ms may span several whole days for lifetime charts.
		std::map<std::int64_t, Totals> days;
		std::map<std::int64_t, Totals> hours;
		std::map<ProviderPeriodKey, Totals> provider_days;
		std::map<ProviderPeriodKey, Totals> provider_hours;
		std::size_t duplicates = 0;
		std::size_t unidentified_records = 0;
	};

	/// <summary>Aggregate a half-open window with exact provider-scoped sessions and optional whole-day chart cohorts.</summary>
	std::optional<Summary> Aggregate(const std::vector<Record>& records, std::int64_t start_ms, std::int64_t end_ms,
	                                 const std::map<ModelKey, ModelPrices>& prices = {}, std::int64_t chart_start_ms = 0, std::int64_t chart_bucket_ms = 86400000);
	/// <summary>Aggregate worker-owned cached records without copying their strings and token data.</summary>
	std::optional<Summary> AggregateReferences(const std::vector<const Record*>& records, std::int64_t start_ms, std::int64_t end_ms,
	                                           const std::map<ModelKey, ModelPrices>& prices = {}, std::int64_t chart_start_ms = 0, std::int64_t chart_bucket_ms = 86400000);
}
