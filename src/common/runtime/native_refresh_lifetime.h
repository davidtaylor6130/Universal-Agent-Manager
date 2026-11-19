#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace uam::native_history
{
	enum class NativeRefreshWorkerPool { Shared, TerminalParser, DiscoveryParser, RemoteRefresh };

	namespace detail
	{
		// Shared operations and periodic parsers have independent bounded lifetimes.
		// A completed periodic snapshot must not pin managed/branch/metadata capacity.
		inline std::atomic<unsigned> refresh_owners{0};
		inline constexpr unsigned kMaxRefreshOwners = 2;
		inline std::atomic<unsigned> terminal_parser_owners{0};
		inline constexpr unsigned kMaxTerminalParserOwners = 2;
		// Discovery can retain candidates while awaiting a user decision. It must
		// neither consume batch admission capacity nor grow unbounded producers.
		inline std::atomic<unsigned> discovery_parser_owners{0};
		inline constexpr unsigned kMaxDiscoveryParserOwners = 2;
		// Explicit remote refreshes hold their owner through a slow SSH read; they
		// must not starve local branch, managed-run or metadata preparation. Each
		// host has its own slots and each read its own thread, so one slow host's
		// reads only delay its own refreshes, and a total cap bounds all remote work.
		// Admission refuses (busy) rather than queues.
		// ponytail: two hung hosts fill the total cap; raise it if that is seen.
		inline std::atomic<unsigned> remote_refresh_owners{0}; // Total; written under the mutex.
		inline constexpr unsigned kMaxRemoteRefreshOwners = 4;
		inline constexpr unsigned kMaxRemoteRefreshOwnersPerHost = 2;
		struct RemoteRefreshHosts
		{
			std::mutex mutex;
			std::map<std::string, unsigned, std::less<>> owners;
		};
		// ponytail: leaked on purpose. Detached cleanup workers can outlive static destruction at exit.
		inline RemoteRefreshHosts& RemoteRefreshHostsState()
		{
			static RemoteRefreshHosts* const hosts = new RemoteRefreshHosts;
			return *hosts;
		}
		inline bool TryAcquireRemoteRefresh(std::string_view host) noexcept
		{
			try
			{
				RemoteRefreshHosts& hosts = RemoteRefreshHostsState();
				std::lock_guard lock(hosts.mutex);
				if (remote_refresh_owners.load() >= kMaxRemoteRefreshOwners) return false;
				auto found = hosts.owners.find(host);
				if (found == hosts.owners.end()) found = hosts.owners.emplace(std::string(host), 0u).first;
				if (found->second >= kMaxRemoteRefreshOwnersPerHost) return false;
				++found->second;
				remote_refresh_owners.fetch_add(1);
				return true;
			}
			catch (...) { return false; }
		}
		inline void ReleaseRemoteRefresh(std::string_view host) noexcept
		{
			RemoteRefreshHosts& hosts = RemoteRefreshHostsState();
			std::lock_guard lock(hosts.mutex);
			const auto found = hosts.owners.find(host);
			if (found != hosts.owners.end() && --found->second == 0) hosts.owners.erase(found);
			remote_refresh_owners.fetch_sub(1);
		}
		inline unsigned RemoteRefreshOwnersOnHost(std::string_view host) noexcept
		{
			RemoteRefreshHosts& hosts = RemoteRefreshHostsState();
			std::lock_guard lock(hosts.mutex);
			const auto found = hosts.owners.find(host);
			return found == hosts.owners.end() ? 0u : found->second;
		}
	}

	/// Arm before admitting any payload, prepared file or transcript. CEF tasks
	/// retain this one handle; even discarded tasks retire its value on a worker.
	/// T must default-construct empty and have a nonthrowing destructor independent
	/// of app/CEF lifetime. Mutable access must follow serial task handoffs.
	/// `host` selects the per-host slots of RemoteRefresh; other pools ignore it.
	template <typename T, NativeRefreshWorkerPool Pool = NativeRefreshWorkerPool::Shared>
	std::shared_ptr<T> TryCreateWorkerOwnedNativeRefresh(std::string_view host = {}) noexcept
	{
		constexpr bool per_host = Pool == NativeRefreshWorkerPool::RemoteRefresh;
		auto& owners = []() -> std::atomic<unsigned>&
		{
			if constexpr (Pool == NativeRefreshWorkerPool::TerminalParser) return detail::terminal_parser_owners;
			else if constexpr (Pool == NativeRefreshWorkerPool::DiscoveryParser) return detail::discovery_parser_owners;
			else if constexpr (Pool == NativeRefreshWorkerPool::RemoteRefresh) return detail::remote_refresh_owners;
			else return detail::refresh_owners;
		}();
		constexpr unsigned limit = Pool == NativeRefreshWorkerPool::TerminalParser
		    ? detail::kMaxTerminalParserOwners : Pool == NativeRefreshWorkerPool::DiscoveryParser
		        ? detail::kMaxDiscoveryParserOwners : Pool == NativeRefreshWorkerPool::RemoteRefresh
		            ? detail::kMaxRemoteRefreshOwners : detail::kMaxRefreshOwners;
		if constexpr (per_host)
		{
			if (!detail::TryAcquireRemoteRefresh(host)) return {};
		}
		else
		{
			unsigned count = owners.load();
			do
			{
				if (count >= limit) return {};
			}
			while (!owners.compare_exchange_weak(count, count + 1));
		}
		const auto release = [counter = &owners](std::string_view key) noexcept
		{
			if constexpr (per_host) detail::ReleaseRemoteRefresh(key);
			else counter->fetch_sub(1);
		};

		struct State
		{
			std::mutex mutex;
			std::condition_variable ready;
			bool released = false;
			std::unique_ptr<T> value = std::make_unique<T>();
		};
		std::shared_ptr<State> state;
		std::shared_ptr<T> result;
		std::thread worker;
		bool launched = false;
		try
		{
			state = std::make_shared<State>();
			result = std::shared_ptr<T>(state->value.get(), [state](T*) noexcept
			{
				{
					std::lock_guard lock(state->mutex);
					state->released = true;
				}
				state->ready.notify_one();
			});
			worker = std::thread([state, release, key = per_host ? std::string(host) : std::string()]() noexcept
			{
				std::unique_ptr<T> value;
				{
					std::unique_lock lock(state->mutex);
					state->ready.wait(lock, [&] { return state->released; });
					value = std::move(state->value);
				}
				value.reset();
				release(key);
			});
			launched = true;
			worker.detach();
			return result;
		}
		catch (...)
		{
			// Startup failed before heavy data was admitted. If detach failed,
			// let the empty bootstrap exit before its joinable handle unwinds.
			result.reset();
			if (worker.joinable()) worker.join();
			if (!launched) release(host);
			return {};
		}
	}
}
