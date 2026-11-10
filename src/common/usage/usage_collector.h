#pragma once

#include "common/usage/usage_history_service.h"
#include "common/usage/usage_store.h"
#include "common/usage/usage_pricing_cache.h"
#include <atomic>
#include <chrono>
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
		/// <summary>A positive idle_after parks collection once no export has been requested for that long; the next export resumes it.</summary>
		explicit Collector(std::filesystem::path data_root, bool export_accounting = false, std::chrono::milliseconds idle_after = {});
		Collector(std::filesystem::path data_root, std::vector<HistorySource> sources, RateFetcher fetch_rates = {}, bool export_accounting = false,
		          std::chrono::milliseconds idle_after = {}, std::chrono::milliseconds pass_interval = std::chrono::seconds(30), std::chrono::milliseconds publish_min_interval = {});
		~Collector();
		nlohmann::json Query(int days, const std::map<ModelKey, ModelPrices>& prices = {}) const;
		/// <summary>Returns immutable bounded wire chunks; never reads provider files or accounting storage.</summary>
		nlohmann::json ExportAccounting(std::size_t offset = 0, std::string_view fingerprint = {}) const;
		std::shared_ptr<const std::string> AccountingSnapshot() const;
		std::shared_ptr<const ReceiptSnapshot> Receipts() const;
		nlohmann::json ExportReceipts(std::uint64_t offset = 0, std::string_view fingerprint = {}) const;
		std::pair<bool, std::string> CollectionStatus() const;
		/// <summary>Prices an immutable combined ledger using the locally cached catalog, without I/O.</summary>
		nlohmann::json Render(const Ledger& ledger, nlohmann::json metadata, int days, const std::map<ModelKey, ModelPrices>& prices = {}) const;
		const std::filesystem::path& DataRoot() const { return m_dataRoot; }
		/// <summary>Records that someone will read the accounting export, so the next pass refreshes it.</summary>
		void NoteDemand() const;
		/// <summary>True while this collector is the only one that can own its data root's scratch files.</summary>
		bool OwnsScratch() const { return m_scratchLease != nullptr; }

	private:
		void Run(std::stop_token stop);
		bool Publish(std::int64_t generated_ms, const nlohmann::json& metadata, bool ledger, bool persist_ledger, bool receipts, std::string& error, const std::function<bool()>& cancelled);
		const std::filesystem::path m_dataRoot;
		const bool m_exportAccounting;
		const std::chrono::milliseconds m_idleAfter;
		const std::chrono::milliseconds m_passInterval;
		const std::chrono::milliseconds m_ledgerMinInterval;
		const std::chrono::milliseconds m_receiptMinInterval;
		mutable std::atomic<std::int64_t> m_lastDemandMs;
		mutable std::atomic<std::int64_t> m_lastQueryMs;
		mutable std::atomic<bool> m_passWake{false};
		std::shared_ptr<void> m_scratchLease;
		Store m_store;
		HistoryService m_history;
		PricingCache m_pricing;
		std::shared_ptr<const RateCatalog> m_catalog;
		ModelAliases m_aliases;
		nlohmann::json m_pricingMetadata;
		mutable std::mutex m_mutex;
		std::shared_ptr<const Ledger> m_ledger;
		std::shared_ptr<const std::string> m_accountingBytes;
		std::shared_ptr<const ReceiptSnapshot> m_receipts;
		mutable std::map<std::string, std::pair<std::shared_ptr<const ReceiptSnapshot>, std::chrono::steady_clock::time_point>> m_receiptExports;
		std::string m_accountingFingerprint;
		nlohmann::json m_metadata;
		std::string m_error;
		bool m_collecting = true;
		mutable std::mutex m_waitMutex;
		mutable std::condition_variable_any m_wake;
		std::jthread m_worker;
	};
}
