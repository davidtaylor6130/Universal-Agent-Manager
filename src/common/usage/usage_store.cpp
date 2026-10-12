#include "common/usage/usage_store.h"
#include "common/usage/antigravity_accounting.h"
#include "common/utils/io_utils.h"

#include <cmath>
#include <memory>
#include <sqlite3.h>

namespace uam::usage
{
	namespace
	{
		using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
		using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

		Database Open(const std::filesystem::path& path, std::string& error)
		{
			Database database(nullptr, sqlite3_close);
			if (path.empty()) { error = "Usage storage has no data root."; return database; }
			std::error_code filesystem_error;
			for (const std::filesystem::path& checked : {path.parent_path(), path})
			{
				const std::filesystem::file_status status = std::filesystem::symlink_status(checked, filesystem_error);
				if (filesystem_error && filesystem_error != std::errc::no_such_file_or_directory) { error = "Could not inspect usage storage."; return database; }
				if (std::filesystem::is_symlink(status)) { error = "Usage storage cannot be a symbolic link."; return database; }
			}
			std::filesystem::path native;
			if (!io::NativeFilePath(path, native, error)) return database;
			if (!paths::CreateDirectoriesNoThrow(native.parent_path())) { error = "Could not create usage storage."; return database; }
			const std::u8string utf8 = native.u8string();
			const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
			sqlite3* raw = nullptr;
			const int status = sqlite3_open_v2(filename.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
			database.reset(raw);
			if (status != SQLITE_OK) { error = "Could not open usage storage."; database.reset(); return database; }
			sqlite3_busy_timeout(database.get(), 50);
			sqlite3_limit(database.get(), SQLITE_LIMIT_LENGTH, 64 * 1024);
			sqlite3_stmt* probe_raw = nullptr;
			if (sqlite3_prepare_v2(database.get(), "PRAGMA user_version", -1, &probe_raw, nullptr) != SQLITE_OK) { error = "Could not inspect usage storage version."; database.reset(); return database; }
			Statement probe(probe_raw, sqlite3_finalize);
			if (sqlite3_step(probe.get()) != SQLITE_ROW) { error = "Could not inspect usage storage version."; database.reset(); return database; }
			const int version = sqlite3_column_int(probe.get(), 0);
			probe.reset();
			if (version != 0 && version != 1 && version != 2) { error = "Usage storage version is unsupported."; database.reset(); return database; }
			if (version == 0 && sqlite3_exec(database.get(), "BEGIN; CREATE TABLE facts(identity TEXT PRIMARY KEY, created INTEGER NOT NULL, data TEXT NOT NULL) WITHOUT ROWID; CREATE INDEX facts_time ON facts(created); CREATE TABLE archived_facts(identity TEXT PRIMARY KEY, created INTEGER NOT NULL, data TEXT NOT NULL) WITHOUT ROWID; CREATE INDEX archived_facts_time ON archived_facts(created); PRAGMA user_version=2; COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
			{ error = "Could not initialize usage storage."; database.reset(); return database; }
			if (version == 1 && sqlite3_exec(database.get(), "BEGIN IMMEDIATE; CREATE TABLE archived_facts(identity TEXT PRIMARY KEY, created INTEGER NOT NULL, data TEXT NOT NULL) WITHOUT ROWID; CREATE INDEX archived_facts_time ON archived_facts(created); PRAGMA user_version=2; COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
			{ error = "Could not migrate usage storage."; database.reset(); return database; }
			return database;
		}

		bool Valid(const Record& record)
		{
			if (!provider_ids::IsKnownCliProviderId(record.provider_id) || record.model_id.empty() || record.model_id.size() > 512 || record.session_id.size() > 8192 || record.dedupe_key.size() > 8192 ||
			    record.timestamp_ms < 0 || record.timestamp_ms >= kMaxExactTokenCount || record.record_count != 1 || record.unidentified_records ||
			    (record.speed != "standard" && record.speed != "fast" && record.speed != "ultrafast")) return false;
			for (const std::int64_t count : {record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output, record.tokens.reasoning})
				if (count < 0 || count > kMaxExactTokenCount) return false;
			if (record.tokens.Total() > kMaxExactTokenCount || record.tokens.reasoning > record.tokens.output) return false;
			for (const std::optional<double> cost : {record.reported_cost_usd, record.estimated_cost_usd})
				if (cost && (!std::isfinite(*cost) || *cost < 0)) return false;
			if (record.provider_id == provider_ids::kAntigravityCli && record.dedupe_key.starts_with(kAntigravityAccountingPrefix) && !DecodeAntigravityAccountingRecord(record)) return false;
			return true;
		}

		int Stop(void* context)
		{
			const std::function<bool()>& cancelled = *static_cast<const std::function<bool()>*>(context);
			return cancelled && cancelled();
		}
	}

	Store::Store(std::filesystem::path data_root) : m_path(data_root.empty() ? std::filesystem::path{} : data_root / "usage" / "history-index-v1.sqlite") {}

	bool Store::Import(const std::vector<SourceRecord>& records, std::string& error, const std::function<bool()>& cancelled) const
	{
		error.clear();
		if (records.size() > 2048) { error = "Usage import exceeds its batch limit."; return false; }
		for (const SourceRecord& entry : records)
			if (!entry.record || !Valid(*entry.record) || (entry.record->dedupe_key.empty() && (entry.source_id.empty() || entry.source_id.size() > 8192)))
			{ error = "Usage accounting record is invalid."; return false; }
		if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return false; }
		Database database = Open(m_path, error);
		if (!database) return false;
		sqlite3_progress_handler(database.get(), 1000, Stop, const_cast<std::function<bool()>*>(&cancelled));
		if (sqlite3_exec(database.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Usage storage is busy."; return false; }
		sqlite3_stmt* raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "INSERT INTO facts(identity,created,data) SELECT ?1,?2,?3 WHERE NOT EXISTS(SELECT 1 FROM archived_facts WHERE identity=?1) ON CONFLICT(identity) DO UPDATE SET created=excluded.created,data=excluded.data WHERE excluded.created<=facts.created AND (excluded.created!=facts.created OR excluded.data!=facts.data)", -1, &raw, nullptr) != SQLITE_OK)
		{ error = "Could not prepare usage accounting update."; return false; }
		Statement statement(raw, sqlite3_finalize);
		if (sqlite3_prepare_v2(database.get(), "UPDATE archived_facts SET created=?2,data=?3 WHERE identity=?1 AND ?2<=created AND (?2!=created OR ?3!=data)", -1, &raw, nullptr) != SQLITE_OK)
		{ error = "Could not prepare archived accounting update."; return false; }
		Statement archived_update(raw, sqlite3_finalize);
		for (const SourceRecord& entry : records)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return false; }
			const Record& record = *entry.record;
			const std::string identity = record.dedupe_key.empty()
			    ? nlohmann::json::array({"source", record.provider_id, entry.source_id, entry.ordinal, record.timestamp_ms}).dump()
			    : nlohmann::json::array({"native", record.provider_id, record.dedupe_key}).dump();
			const std::string data = nlohmann::json::array({record.provider_id, record.model_id, record.session_id, record.speed, record.dedupe_key.empty(),
			    record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output, record.tokens.reasoning,
			    record.reported_cost_usd ? nlohmann::json(*record.reported_cost_usd) : nlohmann::json(nullptr),
			    record.estimated_cost_usd ? nlohmann::json(*record.estimated_cost_usd) : nlohmann::json(nullptr)}).dump();
			// Archived identities remain canonical when a copied transcript is stamped more recently.
			for (sqlite3_stmt* update : {archived_update.get(), statement.get()})
			{
				sqlite3_reset(update); sqlite3_clear_bindings(update);
				sqlite3_bind_text(update, 1, identity.c_str(), static_cast<int>(identity.size()), SQLITE_TRANSIENT);
				sqlite3_bind_int64(update, 2, record.timestamp_ms);
				sqlite3_bind_text(update, 3, data.c_str(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
				if (sqlite3_step(update) != SQLITE_DONE) { error = "Could not update usage accounting."; return false; }
			}
		}
		statement.reset();
		if (sqlite3_exec(database.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Could not commit usage accounting."; return false; }
		return true;
	}

	nlohmann::json EncodeAccountingRecord(const AccountingRecord& entry)
	{
		const Record& record = entry.record;
		return nlohmann::json::array({entry.identity, record.timestamp_ms, nlohmann::json::array({record.provider_id, record.model_id, record.session_id, record.speed, record.dedupe_key.empty(),
		    record.tokens.input, record.tokens.cached_input, record.tokens.cache_write, record.tokens.output, record.tokens.reasoning,
		    record.reported_cost_usd ? nlohmann::json(*record.reported_cost_usd) : nlohmann::json(nullptr),
		    record.estimated_cost_usd ? nlohmann::json(*record.estimated_cost_usd) : nlohmann::json(nullptr)})});
	}

	std::optional<AccountingRecord> DecodeAccountingRecord(const nlohmann::json& wire)
	{
		if (!wire.is_array() || wire.size() != 3 || !wire[0].is_string() || !wire[1].is_number_integer() || wire[1] < 0 || wire[1] >= kMaxExactTokenCount) return std::nullopt;
		const std::string& identity_text = wire[0].get_ref<const std::string&>();
		if (identity_text.empty() || identity_text.size() > 64 * 1024) return std::nullopt;
		const nlohmann::json identity = nlohmann::json::parse(identity_text, nullptr, false);
		const nlohmann::json& row = wire[2];
		if (!row.is_array() || row.size() != 12 || !row[4].is_boolean()) return std::nullopt;
		Record record;
		try
		{
			record.provider_id = row[0].get<std::string>(); record.model_id = row[1].get<std::string>(); record.session_id = row[2].get<std::string>(); record.speed = row[3].get<std::string>();
			record.timestamp_ms = wire[1].get<std::int64_t>();
			const bool unidentified = row[4].get<bool>();
			if (!identity.is_array() || identity.size() != (unidentified ? 5 : 3) || identity[0] != (unidentified ? "source" : "native") || identity[1] != record.provider_id)
			{ return std::nullopt; }
			if (unidentified)
			{
				if (!identity[2].is_string() || identity[2].get_ref<const std::string&>().empty() || identity[2].get_ref<const std::string&>().size() > 8192 ||
				    !identity[3].is_number_unsigned() || identity[4] != record.timestamp_ms) { return std::nullopt; }
			}
			else
			{
				record.dedupe_key = identity[2].get<std::string>();
				if (record.dedupe_key.empty()) return std::nullopt;
			}
			for (const std::pair<int, std::int64_t*> field : {std::pair<int, std::int64_t*>{5, &record.tokens.input}, {6, &record.tokens.cached_input}, {7, &record.tokens.cache_write}, {8, &record.tokens.output}, {9, &record.tokens.reasoning}})
			{
				if (!row[field.first].is_number_integer() || (row[field.first].is_number_unsigned() && row[field.first].get<std::uint64_t>() > static_cast<std::uint64_t>(kMaxExactTokenCount))) { return std::nullopt; }
				*field.second = row[field.first].get<std::int64_t>();
			}
			if (!row[10].is_null()) record.reported_cost_usd = row[10].get<double>();
			if (!row[11].is_null()) record.estimated_cost_usd = row[11].get<double>();
		}
		catch (const nlohmann::json::exception&) { return std::nullopt; }
		if (!Valid(record) || identity.dump() != identity_text) return std::nullopt;
		return AccountingRecord{identity_text, std::move(record)};
	}

	std::optional<std::vector<AccountingRecord>> Store::ReadRecords(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled) const
	{
		return ReadRecordsImpl(generated_ms, error, cancelled, false);
	}

	std::optional<std::vector<AccountingRecord>> Store::ReadLifetimeRecords(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled) const
	{
		return ReadRecordsImpl(generated_ms, error, cancelled, true);
	}

	std::optional<std::vector<AccountingRecord>> Store::ReadRecordsImpl(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled, bool lifetime) const
	{
		error.clear();
		if (generated_ms <= 0 || generated_ms > kMaxExactTokenCount) { error = "Usage collection time is invalid."; return std::nullopt; }
		if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return std::nullopt; }
		Database database = Open(m_path, error);
		if (!database) return std::nullopt;
		sqlite3_progress_handler(database.get(), 1000, Stop, const_cast<std::function<bool()>*>(&cancelled));
		const std::int64_t retention_start = std::max<std::int64_t>(0, generated_ms / 86400000 * 86400000 - 89LL * 86400000);
		const std::int64_t start = lifetime ? 0 : retention_start;
		const std::string alias_lower = "[\"native\",\"antigravity-cli\",";
		std::string alias_upper = alias_lower; ++alias_upper.back();
		// Resolve aliases before pruning: an old reliable timestamp can suppress a recently stamped copy.
		if (sqlite3_exec(database.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Usage storage is busy."; return std::nullopt; }
		sqlite3_stmt* raw = nullptr;
		// Separate predicates keep rolling reads on the archive identity index instead of scanning lifetime facts.
		const std::string query = std::string("SELECT identity,created,data FROM facts WHERE (created>=?1 AND created<?2) OR (identity>=?3 AND identity<?4) UNION ALL SELECT identity,created,data FROM archived_facts WHERE ") +
		    (lifetime ? "created<?2 OR (identity>=?3 AND identity<?4)" : "identity>=?3 AND identity<?4") + " ORDER BY created LIMIT 500001";
		if (sqlite3_prepare_v2(database.get(), query.c_str(), -1, &raw, nullptr) != SQLITE_OK)
		{ error = "Could not read usage accounting."; return std::nullopt; }
		Statement statement(raw, sqlite3_finalize);
		sqlite3_bind_int64(statement.get(), 1, start); sqlite3_bind_int64(statement.get(), 2, generated_ms);
		sqlite3_bind_text(statement.get(), 3, alias_lower.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement.get(), 4, alias_upper.c_str(), -1, SQLITE_TRANSIENT);
		std::vector<AccountingRecord> records;
		std::size_t accounting_bytes = 0;
		int status = SQLITE_OK;
		while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
		{
			if (records.size() >= 500000) { error = "Usage accounting exceeds its record limit."; return std::nullopt; }
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return std::nullopt; }
			const char* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
			const int length = sqlite3_column_bytes(statement.get(), 2);
			if (!text || length <= 0 || length > 64 * 1024) { error = "Usage accounting record is corrupt."; return std::nullopt; }
			const nlohmann::json row = nlohmann::json::parse(text, text + length, nullptr, false);
			if (!row.is_array() || row.size() != 12 || !row[4].is_boolean()) { error = "Usage accounting record is corrupt."; return std::nullopt; }
			const char* identity_text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
			const int identity_size = sqlite3_column_bytes(statement.get(), 0);
			if (!identity_text || identity_size <= 0 || identity_size > 64 * 1024) { error = "Usage accounting identity is corrupt."; return std::nullopt; }
			accounting_bytes += static_cast<std::size_t>(identity_size + length);
			if (accounting_bytes > 64 * 1024 * 1024) { error = "Usage accounting exceeds its byte limit."; return std::nullopt; }
			std::optional<AccountingRecord> record = DecodeAccountingRecord(nlohmann::json::array({std::string(identity_text, identity_size), sqlite3_column_int64(statement.get(), 1), row}));
			if (!record) { error = "Usage accounting record is corrupt or invalid."; return std::nullopt; }
			records.push_back(std::move(*record));
		}
		if (status != SQLITE_DONE) { error = "Could not complete usage accounting read."; return std::nullopt; }
		statement.reset();
		std::vector<AntigravityUsageCandidate> candidates;
		std::vector<std::size_t> candidate_rows;
		std::size_t alias_bytes = 0;
		std::vector<bool> keep(records.size(), true);
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return std::nullopt; }
			const Record& record = records[index].record;
			if (record.provider_id == provider_ids::kAntigravityCli && record.dedupe_key.starts_with(kAntigravityAccountingPrefix))
			{
				const std::size_t weight = sizeof(AntigravityUsageCandidate) + record.model_id.size() + record.session_id.size() + 3 * record.dedupe_key.size();
				if (weight > 64 * 1024 * 1024 - alias_bytes) { error = "Antigravity aliases exceed the accounting limit."; return std::nullopt; }
				alias_bytes += weight;
				std::optional<AntigravityUsageCandidate> candidate = DecodeAntigravityAccountingRecord(record);
				if (!candidate) { error = "Antigravity accounting aliases are invalid."; return std::nullopt; }
				candidates.push_back(std::move(*candidate)); candidate_rows.push_back(index);
			}
			else keep[index] = record.timestamp_ms >= start;
		}
		std::vector<const AntigravityUsageCandidate*> references;
		for (const AntigravityUsageCandidate& candidate : candidates) references.push_back(&candidate);
		std::vector<std::size_t> owners;
		if (!MergeAntigravityCandidates(references, &owners)) { error = "Antigravity accounting aliases exceed supported limits."; return std::nullopt; }
		std::vector<bool> recent(candidates.size(), false);
		for (std::size_t index = 0; index < candidates.size(); ++index)
			if (candidates[index].record.timestamp_ms >= start) recent[owners[index]] = true;
		for (std::size_t index = 0; index < candidates.size(); ++index) keep[candidate_rows[index]] = recent[owners[index]];
		// Copy and remove within one transaction: cancellation or storage failure preserves the source.
		sqlite3_stmt* archive_raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "INSERT INTO archived_facts(identity,created,data) SELECT identity,created,data FROM facts WHERE created<?1 ON CONFLICT(identity) DO UPDATE SET created=excluded.created,data=excluded.data WHERE excluded.created<=archived_facts.created", -1, &archive_raw, nullptr) != SQLITE_OK)
		{ error = "Could not prepare accounting archival."; return std::nullopt; }
		Statement archive(archive_raw, sqlite3_finalize);
		sqlite3_bind_int64(archive.get(), 1, retention_start);
		if (sqlite3_step(archive.get()) != SQLITE_DONE) { error = "Could not archive usage accounting."; return std::nullopt; }
		archive.reset();
		sqlite3_stmt* prune_raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "DELETE FROM facts WHERE created<?1", -1, &prune_raw, nullptr) != SQLITE_OK)
		{ error = "Could not maintain usage retention."; return std::nullopt; }
		Statement prune(prune_raw, sqlite3_finalize);
		sqlite3_bind_int64(prune.get(), 1, retention_start);
		if (sqlite3_step(prune.get()) != SQLITE_DONE) { error = "Could not maintain usage retention."; return std::nullopt; }
		prune.reset();
		std::vector<AccountingRecord> retained;
		for (std::size_t index = 0; index < records.size(); ++index)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return std::nullopt; }
			if (keep[index]) retained.push_back(std::move(records[index]));
		}
		if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return std::nullopt; }
		if (sqlite3_exec(database.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Could not commit usage retention."; return std::nullopt; }
		records = std::move(retained);
		return records;
	}

	std::optional<Ledger> Store::Read(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled) const
	{
		const std::optional<std::vector<AccountingRecord>> records = ReadRecords(generated_ms, error, cancelled);
		if (!records) return std::nullopt;
		std::vector<const Record*> references;
		references.reserve(records->size());
		for (const AccountingRecord& entry : *records) references.push_back(&entry.record);
		std::optional<Ledger> ledger = Ledger::Create(references, generated_ms);
		if (!ledger) error = "Usage accounting totals exceed supported limits.";
		return ledger;
	}
}
