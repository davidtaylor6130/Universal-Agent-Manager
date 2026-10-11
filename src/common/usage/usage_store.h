#pragma once

#include "common/usage/usage_ledger.h"
#include <filesystem>
#include <functional>

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
		explicit Store(std::filesystem::path data_root);
		bool Import(const std::vector<SourceRecord>& records, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::optional<Ledger> Read(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		std::optional<std::vector<AccountingRecord>> ReadRecords(std::int64_t generated_ms, std::string& error, const std::function<bool()>& cancelled = {}) const;
		const std::filesystem::path& Path() const { return m_path; }

	private:
		std::filesystem::path m_path;
	};
}
