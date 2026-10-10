#include "test_harness.h"
#include "common/usage/usage_collector.h"
#include <thread>
#include <sqlite3.h>

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
