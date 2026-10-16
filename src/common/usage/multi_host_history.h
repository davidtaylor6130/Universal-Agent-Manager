#pragma once

#include "common/usage/usage_collector.h"
#include "common/models/app_models.h"
#include <atomic>
#include <condition_variable>

namespace uam::usage
{
	inline constexpr std::string_view kAllMachinesHostId = "*";
	using RemoteAccountingFetcher = std::function<std::optional<nlohmann::json>(const ExecutionHost&, std::stop_token, std::string&)>;

	/// <summary>Owns remote collection and peer caches; host and range queries only render immutable ledgers.</summary>
	class MultiHostHistory
	{
	public:
		MultiHostHistory(std::filesystem::path data_root, std::vector<ExecutionHost> hosts);
		MultiHostHistory(std::filesystem::path data_root, std::shared_ptr<Collector> local, std::vector<ExecutionHost> hosts, RemoteAccountingFetcher fetch);
		~MultiHostHistory();
		void SetHosts(std::vector<ExecutionHost> hosts);
		nlohmann::json Query(std::string_view host_id, int days, const std::map<ModelKey, ModelPrices>& prices = {}) const;
		const std::filesystem::path& DataRoot() const { return m_dataRoot; }
		/// <summary>Counts successfully imported receipt rows for background-work diagnostics.</summary>
		std::uint64_t ImportedRecordCount() const { return m_importedRecords.load(); }
		std::uint64_t LedgerBuildCount() const { return m_ledgerBuilds.load(); }

	private:
		struct View
		{
			std::shared_ptr<const Ledger> ledger;
			nlohmann::json metadata;
			nlohmann::json coverage;
		};
		void Run(std::stop_token stop);
		const std::filesystem::path m_dataRoot;
		const std::shared_ptr<Collector> m_local;
		const RemoteAccountingFetcher m_fetch;
		std::atomic<std::uint64_t> m_importedRecords{0};
		std::atomic<std::uint64_t> m_ledgerBuilds{0};
		mutable std::mutex m_mutex;
		std::vector<ExecutionHost> m_hosts;
		std::map<std::string, View> m_views;
		std::shared_ptr<std::stop_source> m_scopeStop = std::make_shared<std::stop_source>();
		std::atomic<std::uint64_t> m_generation{1};
		std::mutex m_waitMutex;
		std::condition_variable_any m_wake;
		std::jthread m_worker;
	};
}
