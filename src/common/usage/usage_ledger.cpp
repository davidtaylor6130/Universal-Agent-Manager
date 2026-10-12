#include "common/usage/usage_ledger.h"
#include "common/usage/antigravity_accounting.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace uam::usage
{
	namespace
	{
		constexpr std::int64_t kDayMs = 86400000;
		constexpr std::size_t kMaxRows = 500000;
		using CohortKey = std::tuple<std::int64_t, std::string, std::string, std::string, std::string, int, int>;

		CohortKey Cohort(const Record& record)
		{
			const int cost_kind = record.reported_cost_usd ? 2 : record.estimated_cost_usd ? 1 : 0;
			// Partial rate overrides can price one event but not another; retain each event's required rate fields.
			const int price_fields = cost_kind == 2 ? 0 : (record.tokens.input > 0 ? 1 : 0) | (record.tokens.cached_input > 0 ? 2 : 0) |
			    (record.tokens.cache_write > 0 ? 4 : 0) | (record.tokens.output > 0 ? 8 : 0);
			return {record.timestamp_ms / kDayMs * kDayMs, record.provider_id, record.model_id, record.speed, record.session_id, cost_kind, price_fields};
		}

		bool Count(const nlohmann::json& value, std::int64_t& count)
		{
			if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxExactTokenCount))) return false;
			count = value.get<std::int64_t>();
			return count >= 0 && count <= kMaxExactTokenCount;
		}

		bool Text(const nlohmann::json& value, std::string& text, std::size_t limit)
		{
			if (!value.is_string() || value.get_ref<const std::string&>().size() > limit) return false;
			text = value.get<std::string>();
			return true;
		}

		bool Cost(const nlohmann::json& value, std::optional<double>& cost)
		{
			if (value.is_null()) return true;
			if (!value.is_number()) return false;
			const double amount = value.get<double>();
			if (!std::isfinite(amount) || amount < 0) return false;
			cost = amount;
			return true;
		}
	}

	std::optional<Ledger> Ledger::Create(const std::vector<const Record*>& input_records, std::int64_t generated_ms, LedgerRange range)
	{
		if ((range != LedgerRange::Rolling90Days && range != LedgerRange::AllRecorded) || generated_ms <= 0 || generated_ms > kMaxExactTokenCount || input_records.size() > kMaxRows) return std::nullopt;
		std::vector<const Record*> records;
		std::vector<AntigravityUsageCandidate> candidates;
		std::size_t alias_bytes = 0;
		for (const Record* record : input_records)
		{
			if (!record) return std::nullopt;
			if (record->provider_id == provider_ids::kAntigravityCli && record->dedupe_key.starts_with(kAntigravityAccountingPrefix))
			{
				const std::size_t weight = sizeof(AntigravityUsageCandidate) + record->model_id.size() + record->session_id.size() + 3 * record->dedupe_key.size();
				if (record->dedupe_key.size() > 8192 || weight > 64 * 1024 * 1024 - alias_bytes) return std::nullopt;
				alias_bytes += weight;
				std::optional<AntigravityUsageCandidate> candidate = DecodeAntigravityAccountingRecord(*record);
				if (!candidate) return std::nullopt;
				candidates.push_back(std::move(*candidate));
			}
			else records.push_back(record);
		}
		std::vector<const AntigravityUsageCandidate*> aliases;
		for (const AntigravityUsageCandidate& candidate : candidates) aliases.push_back(&candidate);
		std::optional<std::vector<Record>> merged = MergeAntigravityCandidates(aliases);
		if (!merged) return std::nullopt;
		for (const Record& record : *merged) records.push_back(&record);
		// Validate before combining cohorts, including zero-token monetary records and overflow.
		const std::optional<Summary> checked = AggregateReferences(records, 0, kMaxExactTokenCount);
		if (!checked) return std::nullopt;
		Ledger ledger;
		ledger.m_generatedMs = generated_ms;
		ledger.m_range = range;
		ledger.m_duplicates = checked->duplicates + candidates.size() - merged->size();
		const std::int64_t start = ledger.IncludesLifetime() ? 0 : ledger.WindowStart(90), cutoff = ledger.WindowStart(1);
		std::map<std::pair<std::string, std::string>, std::size_t> originals;
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			const Record* record = records[index];
			if (record->timestamp_ms < 0 || record->timestamp_ms >= kMaxExactTokenCount || record->record_count != 1 || record->unidentified_records ||
			    !provider_ids::IsKnownCliProviderId(record->provider_id) || record->model_id.empty() || record->model_id.size() > 512 || record->session_id.size() > 8192 || record->dedupe_key.size() > 8192 ||
			    (record->speed != "standard" && record->speed != "fast" && record->speed != "ultrafast")) return std::nullopt;
			for (const std::optional<double> cost : {record->reported_cost_usd, record->estimated_cost_usd})
				if (cost && (!std::isfinite(*cost) || *cost < 0)) return std::nullopt;
			if (record->dedupe_key.empty()) continue;
			const std::pair<std::string, std::string> identity{record->provider_id, record->dedupe_key};
			const std::map<std::pair<std::string, std::string>, std::size_t>::iterator found = originals.find(identity);
			if (found == originals.end()) originals.emplace(identity, index);
			else if (record->timestamp_ms < records[found->second]->timestamp_ms) found->second = index;
		}
		std::map<CohortKey, Record> cohorts;
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			const Record* record = records[index];
			if (!record->dedupe_key.empty() && originals.at({record->provider_id, record->dedupe_key}) != index) continue;
			if (record->timestamp_ms < start || record->timestamp_ms >= generated_ms) continue;
			if (record->timestamp_ms >= cutoff) { ledger.m_records.push_back(*record); continue; }
			// Keep reported, saved estimated and unknown costs separate so later price overrides remain correct.
			const CohortKey key = Cohort(*record);
			const std::pair<std::map<CohortKey, Record>::iterator, bool> entry = cohorts.try_emplace(key, *record);
			Record& cohort = entry.first->second;
			if (entry.second)
			{
				cohort.timestamp_ms = std::get<0>(key);
				cohort.unidentified_records = record->dedupe_key.empty() ? 1 : 0;
				cohort.dedupe_key.clear();
			}
			else
			{
				cohort.tokens.input += record->tokens.input; cohort.tokens.cached_input += record->tokens.cached_input;
				cohort.tokens.cache_write += record->tokens.cache_write; cohort.tokens.output += record->tokens.output; cohort.tokens.reasoning += record->tokens.reasoning;
				++cohort.record_count;
				*cohort.unidentified_records += record->dedupe_key.empty() ? 1 : 0;
				if (cohort.reported_cost_usd) *cohort.reported_cost_usd += *record->reported_cost_usd;
				else if (cohort.estimated_cost_usd) *cohort.estimated_cost_usd += *record->estimated_cost_usd;
			}
		}
		for (std::pair<const CohortKey, Record>& cohort : cohorts) ledger.m_records.push_back(std::move(cohort.second));
		if (!Aggregate(ledger.m_records, start, generated_ms)) return std::nullopt;
		return ledger;
	}

	std::int64_t Ledger::WindowStart(int days) const
	{
		if (days == 0) return IncludesLifetime() ? 0 : -1;
		if (days == 1) return std::max<std::int64_t>(0, m_generatedMs - kDayMs);
		if (days != 7 && days != 30 && days != 90) return -1;
		return std::max<std::int64_t>(0, m_generatedMs / kDayMs * kDayMs - (days - 1LL) * kDayMs);
	}

	std::optional<Summary> Ledger::Query(std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices) const
	{
		const std::int64_t cutoff = WindowStart(1);
		if (start_ms < (IncludesLifetime() ? 0 : WindowStart(90)) || end_ms > m_generatedMs || (start_ms < cutoff && start_ms % kDayMs != 0) || (end_ms < cutoff && end_ms % kDayMs != 0)) return std::nullopt;
		std::optional<Summary> summary = Aggregate(m_records, start_ms, end_ms, prices);
		if (summary) summary->duplicates += m_duplicates;
		return summary;
	}

	nlohmann::json Ledger::Encode() const
	{
		nlohmann::json rows = nlohmann::json::array();
		for (const Record& record : m_records)
			rows.push_back(nlohmann::json::array({record.provider_id, record.model_id, record.session_id, record.speed, record.dedupe_key, record.timestamp_ms,
			    record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output, record.tokens.reasoning,
			    record.reported_cost_usd ? nlohmann::json(*record.reported_cost_usd) : nlohmann::json(nullptr),
			    record.estimated_cost_usd ? nlohmann::json(*record.estimated_cost_usd) : nlohmann::json(nullptr), record.record_count,
			    record.unidentified_records ? nlohmann::json(*record.unidentified_records) : nlohmann::json(nullptr)}));
		nlohmann::json payload{{"version", IncludesLifetime() ? 2 : 1}, {"generatedMs", m_generatedMs}, {"duplicates", m_duplicates}, {"rows", std::move(rows)}};
		if (IncludesLifetime()) payload["range"] = "all-recorded";
		return payload;
	}

	std::optional<Ledger> Ledger::Decode(const nlohmann::json& payload)
	{
		if (!payload.is_object() || !payload.contains("version") || (payload["version"] != 1 && payload["version"] != 2) || !payload.contains("generatedMs") || !payload.contains("duplicates") ||
		    !payload.contains("rows") || !payload["rows"].is_array() || payload["rows"].size() > kMaxRows) return std::nullopt;
		Ledger ledger;
		if (payload["version"] == 2)
		{
			if (!payload.contains("range") || payload["range"] != "all-recorded") return std::nullopt;
			ledger.m_range = LedgerRange::AllRecorded;
		}
		else if (payload.contains("range")) return std::nullopt;
		std::int64_t duplicates = 0;
		if (!Count(payload["generatedMs"], ledger.m_generatedMs) || ledger.m_generatedMs <= 0 || !Count(payload["duplicates"], duplicates)) return std::nullopt;
		ledger.m_duplicates = static_cast<std::size_t>(duplicates);
		const std::int64_t start = ledger.IncludesLifetime() ? 0 : ledger.WindowStart(90), cutoff = ledger.WindowStart(1);
		std::set<CohortKey> cohorts;
		std::set<std::pair<std::string, std::string>> recent_identities;
		for (const nlohmann::json& row : payload["rows"])
		{
			if (!row.is_array() || row.size() != 15) return std::nullopt;
			Record record;
			std::int64_t count = 0, unidentified = 0;
			if (!Text(row[0], record.provider_id, 160) || !provider_ids::IsKnownCliProviderId(record.provider_id) || !Text(row[1], record.model_id, 512) || record.model_id.empty() ||
			    !Text(row[2], record.session_id, 8192) || !Text(row[3], record.speed, 16) || (record.speed != "standard" && record.speed != "fast" && record.speed != "ultrafast") ||
			    !Text(row[4], record.dedupe_key, 8192) || !Count(row[5], record.timestamp_ms) || record.timestamp_ms < start || record.timestamp_ms >= ledger.m_generatedMs ||
			    !Count(row[6], record.tokens.input) || !Count(row[7], record.tokens.cached_input) || !Count(row[8], record.tokens.cache_write) ||
			    !Count(row[9], record.tokens.output) || !Count(row[10], record.tokens.reasoning) || !Cost(row[11], record.reported_cost_usd) || !Cost(row[12], record.estimated_cost_usd) ||
			    !Count(row[13], count) || count == 0 || (!row[14].is_null() && !Count(row[14], unidentified))) return std::nullopt;
			record.record_count = static_cast<std::size_t>(count);
			if (!row[14].is_null()) record.unidentified_records = static_cast<std::size_t>(unidentified);
			if (record.timestamp_ms < cutoff)
			{
				if (record.timestamp_ms % kDayMs != 0 || !record.dedupe_key.empty() || !record.unidentified_records || !cohorts.insert(Cohort(record)).second) return std::nullopt;
			}
			else if (record.record_count != 1 || record.unidentified_records) return std::nullopt;
			else if (!record.dedupe_key.empty() && !recent_identities.insert({record.provider_id, record.dedupe_key}).second) return std::nullopt;
			ledger.m_records.push_back(std::move(record));
		}
		if (!Aggregate(ledger.m_records, start, ledger.m_generatedMs)) return std::nullopt;
		return ledger;
	}
}
