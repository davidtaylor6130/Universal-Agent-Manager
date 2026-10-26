#include "common/usage/opencode_history_scan.h"
// Builds without this provider use the inline stubs in the header; test targets may still list this file.
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "common/utils/io_utils.h"

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace uam::usage
{
	namespace
	{
		using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
		using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

		struct QueryBudget
		{
			const std::function<bool()>& cancelled;
			std::chrono::steady_clock::time_point deadline;
		};

		int StopQuery(void* context)
		{
			const QueryBudget& budget = *static_cast<const QueryBudget*>(context);
			return (budget.cancelled && budget.cancelled()) || std::chrono::steady_clock::now() >= budget.deadline;
		}

		std::string ColumnText(sqlite3_stmt* statement, int column)
		{
			const unsigned char* bytes = sqlite3_column_text(statement, column);
			const int size = sqlite3_column_bytes(statement, column);
			return bytes && size > 0 && size <= 8192 ? std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size)) : std::string{};
		}
	}

	void OpenCodeHistoryScan::Reset()
	{
		m_path.clear(); m_startMs = -1; m_endMs = -1;
		m_table = 0; m_sawTable = false; m_cursor = 0; m_invalidRows = 0; m_suppressedCopies = 0; m_records.clear(); m_recordBase = 0;
	}

	ScanResult OpenCodeHistoryScan::Refresh(const std::filesystem::path& path, std::int64_t start_ms, std::int64_t end_ms, const std::function<bool()>& cancelled)
	{
		ScanResult result;
		if (cancelled && cancelled()) { result.cancelled = true; return result; }
		if (start_ms < 0 || end_ms <= start_ms || end_ms > kMaxExactTokenCount) { result.error = "Invalid OpenCode usage window."; return result; }
		if (m_path != path || m_startMs != start_ms || m_endMs != end_ms)
		{
			Reset(); m_path = path; m_startMs = start_ms; m_endMs = end_ms; result.reset = true;
		}
		if (m_table == 2) { result.complete = true; return result; }
		std::filesystem::path native_path;
		std::string error;
		if (!io::NativeFilePath(path, native_path, error)) { result.error = "Could not resolve OpenCode history path."; return result; }
		const std::u8string utf8 = native_path.u8string();
		const std::string filename(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		sqlite3* raw_database = nullptr;
		const int opened = sqlite3_open_v2(filename.c_str(), &raw_database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
		Database database(raw_database, sqlite3_close);
		if (opened != SQLITE_OK) { result.error = "Could not open OpenCode usage history read-only."; return result; }
		sqlite3_busy_timeout(database.get(), 50);
		sqlite3_limit(database.get(), SQLITE_LIMIT_LENGTH, 8 * 1024 * 1024);
		QueryBudget budget{cancelled, std::chrono::steady_clock::now() + std::chrono::milliseconds(250)};
		sqlite3_progress_handler(database.get(), 1000, StopQuery, &budget);
		const char* table = m_table == 0 ? "message" : "session_message";
		sqlite3_stmt* raw_probe = nullptr;
		if (sqlite3_prepare_v2(database.get(), "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", -1, &raw_probe, nullptr) != SQLITE_OK)
		{ result.error = "Could not inspect OpenCode usage schema."; return result; }
		Statement probe(raw_probe, sqlite3_finalize);
		sqlite3_bind_text(probe.get(), 1, table, -1, SQLITE_STATIC);
		const int present = sqlite3_step(probe.get());
		if (present == SQLITE_DONE)
		{
			++m_table; m_cursor = 0; result.bytes_read = 1; result.complete = m_table == 2;
			if (result.complete && !m_sawTable) result.error = "OpenCode usage schema is unsupported.";
			return result;
		}
		if (present != SQLITE_ROW) { result.error = "Could not inspect OpenCode usage schema."; return result; }
		m_sawTable = true;
		// Project only accounting fields; never transfer message content into the usage service or renderer.
		// Rowid keysets bound each batch even when legacy databases lack a global timestamp index.
		const std::string query = "SELECT m.rowid,m.id,m.session_id,m.time_created, CASE WHEN m.time_created>=?2 AND m.time_created<?3 AND json_valid(m.data) THEN "
		    "json_object('role',json_extract(m.data,'$.role'),'type'," + std::string(m_table == 0 ? "NULL" : "m.type") +
		    ", 'tokens',json_extract(m.data,'$.tokens'),'model',json_extract(m.data,'$.model'),"
		    "'modelID',json_extract(m.data,'$.modelID'),'providerID',json_extract(m.data,'$.providerID'),'cost',json_extract(m.data,'$.cost')) END,s.time_created "
		    "FROM " + table + " m LEFT JOIN session s ON s.id=m.session_id WHERE m.rowid>?1 ORDER BY m.rowid LIMIT 512";
		sqlite3_stmt* raw_statement = nullptr;
		if (sqlite3_prepare_v2(database.get(), query.c_str(), -1, &raw_statement, nullptr) != SQLITE_OK)
		{ result.error = "OpenCode usage schema is unsupported or unreadable."; return result; }
		Statement statement(raw_statement, sqlite3_finalize);
		sqlite3_bind_int64(statement.get(), 1, m_cursor);
		sqlite3_bind_int64(statement.get(), 2, start_ms); sqlite3_bind_int64(statement.get(), 3, end_ms);
		std::vector<Record> records;
		std::int64_t cursor = m_cursor;
		std::size_t invalid = 0, rows = 0, suppressed = 0;
		int status = SQLITE_OK;
		while ((status = sqlite3_step(statement.get())) == SQLITE_ROW)
		{
			if (cancelled && cancelled()) { result.cancelled = true; return result; }
			cursor = sqlite3_column_int64(statement.get(), 0); ++rows;
			const std::int64_t created = sqlite3_column_int64(statement.get(), 3);
			const std::string projected = ColumnText(statement.get(), 4);
			result.bytes_read += projected.size() + 1;
			if (created < start_ms || created >= end_ms) continue;
			if (sqlite3_column_type(statement.get(), 3) != SQLITE_INTEGER) { ++invalid; continue; }
			const nlohmann::json message = nlohmann::json::parse(projected, nullptr, false);
			if (message.is_discarded()) { ++invalid; continue; }
			if (message.value("role", nlohmann::json{}) != "assistant" && message.value("type", nlohmann::json{}) != "assistant") continue;
			const std::int64_t session_created = sqlite3_column_int64(statement.get(), 5);
			if (sqlite3_column_type(statement.get(), 5) != SQLITE_INTEGER || session_created < 0 || session_created > kMaxExactTokenCount) { ++invalid; continue; }
			// Forks keep message timestamps and costs but receive new IDs and a later session creation time.
			// shortcut: clock rollback can resemble a fork copy; use original-message identity when OpenCode exposes it.
			if (created < session_created) { ++suppressed; continue; }
			std::optional<Record> record;
			try { record = OpenCodeRecord(message, ColumnText(statement.get(), 2), ColumnText(statement.get(), 1), created); }
			catch (const nlohmann::json::exception&) { ++invalid; continue; }
			if (record) records.push_back(std::move(*record)); else ++invalid;
		}
		if (status != SQLITE_DONE)
		{
			if (cancelled && cancelled()) result.cancelled = true;
			else result.error = status == SQLITE_INTERRUPT ? "OpenCode usage query exceeded its time budget. Refresh to retry." : "OpenCode usage history is locked, oversized or unreadable.";
			return result;
		}
		if (m_records.size() + records.size() > 100000) { result.error = "OpenCode usage exceeds the cached-record limit; totals are incomplete."; return result; }
		m_cursor = cursor; m_invalidRows += invalid; m_suppressedCopies += suppressed;
		for (Record& record : records) m_records.push_back(std::move(record));
		if (rows < 512) { ++m_table; m_cursor = 0; }
		result.complete = m_table == 2;
		return result;
	}
}
#endif // UAM_ENABLE_RUNTIME_OPENCODE_CLI
