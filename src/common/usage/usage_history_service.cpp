#include "common/usage/usage_history_service.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"
#include "common/provider/copilot/cli/copilot_session_paths.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <nlohmann/json.hpp>

namespace uam::usage
{
	namespace
	{
		std::string DatabaseFingerprint(const std::filesystem::path& path)
		{
			std::string fingerprint;
			std::filesystem::path wal = path; wal += "-wal";
			for (const std::filesystem::path& file : {path, wal})
			{
				std::error_code error;
				const bool exists = std::filesystem::exists(file, error);
				if (error) return {};
				if (!exists) { fingerprint += "missing;"; continue; }
				const std::uintmax_t size = std::filesystem::file_size(file, error);
				if (error) return {};
				const std::filesystem::file_time_type modified = std::filesystem::last_write_time(file, error);
				if (error) return {};
				fingerprint += std::to_string(size) + ":" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count()) + ";";
			}
			return fingerprint;
		}
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

	nlohmann::json EmptyHistoryMetadata(std::string_view host_id)
	{
		nlohmann::json result{{"version", 3}, {"source", "native-transcripts"}, {"executionHostId", host_id}, {"complete", false},
		    {"filesChecked", 0}, {"filesTotal", 0}, {"bytesRead", 0}, {"listingLimited", false}};
		result["providers"] = nlohmann::json::array();
		for (const char* provider : provider_ids::kAllCliProviderIds)
			result["providers"].push_back({{"providerId", provider}, {"available", false}, {"files", 0}, {"errors", 0},
			    {"invalidLines", 0}, {"oversizedLines", 0}, {"suppressedForkRecords", 0}, {"pendingLines", 0}});
		return result;
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
		const std::optional<std::filesystem::path> configured_data = env::GetTrimmedPath("XDG_DATA_HOME");
		const std::optional<std::filesystem::path> user_home = env::GetUserHomePath();
		const std::filesystem::path opencode_root = (configured_data && configured_data->is_absolute() ? *configured_data : user_home ? *user_home / ".local" / "share" : std::filesystem::path{}) / "opencode";
		std::vector<std::filesystem::path> opencode_databases;
		const std::optional<std::filesystem::path> configured_database = env::GetTrimmedPath("OPENCODE_DB");
		if (configured_database)
		{
			if (*configured_database != ":memory:") opencode_databases.push_back(configured_database->is_absolute() ? *configured_database : opencode_root / *configured_database);
		}
		else if (user_home || (configured_data && configured_data->is_absolute()))
		{
			opencode_databases.push_back(opencode_root / "opencode.db");
			std::error_code error;
			for (std::filesystem::directory_iterator file(opencode_root, error), end; !error && file != end; file.increment(error))
				if (file->path().filename().string().starts_with("opencode-") && file->path().extension() == ".db") opencode_databases.push_back(file->path());
		}
		const std::filesystem::path codex_home = codex::CodexHomePath();
		const std::optional<std::filesystem::path> configured_claude = env::GetTrimmedPath("CLAUDE_CONFIG_DIR");
		const std::optional<std::filesystem::path> home = env::GetUserHomePath();
		const std::filesystem::path claude_home = configured_claude ? *configured_claude : home ? *home / ".claude" : std::filesystem::path{};
		const std::optional<std::filesystem::path> configured_gemini = env::GetTrimmedPath("GEMINI_CLI_HOME");
		const std::filesystem::path gemini_home = configured_gemini ? *configured_gemini : home.value_or(std::filesystem::path{});
		std::vector<std::filesystem::path> antigravity_roots;
		const std::optional<std::string> configured_antigravity = env::GetTrimmedString("ANTIGRAVITY_DATA_DIR");
		if (configured_antigravity)
		{
			std::size_t position = 0;
			while (position < configured_antigravity->size())
			{
				const std::size_t comma = configured_antigravity->find(',', position);
				std::string value = strings::Trim(configured_antigravity->substr(position, comma == std::string::npos ? comma : comma - position));
				if (home && value.starts_with("~/")) value = paths::Utf8PathString(*home / paths::PathFromUtf8(value.substr(2)));
				const std::filesystem::path path = paths::PathFromUtf8(value);
				if (!value.empty() && path.is_absolute()) antigravity_roots.push_back(path.lexically_normal());
				if (comma == std::string::npos) break;
				position = comma + 1;
			}
		}
		else if (home)
		{
			for (const char* name : {"antigravity", "antigravity-cli", "antigravity-ide", "antigravity-backup"}) antigravity_roots.push_back(*home / ".gemini" / name);
			antigravity_roots.push_back(*home / ".config" / "antigravity");
		}
		for (std::filesystem::path& root : antigravity_roots)
		{
			std::error_code error;
			const std::filesystem::path nested = root / "conversations";
			if (std::filesystem::is_directory(std::filesystem::symlink_status(nested, error))) root = nested;
		}
		return {{provider_ids::kGeminiCli, gemini_home.empty() ? std::vector<std::filesystem::path>{} : std::vector<std::filesystem::path>{gemini_home / ".gemini" / "tmp"}}, {provider_ids::kCodexCli, {codex_home / "sessions", codex_home / "archived_sessions"}},
		    {provider_ids::kOpenCodeCli, {}, std::move(opencode_databases)}, {provider_ids::kCopilotCli, {CopilotSessionStatePath()}}, {provider_ids::kClaudeCli, claude_home.empty() ? std::vector<std::filesystem::path>{} : std::vector<std::filesystem::path>{claude_home / "projects"}}, {provider_ids::kAntigravityCli, std::move(antigravity_roots)}};
	}

	bool HistoryService::Enumerate(const std::function<bool()>& cancelled)
	{
		if (cancelled && cancelled()) return false;
		std::set<std::pair<std::string, std::filesystem::path>> seen;
		m_files.clear();
		m_databases.clear();
		m_nextDatabase = 0;
		m_antigravityDatabases.clear();
		m_nextAntigravityDatabase = 0;
		m_sourceErrors.clear();
		m_listingLimited = false;
		for (const HistorySource& source : m_sources)
		{
			if (source.provider_id == provider_ids::kOpenCodeCli || source.provider_id == provider_ids::kAntigravityCli)
			{
				for (const std::filesystem::path& database : source.databases)
				{
					std::error_code error;
					const std::filesystem::file_status status = std::filesystem::symlink_status(database, error);
					if (std::filesystem::is_regular_file(status))
					{
						const std::filesystem::path normalized = database.lexically_normal();
						if (seen.insert({source.provider_id, normalized}).second)
							(source.provider_id == provider_ids::kAntigravityCli ? m_antigravityDatabases : m_databases).push_back(normalized);
					}
					else if (std::filesystem::is_symlink(status)) ++m_sourceErrors[source.provider_id];
					else if (error && error != std::errc::no_such_file_or_directory) ++m_sourceErrors[source.provider_id];
					if (m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) { m_listingLimited = true; break; }
				}
				if (m_listingLimited && m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) break;
			}
			for (const std::filesystem::path& directory : source.directories)
			{
				std::error_code error;
				if (source.provider_id == provider_ids::kAntigravityCli && std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error)))
				{ ++m_sourceErrors[source.provider_id]; continue; }
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
					if (source.provider_id == provider_ids::kAntigravityCli)
					{
						if (iterator->path().extension() != ".db") continue;
						if (!iterator->is_regular_file(error))
						{
							if (error) { ++m_sourceErrors[source.provider_id]; error.clear(); }
							continue;
						}
						const std::filesystem::path normalized = iterator->path().lexically_normal();
						if (seen.insert({source.provider_id, normalized}).second) m_antigravityDatabases.push_back(normalized);
						if (m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) { m_listingLimited = true; break; }
						continue;
					}
					if (iterator->path().extension() != ".jsonl" && !(source.provider_id == provider_ids::kGeminiCli && iterator->path().extension() == ".json")) continue;
					if (source.provider_id == provider_ids::kCopilotCli && iterator->path().filename() != "events.jsonl") continue;
					if (source.provider_id == provider_ids::kGeminiCli)
					{
						const std::filesystem::path parent = iterator->path().parent_path();
						const bool main_session = parent.filename() == "chats" && iterator->path().filename().string().starts_with("session-");
						const bool subagent_session = parent.parent_path().filename() == "chats";
						if (!main_session && !subagent_session) continue;
					}
					if (!iterator->is_regular_file(error))
					{
						if (error) { ++m_sourceErrors[source.provider_id]; error.clear(); }
						continue;
					}
					const std::pair<std::string, std::filesystem::path> file{source.provider_id, iterator->path().lexically_normal()};
					if (seen.insert(file).second) m_files.push_back(file);
					if (m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) { m_listingLimited = true; break; }
				}
				if (error) ++m_sourceErrors[source.provider_id];
				if (m_listingLimited && m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) break;
			}
			if (m_listingLimited && m_files.size() + m_databases.size() + m_antigravityDatabases.size() >= 20000) break;
		}
		std::sort(m_databases.begin(), m_databases.end());
		m_databases.erase(std::unique(m_databases.begin(), m_databases.end()), m_databases.end());
		std::sort(m_antigravityDatabases.begin(), m_antigravityDatabases.end());
		m_antigravityDatabases.erase(std::unique(m_antigravityDatabases.begin(), m_antigravityDatabases.end()), m_antigravityDatabases.end());
		if (!m_listingLimited && !m_sourceErrors.contains(provider_ids::kAntigravityCli))
			std::erase_if(m_antigravityCache, [&](const std::pair<const std::filesystem::path, AntigravityHistoryScan>& entry) { return !std::binary_search(m_antigravityDatabases.begin(), m_antigravityDatabases.end(), entry.first); });
		std::sort(m_files.begin(), m_files.end());
		if (!m_listingLimited && !m_sourceErrors.contains(provider_ids::kOpenCodeCli))
			std::erase_if(m_databaseCache, [&](const std::pair<const std::filesystem::path, OpenCodeHistoryScan>& entry) { return !std::binary_search(m_databases.begin(), m_databases.end(), entry.first); });
		// Only a successful, complete listing proves that a previously cached file was removed.
		if (!m_listingLimited)
			std::erase_if(m_cache, [&](const std::pair<const std::pair<std::string, std::filesystem::path>, TranscriptScan>& entry)
			{
				return !m_sourceErrors.contains(entry.first.first) && !seen.contains(entry.first);
			});
		m_nextFile = 0;
		return true;
	}

	nlohmann::json HistoryService::Refresh(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms,
	                                     const std::map<ModelKey, ModelPrices>& prices, const std::function<bool()>& cancelled)
	{
		return RefreshImpl(refresh_id, start_ms, end_ms, prices, cancelled, {});
	}

	nlohmann::json HistoryService::Collect(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms, const AccountingSink& sink, const std::function<bool()>& cancelled)
	{
		return RefreshImpl(refresh_id, start_ms, end_ms, {}, cancelled, sink);
	}

	nlohmann::json HistoryService::RefreshImpl(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices, const std::function<bool()>& cancelled, const AccountingSink& sink)
	{
		const std::function<bool(const std::pair<std::string, std::filesystem::path>&, const std::vector<Record>&, bool, std::string&)> account = [&](const std::pair<std::string, std::filesystem::path>& source, const std::vector<Record>& records, bool reset, std::string& error)
		{
			if (!sink) return true;
			std::size_t& position = m_accountedPositions[source];
			if (reset || position > records.size()) position = 0;
			const std::u8string utf8 = source.second.u8string();
			const std::string identity(reinterpret_cast<const char*>(utf8.data()), utf8.size());
			if (position < records.size() && !sink(identity, records, position, error)) return false;
			const std::size_t prior_position = position;
			position = records.size();
			// Future-dated receipts stay pending until the collector window reaches them.
			for (std::size_t index = prior_position; index < records.size(); ++index)
				if (records[index].timestamp_ms >= end_ms) { position = index; break; }
			return true;
		};
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
			std::string accounting_error;
			if (!account(file, entry.first->second.Records(), scanned.reset, accounting_error)) return {{"error", accounting_error}};
			if (!scanned.error.empty()) ++m_sourceErrors[file.first];
			// A stable unfinished tail is cached and resumed on the next refresh, not spun on immediately.
			if (scanned.complete || !scanned.error.empty() || scanned.bytes_read < options.byte_budget) ++m_nextFile;
			else break;
		}
		if (m_nextFile == m_files.size() && m_nextDatabase < m_databases.size())
		{
			const std::filesystem::path& path = m_databases[m_nextDatabase];
			OpenCodeHistoryScan& scan = m_databaseCache[path];
			const std::string fingerprint = sink ? DatabaseFingerprint(path) : std::string{};
			if (sink && !fingerprint.empty() && m_completedDatabaseFingerprints[path] == fingerprint)
			{
				std::string accounting_error;
				if (!account({provider_ids::kOpenCodeCli, path}, scan.Records(), false, accounting_error)) return {{"error", accounting_error}};
				++m_nextDatabase;
			}
			else
			{
				if (sink && m_databaseScanRefreshIds[path] != refresh_id)
				{
					scan.Reset();
					m_accountedPositions.erase({provider_ids::kOpenCodeCli, path});
					m_databaseScanRefreshIds[path] = std::string(refresh_id);
				}
				const ScanResult scanned = scan.Refresh(path, start_ms, end_ms, cancelled);
				if (scanned.reset) m_pendingDatabaseFingerprints[path] = fingerprint;
				bytes_read += scanned.bytes_read;
				if (scanned.cancelled) return {{"cancelled", true}};
				std::string accounting_error;
				if (!account({provider_ids::kOpenCodeCli, path}, scan.Records(), scanned.reset, accounting_error)) return {{"error", accounting_error}};
				if (!scanned.error.empty()) ++m_sourceErrors[provider_ids::kOpenCodeCli];
				if (sink && scanned.complete && scanned.error.empty()) m_completedDatabaseFingerprints[path] = m_pendingDatabaseFingerprints[path];
				if (scanned.complete || !scanned.error.empty()) ++m_nextDatabase;
			}
		}
		if (m_nextFile == m_files.size() && m_nextDatabase == m_databases.size() && m_nextAntigravityDatabase < m_antigravityDatabases.size() && bytes_read < byte_budget)
		{
			const std::filesystem::path& path = m_antigravityDatabases[m_nextAntigravityDatabase];
			AntigravityHistoryScan& scan = m_antigravityCache[path];
			const ScanResult scanned = scan.Refresh(path, cancelled);
			bytes_read += scanned.bytes_read;
			if (scanned.cancelled) return {{"cancelled", true}};
			if (!scanned.error.empty()) ++m_sourceErrors[provider_ids::kAntigravityCli];
			if (scanned.complete || !scanned.error.empty()) ++m_nextAntigravityDatabase;
		}
		std::vector<const Record*> records;
		std::size_t cached_records = 0;
		std::map<std::string, std::size_t> invalid_lines, oversized_lines, suppressed_forks, source_files, pending_lines;
		for (const std::pair<const std::pair<std::string, std::filesystem::path>, TranscriptScan>& entry : m_cache)
		{
			++source_files[entry.first.first];
			invalid_lines[entry.first.first] += entry.second.InvalidLines();
			oversized_lines[entry.first.first] += entry.second.OversizedLines();
			suppressed_forks[entry.first.first] += entry.second.SuppressedForkRecords();
			if (entry.second.PendingTail()) ++pending_lines[entry.first.first];
			if (!sink) for (const Record& record : entry.second.Records()) records.push_back(&record);
			if (entry.second.Records().size() > 500000 - cached_records) return {{"error", "Usage history exceeds the aggregate-record limit; totals are incomplete."}};
			cached_records += entry.second.Records().size();
		}
		for (const std::pair<const std::filesystem::path, OpenCodeHistoryScan>& entry : m_databaseCache)
		{
			++source_files[provider_ids::kOpenCodeCli];
			invalid_lines[provider_ids::kOpenCodeCli] += entry.second.InvalidRows();
			suppressed_forks[provider_ids::kOpenCodeCli] += entry.second.SuppressedForkRecords();
			if (!sink) for (const Record& record : entry.second.Records()) records.push_back(&record);
			if (entry.second.Records().size() > 500000 - cached_records) return {{"error", "Usage history exceeds the aggregate-record limit; totals are incomplete."}};
			cached_records += entry.second.Records().size();
		}
		std::vector<const AntigravityUsageCandidate*> antigravity_candidates;
		for (const std::pair<const std::filesystem::path, AntigravityHistoryScan>& entry : m_antigravityCache)
		{
			++source_files[provider_ids::kAntigravityCli]; invalid_lines[provider_ids::kAntigravityCli] += entry.second.InvalidRows();
			if (entry.second.Candidates().size() > 500000 - cached_records) return {{"error", "Usage history exceeds the aggregate-record limit; totals are incomplete."}};
			cached_records += entry.second.Candidates().size();
			if (!sink || m_nextAntigravityDatabase == m_antigravityDatabases.size())
				for (const AntigravityUsageCandidate& candidate : entry.second.Candidates()) antigravity_candidates.push_back(&candidate);
		}
		std::optional<std::vector<Record>> antigravity_records;
		if (!sink)
		{
			antigravity_records = MergeAntigravityCandidates(antigravity_candidates);
			if (!antigravity_records) return {{"error", "Antigravity accounting aliases exceed supported limits."}};
			for (const Record& record : *antigravity_records) records.push_back(&record);
		}
		else if (m_nextAntigravityDatabase == m_antigravityDatabases.size() && m_antigravityAccountedRefreshId != refresh_id)
		{
			std::vector<Record> accounting;
			std::size_t accounting_bytes = 0;
			for (const AntigravityUsageCandidate* candidate : antigravity_candidates)
			{
				if (cancelled && cancelled()) return {{"cancelled", true}};
				std::optional<Record> record = EncodeAntigravityAccountingRecord(*candidate);
				if (!record) return {{"error", "Antigravity accounting identity exceeds supported limits."}};
				const std::size_t weight = sizeof(Record) + record->dedupe_key.size() + record->session_id.size() + record->model_id.size();
				if (weight > 64 * 1024 * 1024 - accounting_bytes) return {{"error", "Antigravity accounting exceeds the byte limit."}};
				accounting_bytes += weight; accounting.push_back(std::move(*record));
			}
			std::string accounting_error;
			// Import the completed cache together, including unchanged stores and out-of-window time witnesses.
			if (!account({provider_ids::kAntigravityCli, "antigravity-history"}, accounting, true, accounting_error)) return {{"error", accounting_error}};
			m_antigravityAccountedRefreshId = std::string(refresh_id);
		}
		const std::optional<Summary> summary = AggregateReferences(records, start_ms, end_ms, prices);
		if (!summary) return {{"error", "Usage totals or prices exceed supported numeric limits."}};
		nlohmann::json result{{"version", 2}, {"executionHostId", "local"}, {"source", "native-transcripts"}, {"startMs", start_ms}, {"endMs", end_ms},
		    {"totals", SerializeTotals(summary->total)}, {"complete", m_nextFile == m_files.size() && m_nextDatabase == m_databases.size() && m_nextAntigravityDatabase == m_antigravityDatabases.size()}, {"filesChecked", m_nextFile + m_nextDatabase + m_nextAntigravityDatabase}, {"filesTotal", m_files.size() + m_databases.size() + m_antigravityDatabases.size()},
		    {"bytesRead", bytes_read}, {"duplicates", summary->duplicates}, {"unidentifiedRecords", summary->unidentified_records}, {"listingLimited", m_listingLimited}};
		result["providers"] = nlohmann::json::array();
		for (const char* provider : provider_ids::kAllCliProviderIds)
		{
			const std::vector<HistorySource>::const_iterator source = std::find_if(m_sources.begin(), m_sources.end(), [provider](const HistorySource& candidate) { return candidate.provider_id == provider; });
			nlohmann::json row = summary->providers.contains(provider) ? SerializeTotals(summary->providers.at(provider)) : SerializeTotals({});
			row["providerId"] = provider;
			row["available"] = source != m_sources.end() && (!source->directories.empty() || !source->databases.empty());
			row["files"] = source_files[provider];
			row["errors"] = m_sourceErrors[provider];
			row["invalidLines"] = invalid_lines[provider];
			row["oversizedLines"] = oversized_lines[provider];
			row["suppressedForkRecords"] = suppressed_forks[provider];
			row["pendingLines"] = pending_lines[provider];
			result["providers"].push_back(std::move(row));
		}
		ApplyUsageSummary(result, *summary, start_ms, end_ms);
		return result;
	}
	void ApplyUsageSummary(nlohmann::json& result, const Summary& summary, std::int64_t start_ms, std::int64_t end_ms)
	{
		result["startMs"] = start_ms; result["endMs"] = end_ms; result["totals"] = SerializeTotals(summary.total);
		result["duplicates"] = summary.duplicates; result["unidentifiedRecords"] = summary.unidentified_records;
		for (nlohmann::json& row : result["providers"])
		{
			const std::string provider = row.value("providerId", std::string{});
			row.update(SerializeTotals(summary.providers.contains(provider) ? summary.providers.at(provider) : Totals{}));
		}
		result["models"] = nlohmann::json::array();
		for (const std::pair<const ModelKey, Totals>& model : summary.models)
		{
			nlohmann::json row = SerializeTotals(model.second);
			row["providerId"] = std::get<0>(model.first); row["modelId"] = std::get<1>(model.first); row["speed"] = std::get<2>(model.first);
			result["models"].push_back(std::move(row));
		}
		result["days"] = nlohmann::json::array(); result["hours"] = nlohmann::json::array();
		for (const std::pair<const std::int64_t, Totals>& bucket : summary.days)
		{
			nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first; result["days"].push_back(std::move(row));
		}
		if (end_ms - start_ms <= 86400000)
			for (const std::pair<const std::int64_t, Totals>& bucket : summary.hours)
			{
				nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first; result["hours"].push_back(std::move(row));
			}
		result["providerDays"] = nlohmann::json::array(); result["providerHours"] = nlohmann::json::array();
		for (const std::pair<const ProviderPeriodKey, Totals>& bucket : summary.provider_days)
		{
			nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first.first; row["providerId"] = bucket.first.second;
			result["providerDays"].push_back(std::move(row));
		}
		if (end_ms - start_ms <= 86400000)
			for (const std::pair<const ProviderPeriodKey, Totals>& bucket : summary.provider_hours)
			{
				nlohmann::json row = SerializeTotals(bucket.second); row["startMs"] = bucket.first.first; row["providerId"] = bucket.first.second;
				result["providerHours"].push_back(std::move(row));
			}
	}

}
