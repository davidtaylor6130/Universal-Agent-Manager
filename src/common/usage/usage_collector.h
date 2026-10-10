#pragma once

#include "common/usage/usage_history_service.h"
#include "common/usage/usage_store.h"
#include "common/usage/usage_pricing_cache.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace uam::usage
{
	/// <summary>Owns history I/O on a dedicated worker; view queries only use immutable accounting snapshots.</summary>
	class Collector
	{
	public:
		explicit Collector(std::filesystem::path data_root);
		Collector(std::filesystem::path data_root, std::vector<HistorySource> sources, RateFetcher fetch_rates = {});
		~Collector();
		nlohmann::json Query(int days, const std::map<ModelKey, ModelPrices>& prices = {}) const;
		const std::filesystem::path& DataRoot() const { return m_dataRoot; }

	private:
		void Run(std::stop_token stop);
		bool Publish(std::int64_t generated_ms, const nlohmann::json& metadata, std::string& error, const std::function<bool()>& cancelled);
		const std::filesystem::path m_dataRoot;
		Store m_store;
		HistoryService m_history;
		PricingCache m_pricing;
		std::shared_ptr<const RateCatalog> m_catalog;
		ModelAliases m_aliases;
		nlohmann::json m_pricingMetadata;
		mutable std::mutex m_mutex;
		std::shared_ptr<const Ledger> m_ledger;
		nlohmann::json m_metadata;
		std::string m_error;
		bool m_collecting = true;
		std::mutex m_waitMutex;
		std::condition_variable_any m_wake;
		std::jthread m_worker;
	};
}
