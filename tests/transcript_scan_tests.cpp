#include "test_harness.h"
#include "common/usage/transcript_scan.h"
#include "common/usage/opencode_history_scan.h"
#include "common/usage/usage_aggregation.h"
#include "common/usage/usage_history_service.h"
#include <sqlite3.h>
#include <memory>

using namespace uam_test;

namespace
{
	std::string UsageLine(int index, int input = 12)
	{
		return nlohmann::json({{"type", "assistant"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"sessionId", "session"}, {"requestId", "request-" + std::to_string(index)},
		    {"message", {{"id", "message-" + std::to_string(index)}, {"model", "model-\xe2\x98\x83"}, {"usage", {{"input_tokens", input}, {"output_tokens", 3}}}}}}).dump();
	}
}

UAM_TEST(UsageScanResumesSmallChunksAndWaitsForCompleteUtf8LinesWithoutDuplicates)
{
	TempDir temp("uam-usage-scan-append");
	const fs::path path = temp.root / "session.jsonl";
	const std::string first = UsageLine(1);
	const std::size_t split = first.find("\xe2\x98\x83") + 1;
	UAM_ASSERT(split > 1);
	UAM_ASSERT(uam::io::WriteTextFile(path, first.substr(0, split)));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	uam::usage::ScanResult result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.error.empty() && !result.complete);
	UAM_ASSERT(scan.Records().empty());
	UAM_ASSERT_EQ(scan.InvalidLines(), std::size_t(0));
	const std::string appended = first + "\r\n" + UsageLine(2) + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, appended));
	uam::usage::ScanOptions options;
	options.byte_budget = 17;
	std::size_t total_read = split;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		result = scan.Refresh(path, options);
		UAM_ASSERT(result.error.empty());
		UAM_ASSERT(!result.reset);
		UAM_ASSERT(result.bytes_read <= options.byte_budget);
		total_read += result.bytes_read;
		if (result.complete) break;
	}
	UAM_ASSERT(result.complete);
	UAM_ASSERT_EQ(total_read, appended.size());
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT_EQ(scan.Records().front().model_id, std::string("model-\xe2\x98\x83"));
	result = scan.Refresh(path);
	UAM_ASSERT(result.complete && result.bytes_read == 0);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
}

UAM_TEST(UsageScanResetsOnTruncateSameSizeRewriteAndPathChangeAndPreservesCacheOnIoFailure)
{
	TempDir temp("uam-usage-scan-rewrite");
	const fs::path path = temp.root / "session.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(1) + "\n" + UsageLine(2) + "\n"));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	UAM_ASSERT(scan.Refresh(path).complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(3, 13) + "\n"));
	uam::usage::ScanResult result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 13LL);
	const fs::file_time_type prior_time = fs::last_write_time(path);
	UAM_ASSERT(uam::io::WriteTextFile(path, UsageLine(3, 14) + "\n"));
	fs::last_write_time(path, prior_time + std::chrono::seconds(1));
	result = scan.Refresh(path);
	UAM_ASSERT(result.reset && result.complete);
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 14LL);
	const fs::path other = temp.root / "other.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(other, UsageLine(4, 15) + "\n"));
	UAM_ASSERT(scan.Refresh(other).reset);
	UAM_ASSERT_EQ(scan.Records().front().tokens.input, 15LL);
	fs::remove(other);
	result = scan.Refresh(other);
	UAM_ASSERT(!result.error.empty() && !result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
}

UAM_TEST(UsageScanReportsOversizedMalformedAndLimitedHistoryAndSupportsCancellation)
{
	TempDir temp("uam-usage-scan-bounded");
	const fs::path path = temp.root / "session.jsonl";
	const std::string transcript = "not-json\n" + std::string(2048, 'x') + "\n" + UsageLine(1) + "\n" + UsageLine(2) + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, transcript));
	uam::usage::TranscriptScan scan(uam::provider_ids::kClaudeCli);
	uam::usage::ScanOptions options;
	options.byte_budget = 127;
	options.max_line_bytes = 512;
	int cancellation_checks = 0;
	const uam::usage::ScanResult cancelled = scan.Refresh(path, options, [&cancellation_checks]() { return ++cancellation_checks > 1; });
	UAM_ASSERT(cancelled.cancelled && !cancelled.complete);
	UAM_ASSERT_EQ(cancelled.bytes_read, std::size_t(0));
	UAM_ASSERT(scan.Records().empty());
	uam::usage::ScanResult result;
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		result = scan.Refresh(path, options);
		UAM_ASSERT(result.error.empty() && result.bytes_read <= options.byte_budget);
		if (result.complete) break;
	}
	UAM_ASSERT(result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT_EQ(scan.InvalidLines(), std::size_t(1));
	UAM_ASSERT_EQ(scan.OversizedLines(), std::size_t(1));
	uam::usage::TranscriptScan limited(uam::provider_ids::kClaudeCli);
	options.byte_budget = 32768;
	options.max_records = 1;
	result = limited.Refresh(path, options);
	UAM_ASSERT(!result.complete && result.error.find("incomplete") != std::string::npos);
	UAM_ASSERT_EQ(limited.Records().size(), std::size_t(1));
	UAM_ASSERT(!limited.Refresh(path, options).error.empty());
	options.byte_budget = 0;
	UAM_ASSERT(!scan.Refresh(path, options).error.empty());
	uam::usage::TranscriptScan unsupported(uam::provider_ids::kOpenCodeCli);
	UAM_ASSERT(unsupported.Refresh(path).error.find("unavailable") != std::string::npos);
}

UAM_TEST(UsageCopilotScanCachesAppendedAccountingAndAvoidsDoubleCountingChildSessions)
{
	TempDir temp("uam-copilot-usage");
	const fs::path path = temp.root / "session" / "events.jsonl";
	fs::create_directories(path.parent_path());
	nlohmann::json event{{"type", "session.shutdown"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"data", {{"modelMetrics", {{"model", {{"usage", {{"inputTokens", 100}, {"cacheReadTokens", 30}, {"cacheWriteTokens", 0}, {"outputTokens", 20}}}}}}}}}};
	const std::string first = event.dump() + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, first));
	uam::usage::TranscriptScan scan(uam::provider_ids::kCopilotCli);
	UAM_ASSERT(scan.Refresh(path).complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
	UAM_ASSERT_EQ(scan.Records()[0].session_id, std::string("session"));
	UAM_ASSERT_EQ(scan.Refresh(path).bytes_read, std::size_t(0));
	event["data"]["modelMetrics"]["model"]["usage"]["inputTokens"] = 110;
	UAM_ASSERT(uam::io::WriteTextFile(path, first + first + event.dump() + "\n"));
	UAM_ASSERT(scan.Refresh(path).complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(2));
	UAM_ASSERT_EQ(scan.Records()[1].tokens.Total(), 10LL);
	const nlohmann::json header{{"type", "session.start"}, {"data", {{"detachedFromSpawningParentSessionId", "parent"}}}};
	UAM_ASSERT(uam::io::WriteTextFile(path, header.dump() + "\n" + first));
	UAM_ASSERT(scan.Refresh(path).complete);
	UAM_ASSERT(scan.Records().empty());
}

UAM_TEST(UsageOpenCodeDatabaseReadsBothSchemasWithoutWritingOrDuplicateBilling)
{
	TempDir temp("uam-opencode-usage-db");
	const fs::path path = temp.root / "opencode.db";
	sqlite3* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE session(id TEXT PRIMARY KEY,time_created INTEGER); INSERT INTO session VALUES('session',50),('fork',150); CREATE TABLE message(id TEXT, session_id TEXT, time_created INTEGER, data TEXT); CREATE TABLE session_message(id TEXT,session_id TEXT,time_created INTEGER,type TEXT,data TEXT)", nullptr, nullptr, nullptr), SQLITE_OK);
	const nlohmann::json message{{"role", "assistant"}, {"providerID", "anthropic"}, {"modelID", "model"}, {"cost", 0.02},
	    {"tokens", {{"input", 10}, {"output", 5}, {"reasoning", 3}, {"cache", {{"read", 20}, {"write", 7}}}}}};
	const std::string json = message.dump();
	sqlite3_stmt* statement = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO message VALUES('msg','session',100,?1)", -1, &statement, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_bind_text(statement, 1, json.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE); sqlite3_finalize(statement);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "INSERT INTO session_message SELECT id,session_id,time_created,'assistant',data FROM message", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "INSERT INTO message VALUES('broken','session',101,'bad json')", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "INSERT INTO message SELECT 'fork-copy','fork',100,data FROM message WHERE id='msg'; INSERT INTO message SELECT 'fork-new','fork',160,data FROM message WHERE id='msg'", nullptr, nullptr, nullptr), SQLITE_OK);
	database.reset();
	const fs::file_time_type before = fs::last_write_time(path);
	uam::usage::OpenCodeHistoryScan scan;
	uam::usage::ScanResult result = scan.Refresh(path, 0, 200);
	UAM_ASSERT(!result.complete && result.error.empty());
	result = scan.Refresh(path, 0, 200);
	UAM_ASSERT(result.complete && result.error.empty());
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(3));
	UAM_ASSERT_EQ(scan.SuppressedForkRecords(), std::size_t(1));
	UAM_ASSERT_EQ(scan.InvalidRows(), std::size_t(1));
	const std::optional<uam::usage::Summary> summary = uam::usage::Aggregate(scan.Records(), 0, 200);
	UAM_ASSERT(summary);
	UAM_ASSERT_EQ(summary->total.tokens.Total(), 90LL);
	UAM_ASSERT_EQ(summary->duplicates, std::size_t(1));
	UAM_ASSERT_EQ(summary->total.estimated_cost_tokens, 90LL);
	UAM_ASSERT_EQ(summary->total.reported_cost_tokens, 0LL);
	uam::usage::HistoryService service({{uam::provider_ids::kOpenCodeCli, {}, {path}}});
	nlohmann::json reply = service.Refresh("request", 0, 200);
	UAM_ASSERT(!reply["complete"].get<bool>());
	reply = service.Refresh("request", 0, 200);
	UAM_ASSERT(reply["complete"].get<bool>());
	UAM_ASSERT_EQ(reply["totals"]["totalTokens"].get<int>(), 90);
	UAM_ASSERT_EQ(reply["filesTotal"].get<int>(), 1);
	UAM_ASSERT_EQ(reply["totals"]["estimatedCostTokens"].get<int>(), 90);
	UAM_ASSERT_EQ(fs::last_write_time(path), before);
	UAM_ASSERT_EQ(scan.Refresh(path, 0, 200).bytes_read, std::size_t(0));
	UAM_ASSERT(scan.Refresh(path, 200, 300).reset);
	UAM_ASSERT(scan.Records().empty());
	const fs::path absent = temp.root / "missing.db";
	UAM_ASSERT(!scan.Refresh(absent, 0, 200).error.empty());
	UAM_ASSERT(!fs::exists(absent));
}

UAM_TEST(UsageOpenCodeDatabaseBoundsRowsAndPreservesProgressWhenCancelled)
{
	TempDir temp("uam-opencode-usage-pages");
	const fs::path path = temp.root / "opencode.db";
	sqlite3* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "PRAGMA journal_mode=WAL; CREATE TABLE session(id TEXT PRIMARY KEY,time_created INTEGER); INSERT INTO session VALUES('session',50); CREATE TABLE session_message(id TEXT,session_id TEXT,time_created INTEGER,type TEXT,data TEXT)", nullptr, nullptr, nullptr), SQLITE_OK);
	const nlohmann::json message{{"model", {{"id", "model"}, {"providerID", "openai"}}}, {"tokens", {{"input", 10}, {"output", 5}, {"reasoning", 3}, {"cache", {{"read", 20}, {"write", 7}}}}}};
	const std::string json = message.dump();
	sqlite3_stmt* statement = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO session_message VALUES(?1,'session',100,'assistant',?2)", -1, &statement, nullptr), SQLITE_OK);
	for (int index = 0; index < 513; ++index)
	{
		const std::string id = std::to_string(index);
		sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(statement, 2, json.c_str(), -1, SQLITE_TRANSIENT);
		UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE); sqlite3_reset(statement);
	}
	sqlite3_finalize(statement);
	uam::usage::OpenCodeHistoryScan scan;
	UAM_ASSERT(!scan.Refresh(path, 0, 200).complete);
	UAM_ASSERT(scan.Refresh(path, 0, 200, []() { return true; }).cancelled);
	UAM_ASSERT(scan.Records().empty());
	uam::usage::ScanResult result = scan.Refresh(path, 0, 200);
	UAM_ASSERT(result.error.empty() && !result.complete);
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(512));
	result = scan.Refresh(path, 0, 200);
	UAM_ASSERT(result.complete && result.error.empty());
	UAM_ASSERT_EQ(scan.Records().size(), std::size_t(513));
}
