#include "common/usage/usage_history_service.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/utils/env_utils.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>

namespace uam::usage
{
	namespace
	{
		std::optional<std::int64_t> EpochField(const nlohmann::json& payload, const char* key)
		{
			const nlohmann::json* value = nlohmann_json::FindField(payload, key);
			if (!value || !value->is_number_integer()) return std::nullopt;
			if (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxExactTokenCount)) return std::nullopt;
			const std::int64_t epoch = value->get<std::int64_t>();
			return epoch >= 0 && epoch <= kMaxExactTokenCount ? std::optional<std::int64_t>{epoch} : std::nullopt;
		}
		nlohmann::json SerializeTotals(const Totals& totals)
		{
			return {{"inputTokens", totals.tokens.input}, {"cachedInputTokens", totals.tokens.cached_input}, {"cacheWriteTokens", totals.tokens.cache_write},
			    {"outputTokens", totals.tokens.output}, {"reasoningTokens", totals.tokens.reasoning}, {"totalTokens", totals.tokens.Total()},
			    {"reportedCostUsd", totals.reported_cost_usd}, {"estimatedCostUsd", totals.estimated_cost_usd}, {"estimatedCacheSavingsUsd", totals.estimated_cache_savings_usd},
			    {"reportedCostTokens", totals.reported_cost_tokens}, {"estimatedCostTokens", totals.estimated_cost_tokens}, {"unpricedTokens", totals.unpriced_tokens},
			    {"records", totals.records}, {"sessions", totals.sessions}};
		}
	}

	bool ParseHistoryRequest(const nlohmann::json& payload, HistoryRequest& request, std::string& error)
	{
		HistoryRequest parsed;
		parsed.refresh_id = std::string(nlohmann_json::StringViewOrEmpty(payload, "refreshId"));
		const nlohmann::json* host_value = nlohmann_json::FindField(payload, "executionHostId");
		if (host_value && !host_value->is_string()) { error = "Usage execution host must be a string."; return false; }
		const std::string_view host = nlohmann_json::StringViewOrEmpty(payload, "executionHostId");
		if (!host.empty()) parsed.execution_host_id = std::string(host);
		const std::optional<std::int64_t> start = EpochField(payload, "startMs"), end = EpochField(payload, "endMs");
		if (parsed.refresh_id.empty() || parsed.refresh_id.size() > 160 || parsed.execution_host_id.size() > 160 || !start || !end || *end <= *start || *end - *start > 90LL * 86400000)
		{
			error = "Choose a valid usage window of up to 90 days.";
			return false;
		}
		parsed.start_ms = *start; parsed.end_ms = *end;
		const nlohmann::json* overrides = nlohmann_json::FindField(payload, "priceOverrides");
		if (overrides && !overrides->is_null())
		{
			if (!overrides->is_array() || overrides->size() > 200) { error = "Usage price overrides must contain at most 200 models."; return false; }
			for (const nlohmann::json& row : *overrides)
			{
				const std::string provider(nlohmann_json::StringViewOrEmpty(row, "providerId")), model(nlohmann_json::StringViewOrEmpty(row, "modelId"));
				const std::string speed(nlohmann_json::StringViewOrEmpty(row, "speed"));
				if (!provider_ids::IsKnownCliProviderId(provider) || model.empty() || model.size() > 512 || (speed != "standard" && speed != "fast" && speed != "ultrafast"))
				{
					error = "A usage price override has an invalid provider, model or speed."; return false;
				}
				ModelPrices prices;
				for (const std::pair<const char*, std::optional<double>*> field : {
				    std::pair<const char*, std::optional<double>*>{"inputPerMillion", &prices.input_per_million},
				    {"cachedInputPerMillion", &prices.cached_input_per_million}, {"cacheWritePerMillion", &prices.cache_write_per_million}, {"outputPerMillion", &prices.output_per_million}})
				{
					const nlohmann::json* value = nlohmann_json::FindField(row, field.first);
					if (!value || value->is_null()) continue;
					if (!value->is_number()) { error = "Usage prices must be non-negative numbers."; return false; }
					const double price = value->get<double>();
					if (!std::isfinite(price) || price < 0) { error = "Usage prices must be non-negative numbers."; return false; }
					*field.second = price;
				}
				if (!parsed.prices.emplace(ModelKey{provider, model, speed}, prices).second) { error = "A usage model price override is duplicated."; return false; }
			}
		}
		request = std::move(parsed);
		error.clear();
		return true;
	}

	HistoryService::HistoryService() : m_resolveLocalSources(true) {}
	HistoryService::HistoryService(std::vector<HistorySource> sources) : m_sources(std::move(sources)) {}

	std::vector<HistorySource> LocalHistorySources()
	{
		const std::filesystem::path codex_home = codex::CodexHomePath();
		const std::optional<std::filesystem::path> configured_claude = env::GetTrimmedPath("CLAUDE_CONFIG_DIR");
		const std::optional<std::filesystem::path> home = env::GetUserHomePath();
		const std::filesystem::path claude_home = configured_claude ? *configured_claude : home ? *home / ".claude" : std::filesystem::path{};
		return {{provider_ids::kCodexCli, {codex_home / "sessions", codex_home / "archived_sessions"}},
		    {provider_ids::kClaudeCli, claude_home.empty() ? std::vector<std::filesystem::path>{} : std::vector<std::filesystem::path>{claude_home / "projects"}}};
	}

	bool HistoryService::Enumerate(const std::function<bool()>& cancelled)
	{
		std::set<std::pair<std::string, std::filesystem::path>> seen;
		m_files.clear();
		m_sourceErrors.clear();
		m_listingLimited = false;
		for (const HistorySource& source : m_sources)
		{
			for (const std::filesystem::path& directory : source.directories)
			{
				std::error_code error;
				if (!std::filesystem::exists(directory, error))
				{
					if (error) ++m_sourceErrors[source.provider_id];
					continue;
				}
				std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::none, error), end;
				if (error) { ++m_sourceErrors[source.provider_id]; continue; }
				for (; iterator != end; iterator.increment(error))
				{
					if (error) break;
					if (cancelled && cancelled()) return false;
					if (iterator.depth() >= 12) { iterator.disable_recursion_pending(); m_listingLimited = true; }
					const bool symlink = iterator->is_symlink(error);
					if (error) { ++m_sourceErrors[source.provider_id]; error.clear(); continue; }
					if (symlink) { iterator.disable_recursion_pending(); continue; }
					if (iterator->path().extension() != ".jsonl") continue;
					if (!iterator->is_regular_file(error))
					{
						if (error) { ++m_sourceErrors[source.provider_id]; error.clear(); }
						continue;
					}
					const std::pair<std::string, std::filesystem::path> file{source.provider_id, iterator->path().lexically_normal()};
					if (seen.insert(file).second) m_files.push_back(file);
					if (m_files.size() >= 20000) { m_listingLimited = true; break; }
				}
				if (error) ++m_sourceErrors[source.provider_id];
				if (m_listingLimited && m_files.size() >= 20000) break;
			}
			if (m_listingLimited && m_files.size() >= 20000) break;
		}
		std::sort(m_files.begin(), m_files.end());
		m_nextFile = 0;
		return true;
	}

	nlohmann::json HistoryService::Refresh(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms,
	                                     const std::map<ModelKey, ModelPrices>& prices, const std::function<bool()>& cancelled)
	{
		if (refresh_id.empty() || refresh_id.size() > 160 || start_ms < 0 || end_ms <= start_ms || end_ms - start_ms > 90LL * 86400000)
			return {{"error", "Choose a valid usage window of up to 90 days."}};
		if (m_resolveLocalSources)
		{
			m_sources = LocalHistorySources();
			m_resolveLocalSources = false;
		}
		if (m_refreshId != refresh_id)
		{
			m_refreshId = refresh_id;
			m_listingComplete = false;
		}
		if (!m_listingComplete)
		{
			if (!Enumerate(cancelled)) return {{"cancelled", true}};
			m_listingComplete = true;
		}
		std::size_t bytes_read = 0;
		std::size_t files_checked = 0;
		constexpr std::size_t byte_budget = 32 * 1024 * 1024;
		while (m_nextFile < m_files.size() && bytes_read < byte_budget && files_checked < 128)
		{
			if (cancelled && cancelled()) return {{"cancelled", true}};
			const std::pair<std::string, std::filesystem::path>& file = m_files[m_nextFile];
			const std::pair<std::map<std::pair<std::string, std::filesystem::path>, TranscriptScan>::iterator, bool> entry = m_cache.try_emplace(file, file.first);
			ScanOptions options;
			options.byte_budget = byte_budget - bytes_read;
			const ScanResult scanned = entry.first->second.Refresh(file.second, options, cancelled);
			bytes_read += scanned.bytes_read;
			++files_checked;
			if (scanned.cancelled) return {{"cancelled", true}};
			if (!scanned.error.empty()) ++m_sourceErrors[file.first];
			// A stable unfinished tail is cached and resumed on the next refresh, not spun on immediately.
			if (scanned.complete || !scanned.error.empty() || scanned.bytes_read < options.byte_budget) ++m_nextFile;
			else break;
		}
		std::vector<const Record*> records;
		std::map<std::string, std::size_t> invalid_lines, oversized_lines, suppressed_forks, source_files, pending_lines;
		for (const std::pair<const std::pair<std::string, std::filesystem::path>, TranscriptScan>& entry : m_cache)
		{
			++source_files[entry.first.first];
			invalid_lines[entry.first.first] += entry.second.InvalidLines();
			oversized_lines[entry.first.first] += entry.second.OversizedLines();
			suppressed_forks[entry.first.first] += entry.second.SuppressedForkRecords();
			if (entry.second.PendingTail()) ++pending_lines[entry.first.first];
			for (const Record& record : entry.second.Records()) records.push_back(&record);
			if (records.size() > 500000) return {{"error", "Usage history exceeds the aggregate-record limit; totals are incomplete."}};
		}
		const std::optional<Summary> summary = AggregateReferences(records, start_ms, end_ms, prices);
		if (!summary) return {{"error", "Usage totals or prices exceed supported numeric limits."}};
		nlohmann::json result{{"version", 2}, {"executionHostId", "local"}, {"source", "native-transcripts"}, {"startMs", start_ms}, {"endMs", end_ms},
		    {"totals", SerializeTotals(summary->total)}, {"complete", m_nextFile == m_files.size()}, {"filesChecked", m_nextFile}, {"filesTotal", m_files.size()},
		    {"bytesRead", bytes_read}, {"duplicates", summary->duplicates}, {"unidentifiedRecords", summary->unidentified_records}, {"listingLimited", m_listingLimited}};
		result["providers"] = nlohmann::json::array();
		for (const char* provider : provider_ids::kAllCliProviderIds)
		{
			const std::vector<HistorySource>::const_iterator source = std::find_if(m_sources.begin(), m_sources.end(), [provider](const HistorySource& candidate) { return candidate.provider_id == provider; });
			nlohmann::json row = summary->providers.contains(provider) ? SerializeTotals(summary->providers.at(provider)) : SerializeTotals({});
			row["providerId"] = provider;
			row["available"] = source != m_sources.end() && !source->directories.empty();
			row["files"] = source_files[provider];
			row["errors"] = m_sourceErrors[provider];
			row["invalidLines"] = invalid_lines[provider];
			row["oversizedLines"] = oversized_lines[provider];
			row["suppressedForkRecords"] = suppressed_forks[provider];
			row["pendingLines"] = pending_lines[provider];
			result["providers"].push_back(std::move(row));
		}
		result["models"] = nlohmann::json::array();
		for (const std::pair<const ModelKey, Totals>& model : summary->models)
		{
			nlohmann::json row = SerializeTotals(model.second);
			row["providerId"] = std::get<0>(model.first); row["modelId"] = std::get<1>(model.first); row["speed"] = std::get<2>(model.first);
			result["models"].push_back(std::move(row));
		}
		result["days"] = nlohmann::json::array(); result["hours"] = nlohmann::json::array();
		for (const std::pair<const std::int64_t, Totals>& bucket : summary->days)
		{
			nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first; result["days"].push_back(std::move(row));
		}
		if (end_ms - start_ms <= 86400000)
			for (const std::pair<const std::int64_t, Totals>& bucket : summary->hours)
			{
				nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first; result["hours"].push_back(std::move(row));
			}
		result["providerDays"] = nlohmann::json::array(); result["providerHours"] = nlohmann::json::array();
		for (const std::pair<const ProviderPeriodKey, Totals>& bucket : summary->provider_days)
		{
			nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first.first; row["providerId"] = bucket.first.second;
			result["providerDays"].push_back(std::move(row));
		}
		if (end_ms - start_ms <= 86400000)
			for (const std::pair<const ProviderPeriodKey, Totals>& bucket : summary->provider_hours)
			{
				nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first.first; row["providerId"] = bucket.first.second;
				result["providerHours"].push_back(std::move(row));
			}
		return result;
	}
}
