#include "common/usage/usage_aggregation.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace uam::usage
{
	namespace
	{
		bool ValidPrices(const ModelPrices& prices)
		{
			for (const std::optional<double> amount : {prices.input_per_million, prices.cached_input_per_million, prices.cache_write_per_million, prices.output_per_million})
				if (amount && (!std::isfinite(*amount) || *amount < 0)) return false;
			return true;
		}

		bool Add(Totals& totals, const Record& record, const ModelPrices* prices)
		{
			if (record.tokens.Total() > kMaxExactTokenCount - totals.tokens.Total()) return false;
			totals.tokens.input += record.tokens.input;
			totals.tokens.cached_input += record.tokens.cached_input;
			totals.tokens.cache_write += record.tokens.cache_write;
			totals.tokens.output += record.tokens.output;
			totals.tokens.reasoning += record.tokens.reasoning;
			if (record.record_count == 0 || record.record_count > static_cast<std::size_t>(kMaxExactTokenCount) - totals.records) return false;
			totals.records += record.record_count;
			if (record.reported_cost_usd && std::isfinite(*record.reported_cost_usd) && *record.reported_cost_usd >= 0)
			{
				totals.reported_cost_usd += *record.reported_cost_usd;
				totals.reported_cost_tokens += record.tokens.Total();
			}
			else if (prices && (record.tokens.input == 0 || prices->input_per_million) &&
			    (record.tokens.cached_input == 0 || prices->cached_input_per_million) &&
			    (record.tokens.cache_write == 0 || prices->cache_write_per_million) &&
			    (record.tokens.output == 0 || prices->output_per_million))
			{
				const double cost = (record.tokens.input * prices->input_per_million.value_or(0) + record.tokens.cached_input * prices->cached_input_per_million.value_or(0) +
				    record.tokens.cache_write * prices->cache_write_per_million.value_or(0) + record.tokens.output * prices->output_per_million.value_or(0)) / 1000000.0;
				totals.estimated_cost_usd += cost;
				totals.estimated_cost_tokens += record.tokens.Total();
				// Savings compare cached reads with the same model's ordinary input rate; cache writes are billed separately.
				if (prices->input_per_million && prices->cached_input_per_million)
					totals.estimated_cache_savings_usd += record.tokens.cached_input * std::max(0.0, *prices->input_per_million - *prices->cached_input_per_million) / 1000000.0;
			}
			else if (!prices && record.estimated_cost_usd && std::isfinite(*record.estimated_cost_usd) && *record.estimated_cost_usd >= 0)
			{
				totals.estimated_cost_usd += *record.estimated_cost_usd;
				totals.estimated_cost_tokens += record.tokens.Total();
			}
			else totals.unpriced_tokens += record.tokens.Total();
			return std::isfinite(totals.reported_cost_usd) && std::isfinite(totals.estimated_cost_usd) && std::isfinite(totals.estimated_cache_savings_usd);
		}
	}

	std::optional<Summary> Aggregate(const std::vector<Record>& records, std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices)
	{
		std::vector<const Record*> references;
		references.reserve(records.size());
		for (const Record& record : records) references.push_back(&record);
		return AggregateReferences(references, start_ms, end_ms, prices);
	}

	std::optional<Summary> AggregateReferences(const std::vector<const Record*>& records, std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices)
	{
		if (start_ms < 0 || end_ms <= start_ms) return std::nullopt;
		for (const Record* record : records) if (!record) return std::nullopt;
		for (const std::pair<const ModelKey, ModelPrices>& price : prices)
			if (!ValidPrices(price.second)) return std::nullopt;
		Summary summary;
		std::map<std::pair<std::string, std::string>, std::size_t> originals;
		// Choose the earliest event before filtering; a copied fork can re-stamp old usage inside the requested window.
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			const Record& record = *records[index];
			if (record.dedupe_key.empty()) continue;
			const std::pair<std::string, std::string> identity{record.provider_id, record.dedupe_key};
			const std::map<std::pair<std::string, std::string>, std::size_t>::iterator original = originals.find(identity);
			if (original == originals.end()) originals.emplace(identity, index);
			else if (record.timestamp_ms < records[original->second]->timestamp_ms) original->second = index;
		}
		std::set<std::pair<std::string, std::string>> sessions;
		std::map<std::string, std::set<std::string>> provider_sessions;
		std::map<ModelKey, std::set<std::string>> model_sessions;
		std::map<std::int64_t, std::set<std::pair<std::string, std::string>>> day_sessions, hour_sessions;
		std::map<ProviderPeriodKey, std::set<std::string>> provider_day_sessions, provider_hour_sessions;
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			const Record& record = *records[index];
			if (!record.dedupe_key.empty() && originals.at({record.provider_id, record.dedupe_key}) != index)
			{
				++summary.duplicates;
				continue;
			}
			if (record.timestamp_ms < start_ms || record.timestamp_ms >= end_ms) continue;
			if (record.provider_id.empty() || record.model_id.empty()) return std::nullopt;
			for (const std::int64_t count : {record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output, record.tokens.reasoning})
				if (count < 0 || count > kMaxExactTokenCount) return std::nullopt;
			if (record.tokens.reasoning > record.tokens.output || record.tokens.Total() > kMaxExactTokenCount) return std::nullopt;
			const std::size_t unidentified = record.unidentified_records.value_or(record.dedupe_key.empty() ? record.record_count : 0);
			if (unidentified > record.record_count || unidentified > static_cast<std::size_t>(kMaxExactTokenCount) - summary.unidentified_records) return std::nullopt;
			summary.unidentified_records += unidentified;
			const ModelKey model_key{record.provider_id, record.model_id, record.speed};
			const std::map<ModelKey, ModelPrices>::const_iterator price = prices.find(model_key);
			const ModelPrices* model_prices = price == prices.end() ? nullptr : &price->second;
			const std::int64_t day = record.timestamp_ms / 86400000 * 86400000;
			const std::int64_t hour = record.timestamp_ms / 3600000 * 3600000;
			if (!Add(summary.total, record, model_prices) || !Add(summary.providers[record.provider_id], record, model_prices) ||
			    !Add(summary.models[model_key], record, model_prices) || !Add(summary.days[day], record, model_prices) || !Add(summary.hours[hour], record, model_prices) ||
			    !Add(summary.provider_days[{day, record.provider_id}], record, model_prices) || !Add(summary.provider_hours[{hour, record.provider_id}], record, model_prices)) return std::nullopt;
			if (!record.session_id.empty())
			{
				sessions.insert({record.provider_id, record.session_id});
				provider_sessions[record.provider_id].insert(record.session_id);
				model_sessions[model_key].insert(record.session_id);
				day_sessions[day].insert({record.provider_id, record.session_id});
				hour_sessions[hour].insert({record.provider_id, record.session_id});
				provider_day_sessions[{day, record.provider_id}].insert(record.session_id);
				provider_hour_sessions[{hour, record.provider_id}].insert(record.session_id);
			}
		}
		summary.total.sessions = sessions.size();
		for (std::pair<const std::string, Totals>& bucket : summary.providers) bucket.second.sessions = provider_sessions[bucket.first].size();
		for (std::pair<const ModelKey, Totals>& bucket : summary.models) bucket.second.sessions = model_sessions[bucket.first].size();
		for (std::pair<const std::int64_t, Totals>& bucket : summary.days) bucket.second.sessions = day_sessions[bucket.first].size();
		for (std::pair<const std::int64_t, Totals>& bucket : summary.hours) bucket.second.sessions = hour_sessions[bucket.first].size();
		for (std::pair<const ProviderPeriodKey, Totals>& bucket : summary.provider_days) bucket.second.sessions = provider_day_sessions[bucket.first].size();
		for (std::pair<const ProviderPeriodKey, Totals>& bucket : summary.provider_hours) bucket.second.sessions = provider_hour_sessions[bucket.first].size();
		return summary;
	}
}
