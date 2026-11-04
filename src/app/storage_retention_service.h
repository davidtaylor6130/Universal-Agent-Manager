#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>
#include <string>

namespace uam
{
	struct AppState;

	struct StorageRetentionPolicy
	{
		int backup_log_retention_days = 14;
		int http_cache_clear_interval_days = 0;
		bool operator==(const StorageRetentionPolicy&) const = default;
	};

	/// Owns storage-retention.json independently of ordinary settings snapshots; browser profiles are never traversed.
	class StorageRetentionService
	{
	  public:
		using Completion = std::function<void(bool, const std::string&)>;
		using CacheClearStarter = std::function<bool(std::function<void()>)>;
		enum class Publication { Queued, Publishing, Finished, Canceled };
		struct Owner
		{
			std::atomic<Publication> publication{Publication::Queued};
			std::weak_ptr<void> lifetime;
			bool lifetime_bound = false;
			std::stop_source cancellation;
			std::weak_ptr<void> root_lease;
			bool root_lease_bound = false;
		};
		struct Record
		{
			bool ok = false;
			StorageRetentionPolicy policy;
			int64_t clear_at_ms = 0;
			std::string error;
		};

		explicit StorageRetentionService(std::shared_ptr<void> root_lease = {}) : m_originallyBound(root_lease != nullptr), m_rootLease(std::move(root_lease)) {}
		~StorageRetentionService();
		void Poll(AppState& app, const CacheClearStarter& clear_cache = {}, int64_t now_ms = 0);
		/// Queued updates supersede older queued owners; an active atomic publication returns false without waiting.
		bool RequestPolicy(const std::filesystem::path& root, StorageRetentionPolicy policy, std::weak_ptr<void> lifetime, Completion completion);
		/// Revokes queued adoption without joining; a writer already doing IO retains only its captured root lease.
		void Stop();
		bool Ready() const { return m_loaded && !m_stopped; }
		static Record LoadPolicy(const std::filesystem::path& root);
		static bool SavePolicy(const std::filesystem::path& root, const StorageRetentionPolicy& policy, const std::shared_ptr<Owner>& owner, std::string& error);
		static bool Valid(const StorageRetentionPolicy& policy);

	  private:
		enum class Action { Load, Policy, Cadence };
		struct Task
		{
			Action action = Action::Load;
			std::filesystem::path root;
			StorageRetentionPolicy policy;
			int64_t clear_at_ms = 0;
			std::shared_ptr<Owner> owner = std::make_shared<Owner>();
			Completion completion;
			std::future<Record> worker;
		};
		struct ClearRequest
		{
			std::atomic<bool> finished{false};
			std::filesystem::path root;
			StorageRetentionPolicy policy;
			int64_t started_at_ms = 0;
			uint64_t policy_generation = 0;
		};
		void ResetScope();
		void Start(const std::shared_ptr<Task>& task);
		const bool m_originallyBound;
		bool m_stopped = false;
		std::filesystem::path m_root;
		std::shared_ptr<void> m_rootLease;
		StorageRetentionPolicy m_policy{0, 0};
		bool m_loaded = false;
		std::shared_ptr<Task> m_task;
		std::shared_ptr<Task> m_queued;
		std::shared_ptr<ClearRequest> m_clear;
		uint64_t m_policyGeneration = 0;
		int64_t m_clearAtMs = 0;
		int64_t m_pendingCadenceAtMs = 0;
		int64_t m_retryAtMs = 0;
	};
}
