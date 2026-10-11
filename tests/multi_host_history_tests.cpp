#include "test_harness.h"
#include "common/usage/multi_host_history.h"
#include <atomic>
#include <thread>

using namespace uam_test;

namespace
{
	std::int64_t UsageNow()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}
	ExecutionHost UsageHost(const std::string& id)
	{
		ExecutionHost host; host.id = id; host.label = id; host.ssh_alias = id; host.transport = "ssh";
		return host;
	}
	nlohmann::json UsageSnapshot(const std::string& provider, const std::string& key, std::int64_t timestamp = 0)
	{
		const std::int64_t now = UsageNow();
		uam::usage::Record record; record.provider_id = provider; record.model_id = "test-model"; record.session_id = "session";
		record.dedupe_key = key; record.timestamp_ms = timestamp ? timestamp : now - 2000; record.tokens.input = 10; record.tokens.output = 5;
		const std::string identity = key.empty() ? nlohmann::json::array({"source", provider, "fixture", 0, record.timestamp_ms}).dump()
		    : nlohmann::json::array({"native", provider, key}).dump();
		nlohmann::json metadata = uam::usage::EmptyHistoryMetadata(); metadata["complete"] = true;
		return {{"version", 1}, {"asOfMs", now}, {"metadata", metadata},
		    {"facts", nlohmann::json::array({uam::usage::EncodeAccountingRecord({identity, record})})}};
	}
	nlohmann::json WaitUsage(uam::usage::MultiHostHistory& history, const std::string& host, const std::function<bool(const nlohmann::json&)>& ready)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
		nlohmann::json result;
		do
		{
			result = history.Query(host, 7);
			if (ready(result)) return result;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (std::chrono::steady_clock::now() < deadline);
		return result;
	}
	std::shared_ptr<uam::usage::Collector> EmptyLocal(const fs::path& root)
	{
		return std::make_shared<uam::usage::Collector>(root, std::vector<uam::usage::HistorySource>{}, uam::usage::RateFetcher{}, true);
	}
}

UAM_TEST(UsageMultiHostCombinesNativeIdentitiesAcrossMachinesWithoutLosingProviderOrUnknownFacts)
{
	TempDir temp("uam-usage-multi-total");
	std::atomic<int> calls{0};
	const nlohmann::json native = UsageSnapshot("codex-cli", "receipt"), other = UsageSnapshot("claude-cli", "receipt"), unknown = UsageSnapshot("codex-cli", "");
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {UsageHost("one"), UsageHost("two"), UsageHost("other"), UsageHost("unknown-one"), UsageHost("unknown-two")},
	    [&](const ExecutionHost& host, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    ++calls;
		    return host.id == "other" ? other : host.id.starts_with("unknown") ? unknown : native;
	    });
	const nlohmann::json result = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT(result.contains("totals")); UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 60);
	UAM_ASSERT_EQ(result["duplicates"].get<int>(), 1); UAM_ASSERT_EQ(result["unidentifiedRecords"].get<int>(), 2);
	UAM_ASSERT_EQ(result["coverage"].size(), std::size_t(6)); UAM_ASSERT_EQ(calls.load(), 5);
	const std::int64_t as_of = result["asOfMs"].get<std::int64_t>();
	for (int index = 0; index < 12; ++index)
	{
		UAM_ASSERT_EQ(history.Query("*", index % 2 ? 1 : 90)["totals"]["totalTokens"].get<int>(), 60);
		UAM_ASSERT_EQ(history.Query("one", 7)["totals"]["totalTokens"].get<int>(), 15);
	}
	UAM_ASSERT_EQ(calls.load(), 5); UAM_ASSERT_EQ(history.Query("*", 7)["asOfMs"].get<std::int64_t>(), as_of);
	UAM_ASSERT(history.Query("removed", 7).contains("error")); UAM_ASSERT(history.Query("*", 2).contains("error"));
}

UAM_TEST(UsageMultiHostRestoresOfflineCachesAndUsesCurrentWindowsWithoutQueryingTheRunner)
{
	TempDir temp("uam-usage-multi-cache");
	const ExecutionHost host = UsageHost("peer");
	nlohmann::json document = UsageSnapshot("codex-cli", "receipt", UsageNow() - 2LL * 86400000);
	document["asOfMs"] = UsageNow() - 2LL * 86400000 + 1000;
	{
		uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
		    [document](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json> { return document; });
		const nlohmann::json result = WaitUsage(history, "peer", [](const nlohmann::json& value) { return value.contains("totals") && !value.value("collecting", true); });
		UAM_ASSERT(result.contains("totals")); UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 15);
	}
	std::atomic<int> calls{0};
	uam::usage::MultiHostHistory restarted(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost&, std::stop_token, std::string& error) -> std::optional<nlohmann::json> { ++calls; error = "Machine is offline."; return std::nullopt; });
	const nlohmann::json cached = WaitUsage(restarted, "peer", [](const nlohmann::json& value) { return value.contains("coverage") && value["coverage"][0]["status"] == "cached" && !value.value("collecting", true); });
	UAM_ASSERT_EQ(cached["totals"]["totalTokens"].get<int>(), 15); UAM_ASSERT_EQ(cached["coverage"][0]["error"].get<std::string>(), std::string("Machine is offline."));
	UAM_ASSERT_EQ(restarted.Query("peer", 1)["totals"]["totalTokens"].get<int>(), 0);
	const nlohmann::json total = WaitUsage(restarted, "*", [](const nlohmann::json& value) { return value.contains("totals"); });
	UAM_ASSERT_EQ(total["totals"]["totalTokens"].get<int>(), 15); UAM_ASSERT(!total.value("complete", true));
	for (int index = 0; index < 10; ++index) restarted.Query("peer", 30);
	UAM_ASSERT_EQ(calls.load(), 1);
}

UAM_TEST(UsageMultiHostCancelsRemovedMachinesWhileLocalQueriesRemainAvailable)
{
	TempDir temp("uam-usage-multi-cancel");
	std::atomic<bool> entered{false}, cancelled{false};
	std::shared_ptr<uam::usage::Collector> local = EmptyLocal(temp.root);
	uam::usage::MultiHostHistory history(temp.root, local, {UsageHost("peer")},
	    [&](const ExecutionHost&, std::stop_token stop, std::string&) -> std::optional<nlohmann::json>
	    {
		    entered = true;
		    while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		    cancelled = true; return UsageSnapshot("codex-cli", "obsolete");
	    });
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while ((!entered || !local->AccountingSnapshot()) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	UAM_ASSERT(entered.load()); UAM_ASSERT(history.Query("local", 7).contains("totals"));
	history.SetHosts({});
	UAM_ASSERT(history.Query("peer", 7).contains("error"));
	const nlohmann::json total = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value["coverage"].size() == 1; });
	UAM_ASSERT(cancelled.load()); UAM_ASSERT_EQ(total["totals"]["totalTokens"].get<int>(), 0);
}

UAM_TEST(UsageMultiHostRejectsUnsafeConnectionsAndContainsMalformedProgressPerMachine)
{
	TempDir temp("uam-usage-multi-invalid");
	ExecutionHost invalid = UsageHost("../escape"); std::atomic<int> calls{0};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {invalid, UsageHost("malformed"), UsageHost("throws"), UsageHost("working")},
	    [&](const ExecutionHost& host, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    ++calls;
		    if (host.id == "throws") throw std::runtime_error("fixture failure");
		    if (host.id == "malformed") return nlohmann::json{{"pending", true}, {"collectionError", 17}};
		    return UsageSnapshot("codex-cli", "receipt");
	    });
	const nlohmann::json total = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && !value.value("collecting", true); });
	UAM_ASSERT_EQ(total["totals"]["totalTokens"].get<int>(), 15); UAM_ASSERT_EQ(calls.load(), 3);
	UAM_ASSERT_EQ(total["coverage"].size(), std::size_t(5)); UAM_ASSERT(!total.value("complete", true));
	UAM_ASSERT(!history.Query("malformed", 7)["collectionError"].get<std::string>().empty());
	UAM_ASSERT(!fs::exists(temp.root.parent_path() / "escape"));
}

UAM_TEST(UsageMultiHostKeepsThePreviousCacheAfterInvalidSnapshotAndRejectsReconfiguredOwnership)
{
	TempDir temp("uam-usage-multi-preserve");
	std::atomic<bool> invalid{false}; std::atomic<int> calls{0};
	ExecutionHost host = UsageHost("peer"); const nlohmann::json document = UsageSnapshot("codex-cli", "receipt");
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    ++calls; nlohmann::json value = document;
		    if (invalid) value["facts"][0][2][5] = -1;
		    return value;
	    });
	UAM_ASSERT(WaitUsage(history, "peer", [](const nlohmann::json& value) { return value.contains("totals"); }).contains("totals"));
	invalid = true; host.runner_version = "changed"; history.SetHosts({host});
	const nlohmann::json cached = WaitUsage(history, "peer", [](const nlohmann::json& value) { return value.contains("totals") && !value.value("collectionError", "").empty(); });
	UAM_ASSERT_EQ(cached["totals"]["totalTokens"].get<int>(), 15); UAM_ASSERT_EQ(cached["coverage"][0]["status"].get<std::string>(), std::string("cached"));
	host.ssh_alias = "different-machine"; history.SetHosts({host});
	const nlohmann::json rejected = WaitUsage(history, "peer", [](const nlohmann::json& value) { return !value.value("collectionError", "").empty(); });
	UAM_ASSERT(!rejected.contains("totals")); UAM_ASSERT(calls.load() >= 3);
}

UAM_TEST(UsageMultiHostRecoversFromUnsupportedMachineCountWithoutLaunchingConnections)
{
	TempDir temp("uam-usage-multi-capacity");
	std::vector<ExecutionHost> hosts;
	for (int index = 0; index < 33; ++index) hosts.push_back(UsageHost("peer-" + std::to_string(index)));
	std::atomic<int> calls{0};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), hosts,
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json> { ++calls; return UsageSnapshot("codex-cli", "receipt"); });
	const nlohmann::json rejected = WaitUsage(history, "peer-0", [](const nlohmann::json& value) { return !value.value("collectionError", "").empty(); });
	UAM_ASSERT(rejected["collectionError"].get<std::string>().find("32") != std::string::npos); UAM_ASSERT_EQ(calls.load(), 0);
	history.SetHosts({hosts.front()});
	const nlohmann::json restored = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false); });
	UAM_ASSERT_EQ(restored["totals"]["totalTokens"].get<int>(), 15); UAM_ASSERT_EQ(calls.load(), 1);
}
