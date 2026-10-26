#include "test_harness.h"
#include "common/usage/account_allowance_service.h"
#include <atomic>
#include <mutex>
#include <thread>

using namespace uam_test;
namespace
{
	using Service = uam::usage::AccountAllowanceService;
	const std::vector<uam::usage::AccountAllowanceProviderConfig> configs = {{"codex-cli", {"codex"}}};
	uam::usage::AccountAllowanceFetchResult Snapshot()
	{
		return uam::usage::NormalizeCodexAccountAllowance({{"rateLimits", {{"primary", {{"usedPercent", 29}, {"windowDurationMins", 10080}}}}}}, 1700000000000LL);
	}
	nlohmann::json CodexRow(const nlohmann::json& value)
	{
		for (const auto& row : value.at("providers")) if (row.value("providerId", "") == "codex-cli") return row;
		return nlohmann::json::object();
	}
	nlohmann::json WaitAccount(Service& service, const std::function<bool(const nlohmann::json&)>& predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		nlohmann::json value;
		do { value = service.Query(); if (predicate(CodexRow(value))) return value; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
		while (std::chrono::steady_clock::now() < deadline);
		return value;
	}
}

UAM_TEST(AccountAllowanceServiceCollectsWithoutAnyChatOrHistory)
{
	TempDir temp("uam-account-independent"); std::atomic<int> calls{0};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token, bool) { ++calls; return Snapshot(); });
	const auto value = WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 1);
	UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	UAM_ASSERT(!fs::exists(temp.root / "chats"));
	UAM_ASSERT(CodexRow(service.Query("remote"))["limits"].is_null());
}

UAM_TEST(AccountAllowanceServiceRestoresImmediatelyWhileProviderReadWaits)
{
	TempDir temp("uam-account-cache");
	{
		Service first(temp.root, configs, [](const auto&, const auto&, std::stop_token, bool) { return Snapshot(); });
		UAM_ASSERT_EQ(CodexRow(WaitAccount(first, [](const auto& row) { return row.value("status", "") == "ready"; }))["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	}
	std::atomic<bool> release{false};
	Service second(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop, bool) { while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	const auto start = std::chrono::steady_clock::now(); const auto value = second.Query();
	UAM_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	UAM_ASSERT(value.value("refreshing", false)); release = true;
}

UAM_TEST(AccountAllowanceServiceCoalescesRefreshAndKeepsGoodSnapshotOnFailure)
{
	TempDir temp("uam-account-refresh"); std::atomic<int> calls{0}; std::atomic<bool> release{false}; std::atomic<std::int64_t> clock_ms{1000};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop, bool)
	{
		if (++calls == 1) return Snapshot();
		while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		uam::usage::AccountAllowanceFetchResult result; result.error = "Account refresh unavailable."; return result;
	}, [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clock_ms.load())); });
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	clock_ms = 31000;
	for (int i = 0; i < 20; ++i) UAM_ASSERT_EQ(CodexRow(service.Query("local", true))["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	release = true;
	const auto value = WaitAccount(service, [](const auto& row) { return row.value("status", "") == "error"; });
	UAM_ASSERT_EQ(calls.load(), 2); UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
}

UAM_TEST(AccountAllowanceServiceDoesNotRestoreCacheForChangedProviderCommand)
{
	TempDir temp("uam-account-scope");
	{ Service first(temp.root, configs, [](const auto&, const auto&, std::stop_token, bool) { return Snapshot(); }); WaitAccount(first, [](const auto& row) { return row.value("status", "") == "ready"; }); }
	Service second(temp.root, {{"codex-cli", {"different-codex"}}}, [](const auto&, const auto&, std::stop_token stop, bool) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	UAM_ASSERT(CodexRow(second.Query())["limits"].is_null());
}

UAM_TEST(AccountAllowanceServiceChangesPublicScopeWhenCommandChanges)
{
	TempDir temp("uam-account-public-scope");
	Service service(temp.root, configs, [](const auto&, const auto&, std::stop_token, bool) { return Snapshot(); });
	const auto first = CodexRow(WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; }));
	UAM_ASSERT(first.contains("scope") && first["scope"].is_string() && !first["scope"].get<std::string>().empty());
	service.SetProviders({{"codex-cli", {"different-codex"}}});
	const auto next = CodexRow(service.Query());
	UAM_ASSERT(next["scope"] != first["scope"]);
	const auto current = service.Query();
	for (const auto& row : current["providers"]) UAM_ASSERT(row.contains("scope") && row["scope"].is_string());
}

UAM_TEST(AccountAllowanceServiceThrottlesManualReadsWithoutWaitingOrErasingSnapshots)
{
	TempDir temp("uam-account-throttle"); std::atomic<int> calls{0}, manual_calls{0}; std::atomic<std::int64_t> clock_ms{1000};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token, bool manual) { ++calls; if (manual) ++manual_calls; return Snapshot(); },
	    [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clock_ms.load())); });
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	// The silent startup read does not hold back the first explicit refresh.
	UAM_ASSERT_EQ(service.Query()["refreshAfterMs"], nlohmann::json(0));
	service.Query("local", true);
	const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (manual_calls.load() < 1 && std::chrono::steady_clock::now() < first_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	for (int i = 0; i < 20; ++i)
	{
		const auto value = service.Query("local", true);
		UAM_ASSERT(!value.value("refreshing", true));
		UAM_ASSERT(value.contains("refreshAfterMs") && value["refreshAfterMs"] == 30000);
		UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	}
	UAM_ASSERT_EQ(calls.load(), 2); UAM_ASSERT_EQ(manual_calls.load(), 1);
	clock_ms = 31000;
	service.Query("local", true);
	const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (manual_calls.load() < 2 && std::chrono::steady_clock::now() < second_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 3); UAM_ASSERT_EQ(manual_calls.load(), 2);
	// Only an explicit refresh may permit operating-system permission prompts; scheduled reads stay silent.
	clock_ms = 400000;
	service.Query();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (calls.load() < 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	UAM_ASSERT_EQ(calls.load(), 4); UAM_ASSERT_EQ(manual_calls.load(), 2);
}

UAM_TEST(AccountAllowanceServiceInvalidLoginErasesMemoryAndPersistedSnapshot)
{
	TempDir temp("uam-account-signout"); std::atomic<int> calls{0}; std::atomic<std::int64_t> clock_ms{1000};
	{
		Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token, bool)
		{
			if (++calls == 1) return Snapshot();
			uam::usage::AccountAllowanceFetchResult result; result.error = "Existing login unavailable."; result.account_unavailable = true; return result;
		}, [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clock_ms.load())); });
		WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
		clock_ms = 31000; service.Query("local", true);
		const auto value = WaitAccount(service, [](const auto& row) { return row.value("status", "") == "error"; });
		UAM_ASSERT(CodexRow(value)["limits"].is_null());
		clock_ms = 600000;
		UAM_ASSERT(!service.Query().value("refreshing", true));
		UAM_ASSERT_EQ(calls.load(), 2);
	}
	Service restarted(temp.root, configs, [](const auto&, const auto&, std::stop_token stop, bool) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	UAM_ASSERT(CodexRow(restarted.Query())["limits"].is_null());
}

UAM_TEST(AccountAllowanceServicePreservesAndRestoresEveryModelWindow)
{
	TempDir temp("uam-account-model-windows");
	{
		Service service(temp.root, configs, [](const auto&, const auto&, std::stop_token, bool)
		{
			auto result = Snapshot();
			result.windows = {{{"id", "model-a"}, {"label", "Model A"}, {"usedPercent", 23.5}, {"resetsAt", nullptr}, {"windowDurationMinutes", nullptr}},
			    {{"id", "model-b"}, {"label", "Model B"}, {"usedPercent", 0}, {"resetsAt", 1900000000}, {"windowDurationMinutes", nullptr}}};
			return result;
		});
		const auto row = CodexRow(WaitAccount(service, [](const auto& item) { return item.value("status", "") == "ready"; }));
		UAM_ASSERT(row.contains("windows") && row["windows"].size() == 2);
		UAM_ASSERT_EQ(row["windows"][0]["usedPercent"], nlohmann::json(23.5));
	}
	Service restored(temp.root, configs, [](const auto&, const auto&, std::stop_token stop, bool) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	const auto row = CodexRow(restored.Query());
	UAM_ASSERT(row.contains("windows") && row["windows"].size() == 2);
	UAM_ASSERT_EQ(row["windows"][1]["usedPercent"], nlohmann::json(0));
}

UAM_TEST(AccountAllowanceServiceFollowsInFlightAutomaticReadWithOnePromptingRefresh)
{
	TempDir temp("uam-account-inflight"); std::atomic<int> calls{0}, manual_calls{0}; std::atomic<bool> release{false};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop, bool manual)
	{
		++calls; if (manual) ++manual_calls;
		while (!manual && !release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return Snapshot();
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (calls.load() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	for (int i = 0; i < 5; ++i) service.Query("local", true);
	release = true;
	while (manual_calls.load() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 2); UAM_ASSERT_EQ(manual_calls.load(), 1);
}

UAM_TEST(AccountAllowanceServiceRefreshTakesOverReadQueuedBehindManualRead)
{
	TempDir temp("uam-account-takeover"); std::vector<bool> modes; std::mutex modes_mutex; std::atomic<bool> release{false};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop, bool manual)
	{
		std::size_t call; { std::lock_guard lock(modes_mutex); modes.push_back(manual); call = modes.size(); }
		while (call == 2 && !release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return Snapshot();
	});
	const auto count = [&] { std::lock_guard lock(modes_mutex); return modes.size(); };
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	service.Query("local", true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (count() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	// A configuration change queues an automatic read behind the held manual one; the next refresh must take it over.
	service.SetProviders({{"codex-cli", {"codex-next"}}});
	service.Query("local", true);
	release = true;
	while (count() < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	std::lock_guard lock(modes_mutex);
	UAM_ASSERT_EQ(modes.size(), static_cast<std::size_t>(3)); UAM_ASSERT(modes.at(1)); UAM_ASSERT(modes.at(2));
}

UAM_TEST(AccountAllowanceServiceConfigurationChangeDropsQueuedPromptPermission)
{
	TempDir temp("uam-account-config-reset"); std::atomic<int> calls{0}, manual_calls{0}; std::atomic<bool> release{false};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop, bool manual)
	{
		++calls; if (manual) ++manual_calls;
		while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return Snapshot();
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (calls.load() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	service.Query("local", true);
	service.SetProviders({{"codex-cli", {"codex-next"}}});
	release = true;
	while (calls.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 2); UAM_ASSERT_EQ(manual_calls.load(), 0);
}

UAM_TEST(AccountAllowanceServiceSilentRefreshNeverPromptsAndKeepsCooldown)
{
	TempDir temp("uam-account-silent-refresh"); std::atomic<int> calls{0}, manual_calls{0}; std::atomic<std::int64_t> clock_ms{1000};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token, bool manual) { ++calls; if (manual) ++manual_calls; return Snapshot(); },
	    [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clock_ms.load())); });
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	// A refresh that may not prompt is still limited by the 30-second cooldown of the automatic read.
	UAM_ASSERT_EQ(service.Query("local", true, false)["refreshAfterMs"], nlohmann::json(30000));
	UAM_ASSERT_EQ(calls.load(), 1);
	clock_ms = 31000;
	service.Query("local", true, false);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (calls.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 2); UAM_ASSERT_EQ(manual_calls.load(), 0);
}
