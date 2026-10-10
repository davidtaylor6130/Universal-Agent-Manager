#pragma once

#include "common/usage/transcript_scan.h"
#include "common/usage/usage_aggregation.h"

namespace uam::usage
{
	struct HistorySource
	{
		std::string provider_id;
		std::vector<std::filesystem::path> directories;
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

	private:
		bool Enumerate(const std::function<bool()>& cancelled);
		std::vector<HistorySource> m_sources;
		bool m_resolveLocalSources = false;
		std::string m_refreshId;
		std::vector<std::pair<std::string, std::filesystem::path>> m_files;
		std::map<std::pair<std::string, std::filesystem::path>, TranscriptScan> m_cache;
		std::map<std::string, std::size_t> m_sourceErrors;
		std::size_t m_nextFile = 0;
		bool m_listingComplete = false;
		bool m_listingLimited = false;
	};

	/// <summary>Resolve provider-owned local history roots; callers never accept arbitrary paths from the frontend.</summary>
	std::vector<HistorySource> LocalHistorySources();
}
