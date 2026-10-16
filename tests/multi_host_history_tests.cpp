#include "test_harness.h"
#include "common/usage/multi_host_history.h"
#include "common/usage/antigravity_accounting.h"
#include <atomic>
#include "common/utils/hash_utils.h"
#include <thread>
#include <iostream>

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
	UAM_ASSERT(!cached.value("complete", true));
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


UAM_TEST(UsageMultiHostRetainsAntigravityTimeWitnessesOutsideTheAccountingWindow)
{
	TempDir temp("uam-usage-antigravity-witnesses");
	const std::int64_t now = UsageNow();
	nlohmann::json document = UsageSnapshot("antigravity-cli","unused"); document["facts"] = nlohmann::json::array();
	for (int index = 0; index < 4; ++index)
	{
		uam::usage::AntigravityUsageCandidate candidate;
		candidate.record.provider_id = uam::provider_ids::kAntigravityCli; candidate.record.model_id = "gemini-3-pro";
		candidate.record.session_id = "session"; candidate.record.tokens.input = 4;
		candidate.location_id = nlohmann::json::array({"antigravity","session","step",index/2,0}).dump();
		candidate.aliases = {nlohmann::json::array({12,index<2 ? "old-request" : "future-request"}).dump()};
		candidate.timestamp_quality = index%2 ? 0 : 2;
		candidate.record.timestamp_ms = index%2 ? now-1000 : index==0 ? now-100*86400000LL : now+86400000;
		const std::optional<uam::usage::Record> record = uam::usage::EncodeAntigravityAccountingRecord(candidate); UAM_ASSERT(record);
		document["facts"].push_back(uam::usage::EncodeAccountingRecord({nlohmann::json::array({"native",record->provider_id,record->dedupe_key}).dump(),*record}));
	}
	{
		uam::usage::MultiHostHistory history(temp.root,EmptyLocal(temp.root),{UsageHost("peer")},
		    [document](const ExecutionHost&,std::stop_token,std::string&) -> std::optional<nlohmann::json> { return document; });
		const nlohmann::json result = WaitUsage(history,"peer",[](const nlohmann::json& value) { return value.contains("totals") && !value.value("collecting",true); });
		UAM_ASSERT(result.contains("totals")); UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(),0);
		UAM_ASSERT_EQ(result["coverage"][0]["status"].get<std::string>(),std::string("ready"));
	}
	uam::usage::MultiHostHistory offline(temp.root,EmptyLocal(temp.root),{UsageHost("peer")},
	    [](const ExecutionHost&,std::stop_token,std::string& error) -> std::optional<nlohmann::json> { error="Offline"; return std::nullopt; });
	const nlohmann::json restored = WaitUsage(offline,"peer",[](const nlohmann::json& value) { return value.contains("coverage") && value["coverage"][0]["status"]=="cached" && !value.value("collecting",true); });
	UAM_ASSERT_EQ(restored["totals"]["totalTokens"].get<int>(),0);
}

UAM_TEST(UsageMultiHostLifetimeDeduplicatesOldReceiptsAndRejectsLegacyScope)
{
	TempDir temp("uam-usage-lifetime-peers");
	std::atomic<int> calls{0};
	nlohmann::json lifetime = UsageSnapshot("codex-cli", "old-mirrored-receipt", UsageNow() - 180LL * 86400000);
	lifetime["version"] = 2;
	lifetime["range"] = "all-recorded";
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {UsageHost("one"), UsageHost("two")},
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json> { ++calls; return lifetime; });
	const nlohmann::json rolling = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT(rolling.contains("totals"));
	UAM_ASSERT_EQ(rolling["totals"]["totalTokens"].get<int>(), 0);
	const nlohmann::json all = history.Query("*", 0);
	UAM_ASSERT(all.contains("totals"));
	UAM_ASSERT_EQ(all["totals"]["totalTokens"].get<int>(), 15);
	UAM_ASSERT_EQ(all["duplicates"].get<int>(), 1);
	UAM_ASSERT_EQ(history.Query("one", 0)["totals"]["totalTokens"].get<int>(), 15);
	UAM_ASSERT_EQ(calls.load(), 2);
	const nlohmann::json legacy = UsageSnapshot("codex-cli", "recent-legacy-receipt");
	uam::usage::MultiHostHistory old_runner(temp.root / "legacy", EmptyLocal(temp.root / "legacy"), {UsageHost("legacy")},
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json> { return legacy; });
	UAM_ASSERT(WaitUsage(old_runner, "legacy", [](const nlohmann::json& value) { return value.contains("totals"); }).contains("totals"));
	UAM_ASSERT(old_runner.Query("legacy", 0).contains("error"));
	UAM_ASSERT(old_runner.Query("*", 0).contains("error"));
	UAM_ASSERT_EQ(old_runner.Query("legacy", 7)["totals"]["totalTokens"].get<int>(), 15);
}

UAM_TEST(UsageMultiHostStreamedLifetimeDeduplicatesPeersBeyondCombinedDocumentCap)
{
	TempDir temp("uam-usage-streamed-peers"); uam::usage::Store store(temp.root / "fixture"); std::string error;
	const std::int64_t now = UsageNow();
	for (int batch_index = 0; batch_index < 3; ++batch_index)
	{
		std::vector<uam::usage::Record> records;
		for (int index = 0; index < 1500; ++index)
		{
			uam::usage::Record record; record.provider_id = "codex-cli"; record.model_id = "old-model"; record.session_id = "one-session"; record.dedupe_key = std::string(8000, 'r') + std::to_string(batch_index * 1500 + index);
			record.timestamp_ms = now - 180LL * 86400000; record.tokens.input = 2; records.push_back(std::move(record));
		}
		std::vector<uam::usage::SourceRecord> references; for (const uam::usage::Record& record : records) references.push_back({&record, "fixture", 0}); UAM_ASSERT(store.Import(references, error));
	}
	nlohmann::json metadata = uam::usage::EmptyHistoryMetadata(); metadata["complete"] = true;
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> snapshot = store.WriteReceiptSnapshot(now, metadata, error); UAM_ASSERT(snapshot); UAM_ASSERT(2 * snapshot->bytes > 64 * 1024 * 1024);
	const std::vector<ExecutionHost> hosts{UsageHost("one"), UsageHost("two")};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), hosts,
	    [&](const ExecutionHost& host, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    const std::string scope = "host-" + host.id + "-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(nlohmann::json::array({host.ssh_alias, host.platform, host.runner_directory}).dump()));
		    const fs::path path = temp.root / "usage" / "remote" / scope / "snapshot-v3.jsonl"; fs::create_directories(path.parent_path()); fs::copy_file(snapshot->path, path, fs::copy_options::overwrite_existing);
		    return nlohmann::json{{"receiptSnapshotPath", uam::paths::Utf8PathString(path)}};
	    });
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20); nlohmann::json total;
	do { total = history.Query("*", 0); if (total.contains("totals") && total.value("complete", false) && !total.value("collecting", true)) break; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } while (std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(total.contains("totals")); UAM_ASSERT(total.value("complete", false)); UAM_ASSERT_EQ(total["totals"]["totalTokens"].get<std::int64_t>(), 9000LL); UAM_ASSERT_EQ(total["totals"]["records"].get<std::size_t>(), std::size_t{4500}); UAM_ASSERT_EQ(total["duplicates"].get<std::size_t>(), std::size_t{4500}); UAM_ASSERT_EQ(total["totals"]["sessions"].get<std::size_t>(), std::size_t{1});
	UAM_ASSERT_EQ(history.Query("*", 90)["totals"]["totalTokens"].get<std::int64_t>(), 0LL);
}

UAM_TEST(UsageMultiHostPinsLastGoodStreamedReceiptsWhenCacheReplacementFailsCollection)
{
	TempDir temp("uam-usage-pinned-cache"); uam::usage::Store store(temp.root / "fixture"); std::string error;
	uam::usage::Record record; record.provider_id = "codex-cli"; record.model_id = "model"; record.session_id = "session"; record.dedupe_key = "receipt"; record.timestamp_ms = UsageNow() - 3LL * 86400000; record.tokens.input = 15;
	UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error)); nlohmann::json metadata = uam::usage::EmptyHistoryMetadata(); metadata["complete"] = true;
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> snapshot = store.WriteReceiptSnapshot(UsageNow(), metadata, error); UAM_ASSERT(snapshot);
	ExecutionHost host = UsageHost("peer"); std::atomic<bool> fail{false};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost& peer, std::stop_token, std::string& fetch_error) -> std::optional<nlohmann::json>
	    {
		    const std::string scope = "host-" + peer.id + "-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(nlohmann::json::array({peer.ssh_alias, peer.platform, peer.runner_directory}).dump()));
		    const fs::path path = temp.root / "usage" / "remote" / scope / "snapshot-v3.jsonl"; fs::create_directories(path.parent_path());
		    if (fail) { UAM_ASSERT(uam::io::AtomicWriteFile(path, "invalid replacement")); fetch_error = "Offline"; return std::nullopt; }
		    fs::copy_file(snapshot->path, path, fs::copy_options::overwrite_existing); fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(48)); return nlohmann::json{{"receiptSnapshotPath", uam::paths::Utf8PathString(path)}};
	    });
	UAM_ASSERT(WaitUsage(history, "peer", [](const nlohmann::json& value) { return value.contains("totals"); }).contains("totals"));
	fail = true; host.runner_version = "changed"; history.SetHosts({host});
	const nlohmann::json cached = WaitUsage(history, "peer", [](const nlohmann::json& value) { return !value.value("collectionError", "").empty(); });
	UAM_ASSERT(cached.contains("totals")); UAM_ASSERT_EQ(cached["totals"]["totalTokens"].get<std::int64_t>(), 15LL); UAM_ASSERT_EQ(cached["coverage"][0]["status"].get<std::string>(), std::string("cached"));
}

UAM_TEST(UsageMultiHostDoesNotReimportUnchangedReceiptsForMetadataRefresh)
{
	TempDir temp("uam-usage-multi-import-count");
	ExecutionHost host = UsageHost("peer");
	const nlohmann::json document = UsageSnapshot("codex-cli", "receipt");
	std::atomic<int> calls{0};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    nlohmann::json refreshed = document;
		    const int count = ++calls;
		    refreshed["metadata"]["filesChecked"] = count;
		    refreshed["metadata"]["filesTotal"] = count;
		    return refreshed;
	    });
	const nlohmann::json initial = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT(initial.contains("totals"));
	const std::uint64_t imports = history.ImportedRecordCount();
	const std::uint64_t builds = history.LedgerBuildCount();
	UAM_ASSERT(imports >= 2);
	host.label = "Renamed peer";
	history.SetHosts({host});
	const nlohmann::json refreshed = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("filesChecked", 0) == 2 && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT_EQ(refreshed.value("filesChecked", 0), 2);
	UAM_ASSERT_EQ(refreshed["totals"]["totalTokens"].get<int>(), 15);
	UAM_ASSERT_EQ(refreshed["coverage"][1]["label"].get<std::string>(), std::string("Renamed peer"));
	UAM_ASSERT_EQ(history.ImportedRecordCount(), imports);
	const std::uint64_t refreshed_builds = history.LedgerBuildCount();
	std::cout << "Metadata refresh imported rows " << imports << " -> " << history.ImportedRecordCount()
	    << "; lifetime ledger builds " << builds << " -> " << refreshed_builds << std::endl;
	if (initial["asOfMs"].get<std::int64_t>() / 30000 == refreshed["asOfMs"].get<std::int64_t>() / 30000) UAM_ASSERT_EQ(refreshed_builds, builds);
}

UAM_TEST(UsageMultiHostInvalidatesRetainedAccountingForMutableTokensAndCost)
{
	TempDir temp("uam-usage-multi-mutable-cache");
	ExecutionHost host = UsageHost("peer");
	const nlohmann::json document = UsageSnapshot("codex-cli", "receipt");
	std::atomic<bool> updated{false};
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost&, std::stop_token, std::string&) -> std::optional<nlohmann::json>
	    {
		    nlohmann::json refreshed = document;
		    if (updated)
		    {
			    refreshed["facts"][0][2][5] = 20;
			    refreshed["facts"][0][2][10] = 0.25;
		    }
		    return refreshed;
	    });
	const nlohmann::json initial = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT(initial.contains("totals"));
	UAM_ASSERT_EQ(initial["totals"]["totalTokens"].get<int>(), 15);
	const std::uint64_t imports = history.ImportedRecordCount();
	updated = true;
	host.label = "Updated peer";
	history.SetHosts({host});
	const nlohmann::json refreshed = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value["totals"]["totalTokens"] == 25; });
	UAM_ASSERT_EQ(refreshed["totals"]["totalTokens"].get<int>(), 25);
	UAM_ASSERT_EQ(refreshed["totals"]["reportedCostUsd"].get<double>(), 0.25);
	UAM_ASSERT_EQ(history.Query("peer", 7)["totals"]["totalTokens"].get<int>(), 25);
	UAM_ASSERT_EQ(history.Query("peer", 7)["totals"]["reportedCostUsd"].get<double>(), 0.25);
	UAM_ASSERT(history.ImportedRecordCount() >= imports + 2);
}

UAM_TEST(UsageMultiHostStreamedMetadataRefreshDoesNotReimportUnchangedFacts)
{
	TempDir temp("uam-usage-streamed-metadata");
	uam::usage::Store store(temp.root / "fixture");
	std::string error;
	const std::int64_t now = UsageNow() - 1000;
	uam::usage::Record record; record.provider_id = "codex-cli"; record.model_id = "model"; record.session_id = "session"; record.dedupe_key = "receipt"; record.timestamp_ms = now - 1000; record.tokens.input = 15;
	UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error));
	std::atomic<int> calls{0};
	ExecutionHost host = UsageHost("peer");
	uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
	    [&](const ExecutionHost& peer, std::stop_token, std::string& fetch_error) -> std::optional<nlohmann::json>
	    {
		    const int count = ++calls;
		    nlohmann::json metadata = uam::usage::EmptyHistoryMetadata(); metadata["complete"] = true; metadata["filesChecked"] = count; metadata["filesTotal"] = count;
		    const auto snapshot = store.WriteReceiptSnapshot(now + count, metadata, fetch_error);
		    if (!snapshot) return std::nullopt;
		    const std::string scope = "host-" + peer.id + "-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(nlohmann::json::array({peer.ssh_alias, peer.platform, peer.runner_directory}).dump()));
		    const fs::path path = temp.root / "usage" / "remote" / scope / "snapshot-v3.jsonl";
		    fs::create_directories(path.parent_path()); fs::copy_file(snapshot->path, path, fs::copy_options::overwrite_existing);
		    return nlohmann::json{{"receiptSnapshotPath", uam::paths::Utf8PathString(path)}};
	    });
	const auto initial = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("complete", false) && !value.value("collecting", true); });
	UAM_ASSERT(initial.contains("totals"));
	const std::uint64_t imports = history.ImportedRecordCount();
	host.label = "Refreshed peer"; history.SetHosts({host});
	const auto refreshed = WaitUsage(history, "*", [](const nlohmann::json& value) { return value.contains("totals") && value.value("filesChecked", 0) == 2 && value.value("complete", false); });
	UAM_ASSERT_EQ(refreshed.value("filesChecked", 0), 2);
	UAM_ASSERT_EQ(refreshed["totals"]["totalTokens"].get<int>(), 15);
	std::cout << "Streamed header refresh imported rows " << imports << " -> " << history.ImportedRecordCount() << std::endl;
	UAM_ASSERT_EQ(history.ImportedRecordCount(), imports);
}

UAM_TEST(UsageMultiHostStreamedFactReuseRevalidatesRegressedAndRollingBounds)
{
	for (const bool lifetime : {true, false})
	{
		TempDir temp(lifetime ? "uam-usage-regressed-bound" : "uam-usage-rolling-bound");
		uam::usage::Store store(temp.root / "fixture");
		std::string error;
		const std::int64_t now = UsageNow();
		const std::int64_t first_as_of = lifetime ? now - 1000 : now - 2LL * 86400000;
		uam::usage::Record record; record.provider_id = "codex-cli"; record.model_id = "model"; record.session_id = "session"; record.dedupe_key = "receipt"; record.tokens.input = 15;
		record.timestamp_ms = lifetime ? first_as_of - 1000 : first_as_of / 86400000 * 86400000 - 89LL * 86400000;
		UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error));
		nlohmann::json metadata = uam::usage::EmptyHistoryMetadata(); metadata["complete"] = true;
		const auto snapshot = store.WriteReceiptSnapshot(now - 1, metadata, error); UAM_ASSERT(snapshot);
		const std::string bytes = ReadFile(snapshot->path);
		const std::size_t header_end = bytes.find('\n'); UAM_ASSERT(header_end != std::string::npos);
		const std::string facts = bytes.substr(header_end + 1);
		std::atomic<bool> narrowed{false};
		ExecutionHost host = UsageHost("peer");
		uam::usage::MultiHostHistory history(temp.root, EmptyLocal(temp.root), {host},
		    [&](const ExecutionHost& peer, std::stop_token, std::string&) -> std::optional<nlohmann::json>
		    {
			    nlohmann::json header = nlohmann::json::parse(bytes.substr(0, header_end));
			    header["range"] = lifetime ? "all-recorded" : "rolling-90-days";
			    header["asOfMs"] = narrowed ? (lifetime ? record.timestamp_ms : first_as_of + 86400000) : first_as_of;
			    const std::string scope = "host-" + peer.id + "-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(nlohmann::json::array({peer.ssh_alias, peer.platform, peer.runner_directory}).dump()));
			    const fs::path path = temp.root / "usage" / "remote" / scope / "snapshot-v3.jsonl";
			    fs::create_directories(path.parent_path()); UAM_ASSERT(uam::io::AtomicWriteFile(path, header.dump() + "\n" + facts));
			    return nlohmann::json{{"receiptSnapshotPath", uam::paths::Utf8PathString(path)}};
		    });
		const auto initial = WaitUsage(history, "peer", [](const nlohmann::json& value) { return value.contains("totals") && !value.value("collecting", true); });
		UAM_ASSERT(initial.contains("totals"));
		UAM_ASSERT_EQ(initial["totals"]["records"].get<int>(), lifetime ? 1 : 0);
		narrowed = true; host.label = "Narrowed snapshot"; history.SetHosts({host});
		const auto rejected = WaitUsage(history, "peer", [](const nlohmann::json& value) { return !value.value("collectionError", "").empty(); });
		UAM_ASSERT(!rejected.contains("totals"));
		UAM_ASSERT(rejected.value("collectionError", "").find("scope") != std::string::npos);
	}
}
