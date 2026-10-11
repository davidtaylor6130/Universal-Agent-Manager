#pragma once

#include "common/usage/transcript_scan.h"
#include "common/usage/opencode_history_scan.h"
#include "common/usage/usage_aggregation.h"

namespace uam::usage
{
	using AccountingSink = std::function<bool(const std::string&, const std::vector<Record>&, std::size_t, std::string&)>;
	nlohmann::json EmptyHistoryMetadata(std::string_view host_id = "local");
	void ApplyUsageSummary(nlohmann::json& result, const Summary& summary, std::int64_t start_ms, std::int64_t end_ms);
	struct HistorySource
	{
		std::string provider_id;
		std::vector<std::filesystem::path> directories;
		std::vector<std::filesystem::path> databases;
	};

	struct HistoryRequest
	{
		std::string refresh_id;
		std::string execution_host_id = "local";
		std::int64_t start_ms = 0;
		std::int64_t end_ms = 0;
		std::map<ModelKey, ModelPrices> prices;
	};
	bool ParseHistoryRequest(const nlohmann::json& payload, HistoryRequest& request, std::string& error);

	/// <summary>Background-only history collection. Call sequentially from the owning worker queue.</summary>
	class HistoryService
	{
	public:
		HistoryService();
		explicit HistoryService(std::vector<HistorySource> sources);
		nlohmann::json Refresh(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms,
		                       const std::map<ModelKey, ModelPrices>& prices = {}, const std::function<bool()>& cancelled = {});

		nlohmann::json Collect(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms, const AccountingSink& sink, const std::function<bool()>& cancelled = {});

	private:
		nlohmann::json RefreshImpl(std::string_view refresh_id, std::int64_t start_ms, std::int64_t end_ms, const std::map<ModelKey, ModelPrices>& prices, const std::function<bool()>& cancelled, const AccountingSink& sink);
		std::map<std::pair<std::string, std::filesystem::path>, std::size_t> m_accountedPositions;
		bool Enumerate(const std::function<bool()>& cancelled);
		std::vector<HistorySource> m_sources;
		bool m_resolveLocalSources = false;
		std::string m_refreshId;
		std::vector<std::pair<std::string, std::filesystem::path>> m_files;
		std::map<std::pair<std::string, std::filesystem::path>, TranscriptScan> m_cache;
		std::vector<std::filesystem::path> m_databases;
		std::map<std::filesystem::path, OpenCodeHistoryScan> m_databaseCache;
		std::map<std::filesystem::path, std::string> m_completedDatabaseFingerprints;
		std::map<std::filesystem::path, std::string> m_pendingDatabaseFingerprints;
		std::map<std::filesystem::path, std::string> m_databaseScanRefreshIds;
		std::size_t m_nextDatabase = 0;
		std::map<std::string, std::size_t> m_sourceErrors;
		std::size_t m_nextFile = 0;
		bool m_listingComplete = false;
		bool m_listingLimited = false;
	};

	/// <summary>Resolve provider-owned local history roots; callers never accept arbitrary paths from the frontend.</summary>
	std::vector<HistorySource> LocalHistorySources();
}
