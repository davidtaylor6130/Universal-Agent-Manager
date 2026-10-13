#include "common/usage/usage_store.h"
#include "common/usage/antigravity_accounting.h"
#include "common/utils/io_utils.h"

#include <cmath>
#include <memory>
#include <sqlite3.h>
#include <fstream>
#include <atomic>
#include <set>
#include "common/utils/hash_utils.h"

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
		/// <summary>Disk-backed union/find preserves alias precedence without a complete candidate cache.</summary>
		class AliasReduction
		{
		public:
			AliasReduction()
			{
				sqlite3* raw = nullptr;
				if (sqlite3_open("", &raw) != SQLITE_OK) { sqlite3_close(raw); return; }
				m_database.reset(raw);
				if (sqlite3_exec(raw, "PRAGMA cache_size=-2048; CREATE TABLE groups(id INTEGER PRIMARY KEY,parent INTEGER,size INTEGER,owner INTEGER,quality INTEGER,model_owner INTEGER,step INTEGER,data TEXT); CREATE TABLE aliases(alias TEXT PRIMARY KEY,node INTEGER) WITHOUT ROWID; CREATE TABLE locations(location TEXT PRIMARY KEY,node INTEGER,created INTEGER) WITHOUT ROWID; CREATE TABLE provisional(node INTEGER PRIMARY KEY,location TEXT); BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) m_database.reset();
			}
			bool Add(const AntigravityUsageCandidate& candidate)
			{
				if (!m_database) return false;
				const std::int64_t index = m_count++;
				Group group{index, index, 1, index, candidate.timestamp_quality, index, candidate.is_step, candidate.record};
				if (!Write(group)) return false;
				std::vector<std::string> aliases = candidate.aliases;
				if (aliases.empty() && !candidate.location_id.empty())
				{
					aliases.push_back("location:" + candidate.location_id);
					if (!Execute("INSERT INTO provisional VALUES(?1,?2)", index, candidate.location_id)) return false;
				}
				else if (!candidate.location_id.empty())
				{
					Statement statement = Prepare("INSERT INTO locations VALUES(?1,?2,?3) ON CONFLICT(location) DO UPDATE SET node=excluded.node,created=excluded.created WHERE excluded.created<locations.created");
					if (!statement) return false;
					sqlite3_bind_text(statement.get(), 1, candidate.location_id.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(statement.get(), 2, index); sqlite3_bind_int64(statement.get(), 3, candidate.record.timestamp_ms);
					if (sqlite3_step(statement.get()) != SQLITE_DONE) return false;
				}
				if (aliases.empty()) aliases.push_back("fallback:" + candidate.record.dedupe_key);
				for (const std::string& alias : aliases)
				{
					Statement find = Prepare("SELECT node FROM aliases WHERE alias=?1");
					if (!find) return false;
					sqlite3_bind_text(find.get(), 1, alias.c_str(), -1, SQLITE_TRANSIENT);
					const int status = sqlite3_step(find.get());
					if (status == SQLITE_ROW)
					{
						const std::optional<Group> left_root = Root(index), right_root = Root(sqlite3_column_int64(find.get(), 0));
						if (!left_root || !right_root) return false;
						if (left_root->id != right_root->id)
						{
							Group target = *left_root, source = *right_root;
							if (target.size < source.size) std::swap(target, source);
							const Group& first = target.owner < source.owner ? target : source;
							const Group& best_time = source.quality > target.quality || (source.quality == target.quality && source.record.timestamp_ms < target.record.timestamp_ms) ? source : target;
							const Group* model = source.step > target.step || (source.step == target.step && source.model_owner < target.model_owner) ? &source : &target;
							const Group* other = model == &target ? &source : &target;
							if (model->record.model_id == "antigravity-unknown" && other->record.model_id != "antigravity-unknown") model = other;
							Record merged = first.record; merged.timestamp_ms = best_time.record.timestamp_ms; merged.model_id = model->record.model_id;
							merged.tokens.input = std::max(target.record.tokens.input, source.record.tokens.input); merged.tokens.cached_input = std::max(target.record.tokens.cached_input, source.record.tokens.cached_input);
							merged.tokens.cache_write = std::max(target.record.tokens.cache_write, source.record.tokens.cache_write); merged.tokens.output = std::max(target.record.tokens.output, source.record.tokens.output);
							merged.tokens.reasoning = std::min(merged.tokens.output, std::max(target.record.tokens.reasoning, source.record.tokens.reasoning)); merged.dedupe_key = std::min(target.record.dedupe_key, source.record.dedupe_key);
							const std::int64_t model_owner = model->model_owner, owner = first.owner; const int quality = best_time.quality; const bool step = model->step;
							target.record = std::move(merged); target.quality = quality; target.owner = owner; target.model_owner = model_owner; target.step = step; target.size += source.size; source.parent = target.id;
							if (!Write(target) || !Write(source)) return false;
						}
					}
					else if (status != SQLITE_DONE) return false;
					find.reset();
					Statement insert = Prepare("INSERT INTO aliases VALUES(?1,?2) ON CONFLICT(alias) DO UPDATE SET node=excluded.node");
					if (!insert) return false;
					sqlite3_bind_text(insert.get(), 1, alias.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_int64(insert.get(), 2, index);
					if (sqlite3_step(insert.get()) != SQLITE_DONE) return false;
				}
				return true;
			}
			bool Finish(const std::function<bool()>& cancelled)
			{
				if (!m_database) return false;
				Statement statement = Prepare("SELECT provisional.node,locations.node FROM provisional JOIN locations USING(location) ORDER BY provisional.node");
				if (!statement) return false;
				int status;
				while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
				{
					if (cancelled && cancelled()) return false;
					std::optional<Group> provisional = Root(sqlite3_column_int64(statement.get(), 0)), target = Root(sqlite3_column_int64(statement.get(), 1));
					if (!provisional || !target) return false;
					if (provisional->id == target->id) continue;
					if (provisional->quality > target->quality || (provisional->quality == target->quality && provisional->record.timestamp_ms < target->record.timestamp_ms))
					{ target->record.timestamp_ms = provisional->record.timestamp_ms; target->quality = provisional->quality; }
					provisional->parent = target->id;
					if (!Write(*provisional) || !Write(*target)) return false;
				}
				if (status != SQLITE_DONE) return false;
				statement.reset();
				Statement count = Prepare("SELECT count(*) FROM groups WHERE id=parent");
				if (!count || sqlite3_step(count.get()) != SQLITE_ROW) return false;
				m_duplicates = static_cast<std::size_t>(m_count - sqlite3_column_int64(count.get(), 0));
				return true;
			}
			bool Visit(const std::function<bool(const Record&)>& visitor, const std::function<bool()>& cancelled)
			{
				Statement statement = Prepare("SELECT id FROM groups WHERE id=parent ORDER BY id");
				if (!statement) return false;
				int status;
				while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
				{
					if (cancelled && cancelled()) return false;
					const std::optional<Group> group = Read(sqlite3_column_int64(statement.get(), 0));
					if (!group || !visitor(group->record)) return false;
				}
				return status == SQLITE_DONE;
			}
			std::size_t Duplicates() const { return m_duplicates; }
		private:
			struct Group
			{
				std::int64_t id, parent, size, owner;
				int quality;
				std::int64_t model_owner;
				bool step;
				Record record;
			};
			Statement Prepare(const char* sql)
			{
				sqlite3_stmt* raw = nullptr;
				if (!m_database || sqlite3_prepare_v2(m_database.get(), sql, -1, &raw, nullptr) != SQLITE_OK) return Statement(nullptr, sqlite3_finalize);
				return Statement(raw, sqlite3_finalize);
			}
			bool Execute(const char* sql, std::int64_t index, const std::string& text)
			{
				Statement statement = Prepare(sql); if (!statement) return false;
				sqlite3_bind_int64(statement.get(), 1, index); sqlite3_bind_text(statement.get(), 2, text.c_str(), -1, SQLITE_TRANSIENT);
				return sqlite3_step(statement.get()) == SQLITE_DONE;
			}
			bool Write(const Group& group)
			{
				Statement statement = Prepare("INSERT OR REPLACE INTO groups VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"); if (!statement) return false;
				const std::string data = EncodeAccountingRecord({nlohmann::json::array({"native", group.record.provider_id, group.record.dedupe_key}).dump(), group.record}).dump();
				sqlite3_bind_int64(statement.get(), 1, group.id); sqlite3_bind_int64(statement.get(), 2, group.parent); sqlite3_bind_int64(statement.get(), 3, group.size); sqlite3_bind_int64(statement.get(), 4, group.owner);
				sqlite3_bind_int(statement.get(), 5, group.quality); sqlite3_bind_int64(statement.get(), 6, group.model_owner); sqlite3_bind_int(statement.get(), 7, group.step); sqlite3_bind_text(statement.get(), 8, data.c_str(), -1, SQLITE_TRANSIENT);
				return sqlite3_step(statement.get()) == SQLITE_DONE;
			}
			std::optional<Group> Read(std::int64_t index)
			{
				Statement statement = Prepare("SELECT parent,size,owner,quality,model_owner,step,data FROM groups WHERE id=?1"); if (!statement) return std::nullopt;
				sqlite3_bind_int64(statement.get(), 1, index); if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
				const char* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 6));
				if (!text) return std::nullopt;
				try
				{
					// A merged alias group keeps the first session and minimum identity, which may belong to different copies.
					const nlohmann::json wire = nlohmann::json::parse(text), row = wire.at(2), identity = nlohmann::json::parse(wire.at(0).get<std::string>());
					Record record; record.provider_id = row.at(0).get<std::string>(); record.model_id = row.at(1).get<std::string>(); record.session_id = row.at(2).get<std::string>(); record.speed = row.at(3).get<std::string>(); record.dedupe_key = identity.at(2).get<std::string>(); record.timestamp_ms = wire.at(1).get<std::int64_t>();
					record.tokens = {row.at(5).get<std::int64_t>(), row.at(6).get<std::int64_t>(), row.at(7).get<std::int64_t>(), row.at(8).get<std::int64_t>(), row.at(9).get<std::int64_t>()};
					if (!row.at(10).is_null()) record.reported_cost_usd = row.at(10).get<double>(); if (!row.at(11).is_null()) record.estimated_cost_usd = row.at(11).get<double>();
					return Group{index, sqlite3_column_int64(statement.get(), 0), sqlite3_column_int64(statement.get(), 1), sqlite3_column_int64(statement.get(), 2), sqlite3_column_int(statement.get(), 3), sqlite3_column_int64(statement.get(), 4), sqlite3_column_int(statement.get(), 5) != 0, std::move(record)};
				}
				catch (const nlohmann::json::exception&) { return std::nullopt; }
			}
			std::optional<Group> Root(std::int64_t index)
			{
				std::optional<Group> group = Read(index);
				while (group && group->parent != group->id) group = Read(group->parent);
				return group;
			}
			Database m_database{nullptr, sqlite3_close};
			std::int64_t m_count = 0;
			std::size_t m_duplicates = 0;
		};

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

	ReceiptSnapshot::~ReceiptSnapshot()
	{
		if (owned) { std::error_code error; std::filesystem::remove(path, error); }
	}

	bool ReceiptSnapshot::Visit(const std::function<bool(const AccountingRecord&)>& visitor, std::string& error, const std::function<bool()>& cancelled) const
	{
		error.clear();
		std::ifstream input(path, std::ios::binary);
		if (!input) { error = "Could not open accounting receipts."; return false; }
		std::vector<char> buffer(512 * 1024 + 1);
		if (!input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.eof()) { error = "Accounting metadata is invalid."; return false; }
		const nlohmann::json header = nlohmann::json::parse(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount() - 1)), nullptr, false);
		if (!header.is_object() || header.value("version", 0) != 3 || header.value("range", "") != (lifetime ? "all-recorded" : "rolling-90-days") || header.value("asOfMs", std::int64_t{0}) != as_of || !header.contains("metadata") || header["metadata"] != metadata)
		{ error = "Accounting snapshot changed while loading."; return false; }
		sqlite3* raw_database = nullptr;
		if (sqlite3_open("", &raw_database) != SQLITE_OK) { sqlite3_close(raw_database); error = "Could not validate accounting identities."; return false; }
		Database identities(raw_database, sqlite3_close);
		if (sqlite3_exec(identities.get(), "PRAGMA cache_size=-2048; CREATE TABLE identities(identity TEXT PRIMARY KEY) WITHOUT ROWID; BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Could not validate accounting identities."; return false; }
		sqlite3_stmt* identity_raw = nullptr;
		if (sqlite3_prepare_v2(identities.get(), "INSERT INTO identities VALUES(?1)", -1, &identity_raw, nullptr) != SQLITE_OK) { error = "Could not validate accounting identities."; return false; }
		Statement unique(identity_raw, sqlite3_finalize);
		while (input.peek() != std::char_traits<char>::eof())
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return false; }
			if (!input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.eof()) { error = "Accounting receipt is oversized or incomplete."; return false; }
			const std::optional<AccountingRecord> entry = DecodeAccountingRecord(nlohmann::json::parse(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount() - 1)), nullptr, false));
			const std::int64_t start = lifetime ? 0 : std::max<std::int64_t>(0, as_of / 86400000 * 86400000 - 89LL * 86400000);
			if (!entry || ((entry->record.timestamp_ms < start || entry->record.timestamp_ms >= as_of) && !DecodeAntigravityAccountingRecord(entry->record)))
			{ error = "Accounting receipt scope or identity is invalid."; return false; }
			sqlite3_reset(unique.get()); sqlite3_bind_text(unique.get(), 1, entry->identity.c_str(), -1, SQLITE_TRANSIENT);
			if (sqlite3_step(unique.get()) != SQLITE_DONE) { error = "Accounting snapshot contains duplicate identities."; return false; }
			if (!visitor(*entry)) { if (error.empty()) error = "Usage accounting streaming was interrupted."; return false; }
		}
		if (input.bad()) { error = "Could not read accounting receipts."; return false; }
		return true;
	}

	std::shared_ptr<const ReceiptSnapshot> ReadReceiptSnapshot(const std::filesystem::path& path, std::string& error, const std::function<bool()>& cancelled)
	{
		error.clear();
		std::ifstream input(path, std::ios::binary);
		std::vector<char> buffer(512 * 1024 + 1);
		if (!input || !input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.eof()) { error = "Accounting metadata is invalid."; return {}; }
		const nlohmann::json header = nlohmann::json::parse(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount() - 1)), nullptr, false);
		if (!header.is_object() || !header.contains("version") || header["version"] != 3 || !header.contains("range") || (header["range"] != "all-recorded" && header["range"] != "rolling-90-days") ||
		    !header.contains("asOfMs") || !header["asOfMs"].is_number_integer() || header["asOfMs"] <= 0 || header["asOfMs"] >= kMaxExactTokenCount || header["asOfMs"] > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() || !header.contains("metadata") || !header["metadata"].is_object())
		{ error = "Accounting metadata scope is invalid."; return {}; }
		std::shared_ptr<ReceiptSnapshot> snapshot = std::make_shared<ReceiptSnapshot>();
		snapshot->path = path; snapshot->metadata = header["metadata"]; snapshot->as_of = header["asOfMs"].get<std::int64_t>(); snapshot->lifetime = header["range"] == "all-recorded";
		const nlohmann::json& metadata = snapshot->metadata;
		if (!metadata.contains("complete") || !metadata["complete"].is_boolean() || !metadata.contains("listingLimited") || !metadata["listingLimited"].is_boolean() || !metadata.contains("providers") || !metadata["providers"].is_array() || metadata["providers"].size() != provider_ids::kAllCliProviderIds.size())
		{ error = "Accounting coverage metadata is invalid."; return {}; }
		for (const char* key : {"filesChecked", "filesTotal", "bytesRead"})
			if (!metadata.contains(key) || !metadata[key].is_number_integer() || metadata[key] < 0 || metadata[key] > kMaxExactTokenCount) { error = "Accounting coverage counters are invalid."; return {}; }
		if (metadata["filesChecked"] > metadata["filesTotal"]) { error = "Accounting file coverage is invalid."; return {}; }
		std::set<std::string> providers;
		for (const nlohmann::json& row : metadata["providers"])
		{
			if (!row.is_object() || !row.contains("providerId") || !row["providerId"].is_string() || !provider_ids::IsKnownCliProviderId(row["providerId"].get_ref<const std::string&>()) || !providers.insert(row["providerId"].get<std::string>()).second || !row.contains("available") || !row["available"].is_boolean())
			{ error = "Accounting provider coverage is invalid."; return {}; }
			for (const char* key : {"files", "errors", "invalidLines", "oversizedLines", "suppressedForkRecords", "pendingLines"})
				if (!row.contains(key) || !row[key].is_number_integer() || row[key] < 0 || row[key] > kMaxExactTokenCount) { error = "Accounting provider counters are invalid."; return {}; }
		}

		input.clear(); input.seekg(0);
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		while (input)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return {}; }
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::size_t count = static_cast<std::size_t>(input.gcount());
			hashing::UpdateFnv1a64(hash, std::string_view(buffer.data(), count)); snapshot->bytes += count;
		}
		if (input.bad() || snapshot->bytes > static_cast<std::uint64_t>(kMaxExactTokenCount)) { error = "Could not read accounting receipts."; return {}; }
		snapshot->fingerprint = hashing::Hex64Padded(hash);
		return snapshot;
	}

	std::shared_ptr<const ReceiptSnapshot> Store::WriteReceiptSnapshot(std::int64_t generated_ms, const nlohmann::json& metadata, std::string& error, const std::function<bool()>& cancelled) const
	{
		const std::filesystem::path path = io::MakeTempWritePath(m_path.parent_path() / "receipts.jsonl");
		if (!paths::CreateDirectoriesNoThrow(path.parent_path())) { error = "Could not create accounting snapshot."; return {}; }
		std::shared_ptr<ReceiptSnapshot> snapshot = std::make_shared<ReceiptSnapshot>(); snapshot->path = path; snapshot->owned = true; snapshot->metadata = metadata; snapshot->as_of = generated_ms;
		std::uint64_t hash = hashing::kFnv1a64OffsetBasis;
		io::AtomicWriteResult written;
		const bool success = io::WriteAndSyncAtomicTemp(path, io::AtomicTempProducer([&](const std::function<bool(std::string_view)>& append)
		{
			const std::function<bool(const nlohmann::json&)> write = [&](const nlohmann::json& row)
			{
				const std::string line = row.dump() + "\n";
				if (line.size() > 512 * 1024 || line.size() > static_cast<std::uint64_t>(kMaxExactTokenCount) - snapshot->bytes || !append(line)) return false;
				hashing::UpdateFnv1a64(hash, line); snapshot->bytes += line.size(); return true;
			};
			return write({{"version", 3}, {"range", "all-recorded"}, {"asOfMs", generated_ms}, {"metadata", metadata}}) &&
			    VisitLifetimeRecords(generated_ms, [&](const AccountingRecord& entry) { return write(EncodeAccountingRecord(entry)); }, error, cancelled);
		}), {}, written);
		if (!success) { if (error.empty()) error = "Could not save accounting snapshot."; return {}; }

		snapshot->fingerprint = hashing::Hex64Padded(hash);
		return snapshot;
	}

	bool Store::VisitLifetimeRecords(std::int64_t generated_ms, const std::function<bool(const AccountingRecord&)>& visitor, std::string& error, const std::function<bool()>& cancelled) const
	{
		error.clear();
		if (!visitor || generated_ms <= 0 || generated_ms > kMaxExactTokenCount) { error = "Usage collection time is invalid."; return false; }
		Database database = Open(m_path, error);
		if (!database) return false;
		sqlite3_progress_handler(database.get(), 1000, Stop, const_cast<std::function<bool()>*>(&cancelled));
		sqlite3_stmt* raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "SELECT name FROM sqlite_master WHERE type='table' AND name='imported_facts'", -1, &raw, nullptr) != SQLITE_OK) return false;
		Statement probe(raw, sqlite3_finalize);
		const bool imported = sqlite3_step(probe.get()) == SQLITE_ROW; probe.reset();
		const std::string predicate = "created<?1 OR identity>=?2 AND identity<?3";
		const std::string query = imported ? "SELECT identity,created,data FROM imported_facts WHERE " + predicate + " ORDER BY created,scope,identity" :
		    "SELECT identity,created,data FROM facts WHERE " + predicate + " UNION ALL SELECT identity,created,data FROM archived_facts WHERE " + predicate + " ORDER BY created,identity";
		if (sqlite3_prepare_v2(database.get(), query.c_str(), -1, &raw, nullptr) != SQLITE_OK) { error = "Could not read usage accounting."; return false; }
		Statement statement(raw, sqlite3_finalize);
		const std::string lower = "[\"native\",\"antigravity-cli\",";
		std::string upper = lower; ++upper.back();
		sqlite3_bind_int64(statement.get(), 1, generated_ms);
		sqlite3_bind_text(statement.get(), 2, lower.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement.get(), 3, upper.c_str(), -1, SQLITE_TRANSIENT);
		int status = SQLITE_OK;
		while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return false; }
			const char* identity = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
			const char* text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
			const int identity_size = sqlite3_column_bytes(statement.get(), 0), size = sqlite3_column_bytes(statement.get(), 2);
			if (!identity || !text || identity_size <= 0 || identity_size > 64 * 1024 || size <= 0 || size > 64 * 1024) { error = "Usage accounting record is corrupt."; return false; }
			const std::optional<AccountingRecord> record = DecodeAccountingRecord(nlohmann::json::array({std::string(identity, identity_size), sqlite3_column_int64(statement.get(), 1), nlohmann::json::parse(text, text + size, nullptr, false)}));
			if (!record) { error = "Usage accounting record is corrupt or invalid."; return false; }
			if (!visitor(*record)) { if (error.empty()) error = "Usage accounting streaming was interrupted."; return false; }
		}
		if (status != SQLITE_DONE) { error = "Could not complete usage accounting read."; return false; }
		return true;
	}

	std::optional<Ledger> Store::ReadLifetimeLedger(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled) const
	{
		AliasReduction aliases;
		if (!VisitLifetimeRecords(generated_ms, [&](const AccountingRecord& entry)
		{
			if (entry.record.provider_id != provider_ids::kAntigravityCli || !entry.record.dedupe_key.starts_with(kAntigravityAccountingPrefix)) return true;
			const std::optional<AntigravityUsageCandidate> candidate = DecodeAntigravityAccountingRecord(entry.record);
			return candidate && aliases.Add(*candidate);
		}, error, cancelled) || !aliases.Finish(cancelled))
		{ if (error.empty()) error = "Could not resolve accounting aliases."; return std::nullopt; }
		std::size_t duplicates = aliases.Duplicates();
		Database database = Open(m_path, error);
		if (!database) return std::nullopt;
		sqlite3_stmt* stats_raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "SELECT duplicates FROM imported_stats", -1, &stats_raw, nullptr) == SQLITE_OK)
		{
			Statement stats(stats_raw, sqlite3_finalize);
			if (sqlite3_step(stats.get()) != SQLITE_ROW) { error = "Combined accounting duplicate count is invalid."; return std::nullopt; }
			const std::int64_t imported_duplicates = sqlite3_column_int64(stats.get(), 0);
			if (imported_duplicates < 0 || imported_duplicates > kMaxExactTokenCount - static_cast<std::int64_t>(duplicates)) { error = "Combined accounting duplicate count exceeds supported limits."; return std::nullopt; }
			duplicates += static_cast<std::size_t>(imported_duplicates);
		}
		std::optional<Ledger> ledger = Ledger::CreateCanonical([&](const std::function<bool(const Record&)>& visitor)
		{
			return VisitLifetimeRecords(generated_ms, [&](const AccountingRecord& entry)
			{
				return entry.record.provider_id == provider_ids::kAntigravityCli && entry.record.dedupe_key.starts_with(kAntigravityAccountingPrefix) ? true : visitor(entry.record);
			}, error, cancelled) && aliases.Visit(visitor, cancelled);
		}, generated_ms, duplicates);
		if (!ledger && error.empty()) error = "Usage accounting totals exceed supported limits.";
		return ledger;
	}

	bool Store::ImportAccounting(const std::vector<AccountingRecord>& records, std::string_view scope, std::string& error, const std::function<bool()>& cancelled) const
	{
		error.clear();
		if (records.size() > 2048 || scope.empty() || scope.size() > 512) { error = "Usage accounting batch is invalid."; return false; }
		Database database = Open(m_path, error);
		if (!database) return false;
		sqlite3_progress_handler(database.get(), 1000, Stop, const_cast<std::function<bool()>*>(&cancelled));
		if (sqlite3_exec(database.get(), "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS imported_facts(scope TEXT,identity TEXT,created INTEGER,data TEXT,PRIMARY KEY(scope,identity)) WITHOUT ROWID; CREATE TABLE IF NOT EXISTS imported_stats(duplicates INTEGER); INSERT INTO imported_stats SELECT 0 WHERE NOT EXISTS(SELECT 1 FROM imported_stats)", nullptr, nullptr, nullptr) != SQLITE_OK)
		{ error = "Could not prepare combined usage accounting."; return false; }
		sqlite3_stmt* raw = nullptr;
		if (sqlite3_prepare_v2(database.get(), "INSERT INTO imported_facts VALUES(?1,?2,?3,?4) ON CONFLICT(scope,identity) DO UPDATE SET created=excluded.created,data=excluded.data WHERE excluded.created<imported_facts.created", -1, &raw, nullptr) != SQLITE_OK) return false;
		Statement statement(raw, sqlite3_finalize);
		for (const AccountingRecord& entry : records)
		{
			if (cancelled && cancelled()) { error = "Usage collection was cancelled."; return false; }
			const nlohmann::json wire = EncodeAccountingRecord(entry);
			if (!DecodeAccountingRecord(wire)) { error = "Usage accounting record is invalid."; return false; }
			const std::string host_scope = entry.record.dedupe_key.empty() ? std::string(scope) : "";
			const std::string data = wire[2].dump();
			if (host_scope.empty())
			{
				sqlite3_stmt* duplicate_raw = nullptr;
				if (sqlite3_prepare_v2(database.get(), "UPDATE imported_stats SET duplicates=duplicates+1 WHERE EXISTS(SELECT 1 FROM imported_facts WHERE scope='' AND identity=?1)", -1, &duplicate_raw, nullptr) != SQLITE_OK) return false;
				Statement duplicate(duplicate_raw, sqlite3_finalize); sqlite3_bind_text(duplicate.get(), 1, entry.identity.c_str(), -1, SQLITE_TRANSIENT);
				if (sqlite3_step(duplicate.get()) != SQLITE_DONE) return false;
			}
			sqlite3_reset(statement.get()); sqlite3_clear_bindings(statement.get());
			sqlite3_bind_text(statement.get(), 1, host_scope.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(statement.get(), 2, entry.identity.c_str(), -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(statement.get(), 3, entry.record.timestamp_ms);
			sqlite3_bind_text(statement.get(), 4, data.c_str(), -1, SQLITE_TRANSIENT);
			if (sqlite3_step(statement.get()) != SQLITE_DONE) { error = "Could not merge usage accounting."; return false; }
		}
		statement.reset();
		if (sqlite3_exec(database.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) { error = "Could not commit combined usage accounting."; return false; }
		return true;
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
