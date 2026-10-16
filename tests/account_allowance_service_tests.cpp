#include "test_harness.h"
#include "common/usage/account_allowance_service.h"
#include <atomic>
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
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token) { ++calls; return Snapshot(); });
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
		Service first(temp.root, configs, [](const auto&, const auto&, std::stop_token) { return Snapshot(); });
		UAM_ASSERT_EQ(CodexRow(WaitAccount(first, [](const auto& row) { return row.value("status", "") == "ready"; }))["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	}
	std::atomic<bool> release{false};
	Service second(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop) { while (!release && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	const auto start = std::chrono::steady_clock::now(); const auto value = second.Query();
	UAM_ASSERT(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	UAM_ASSERT(value.value("refreshing", false)); release = true;
}

UAM_TEST(AccountAllowanceServiceCoalescesRefreshAndKeepsGoodSnapshotOnFailure)
{
	TempDir temp("uam-account-refresh"); std::atomic<int> calls{0}; std::atomic<bool> release{false}; std::atomic<std::int64_t> clock_ms{1000};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token stop)
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
	{ Service first(temp.root, configs, [](const auto&, const auto&, std::stop_token) { return Snapshot(); }); WaitAccount(first, [](const auto& row) { return row.value("status", "") == "ready"; }); }
	Service second(temp.root, {{"codex-cli", {"different-codex"}}}, [](const auto&, const auto&, std::stop_token stop) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	UAM_ASSERT(CodexRow(second.Query())["limits"].is_null());
}

UAM_TEST(AccountAllowanceServiceChangesPublicScopeWhenCommandChanges)
{
	TempDir temp("uam-account-public-scope");
	Service service(temp.root, configs, [](const auto&, const auto&, std::stop_token) { return Snapshot(); });
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
	TempDir temp("uam-account-throttle"); std::atomic<int> calls{0}; std::atomic<std::int64_t> clock_ms{1000};
	Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token) { ++calls; return Snapshot(); },
	    [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clock_ms.load())); });
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	for (int i = 0; i < 20; ++i)
	{
		const auto value = service.Query("local", true);
		UAM_ASSERT(!value.value("refreshing", true));
		UAM_ASSERT(value.contains("refreshAfterMs") && value["refreshAfterMs"] == 30000);
		UAM_ASSERT_EQ(CodexRow(value)["limits"]["primary"]["usedPercent"], nlohmann::json(29));
	}
	UAM_ASSERT_EQ(calls.load(), 1);
	clock_ms = 31000;
	service.Query("local", true);
	WaitAccount(service, [](const auto& row) { return row.value("status", "") == "ready"; });
	UAM_ASSERT_EQ(calls.load(), 2);
}

UAM_TEST(AccountAllowanceServiceInvalidLoginErasesMemoryAndPersistedSnapshot)
{
	TempDir temp("uam-account-signout"); std::atomic<int> calls{0}; std::atomic<std::int64_t> clock_ms{1000};
	{
		Service service(temp.root, configs, [&](const auto&, const auto&, std::stop_token)
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
	Service restarted(temp.root, configs, [](const auto&, const auto&, std::stop_token stop) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	UAM_ASSERT(CodexRow(restarted.Query())["limits"].is_null());
}

UAM_TEST(AccountAllowanceServicePreservesAndRestoresEveryModelWindow)
{
	TempDir temp("uam-account-model-windows");
	{
		Service service(temp.root, configs, [](const auto&, const auto&, std::stop_token)
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
	Service restored(temp.root, configs, [](const auto&, const auto&, std::stop_token stop) { while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2)); return Snapshot(); });
	const auto row = CodexRow(restored.Query());
	UAM_ASSERT(row.contains("windows") && row["windows"].size() == 2);
	UAM_ASSERT_EQ(row["windows"][1]["usedPercent"], nlohmann::json(0));
}
