#include "test_harness.h"
#include "app/storage_retention_service.h"
#include "common/utils/diagnostic_log.h"

using namespace uam_test;

namespace
{
	void PollStorage(uam::StorageRetentionService& service, uam::AppState& app, int64_t now,
	    const uam::StorageRetentionService::CacheClearStarter& clear = {})
	{
		for (int attempt = 0; attempt < 200; ++attempt)
		{
			service.Poll(app, clear, now);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}

UAM_TEST(StorageRetentionOwnedSidecarDefaultsBoundsAndCancellation)
{
	TempDir temp("uam-storage-policy");
	const uam::StorageRetentionService::Record defaults = uam::StorageRetentionService::LoadPolicy(temp.root);
	UAM_ASSERT(defaults.ok);
	UAM_ASSERT(defaults.policy.backup_log_retention_days == 14 && defaults.policy.http_cache_clear_interval_days == 0);
	std::string error;
	const std::shared_ptr<uam::StorageRetentionService::Owner> owner = std::make_shared<uam::StorageRetentionService::Owner>();
	UAM_ASSERT(uam::StorageRetentionService::SavePolicy(temp.root, {30, 7}, owner, error));
	UAM_ASSERT(uam::StorageRetentionService::LoadPolicy(temp.root).policy == (uam::StorageRetentionPolicy{30, 7}));
	const std::shared_ptr<uam::StorageRetentionService::Owner> canceled = std::make_shared<uam::StorageRetentionService::Owner>();
	canceled->publication.store(uam::StorageRetentionService::Publication::Canceled);
	UAM_ASSERT(!uam::StorageRetentionService::SavePolicy(temp.root, {0, 0}, canceled, error));
	UAM_ASSERT(uam::StorageRetentionService::LoadPolicy(temp.root).policy == (uam::StorageRetentionPolicy{30, 7}));
	UAM_ASSERT(!uam::StorageRetentionService::Valid({-1, 1}));
	UAM_ASSERT(!uam::StorageRetentionService::Valid({1, 3651}));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "storage-retention.json", "{\"version\":1,\"backupLogRetentionDays\":1.5,\"httpCacheClearIntervalDays\":1}"));
	UAM_ASSERT(!uam::StorageRetentionService::LoadPolicy(temp.root).ok);
}

UAM_TEST(StorageRetentionMalformedStartupFailsClosed)
{
	TempDir temp("uam-storage-malformed");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "storage-retention.json", "invalid policy"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "uam.log.1", "copied backup survivor"));
	fs::last_write_time(temp.root / "uam.log.1", fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(temp.root / "uam.log", error, 0));
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	PollStorage(service, app, 1000000000000);
	UAM_ASSERT(service.Ready());
	UAM_ASSERT(app.settings.backup_log_retention_days == 0);
	uam::diagnostics::StopFileLog();
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "uam.log.1"), "copied backup survivor");
}

UAM_TEST(StorageRetentionBackupPolicyPreservesActiveLogs)
{
	TempDir temp("uam-storage-log-age");
	fs::create_directories(temp.root / "logs");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "cef.log", "current browser log"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "logs/crash-recovery.log", "current supervisor log"));
	for (const int days : {0, 30, 14})
	{
		UAM_ASSERT(uam::io::WriteTextFile(temp.root / "uam.log.1", "old copied backup"));
		fs::last_write_time(temp.root / "uam.log.1", fs::file_time_type::clock::now() - std::chrono::hours(24 * 15));
		std::string error;
		UAM_ASSERT(uam::diagnostics::StartFileLog(temp.root / "uam.log", error, days));
		uam::diagnostics::Write("active diagnostic survivor");
		uam::diagnostics::StopFileLog();
		UAM_ASSERT(fs::exists(temp.root / "uam.log.1") == (days != 14));
	}
	UAM_ASSERT(uam::strings::Contains(uam::io::ReadTextFile(temp.root / "uam.log"), "active diagnostic survivor"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "cef.log"), "current browser log");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "logs/crash-recovery.log"), "current supervisor log");
}

UAM_TEST(StorageRetentionFailedSaveRetainsPreviousPolicy)
{
	TempDir temp("uam-storage-policy-failed-save");
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	PollStorage(service, app, 1000000000000);
	UAM_ASSERT(app.settings.backup_log_retention_days == 14);
	fs::create_directory(temp.root / "storage-retention.json");
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	bool completed = false, saved = true;
	UAM_ASSERT(service.RequestPolicy(temp.root, {0, 1}, lifetime, [&](bool ok, const std::string&) { completed = true; saved = ok; }));
	PollStorage(service, app, 1000000000000);
	UAM_ASSERT(completed && !saved);
	UAM_ASSERT(app.settings.backup_log_retention_days == 14 && app.settings.http_cache_clear_interval_days == 0);
	UAM_ASSERT(fs::is_directory(temp.root / "storage-retention.json"));
}

UAM_TEST(StorageRetentionCacheCadencePreservesProfilesAndBackwardClock)
{
	TempDir temp("uam-storage-cache-cadence");
	fs::create_directories(temp.root / "cef");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "cef/Cookies", "auth survivor"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "cef/Preferences", "profile survivor"));
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	constexpr int64_t now = 1000000000000, day = 86400000;
	int clears = 0;
	std::function<void()> done;
	const uam::StorageRetentionService::CacheClearStarter starter = [&](std::function<void()> completion)
	{ ++clears; done = std::move(completion); return true; };
	PollStorage(service, app, now, starter);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	bool saved = false;
	UAM_ASSERT(service.RequestPolicy(temp.root, {30, 1}, lifetime, [&](bool ok, const std::string&) { saved = ok; }));
	UAM_ASSERT(app.settings.backup_log_retention_days == 14);
	PollStorage(service, app, now, starter);
	UAM_ASSERT(saved && clears == 0);
	PollStorage(service, app, now - day, starter);
	UAM_ASSERT(clears == 0);
	PollStorage(service, app, now + day, starter);
	UAM_ASSERT(clears == 1 && done);
	PollStorage(service, app, now + 2 * day, starter);
	UAM_ASSERT(clears == 1);
	done();
	PollStorage(service, app, now + 2 * day, starter);
	UAM_ASSERT(clears == 1);
	PollStorage(service, app, now + 2 * day - 1, starter);
	UAM_ASSERT(clears == 1);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "cef/Cookies"), "auth survivor");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "cef/Preferences"), "profile survivor");
}

UAM_TEST(StorageRetentionRootAndExpiredQueryCannotPublishPolicy)
{
	TempDir temp("uam-storage-policy-owner");
	const std::shared_ptr<uam::StorageRetentionService::Owner> expired = std::make_shared<uam::StorageRetentionService::Owner>();
	expired->lifetime_bound = true;
	std::string error;
	UAM_ASSERT(!uam::StorageRetentionService::SavePolicy(temp.root, {0, 1}, expired, error));
	UAM_ASSERT(!fs::exists(temp.root / "storage-retention.json"));
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	PollStorage(service, app, 1000000000000);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	bool saved = false;
	UAM_ASSERT(service.RequestPolicy(temp.root, {0, 1}, lifetime, [&](bool ok, const std::string&) { saved = ok; }));
	fs::create_directories(temp.root / "replacement");
	app.data_root = temp.root / "replacement";
	PollStorage(service, app, 1000000000000);
	UAM_ASSERT(!saved);
	UAM_ASSERT(!fs::exists(temp.root / "storage-retention.json"));
	UAM_ASSERT(app.settings.backup_log_retention_days == 14 && app.settings.http_cache_clear_interval_days == 0);
}

UAM_TEST(StorageRetentionLatestQueuedPolicyOwnsPublication)
{
	TempDir temp("uam-storage-policy-latest");
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	PollStorage(service, app, 1000000000000);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	bool superseded = false, latest_saved = false;
	UAM_ASSERT(service.RequestPolicy(temp.root, {0, 1}, lifetime, [&](bool ok, const std::string&) { superseded = !ok; }));
	UAM_ASSERT(service.RequestPolicy(temp.root, {30, 7}, lifetime, [&](bool ok, const std::string&) { latest_saved = ok; }));
	PollStorage(service, app, 1000000000000);
	UAM_ASSERT(superseded && latest_saved);
	UAM_ASSERT(uam::StorageRetentionService::LoadPolicy(temp.root).policy == (uam::StorageRetentionPolicy{30, 7}));
	UAM_ASSERT(app.settings.backup_log_retention_days == 30 && app.settings.http_cache_clear_interval_days == 7);
}

UAM_TEST(StorageRetentionFailedCadenceRetriesReceiptWithoutRepeatedClear)
{
	TempDir temp("uam-storage-cadence-failure");
	uam::AppState app; app.data_root = temp.root;
	uam::StorageRetentionService service;
	constexpr int64_t now = 1000000000000, day = 86400000;
	PollStorage(service, app, now);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	UAM_ASSERT(service.RequestPolicy(temp.root, {14, 1}, lifetime, [](bool, const std::string&) {}));
	int clears = 0;
	std::function<void()> done;
	const uam::StorageRetentionService::CacheClearStarter starter = [&](std::function<void()> completion)
	{ ++clears; done = std::move(completion); return true; };
	PollStorage(service, app, now, starter);
	PollStorage(service, app, now + day, starter);
	UAM_ASSERT(clears == 1);
	fs::remove(temp.root / "http-cache-cleanup.json");
	fs::create_directory(temp.root / "http-cache-cleanup.json");
	done();
	PollStorage(service, app, now + day, starter);
	PollStorage(service, app, now + day + 60000, starter);
	UAM_ASSERT(clears == 1);
	fs::remove(temp.root / "http-cache-cleanup.json");
	PollStorage(service, app, now + day + 120000, starter);
	UAM_ASSERT(clears == 1);
	UAM_ASSERT(fs::is_regular_file(temp.root / "http-cache-cleanup.json"));
}

#if !defined(_WIN32)
UAM_TEST(StorageRetentionRejectsLinkedPolicyAndPreservesForeignFile)
{
	TempDir temp("uam-storage-policy-link");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "foreign.json", "foreign survivor"));
	fs::create_symlink(temp.root / "foreign.json", temp.root / "storage-retention.json");
	UAM_ASSERT(!uam::StorageRetentionService::LoadPolicy(temp.root).ok);
	const std::shared_ptr<uam::StorageRetentionService::Owner> owner = std::make_shared<uam::StorageRetentionService::Owner>();
	std::string error;
	UAM_ASSERT(!uam::StorageRetentionService::SavePolicy(temp.root, {0, 1}, owner, error));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "foreign.json"), "foreign survivor");
	UAM_ASSERT(fs::is_symlink(temp.root / "storage-retention.json"));
}
#endif

#if defined(__APPLE__)
#include "common/platform/platform_services_macos_impl_internal.h"

UAM_TEST(StorageRetentionWriterHoldsActualRootLockUntilCompletion)
{
	TempDir temp("uam-storage-root-lock");
	const fs::path lock_path = temp.root / ".fixture-root.lock";
	const int descriptor = open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
	UAM_ASSERT(descriptor >= 0 && flock(descriptor, LOCK_EX | LOCK_NB) == 0);
	std::shared_ptr<void> root_lock = std::make_shared<uam::platform_macos_impl::MacDataRootLock>(descriptor);
	const std::weak_ptr<void> weak_root = root_lock;
	const std::function<bool()> can_reacquire = [&]()
	{
		const int next = open(lock_path.c_str(), O_RDWR);
		UAM_ASSERT(next >= 0);
		const bool acquired = flock(next, LOCK_EX | LOCK_NB) == 0;
		if (acquired) (void)flock(next, LOCK_UN);
		(void)close(next);
		return acquired;
	};
	std::promise<void> entered, release;
	const std::shared_future<void> resume = release.get_future().share();
	std::thread blocker([&]()
	{
		(void)uam::io::AtomicWriteFileDetailed(temp.root / "fixture-blocker", "fixture", [&](uam::io::AtomicWriteStage stage)
		{
			if (stage == uam::io::AtomicWriteStage::DuringWrite) { entered.set_value(); resume.wait(); }
			return true;
		});
	});
	entered.get_future().wait();
	const std::shared_ptr<uam::StorageRetentionService::Owner> owner = std::make_shared<uam::StorageRetentionService::Owner>();
	owner->root_lease = root_lock; owner->root_lease_bound = true;
	std::future<bool> writer = std::async(std::launch::async, [&]()
	{ std::string error; return uam::StorageRetentionService::SavePolicy(temp.root, {0, 1}, owner, error); });
	for (int attempt = 0; weak_root.use_count() < 2 && attempt < 500; ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	const bool writing = weak_root.use_count() >= 2;
	root_lock.reset();
	const bool locked_during_write = !can_reacquire();
	release.set_value(); blocker.join();
	const bool saved = writer.get();
	UAM_ASSERT(writing && locked_during_write && saved);
	UAM_ASSERT(can_reacquire());

	const int queued_descriptor = open(lock_path.c_str(), O_RDWR);
	UAM_ASSERT(queued_descriptor >= 0 && flock(queued_descriptor, LOCK_EX | LOCK_NB) == 0);
	root_lock = std::make_shared<uam::platform_macos_impl::MacDataRootLock>(queued_descriptor);
	uam::StorageRetentionService service(root_lock);
	uam::AppState app; app.data_root = temp.root;
	PollStorage(service, app, 1000000000000);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	UAM_ASSERT(service.RequestPolicy(temp.root, {30, 7}, lifetime, [](bool, const std::string&) {}));
	service.Stop(); root_lock.reset();
	UAM_ASSERT(can_reacquire());

	const int failed_descriptor = open(lock_path.c_str(), O_RDWR);
	UAM_ASSERT(failed_descriptor >= 0 && flock(failed_descriptor, LOCK_EX | LOCK_NB) == 0);
	root_lock = std::make_shared<uam::platform_macos_impl::MacDataRootLock>(failed_descriptor);
	const std::shared_ptr<uam::StorageRetentionService::Owner> failed = std::make_shared<uam::StorageRetentionService::Owner>();
	failed->root_lease = root_lock; failed->root_lease_bound = true;
	fs::remove(temp.root / "storage-retention.json"); fs::create_directory(temp.root / "storage-retention.json");
	std::string error;
	UAM_ASSERT(!uam::StorageRetentionService::SavePolicy(temp.root, {14, 0}, failed, error));
	root_lock.reset(); UAM_ASSERT(can_reacquire());
}
#endif

#if defined(__APPLE__)
UAM_TEST(StorageRetentionBoundServiceRejectsRootRebindAndOldEngineCallback)
{
	TempDir temp("uam-storage-two-root-owner");
	const fs::path first = temp.root / "first", second = temp.root / "second";
	fs::create_directories(first / "cef"); fs::create_directories(second / "cef");
	UAM_ASSERT(uam::io::WriteTextFile(first / "cef/Cookies", "old auth survivor"));
	UAM_ASSERT(uam::io::WriteTextFile(second / "cef/Cookies", "new auth survivor"));
	UAM_ASSERT(uam::io::WriteTextFile(first / "uam.log", "old current log"));
	UAM_ASSERT(uam::io::WriteTextFile(second / "uam.log", "new current log"));
	const std::string second_policy = "{\"version\":1,\"backupLogRetentionDays\":3650,\"httpCacheClearIntervalDays\":0}";
	UAM_ASSERT(uam::io::WriteTextFile(second / "storage-retention.json", second_policy));
	const int first_fd = open((first / ".fixture-root.lock").c_str(), O_RDWR | O_CREAT, 0600);
	const int second_fd = open((second / ".fixture-root.lock").c_str(), O_RDWR | O_CREAT, 0600);
	UAM_ASSERT(first_fd >= 0 && second_fd >= 0);
	UAM_ASSERT(flock(first_fd, LOCK_EX | LOCK_NB) == 0 && flock(second_fd, LOCK_EX | LOCK_NB) == 0);
	const std::shared_ptr<void> first_lock = std::make_shared<uam::platform_macos_impl::MacDataRootLock>(first_fd);
	const std::shared_ptr<void> second_lock = std::make_shared<uam::platform_macos_impl::MacDataRootLock>(second_fd);
	uam::StorageRetentionService service(first_lock);
	uam::AppState app; app.data_root = first;
	constexpr int64_t now = 1000000000000, day = 86400000;
	PollStorage(service, app, now);
	const std::shared_ptr<char> lifetime = std::make_shared<char>();
	UAM_ASSERT(service.RequestPolicy(first, {0, 1}, lifetime, [](bool, const std::string&) {}));
	int clears = 0;
	std::function<void()> old_engine_done;
	const uam::StorageRetentionService::CacheClearStarter old_engine = [&](std::function<void()> completion)
	{ ++clears; old_engine_done = std::move(completion); return true; };
	PollStorage(service, app, now, old_engine);
	const std::string baseline = uam::io::ReadTextFile(first / "http-cache-cleanup.json");
	PollStorage(service, app, now + day, old_engine);
	UAM_ASSERT(clears == 1 && old_engine_done);
	app.data_root = second;
	PollStorage(service, app, now + 2 * day, old_engine);
	UAM_ASSERT(!service.Ready());
	UAM_ASSERT(!service.RequestPolicy(second, {0, 1}, lifetime, [](bool, const std::string&) {}));
	old_engine_done();
	PollStorage(service, app, now + 3 * day, old_engine);
	UAM_ASSERT(clears == 1);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(first / "http-cache-cleanup.json"), baseline);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(second / "storage-retention.json"), second_policy);
	UAM_ASSERT(!fs::exists(second / "http-cache-cleanup.json"));
	app.data_root = first;
	PollStorage(service, app, now + 4 * day, old_engine);
	UAM_ASSERT(!service.Ready() && clears == 1);
	UAM_ASSERT(!service.RequestPolicy(first, {30, 7}, lifetime, [](bool, const std::string&) {}));
	for (const fs::path& root : {first, second})
	{
		const int next = open((root / ".fixture-root.lock").c_str(), O_RDWR);
		UAM_ASSERT(next >= 0 && flock(next, LOCK_EX | LOCK_NB) != 0);
		(void)close(next);
	}
	UAM_ASSERT_EQ(uam::io::ReadTextFile(first / "cef/Cookies"), "old auth survivor");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(second / "cef/Cookies"), "new auth survivor");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(first / "uam.log"), "old current log");
	UAM_ASSERT_EQ(uam::io::ReadTextFile(second / "uam.log"), "new current log");
}
#endif
