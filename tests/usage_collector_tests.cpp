#include "test_harness.h"
#include "common/usage/usage_collector.h"
#include <thread>
#include <sqlite3.h>
#include "common/utils/base64.h"

using namespace uam_test;

namespace
{
	nlohmann::json WaitForAccounting(uam::usage::Collector& collector)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		nlohmann::json result;
		do
		{
			result = collector.Query(7);
			if (result.contains("asOfMs") && !result.value("collecting", true)) return result;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (std::chrono::steady_clock::now() < deadline);
		return result;
	}
}

UAM_TEST(UsageCollectorLoadsAtStartupAndQueriesDoNotReadChangedOrDeletedProviderFiles)
{
	TempDir temp("uam-usage-collector");
	const fs::path provider = temp.root / "provider", data = temp.root / "data";
	fs::create_directories(provider);
	const fs::path transcript = provider / "session.jsonl";
	const std::string line = nlohmann::json({{"type", "assistant"}, {"timestamp", uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(std::time(nullptr) - 2), uam::time::kIsoUtcTimestampFormat)}, {"sessionId", "session"}, {"requestId", "receipt"},
	    {"message", {{"id", "receipt"}, {"model", "claude-sonnet"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(transcript, line));
	{
		uam::usage::Collector collector(data, {{uam::provider_ids::kClaudeCli, {provider}}});
		const nlohmann::json first = WaitForAccounting(collector);
		UAM_ASSERT(first.contains("asOfMs")); UAM_ASSERT(first["complete"].get<bool>());
		UAM_ASSERT_EQ(first["totals"]["totalTokens"].get<int>(), 15);
		UAM_ASSERT(fs::remove(transcript));
		const nlohmann::json last_day = collector.Query(1), ninety_days = collector.Query(90);
		UAM_ASSERT_EQ(last_day["totals"]["totalTokens"].get<int>(), 15);
		UAM_ASSERT_EQ(ninety_days["totals"]["totalTokens"].get<int>(), 15);
		UAM_ASSERT_EQ(first["asOfMs"], last_day["asOfMs"]);
		UAM_ASSERT(collector.Query(2).contains("error"));
	}
	UAM_ASSERT(fs::exists(data / "usage" / "snapshot-v1.json"));
	{
		uam::usage::Collector restarted(data, {{uam::provider_ids::kClaudeCli, {provider}}});
		const nlohmann::json restored = WaitForAccounting(restarted);
		UAM_ASSERT(restored.contains("asOfMs"));
		UAM_ASSERT_EQ(restored["totals"]["totalTokens"].get<int>(), 15);
		UAM_ASSERT_EQ(restored["filesTotal"].get<int>(), 0);
	}
}

UAM_TEST(UsageCollectionSinkRetriesUncommittedReceiptsAndDoesNotImportUnchangedFiles)
{
	TempDir temp("uam-usage-sink");
	const fs::path file = temp.root / "session.jsonl";
	const std::string line = nlohmann::json({{"type", "assistant"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"sessionId", "session"}, {"requestId", "receipt"},
	    {"message", {{"id", "receipt"}, {"model", "claude-sonnet"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(file, line));
	uam::usage::HistoryService service({{uam::provider_ids::kClaudeCli, {temp.root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	std::size_t imports = 0;
	const uam::usage::AccountingSink sink = [&](const std::string& source, const std::vector<uam::usage::Record>& records, std::size_t position, std::string& error)
	{
		UAM_ASSERT_EQ(source, file.string()); UAM_ASSERT_EQ(position, std::size_t(0)); UAM_ASSERT_EQ(records.size(), std::size_t(1));
		if (++imports == 1) { error = "busy"; return false; }
		return true;
	};
	UAM_ASSERT(service.Collect("first", start, start + 86400000, sink).contains("error"));
	UAM_ASSERT(service.Collect("retry", start, start + 86400000, sink)["complete"].get<bool>());
	UAM_ASSERT(service.Collect("unchanged", start, start + 86400000, sink)["complete"].get<bool>());
	UAM_ASSERT_EQ(imports, std::size_t(2));
}

UAM_TEST(UsageCollectionSkipsUnchangedOpenCodeDatabaseAndDetectsMutableUsageInWal)
{
	TempDir temp("uam-usage-collector-wal");
	const fs::path path = temp.root / "opencode.db";
	sqlite3* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
	const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE session(id TEXT PRIMARY KEY,time_created INTEGER); INSERT INTO session VALUES('session',50); CREATE TABLE session_message(id TEXT,session_id TEXT,time_created INTEGER,type TEXT,data TEXT)", nullptr, nullptr, nullptr), SQLITE_OK);
	const std::function<void(int)> write = [&](int input)
	{
		const std::string json = nlohmann::json({{"role", "assistant"}, {"providerID", "inferdeck"}, {"modelID", "model"}, {"tokens", {{"input", input}, {"output", 5}, {"reasoning", 0}, {"cache", {{"read", 0}, {"write", 0}}}}}}).dump();
		sqlite3_stmt* statement = nullptr;
		UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "UPDATE session_message SET data=?1", -1, &statement, nullptr), SQLITE_OK);
		UAM_ASSERT_EQ(sqlite3_bind_text(statement, 1, json.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
		UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE); UAM_ASSERT_EQ(sqlite3_finalize(statement), SQLITE_OK);
	};
	UAM_ASSERT_EQ(sqlite3_exec(raw, "INSERT INTO session_message VALUES('message','session',100,'assistant','{}')", nullptr, nullptr, nullptr), SQLITE_OK);
	write(10);
	const fs::file_time_type main_modified = fs::last_write_time(path);
	uam::usage::HistoryService service({{uam::provider_ids::kOpenCodeCli, {}, {path}}});
	std::size_t imports = 0; std::int64_t input = 0;
	const uam::usage::AccountingSink sink = [&](const std::string&, const std::vector<uam::usage::Record>& records, std::size_t position, std::string&)
	{
		++imports; UAM_ASSERT_EQ(position, std::size_t(0)); UAM_ASSERT_EQ(records.size(), std::size_t(1)); input = records[0].tokens.input; return true;
	};
	const std::function<nlohmann::json(const std::string&)> collect = [&](const std::string& id)
	{
		nlohmann::json result;
		for (int batch = 0; batch < 16; ++batch)
		{
			result = service.Collect(id, 0, 200, sink);
			UAM_ASSERT(!result.contains("error"));
			if (result.value("complete", false)) return result;
		}
		UAM_ASSERT(false); return result;
	};
	collect("first"); UAM_ASSERT_EQ(imports, std::size_t(1)); UAM_ASSERT_EQ(input, 10LL);
	UAM_ASSERT_EQ(collect("unchanged")["bytesRead"].get<int>(), 0); UAM_ASSERT_EQ(imports, std::size_t(1));
	write(40); UAM_ASSERT_EQ(fs::last_write_time(path), main_modified);
	collect("changed-wal"); UAM_ASSERT_EQ(imports, std::size_t(2)); UAM_ASSERT_EQ(input, 40LL);
}

UAM_TEST(UsageCollectorPricesDiscoveredModelsWithoutChangingStoredReceiptsAndHonorsOverrides)
{
	TempDir temp("uam-usage-collector-prices");
	const fs::path provider = temp.root / "provider"; fs::create_directories(provider);
	const std::string line = nlohmann::json({{"type", "assistant"}, {"timestamp", uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(std::time(nullptr) - 2), uam::time::kIsoUtcTimestampFormat)}, {"sessionId", "session"}, {"requestId", "receipt"},
	    {"message", {{"id", "receipt"}, {"model", "test-model"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(provider / "session.jsonl", line));
	uam::usage::Collector collector(temp.root / "data", {{uam::provider_ids::kClaudeCli, {provider}}}, [](std::stop_token) -> std::optional<std::string>
	{
		return nlohmann::json({{"test-model", {{"input_cost_per_token", 0.000002}, {"output_cost_per_token", 0.000010}}}}).dump();
	});
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	nlohmann::json result;
	do
	{
		result = collector.Query(7);
		if (result.contains("models") && !result["models"].empty() && result["models"][0].contains("priceReference")) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	} while (std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(result.contains("models"));
	UAM_ASSERT_EQ(result["models"][0]["priceReference"]["kind"].get<std::string>(), std::string("reference"));
	UAM_ASSERT(std::abs(result["totals"]["estimatedCostUsd"].get<double>() - 0.000054) < 1e-12);
	const nlohmann::json manual = collector.Query(7, {{{"claude-cli", "test-model", "standard"}, {0, 0, 0, 0}}});
	UAM_ASSERT_EQ(manual["totals"]["estimatedCostUsd"].get<double>(), 0.0);
	UAM_ASSERT_EQ(manual["models"][0]["priceReference"]["kind"].get<std::string>(), std::string("manual"));
	UAM_ASSERT_EQ(collector.Query(7)["totals"]["estimatedCostUsd"], result["totals"]["estimatedCostUsd"]);
}

UAM_TEST(UsageCollectorExportsCachedAccountingWithoutProviderIoAndRejectsOtherSnapshots)
{
	TempDir temp("uam-usage-accounting-export");
	const fs::path provider = temp.root / "provider";
	fs::create_directories(provider);
	const fs::path transcript = provider / "session.jsonl";
	const std::string timestamp = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(std::time(nullptr) - 2), uam::time::kIsoUtcTimestampFormat);
	UAM_ASSERT(uam::io::WriteTextFile(transcript, nlohmann::json({{"type", "assistant"}, {"timestamp", timestamp}, {"sessionId", "session"}, {"requestId", "receipt"}, {"message", {{"id", "receipt"}, {"model", "claude-sonnet"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n"));
	uam::usage::Collector collector(temp.root / "data", {{uam::provider_ids::kClaudeCli, {provider}}}, {}, true);
	UAM_ASSERT(WaitForAccounting(collector).contains("asOfMs"));
	const nlohmann::json first = collector.ExportAccounting();
	UAM_ASSERT(first.contains("dataBase64"));
	const fs::path index = temp.root / "data" / "usage" / "history-index-v1.sqlite";
	const fs::file_time_type modified = fs::last_write_time(index);
	UAM_ASSERT(fs::remove(transcript));
	UAM_ASSERT_EQ(collector.ExportAccounting(), first);
	UAM_ASSERT_EQ(fs::last_write_time(index), modified);
	UAM_ASSERT(collector.ExportAccounting(0, "0000000000000000").contains("error"));
	UAM_ASSERT(collector.ExportAccounting(first["totalBytes"].get<std::size_t>() + 1).contains("error"));
	std::string bytes;
	UAM_ASSERT(uam::base64::Decode(first["dataBase64"].get<std::string>(), bytes));
	const nlohmann::json document = nlohmann::json::parse(bytes);
	UAM_ASSERT_EQ(document["version"], nlohmann::json(2));
	UAM_ASSERT_EQ(document["range"], nlohmann::json("all-recorded"));
	UAM_ASSERT_EQ(document["facts"].size(), std::size_t{1});
	const std::optional<uam::usage::AccountingRecord> fact = uam::usage::DecodeAccountingRecord(document["facts"][0]);
	UAM_ASSERT(fact); UAM_ASSERT(!fact->record.dedupe_key.empty());
	UAM_ASSERT_EQ(fact->record.tokens.Total(), 15LL);
}

UAM_TEST(UsageCollectorBackfillsOldReceiptsAndKeepsLifetimeQueriesOffProviderFiles)
{
	TempDir temp("uam-usage-lifetime-collector");
	const fs::path provider = temp.root / "provider", data = temp.root / "data";
	fs::create_directories(provider);
	const fs::path transcript = provider / "old-session.jsonl";
	const std::string timestamp = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(std::time(nullptr) - 180LL * 86400), uam::time::kIsoUtcTimestampFormat);
	const std::string line = nlohmann::json({{"type", "assistant"}, {"timestamp", timestamp}, {"sessionId", "old-session"}, {"requestId", "old-receipt"},
	    {"message", {{"id", "old-receipt"}, {"model", "claude-sonnet"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(transcript, line));
	{
		uam::usage::Collector collector(data, {{uam::provider_ids::kClaudeCli, {provider}}}, {}, true);
		UAM_ASSERT(WaitForAccounting(collector).contains("asOfMs"));
		const nlohmann::json all = collector.Query(0);
		UAM_ASSERT(all.contains("totals"));
		UAM_ASSERT_EQ(all["startMs"], nlohmann::json(0));
		UAM_ASSERT_EQ(all["windowDays"], nlohmann::json(0));
		UAM_ASSERT_EQ(all["totals"]["totalTokens"].get<int>(), 15);
		UAM_ASSERT_EQ(all["historyCoverage"]["kind"], nlohmann::json("recorded-and-available-history"));
		UAM_ASSERT(all["historyCoverage"]["previouslyPrunedReceiptsMayBeMissing"].get<bool>());
		UAM_ASSERT_EQ(collector.Query(90)["totals"]["totalTokens"].get<int>(), 0);
		UAM_ASSERT(fs::remove(transcript));
		UAM_ASSERT_EQ(collector.Query(0)["totals"]["totalTokens"].get<int>(), 15);
	}
	uam::usage::Collector restarted(data, {{uam::provider_ids::kClaudeCli, {provider}}}, {}, true);
	UAM_ASSERT(WaitForAccounting(restarted).contains("asOfMs"));
	UAM_ASSERT_EQ(restarted.Query(0)["totals"]["totalTokens"].get<int>(), 15);
	UAM_ASSERT_EQ(restarted.Query(0)["filesTotal"].get<int>(), 0);
}

UAM_TEST(UsageCollectorStreamedExportKeepsItsLeasedSnapshotAcrossBackgroundPublication)
{
	TempDir temp("uam-usage-export-lease"); uam::usage::Store store(temp.root); std::string error;
	uam::usage::Record record; record.provider_id = "codex-cli"; record.model_id = "model"; record.session_id = "session"; record.dedupe_key = "first";
	record.timestamp_ms = uam::time::SystemEpochMillisecondsNow() - 180LL * 86400000; record.tokens.input = 1;
	UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error));
	uam::usage::Collector collector(temp.root, std::vector<uam::usage::HistorySource>{}, uam::usage::RateFetcher{}, true);
	UAM_ASSERT(WaitForAccounting(collector).contains("totals"));
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> original = collector.Receipts(); UAM_ASSERT(original);
	const nlohmann::json first = collector.ExportReceipts(); UAM_ASSERT(first.contains("fingerprint"));
	record.dedupe_key = "second"; UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
	while (collector.Receipts() == original && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	UAM_ASSERT(collector.Receipts() != original); UAM_ASSERT_EQ(collector.Query(0)["totals"]["totalTokens"].get<std::int64_t>(), 2LL);
	const nlohmann::json leased = collector.ExportReceipts(0, first["fingerprint"].get<std::string>());
	UAM_ASSERT(!leased.contains("error")); UAM_ASSERT_EQ(leased["fingerprint"], first["fingerprint"]); UAM_ASSERT_EQ(leased["totalBytes"], first["totalBytes"]); UAM_ASSERT_EQ(leased["dataBase64"], first["dataBase64"]);
	UAM_ASSERT(collector.ExportReceipts(1).contains("error")); UAM_ASSERT(collector.ExportReceipts(original->bytes + 1, original->fingerprint).contains("error"));
}
