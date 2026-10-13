#include "app/storage_retention_service.h"

#include "common/state/app_state.h"
#include "common/paths/path_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <thread>

namespace uam
{
	namespace
	{
		constexpr int64_t kDayMs = 86400000;
		constexpr std::size_t kRecordLimit = 4096;
		bool SafeStoragePath(const std::filesystem::path& root, const std::filesystem::path& path)
		{
			if (!paths::IsDirectoryNoThrow(root) || paths::IsLinkOrReparsePointNoThrow(root) || path.parent_path() != root || paths::IsLinkOrReparsePointNoThrow(path)) return false;
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			return (error == std::errc::no_such_file_or_directory) || (!error && (status.type() == std::filesystem::file_type::not_found || std::filesystem::is_regular_file(status)));
		}
		bool ReadRecord(const std::filesystem::path& root, const std::string& name, nlohmann::json& record, bool& missing, std::string& error)
		{
			const std::filesystem::path path = root / name;
			if (!SafeStoragePath(root, path)) { error = "Storage retention record path is unsafe."; return false; }
			missing = !paths::PathExistsNoThrow(path);
			if (missing) return true;
			std::string bytes;
			if (!io::TryReadTextFile(path, bytes, kRecordLimit)) { error = "Storage retention record could not be read within its size limit."; return false; }
			record = nlohmann::json::parse(bytes, nullptr, false);
			if (!record.is_object() || !record.contains("version") || !record["version"].is_number_integer() || record["version"] != 1)
			{ error = "Storage retention record is malformed or unsupported."; return false; }
			return true;
		}
		bool BoundedDays(const nlohmann::json& record, const char* name, int& value)
		{
			if (!record.contains(name) || !record[name].is_number_integer() || record[name] < 0 || record[name] > 3650) return false;
			value = record[name].get<int>();
			return true;
		}
		bool WriteRecord(const std::filesystem::path& root, const std::string& name, const nlohmann::json& record,
		    const std::shared_ptr<StorageRetentionService::Owner>& owner, std::string& error)
		{
			const std::filesystem::path path = root / name;
			const std::shared_ptr<void> root_lease = owner->root_lease.lock();
			if (owner->root_lease_bound && !root_lease) { error = "Storage retention root ownership expired."; return false; }
			if (!SafeStoragePath(root, path)) { error = "Storage retention record path is unsafe."; return false; }
			const io::AtomicWriteResult result = io::AtomicWriteFileDetailed(path, record.dump(), [root, path, owner](io::AtomicWriteStage stage)
			{
				if (owner->cancellation.stop_requested() || (owner->lifetime_bound && owner->lifetime.expired())) return false;
				if (stage != io::AtomicWriteStage::BeforeReplace) return true;
				StorageRetentionService::Publication queued = StorageRetentionService::Publication::Queued;
				return SafeStoragePath(root, path) && owner->publication.compare_exchange_strong(queued, StorageRetentionService::Publication::Publishing);
			});
			error = result.error;
			owner->publication.store(StorageRetentionService::Publication::Finished);
			return result.success;
		}
		bool CancelOwner(const std::shared_ptr<StorageRetentionService::Owner>& owner)
		{
			StorageRetentionService::Publication queued = StorageRetentionService::Publication::Queued;
			if (owner->publication.compare_exchange_strong(queued, StorageRetentionService::Publication::Canceled))
			{ owner->cancellation.request_stop(); return true; }
			return queued != StorageRetentionService::Publication::Publishing;
		}
		int64_t WallTimeMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}
	}

	bool StorageRetentionService::Valid(const StorageRetentionPolicy& policy)
	{
		return policy.backup_log_retention_days >= 0 && policy.backup_log_retention_days <= 3650 && policy.http_cache_clear_interval_days >= 0 && policy.http_cache_clear_interval_days <= 3650;
	}

	StorageRetentionService::Record StorageRetentionService::LoadPolicy(const std::filesystem::path& root)
	{
		Record result;
		nlohmann::json record;
		bool missing = false;
		if (!ReadRecord(root, "storage-retention.json", record, missing, result.error)) return result;
		if (!missing && (!BoundedDays(record, "backupLogRetentionDays", result.policy.backup_log_retention_days) || !BoundedDays(record, "httpCacheClearIntervalDays", result.policy.http_cache_clear_interval_days)))
		{ result.error = "Storage retention policy requires integer day values from 0 to 3650."; return result; }
		result.ok = true;
		if (ReadRecord(root, "http-cache-cleanup.json", record, missing, result.error) && !missing && record.contains("successfulClearAtMs") &&
		    record["successfulClearAtMs"].is_number_integer() && record["successfulClearAtMs"] >= 0 && record["successfulClearAtMs"] <= INT64_MAX)
			result.clear_at_ms = record["successfulClearAtMs"].get<int64_t>();
		return result;
	}

	bool StorageRetentionService::SavePolicy(const std::filesystem::path& root, const StorageRetentionPolicy& policy, const std::shared_ptr<Owner>& owner, std::string& error)
	{
		if (!Valid(policy) || !owner) { error = "Storage retention policy is invalid."; return false; }
		return WriteRecord(root, "storage-retention.json", {{"version", 1}, {"backupLogRetentionDays", policy.backup_log_retention_days}, {"httpCacheClearIntervalDays", policy.http_cache_clear_interval_days}}, owner, error);
	}

	StorageRetentionService::~StorageRetentionService() { Stop(); }
	void StorageRetentionService::Stop()
	{
		m_stopped = true;
		ResetScope();
	}

	void StorageRetentionService::ResetScope()
	{
		if (m_task) CancelOwner(m_task->owner);
		if (m_queued) CancelOwner(m_queued->owner);
		m_task.reset(); m_queued.reset(); m_clear.reset(); m_rootLease.reset();
	}

	void StorageRetentionService::Start(const std::shared_ptr<Task>& task)
	{
		task->owner->root_lease = m_rootLease; task->owner->root_lease_bound = m_originallyBound;
		std::packaged_task<Record()> work([task]()
		{
			Record result;
			if (task->owner->cancellation.stop_requested()) return result;
			if (task->action == Action::Load) return LoadPolicy(task->root);
			result.policy = task->policy;
			result.clear_at_ms = task->clear_at_ms;
			result.ok = task->action == Action::Policy ? SavePolicy(task->root, task->policy, task->owner, result.error) :
			    WriteRecord(task->root, "http-cache-cleanup.json", {{"version", 1}, {"successfulClearAtMs", task->clear_at_ms}}, task->owner, result.error);
			return result;
		});
		task->worker = work.get_future();
		m_task = task;
		try { std::thread(std::move(work)).detach(); }
		catch (const std::system_error& exception)
		{
			Record failure; failure.error = "Storage retention worker could not start: " + std::string(exception.what());
			std::promise<Record> ready; task->worker = ready.get_future(); ready.set_value(std::move(failure));
		}
	}

	bool StorageRetentionService::RequestPolicy(const std::filesystem::path& root, StorageRetentionPolicy policy, std::weak_ptr<void> lifetime, Completion completion)
	{
		if (m_stopped || !m_loaded || root != m_root || !Valid(policy) || (m_task && m_task->owner->publication.load() == Publication::Publishing)) return false;
		if (m_task && m_task->action != Action::Load && !CancelOwner(m_task->owner)) return false;
		if (m_queued)
		{
			CancelOwner(m_queued->owner);
			if (m_queued->completion && !m_queued->owner->lifetime.expired()) m_queued->completion(false, "Storage policy update was superseded.");
		}
		m_queued = std::make_shared<Task>();
		m_queued->action = Action::Policy;
		m_queued->root = root; m_queued->policy = policy;
		m_queued->owner->lifetime = std::move(lifetime); m_queued->owner->lifetime_bound = true;
		m_queued->completion = std::move(completion);
		return true;
	}

	void StorageRetentionService::Poll(AppState& app, const CacheClearStarter& clear_cache, int64_t now_ms)
	{
		if (m_stopped) return;
		if (now_ms <= 0) now_ms = WallTimeMs();
		if (m_root != app.data_root)
		{
			if (!m_root.empty() && m_originallyBound)
			{
				Stop();
				diagnostics::Write("[storage retention] Data root changed; a new exclusively owned service and browser context are required.");
				return;
			}
			if (!m_root.empty()) ResetScope();
			m_root = app.data_root; m_loaded = false; m_policy = {0, 0}; m_clearAtMs = 0; m_pendingCadenceAtMs = 0; m_retryAtMs = 0;
			(void)diagnostics::ConfigureBackupRetention(0);
			const std::shared_ptr<Task> task = std::make_shared<Task>(); task->root = m_root; Start(task);
		}
		if (m_task && m_task->worker.valid() && m_task->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			Record result;
			try { result = m_task->worker.get(); } catch (const std::exception& exception) { result.error = exception.what(); }
			if (!result.error.empty()) diagnostics::Write("[storage retention] " + result.error);
			const std::shared_ptr<Task> task = m_task;
			if (task->root == m_root && result.ok && task->owner->publication.load() != Publication::Canceled)
			{
				if (task->action != Action::Cadence)
				{
					if (!diagnostics::ConfigureBackupRetention(result.policy.backup_log_retention_days))
					{
						// Preserve the ready result until the existing log worker releases its brief expiry lease.
						std::promise<Record> ready; task->worker = ready.get_future(); ready.set_value(result); return;
					}
					if (m_policy.http_cache_clear_interval_days == 0 && result.policy.http_cache_clear_interval_days != 0) { m_clearAtMs = 0; m_pendingCadenceAtMs = 0; }
					++m_policyGeneration;
					m_policy = result.policy; m_loaded = true;
					app.settings.backup_log_retention_days = m_policy.backup_log_retention_days;
					app.settings.http_cache_clear_interval_days = m_policy.http_cache_clear_interval_days;
				}
				if (task->action == Action::Load || task->action == Action::Cadence) m_clearAtMs = result.clear_at_ms;
				if (task->action == Action::Cadence) m_pendingCadenceAtMs = 0;
				if (task->completion && !task->owner->lifetime.expired()) task->completion(true, {});
			}
			else
			{
				if (task->action == Action::Load)
				{
					m_loaded = true;
					app.settings.backup_log_retention_days = m_policy.backup_log_retention_days;
					app.settings.http_cache_clear_interval_days = m_policy.http_cache_clear_interval_days;
				}
				if (task->completion && !task->owner->lifetime.expired()) task->completion(false, result.error.empty() ? "Storage policy update was canceled." : result.error);
				if (task->action == Action::Cadence && task->root == m_root && task->policy == m_policy) m_pendingCadenceAtMs = task->clear_at_ms;
				m_retryAtMs = now_ms + 60000;
			}
			m_task.reset();
		}
		if (m_task) return;
		if (m_queued)
		{
			const std::shared_ptr<Task> queued = std::move(m_queued);
			Start(queued); return;
		}
		if (m_clear)
		{
			if (!m_clear->finished.load()) return;
			if (m_clear->root != m_root || m_clear->policy != m_policy || m_clear->policy_generation != m_policyGeneration) { m_clear.reset(); return; }
			const std::shared_ptr<Task> task = std::make_shared<Task>();
			task->action = Action::Cadence; task->root = m_root; task->policy = m_policy;
			task->clear_at_ms = std::max(now_ms, m_clear->started_at_ms);
			m_clearAtMs = task->clear_at_ms; m_pendingCadenceAtMs = task->clear_at_ms;
			m_clear.reset(); Start(task); return;
		}
		if (!m_loaded || m_policy.http_cache_clear_interval_days == 0 || now_ms < m_retryAtMs) return;
		if (m_pendingCadenceAtMs != 0)
		{
			const std::shared_ptr<Task> task = std::make_shared<Task>();
			task->action = Action::Cadence; task->root = m_root; task->policy = m_policy; task->clear_at_ms = m_pendingCadenceAtMs; Start(task); return;
		}
		if (m_clearAtMs == 0)
		{
			const std::shared_ptr<Task> task = std::make_shared<Task>();
			task->action = Action::Cadence; task->root = m_root; task->policy = m_policy; task->clear_at_ms = now_ms; m_clearAtMs = now_ms; m_pendingCadenceAtMs = now_ms; Start(task); return;
		}
		if (!clear_cache || now_ms < m_clearAtMs || now_ms - m_clearAtMs < static_cast<int64_t>(m_policy.http_cache_clear_interval_days) * kDayMs) return;
		m_clear = std::make_shared<ClearRequest>();
		m_clear->root = m_root; m_clear->policy = m_policy; m_clear->started_at_ms = now_ms; m_clear->policy_generation = m_policyGeneration;
		const std::weak_ptr<ClearRequest> owner = m_clear;
		if (!clear_cache([owner]() { if (const std::shared_ptr<ClearRequest> current = owner.lock()) current->finished.store(true); }))
		{ m_clear.reset(); m_retryAtMs = now_ms + 60000; }
	}
}
