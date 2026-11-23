#pragma once

#include "common/platform/host_load.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace uam::host_load
{
	/// Pending: no answer yet. Known: the figures are current. Unsupported: the helper cannot report load (an older
	/// helper). Unreachable: the last read failed. Stale: the last answer is older than the freshness limit. Every
	/// status except Known means unknown load, which must never be read as an idle computer.
	enum class SampleStatus { Pending, Known, Unsupported, Unreachable, Stale };

	struct Reading
	{
		SampleStatus status = SampleStatus::Unreachable;
		uam::platform::HostLoad load;
	};

	struct Sample
	{
		SampleStatus status = SampleStatus::Pending;
		/// Empty unless status is Known.
		uam::platform::HostLoad load;
		std::chrono::steady_clock::time_point taken_at{};
	};

	/// One read; it must return soon after the stop token is requested.
	using Reader = std::function<Reading(std::stop_token)>;
	/// Runs on the sampling thread, so whatever the reader owns (a helper connection) is created, used and
	/// destroyed there and never on the caller's thread.
	using ReaderFactory = std::function<Reader()>;

	/// <summary>
	/// Keeps a recent load reading for each wanted computer. Each computer is read by its own background thread at
	/// most once per interval, so a cached figure is never older than the interval plus one read; a figure older
	/// than the freshness limit reports Stale. Want, Latest and Poll never wait for a read and never join a thread
	/// that is still reading; only the destructor joins, after asking every reader to stop.
	/// </summary>
	class Sampler
	{
	public:
		explicit Sampler(std::chrono::milliseconds interval = std::chrono::seconds(10), std::chrono::milliseconds linger = std::chrono::seconds(30),
		    std::chrono::milliseconds stale_after = std::chrono::seconds(30));
		~Sampler();
		Sampler(const Sampler&) = delete;
		Sampler& operator=(const Sampler&) = delete;

		/// <summary>Keeps <paramref name="key"/> sampled until <c>linger</c> after the last call. A different identity
		/// (another connection or helper version) retires the old thread and starts a fresh reader.</summary>
		void Want(const std::string& key, const std::string& identity, ReaderFactory factory);
		/// <summary>The latest reading for the key's current identity; empty when the key is not sampled.</summary>
		std::optional<Sample> Latest(const std::string& key) const;
		/// <summary>True when a reading changed since the previous call, including a figure turning stale. Reaps
		/// threads that finished; a key nobody wanted for <c>linger</c> stops and is dropped once its thread ends.</summary>
		bool Poll();
		/// <summary>Keys currently sampled, for diagnostics and tests.</summary>
		std::vector<std::string> Keys() const;

	private:
		struct Task
		{
			std::string identity;
			ReaderFactory factory;
			mutable std::mutex mutex;
			std::condition_variable_any wake;
			Sample sample;
			bool reported_stale = false;
			std::atomic<std::int64_t> wanted_until_ns{0};
			std::atomic<bool> finished{false};
			/// Declared last so it is joined before the fields its thread uses are destroyed.
			std::jthread worker;
		};

		void Start(const std::shared_ptr<Task>& task);
		static std::int64_t NowNs();

		std::chrono::milliseconds m_interval;
		std::chrono::milliseconds m_linger;
		std::chrono::milliseconds m_staleAfter;
		std::shared_ptr<std::atomic<bool>> m_changed = std::make_shared<std::atomic<bool>>(false);
		mutable std::mutex m_mutex;
		std::unordered_map<std::string, std::shared_ptr<Task>> m_tasks;
		/// Replaced or abandoned threads that may still be finishing a read.
		std::vector<std::shared_ptr<Task>> m_retiring;
	};
}
