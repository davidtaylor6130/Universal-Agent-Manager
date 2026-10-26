#pragma once

#include "common/usage/account_allowance_fetcher.h"
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace uam::usage
{
	struct AccountAllowanceProviderConfig
	{
		std::string provider_id;
		std::vector<std::string> command;
		bool operator==(const AccountAllowanceProviderConfig&) const = default;
	};
	/// The final flag is true only for an explicit user refresh, which may show operating-system permission prompts.
	using AccountAllowanceFetcher = std::function<AccountAllowanceFetchResult(const AccountAllowanceProviderConfig&, const std::filesystem::path&, std::stop_token, bool)>;
	using AccountAllowanceClock = std::function<std::chrono::steady_clock::time_point()>;

	/// Independent read-only account snapshots; queries never start a chat or wait for provider I/O.
	class AccountAllowanceService
	{
	public:
		AccountAllowanceService(std::filesystem::path data_root, std::vector<AccountAllowanceProviderConfig> providers, AccountAllowanceFetcher fetcher,
		    AccountAllowanceClock clock = [] { return std::chrono::steady_clock::now(); });
		~AccountAllowanceService();
		// allow_prompt=false keeps an explicit refresh silent, as for phone requests when Phone settings disallow Keychain prompts.
		nlohmann::json Query(std::string_view execution_host_id = "local", bool refresh = false, bool allow_prompt = true);
		void SetProviders(std::vector<AccountAllowanceProviderConfig> providers);
		const std::filesystem::path& DataRoot() const { return m_dataRoot; }

	private:
		struct Provider;
		void Run(std::stop_token stop, std::size_t index);
		void Restore();
		void Save();
		std::filesystem::path m_dataRoot;
		AccountAllowanceFetcher m_fetcher;
		AccountAllowanceClock m_clock;
		std::mutex m_mutex;
		std::mutex m_saveMutex;
		std::condition_variable_any m_wake;
		std::vector<std::unique_ptr<Provider>> m_providers;
		std::vector<std::jthread> m_workers;
	};
}
