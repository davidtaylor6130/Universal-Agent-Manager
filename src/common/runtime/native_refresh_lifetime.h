#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
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
		// must not starve local branch, managed-run or metadata preparation.
		// ponytail: one budget for all hosts; per-host slots if one slow host starves others.
		inline std::atomic<unsigned> remote_refresh_owners{0};
		inline constexpr unsigned kMaxRemoteRefreshOwners = 2;
	}

	/// Arm before admitting any payload, prepared file or transcript. CEF tasks
	/// retain this one handle; even discarded tasks retire its value on a worker.
	/// T must default-construct empty and have a nonthrowing destructor independent
	/// of app/CEF lifetime. Mutable access must follow serial task handoffs.
	template <typename T, NativeRefreshWorkerPool Pool = NativeRefreshWorkerPool::Shared>
	std::shared_ptr<T> TryCreateWorkerOwnedNativeRefresh() noexcept
	{
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
		unsigned count = owners.load();
		do
		{
			if (count >= limit) return {};
		}
		while (!owners.compare_exchange_weak(count, count + 1));

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
			worker = std::thread([state, counter = &owners]() noexcept
			{
				std::unique_ptr<T> value;
				{
					std::unique_lock lock(state->mutex);
					state->ready.wait(lock, [&] { return state->released; });
					value = std::move(state->value);
				}
				value.reset();
				counter->fetch_sub(1);
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
			if (!launched) owners.fetch_sub(1);
			return {};
		}
	}
}
