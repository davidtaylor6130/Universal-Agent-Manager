#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace uam::native_history
{
	enum class NativeRefreshWorkerPool { Shared, TerminalParser };

	namespace detail
	{
		// Shared operations and periodic parsers have independent bounded lifetimes.
		// A completed periodic snapshot must not pin managed/branch/metadata capacity.
		inline std::atomic<unsigned> refresh_owners{0};
		inline constexpr unsigned kMaxRefreshOwners = 2;
		inline std::atomic<unsigned> terminal_parser_owners{0};
		inline constexpr unsigned kMaxTerminalParserOwners = 2;
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
			else return detail::refresh_owners;
		}();
		constexpr unsigned limit = Pool == NativeRefreshWorkerPool::TerminalParser
		    ? detail::kMaxTerminalParserOwners : detail::kMaxRefreshOwners;
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
