#pragma once

#include "common/usage/usage_history_service.h"
#include "common/usage/usage_store.h"
#include "common/usage/usage_pricing_cache.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace uam::usage
{
	/// <summary>Minimum spacing of derived publications while read. Zero publishes every due change (the test default).</summary>
	struct PublishIntervals
	{
		std::chrono::milliseconds ledger{};
		std::chrono::milliseconds receipts{};
		/// <summary>Rereads unchanged facts so rolling windows and future witnesses advance.</summary>
		std::chrono::milliseconds clock_refresh = std::chrono::minutes(5);
	};

	/// <summary>Owns history I/O on a dedicated worker; view queries only use immutable accounting snapshots.</summary>
	class Collector
	{
	public:
		/// <summary>A positive idle_after parks collection once no export has been requested for that long; the next export resumes it.</summary>
		explicit Collector(std::filesystem::path data_root, bool export_accounting = false, std::chrono::milliseconds idle_after = {});
		Collector(std::filesystem::path data_root, std::vector<HistorySource> sources, RateFetcher fetch_rates = {}, bool export_accounting = false,
		          std::chrono::milliseconds idle_after = {}, std::chrono::milliseconds pass_interval = std::chrono::seconds(30), PublishIntervals intervals = {});
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
		/// <summary>Ready once this collector has swept abandoned scratch and leased its own; combined stores must wait for it.</summary>
		std::shared_future<void> ScratchReady() const { return m_scratchReady; }
		/// <summary>True while facts changed after the published receipts and their rewrite is being held back.</summary>
		bool ReceiptsBehind() const { return m_receiptsBehind.load(std::memory_order_acquire); }

	private:
		void Run(std::stop_token stop);
		bool Publish(std::int64_t generated_ms, const nlohmann::json& metadata, bool ledger, bool persist_ledger, bool receipts, bool ledger_changed, std::string& error, const std::function<bool()>& cancelled);
		void ClaimScratch(const std::function<bool()>& cancelled);
		const std::filesystem::path m_dataRoot;
		const bool m_exportAccounting;
		const std::chrono::milliseconds m_idleAfter;
		const std::chrono::milliseconds m_passInterval;
		const PublishIntervals m_intervals;
		mutable std::atomic<std::int64_t> m_lastDemandMs;
		mutable std::atomic<std::int64_t> m_lastQueryMs;
		mutable std::atomic<bool> m_passWake{false};
		mutable std::atomic<bool> m_receiptsBehind{false};
		std::shared_ptr<void> m_scratchLease;
		std::promise<void> m_scratchSwept;
		std::shared_future<void> m_scratchReady = m_scratchSwept.get_future().share();
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
		bool m_ledgerBehind = false;
		mutable std::mutex m_waitMutex;
		mutable std::condition_variable_any m_wake;
		std::jthread m_worker;
	};
}
