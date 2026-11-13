#pragma once

#include "common/usage/usage_ledger.h"
#include <filesystem>
#include <functional>
#include <memory>

namespace uam::usage
{
	struct AccountingRecord
	{
		std::string identity;
		Record record;
	};

	/// <summary>Versioned wire rows retain provider identities before daily aggregation.</summary>
	nlohmann::json EncodeAccountingRecord(const AccountingRecord& record);
	std::optional<AccountingRecord> DecodeAccountingRecord(const nlohmann::json& row);

	/// <summary>Immutable streamed receipt file; individual rows remain bounded while history has no total transfer cap.</summary>
	struct ReceiptSnapshot
	{
		std::filesystem::path path;
		nlohmann::json metadata;
		std::int64_t as_of = 0;
		std::uint64_t bytes = 0;
		std::string fingerprint;
		/// <summary>Local cache identity of exact fact lines, excluding the transport header.</summary>
		std::string facts_fingerprint;
		std::uint64_t facts_bytes = 0;
		bool lifetime = true;
		bool owned = false;
		~ReceiptSnapshot();
		bool Visit(const std::function<bool(const AccountingRecord&)>& visitor, std::string& error, const std::function<bool()>& cancelled = {}) const;
	};
	std::shared_ptr<const ReceiptSnapshot> ReadReceiptSnapshot(const std::filesystem::path& path, std::string& error, const std::function<bool()>& cancelled = {});
	/// <summary>Removes receipt snapshots and combined-machine stores a previous run left behind: a day old always, of any age when no other collector can own them.</summary>
	void RemoveAbandonedScratch(const std::filesystem::path& data_root, bool no_other_owner = false);
	/// <summary>Removes pinned remote views and partial downloads a previous run left behind; call before this run pins or fetches any, because a pin inherits its source's age.</summary>
	void RemoveAbandonedRemoteViews(const std::filesystem::path& data_root);
	/// <summary>Rate-limited [performance] note for ledger work; a negative build_ms records a rebuild skipped as unchanged. Never logs usage figures.</summary>
	void LogLedgerWork(std::string_view origin, std::int64_t build_ms);

	struct SourceRecord
	{
		const Record* record = nullptr;
		std::string source_id;
		std::size_t ordinal = 0;
	};

	/// <summary>Worker-owned accounting receipts. Provider files remain read-only; deleted transcripts do not erase usage.</summary>
	class Store
	{
	public:
		/// <summary>An existing store is only opened, never created, so a deleted scratch store fails instead of reading empty.</summary>
		explicit Store(std::filesystem::path data_root, bool existing = false);
		bool Import(const std::vector<SourceRecord>& records, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::optional<Ledger> Read(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::optional<std::vector<AccountingRecord>> ReadRecords(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		/// <summary>Reads bounded canonical archived and recent receipts before any daily aggregation.</summary>
		std::optional<std::vector<AccountingRecord>> ReadLifetimeRecords(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		/// <summary>Streams raw durable receipts without materializing complete history.</summary>
		bool VisitLifetimeRecords(std::int64_t generated_ms, const std::function<bool(const AccountingRecord&)>& visitor, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::shared_ptr<const ReceiptSnapshot> WriteReceiptSnapshot(std::int64_t generated_ms, const nlohmann::json& metadata, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::optional<Ledger> ReadLifetimeLedger(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		/// <summary>Imports validated peer rows, preserving unidentified host receipts and the first equal-time native receipt.</summary>
		bool ImportAccounting(const std::vector<AccountingRecord>& records, std::string_view scope, std::string& error, const std::function<bool()>& cancelled = {}) const;
		const std::filesystem::path& Path() const { return m_path; }
		/// <summary>SQLite's file change counter, advanced by any commit that modified the index; zero when unreadable. Compares two states, carries no count of records.</summary>
		std::uint32_t ChangeCounter() const;
		/// <summary>The exact lifetime scan; ordering is by index so reads never spill a sort to temporary files.</summary>
		static std::string LifetimeRecordsSql(bool imported);

	private:
		std::optional<std::vector<AccountingRecord>> ReadRecordsImpl(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled, bool lifetime) const;
		std::filesystem::path m_path;
		bool m_existing = false;
	};
}
