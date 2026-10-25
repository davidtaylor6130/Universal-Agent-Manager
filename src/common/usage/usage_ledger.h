#pragma once

#include "common/provider/provider_ids.h"
#include "common/usage/usage_aggregation.h"
#include <nlohmann/json.hpp>
#include <functional>

namespace uam::usage
{
	/// <summary>A build that excludes providers keeps a fuller build's accounting on disk but skips (never counts or shows)
	/// records and coverage rows for providers it does not include. Full builds reject unknown providers as before.</summary>
	inline bool SkipsExcludedProvider(const nlohmann::json& provider_id)
	{
		return provider_id.is_string() && provider_ids::IsRetainedExcludedProviderId(provider_id.get_ref<const std::string&>());
	}
	/// <summary>Wire accounting rows are [identity, created, [provider, ...]].</summary>
	inline bool SkipsExcludedProviderRecord(const nlohmann::json& wire)
	{
		return wire.is_array() && wire.size() == 3 && wire[2].is_array() && !wire[2].empty() && SkipsExcludedProvider(wire[2][0]);
	}
	/// <summary>Coverage metadata lists every included provider exactly once; rows of excluded providers are ignored.</summary>
	inline std::size_t IncludedProviderRows(const nlohmann::json& rows)
	{
		std::size_t count = 0;
		for (const nlohmann::json& row : rows)
			if (!row.is_object() || !row.contains("providerId") || !SkipsExcludedProvider(row["providerId"])) ++count;
		return count;
	}

	enum class LedgerRange
	{
		Rolling90Days,
		AllRecorded
	};

	/// <summary>Immutable accounting snapshot: exact last 24 hours, older UTC daily model/session cohorts.</summary>
	class Ledger
	{
	public:
		static std::optional<Ledger> Create(const std::vector<const Record*>& records, std::int64_t generated_ms, LedgerRange range = LedgerRange::Rolling90Days);
		/// <summary>Consumes canonical receipts incrementally after disk-backed identity and alias reduction.</summary>
		static std::optional<Ledger> CreateCanonical(const std::function<bool(const std::function<bool(const Record&)>&)>& stream, std::int64_t generated_ms, std::size_t duplicates = 0);
		static std::optional<Ledger> Decode(const nlohmann::json& payload);
		nlohmann::json Encode() const;
		std::optional<Summary> Query(std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices = {}, bool bounded_chart = false) const;
		std::int64_t WindowStart(int days) const;
		std::int64_t GeneratedMs() const { return m_generatedMs; }
		bool IncludesLifetime() const { return m_range == LedgerRange::AllRecorded; }
		std::size_t Rows() const { return m_records.size(); }

	private:
		LedgerRange m_range = LedgerRange::Rolling90Days;
		std::int64_t m_generatedMs = 0;
		std::size_t m_duplicates = 0;
		std::vector<Record> m_records;
	};
}
