#include "test_harness.h"
#include "common/usage/usage_history_service.h"
#include "common/usage/usage_store.h"
#include <fstream>
#include <sqlite3.h>

using namespace uam_test;

namespace
{
	std::string HistoryLine(const std::string& identity)
	{
		return nlohmann::json({{"type", "assistant"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"sessionId", "session"}, {"requestId", identity},
		    {"message", {{"id", identity}, {"model", "claude-sonnet"}, {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}}}}).dump() + "\n";
	}
}

UAM_TEST(UsageHistoryServiceReturnsBoundedProgressAndCachedProviderModelTimeBreakdowns)
{
	TempDir temp("uam-usage-history-service");
	const fs::path root = temp.root / "projects";
	fs::create_directories(root);
	for (int index = 0; index < 129; ++index)
		UAM_ASSERT(uam::io::WriteTextFile(root / (std::to_string(index) + ".jsonl"), HistoryLine(std::to_string(index))));
	uam::usage::HistoryService service({{uam::provider_ids::kClaudeCli, {root, root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	nlohmann::json result = service.Refresh("request", start, start + 86400000);
	UAM_ASSERT(!result.contains("error"));
	UAM_ASSERT(!result["complete"].get<bool>());
	UAM_ASSERT_EQ(result["filesChecked"].get<int>(), 128);
	UAM_ASSERT_EQ(result["filesTotal"].get<int>(), 129);
	result = service.Refresh("request", start, start + 86400000);
	UAM_ASSERT(result["complete"].get<bool>());
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 129 * 15);
	UAM_ASSERT_EQ(result["totals"]["unpricedTokens"].get<int>(), 129 * 15);
	UAM_ASSERT_EQ(result["totals"]["sessions"].get<int>(), 1);
	UAM_ASSERT_EQ(result["models"].size(), std::size_t(1));
	UAM_ASSERT_EQ(result["days"].size(), std::size_t(1));
	UAM_ASSERT_EQ(result["hours"].size(), std::size_t(1));
	UAM_ASSERT_EQ(result["version"].get<int>(), 2);
	UAM_ASSERT_EQ(result["providerDays"].size(), std::size_t(1));
	UAM_ASSERT_EQ(result["providerHours"].size(), std::size_t(1));
	UAM_ASSERT_EQ(result["providerDays"][0]["providerId"].get<std::string>(), std::string(uam::provider_ids::kClaudeCli));
	UAM_ASSERT_EQ(result["providerDays"][0]["totalTokens"].get<int>(), 129 * 15);
	UAM_ASSERT_EQ(result["providerDays"][0]["sessions"].get<int>(), 1);
	UAM_ASSERT_EQ(result["providers"].size(), std::size_t(6));
	UAM_ASSERT_EQ(service.Refresh("request", start, start + 86400000)["bytesRead"].get<int>(), 0);
	result = service.Refresh("fresh", start, start + 86400000);
	UAM_ASSERT_EQ(result["bytesRead"].get<int>(), 0);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 129 * 15);
}

UAM_TEST(UsageHistoryServiceRefreshesAppendsAndFlagsPendingAndUnavailableSources)
{
	TempDir temp("uam-usage-history-append");
	const fs::path path = temp.root / "session.jsonl";
	const std::string first = HistoryLine("first");
	const std::string second = HistoryLine("second");
	UAM_ASSERT(uam::io::WriteTextFile(path, first + second.substr(0, second.size() - 1)));
	uam::usage::HistoryService service({{uam::provider_ids::kClaudeCli, {temp.root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	nlohmann::json result = service.Refresh("first-refresh", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 15);
	bool found_pending = false;
	for (const nlohmann::json& provider : result["providers"])
	{
		if (provider["providerId"] == uam::provider_ids::kClaudeCli)
		{
			found_pending = true;
			UAM_ASSERT(provider["available"].get<bool>());
			UAM_ASSERT_EQ(provider["pendingLines"].get<int>(), 1);
		}
		else UAM_ASSERT(!provider["available"].get<bool>());
	}
	UAM_ASSERT(found_pending);
	UAM_ASSERT(uam::io::WriteTextFile(path, first + second));
	result = service.Refresh("next-refresh", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 30);
	UAM_ASSERT_EQ(result["bytesRead"].get<int>(), 1);
	UAM_ASSERT(service.Refresh("cancelled", start, start + 86400000, {}, []() { return true; })["cancelled"].get<bool>());
	UAM_ASSERT_EQ(service.Refresh("retry", start, start + 86400000)["totals"]["totalTokens"].get<int>(), 30);
	UAM_ASSERT(service.Refresh("invalid", start, start + 91LL * 86400000).contains("error"));
}

UAM_TEST(UsageHistoryServiceRemovesDeletedFilesWithoutDroppingUnreadableOrCancelledSources)
{
	TempDir temp("uam-usage-history-removals");
	const fs::path root = temp.root / "projects";
	fs::create_directories(root);
	const fs::path first = root / "first.jsonl", second = root / "second.jsonl";
	UAM_ASSERT(uam::io::WriteTextFile(first, HistoryLine("first")));
	UAM_ASSERT(uam::io::WriteTextFile(second, HistoryLine("second")));
	uam::usage::HistoryService service({{uam::provider_ids::kClaudeCli, {root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	UAM_ASSERT_EQ(service.Refresh("initial", start, start + 86400000)["totals"]["totalTokens"].get<int>(), 30);
	UAM_ASSERT(fs::remove(first));
	nlohmann::json result = service.Refresh("removed", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 15);
	UAM_ASSERT_EQ(result["totals"]["records"].get<int>(), 1);
	UAM_ASSERT_EQ(result["filesTotal"].get<int>(), 1);
	UAM_ASSERT_EQ(result["bytesRead"].get<int>(), 0);
	UAM_ASSERT_EQ(result["models"][0]["totalTokens"].get<int>(), 15);
	UAM_ASSERT(fs::remove(second));
	UAM_ASSERT(service.Refresh("cancelled", start, start + 86400000, {}, []() { return true; })["cancelled"].get<bool>());
	UAM_ASSERT(fs::remove(root));
	UAM_ASSERT(uam::io::WriteTextFile(root, "not a directory"));
	result = service.Refresh("unreadable", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 15);
	bool found_error = false;
	for (const nlohmann::json& provider : result["providers"])
		if (provider["providerId"] == uam::provider_ids::kClaudeCli)
		{
			found_error = true;
			UAM_ASSERT(provider["errors"].get<int>() > 0);
			UAM_ASSERT_EQ(provider["files"].get<int>(), 1);
		}
	UAM_ASSERT(found_error);
	UAM_ASSERT(fs::remove(root));
	fs::create_directories(root);
	result = service.Refresh("recovered", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 0);
	UAM_ASSERT_EQ(result["totals"]["sessions"].get<int>(), 0);
	UAM_ASSERT(result["models"].empty());
	UAM_ASSERT(result["days"].empty());
}

UAM_TEST(UsageHistoryRequestsValidateScopeWindowAndPricesWithoutMutatingRejectedState)
{
	uam::usage::HistoryRequest request;
	std::string error;
	nlohmann::json payload{{"refreshId", "refresh"}, {"startMs", 0}, {"endMs", 86400000}, {"executionHostId", "local"},
	    {"priceOverrides", nlohmann::json::array({{{"providerId", "codex-cli"}, {"modelId", "model"}, {"speed", "fast"}, {"inputPerMillion", 2}, {"outputPerMillion", 4}}})}};
	UAM_ASSERT(uam::usage::ParseHistoryRequest(payload, request, error));
	UAM_ASSERT_EQ(request.prices.size(), std::size_t(1));
	UAM_ASSERT(!request.prices.begin()->second.cached_input_per_million);
	for (const nlohmann::json bad_host : {nlohmann::json(123), nlohmann::json(false), nlohmann::json(nullptr)})
	{
		nlohmann::json invalid = payload; invalid["executionHostId"] = bad_host;
		UAM_ASSERT(!uam::usage::ParseHistoryRequest(invalid, request, error));
		UAM_ASSERT_EQ(request.refresh_id, std::string("refresh"));
	}
	for (const nlohmann::json bad_epoch : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("0"), nlohmann::json(9007199254740992ULL)})
	{
		nlohmann::json invalid = payload; invalid["startMs"] = bad_epoch;
		UAM_ASSERT(!uam::usage::ParseHistoryRequest(invalid, request, error));
	}
	for (const nlohmann::json bad_price : {nlohmann::json(-1), nlohmann::json("2"), nlohmann::json(false)})
	{
		nlohmann::json invalid = payload; invalid["priceOverrides"][0]["inputPerMillion"] = bad_price;
		UAM_ASSERT(!uam::usage::ParseHistoryRequest(invalid, request, error));
	}
	payload["priceOverrides"].push_back(payload["priceOverrides"][0]);
	UAM_ASSERT(!uam::usage::ParseHistoryRequest(payload, request, error));
	UAM_ASSERT_EQ(request.prices.size(), std::size_t(1));
}

UAM_TEST(UsageAggregationReferencesMatchOwnedRecordsAndRejectNullPointers)
{
	const uam::usage::Record record = *uam::usage::ClaudeRecord(nlohmann::json::parse(HistoryLine("record")));
	const std::optional<uam::usage::Summary> owned = uam::usage::Aggregate({record}, record.timestamp_ms, record.timestamp_ms + 1);
	const std::optional<uam::usage::Summary> references = uam::usage::AggregateReferences({&record}, record.timestamp_ms, record.timestamp_ms + 1);
	UAM_ASSERT(owned && references);
	UAM_ASSERT_EQ(owned->total.tokens.Total(), references->total.tokens.Total());
	UAM_ASSERT_EQ(owned->total.unpriced_tokens, references->total.unpriced_tokens);
	UAM_ASSERT_EQ(owned->models.size(), references->models.size());
	UAM_ASSERT(!uam::usage::AggregateReferences({nullptr}, 0, 1));
}

UAM_TEST(UsageGeminiHistoryReadsLegacySnapshotsAndJsonlWithoutDuplicateBilling)
{
	TempDir temp("uam-usage-gemini");
	const fs::path chats = temp.root / "project" / "chats";
	fs::create_directories(chats);
	const nlohmann::json message{{"type", "gemini"}, {"id", "reply"}, {"model", "gemini-test"}, {"timestamp", "2026-10-10T12:00:00Z"},
	    {"tokens", {{"input", 100}, {"cached", 40}, {"output", 20}, {"thoughts", 10}, {"total", 130}}}};
	nlohmann::json snapshot{{"sessionId", "session"}, {"messages", nlohmann::json::array({message})}};
	const fs::path path = chats / "session-old.json";
	UAM_ASSERT(uam::io::WriteTextFile(path, snapshot.dump()));
	UAM_ASSERT(uam::io::WriteTextFile(chats / "session-new.jsonl", nlohmann::json({{"sessionId", "session"}}).dump() + "\n" + message.dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(chats / "settings.json", snapshot.dump()));
	uam::usage::HistoryService service({{uam::provider_ids::kGeminiCli, {temp.root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	nlohmann::json result = service.Refresh("first", start, start + 86400000);
	UAM_ASSERT(result["complete"].get<bool>());
	UAM_ASSERT_EQ(result["filesTotal"].get<int>(), 2);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 130);
	UAM_ASSERT_EQ(result["duplicates"].get<int>(), 1);
	UAM_ASSERT_EQ(service.Refresh("cached", start, start + 86400000)["bytesRead"].get<int>(), 0);
	nlohmann::json next = message; next["id"] = "next";
	snapshot["messages"].push_back(next);
	UAM_ASSERT(uam::io::WriteTextFile(path, snapshot.dump()));
	result = service.Refresh("changed", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 260);
	UAM_ASSERT_EQ(result["totals"]["sessions"].get<int>(), 1);
	fs::create_directories(chats / "parent");
	snapshot["sessionId"] = "child";
	snapshot["messages"] = nlohmann::json::array({next});
	UAM_ASSERT(uam::io::WriteTextFile(chats / "parent" / "child.json", snapshot.dump()));
	result = service.Refresh("child", start, start + 86400000);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 390);
	UAM_ASSERT_EQ(result["totals"]["sessions"].get<int>(), 2);
}

UAM_TEST(UsageGeminiSnapshotResumesBoundedReadsAndReportsMalformedRewrites)
{
	TempDir temp("uam-usage-gemini-bounded");
	const fs::path path = temp.root / "session.json";
	const nlohmann::json snapshot{{"sessionId", "session"}, {"messages", nlohmann::json::array({{{"type", "gemini"}, {"id", "reply"}, {"model", "gemini-test"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"tokens", {{"input", 10}, {"output", 2}, {"total", 12}}}}})}};
	UAM_ASSERT(uam::io::WriteTextFile(path, snapshot.dump()));
	uam::usage::TranscriptScan scan(uam::provider_ids::kGeminiCli);
	uam::usage::ScanOptions options; options.byte_budget = 32;
	uam::usage::ScanResult result = scan.Refresh(path, options);
	UAM_ASSERT(!result.complete); UAM_ASSERT_EQ(result.bytes_read, std::size_t(32));
	UAM_ASSERT(scan.Records().empty());
	for (int index = 0; index < 100 && !result.complete; ++index) result = scan.Refresh(path, options);
	UAM_ASSERT(result.complete); UAM_ASSERT_EQ(scan.Records().size(), std::size_t(1));
	UAM_ASSERT_EQ(scan.Refresh(path, options).bytes_read, std::size_t(0));
	UAM_ASSERT(uam::io::WriteTextFile(path, "{broken"));
	result = scan.Refresh(path, options);
	UAM_ASSERT(result.reset); UAM_ASSERT(!result.error.empty()); UAM_ASSERT(scan.Records().empty());
	UAM_ASSERT_EQ(scan.InvalidLines(), std::size_t(1));
	UAM_ASSERT(scan.Refresh(path, options, [] { return true; }).cancelled);
	nlohmann::json large = snapshot; large["padding"] = std::string(50000, 'x');
	UAM_ASSERT(uam::io::WriteTextFile(path, large.dump()));
	options.byte_budget = 65536;
	int checks = 0;
	result = scan.Refresh(path, options, [&]
	{
		if (++checks == 3)
		{
			large["sessionId"] = "changed";
			UAM_ASSERT(uam::io::WriteTextFile(path, large.dump()));
			fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(1));
		}
		return false;
	});
	UAM_ASSERT(result.error.find("changed during scanning") != std::string::npos);
	UAM_ASSERT(scan.Records().empty());
}

UAM_TEST(UsageCopilotHistoryServiceReadsOnlyAccountingEventLogs)
{
	TempDir temp("uam-copilot-usage-service");
	const fs::path root = temp.root / "session-state";
	fs::create_directories(root / "session");
	const nlohmann::json event{{"type", "session.shutdown"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"data", {{"modelMetrics", {{"model", {{"usage", {{"inputTokens", 100}, {"cacheReadTokens", 30}, {"cacheWriteTokens", 10}, {"outputTokens", 20}}}}}}}}}};
	UAM_ASSERT(uam::io::WriteTextFile(root / "session" / "events.jsonl", event.dump() + "\n" + event.dump() + "\n"));
	UAM_ASSERT(uam::io::WriteTextFile(root / "session" / "unrelated.jsonl", event.dump() + "\n"));
	uam::usage::HistoryService service({{uam::provider_ids::kCopilotCli, {root}}});
	const std::int64_t start = *uam::usage::TimestampMilliseconds("2026-10-10T00:00:00Z");
	const nlohmann::json result = service.Refresh("request", start, start + 86400000);
	UAM_ASSERT(!result.contains("error"));
	UAM_ASSERT_EQ(result["filesTotal"].get<int>(), 1);
	UAM_ASSERT_EQ(result["totals"]["totalTokens"].get<int>(), 120);
	UAM_ASSERT_EQ(result["totals"]["cachedInputTokens"].get<int>(), 30);
	UAM_ASSERT_EQ(result["totals"]["unpricedTokens"].get<int>(), 120);
	UAM_ASSERT_EQ(service.Refresh("fresh", start, start + 86400000)["bytesRead"].get<int>(), 0);
}

UAM_TEST(UsageLifetimeRequestRequiresExplicitBeginningAndPreservesRollingBounds)
{
	uam::usage::HistoryRequest request;
	std::string error;
	nlohmann::json payload{{"refreshId", "lifetime"}, {"executionHostId", "local"}, {"startMs", 0}, {"endMs", 180LL * 86400000}, {"windowDays", 0}};
	UAM_ASSERT(uam::usage::ParseHistoryRequest(payload, request, error));
	UAM_ASSERT_EQ(request.start_ms, 0LL);
	for (const int mutation : {0, 1, 2, 3})
	{
		nlohmann::json invalid = payload;
		if (mutation == 0) invalid.erase("windowDays");
		if (mutation == 1) invalid["windowDays"] = 90;
		if (mutation == 2) { invalid["startMs"] = 86400000; invalid["endMs"] = 2LL * 86400000; }
		if (mutation == 3) invalid["endMs"] = uam::usage::kMaxExactTokenCount + 1;
		UAM_ASSERT(!uam::usage::ParseHistoryRequest(invalid, request, error));
		UAM_ASSERT_EQ(request.refresh_id, std::string("lifetime"));
	}
}

UAM_TEST(UsageHistoryStreamingDrainsBatchesWithoutReusingUnidentifiedSourceOrdinals)
{
	TempDir temp("uam-history-streaming"); const fs::path transcript = temp.root / "session.jsonl";
	std::ofstream output(transcript, std::ios::binary);
	for (int index = 0; index < 9001; ++index) output << HistoryLine(""); output.close();
	uam::usage::HistoryService history({{uam::provider_ids::kClaudeCli, {temp.root}}});
	uam::usage::Store store(temp.root / "accounting");
	std::size_t emitted = 0, expected_base = 0;
	const uam::usage::StreamingAccountingSink sink = [&](const std::string& source, const std::vector<uam::usage::Record>& records, std::size_t position, std::size_t base, std::string& error)
	{
		UAM_ASSERT_EQ(position, std::size_t{0}); UAM_ASSERT_EQ(base, expected_base);
		std::vector<uam::usage::SourceRecord> batch;
		for (std::size_t index = position; index < records.size(); ++index)
		{
			batch.push_back({&records[index], source, base + index});
			if (batch.size() == 2048) { UAM_ASSERT(store.Import(batch, error)); batch.clear(); }
		}
		if (!batch.empty() && !store.Import(batch, error)) return false;
		emitted += records.size(); expected_base += records.size(); return true;
	};
	const std::int64_t end = *uam::usage::TimestampMilliseconds("2026-10-12T12:00:00Z"); nlohmann::json progress;
	for (int batch = 0; batch < 30; ++batch) { progress = history.CollectStream("first", 0, end, sink); UAM_ASSERT(!progress.contains("error")); if (progress.value("complete", false)) break; }
	UAM_ASSERT(progress.value("complete", false)); UAM_ASSERT_EQ(emitted, std::size_t{9001});
	UAM_ASSERT(history.CollectStream("unchanged", 0, end, sink).value("complete", false)); UAM_ASSERT_EQ(emitted, std::size_t{9001});
	std::string error; const std::optional<uam::usage::Ledger> ledger = store.ReadLifetimeLedger(end, error); UAM_ASSERT(ledger);
	UAM_ASSERT_EQ(ledger->Query(0, end)->total.records, std::size_t{9001});
}

UAM_TEST(UsageGeminiStreamingProjectsLargeSnapshotsAndSessionIdentityAfterMessages)
{
	TempDir temp("uam-gemini-streaming"); const fs::path chats = temp.root / "chats"; fs::create_directories(chats);
	const fs::path transcript = chats / "session-large.json";
	std::ofstream output(transcript, std::ios::binary); output << "{\"messages\":[";
	for (int index = 0; index < 5000; ++index)
	{
		if (index) output << ',';
		output << nlohmann::json({{"type", "gemini"}, {"id", std::to_string(index)}, {"model", "gemini-test"}, {"timestamp", "2026-10-10T12:00:00Z"}, {"content", std::string(8192, 'c')}, {"tokens", {{"input", 10}, {"cached", 2}, {"output", 3}, {"thoughts", 1}, {"tool", 0}, {"total", 14}}}}).dump();
	}
	output << "],\"sessionId\":\"late-session\"}"; output.close(); UAM_ASSERT(fs::file_size(transcript) > 32 * 1024 * 1024);
	uam::usage::HistoryService history({{uam::provider_ids::kGeminiCli, {temp.root}}}); std::size_t count = 0; std::int64_t tokens = 0;
	const uam::usage::StreamingAccountingSink sink = [&](const std::string&, const std::vector<uam::usage::Record>& records, std::size_t, std::size_t, std::string&)
	{
		UAM_ASSERT(records.size() <= 2048);
		for (const uam::usage::Record& record : records) { UAM_ASSERT_EQ(record.session_id, std::string("late-session")); tokens += record.tokens.Total(); ++count; }
		return true;
	};
	const std::int64_t end = *uam::usage::TimestampMilliseconds("2026-10-12T12:00:00Z");
	const nlohmann::json progress = history.CollectStream("large", 0, end, sink); UAM_ASSERT(!progress.contains("error")); UAM_ASSERT(progress.value("complete", false)); UAM_ASSERT_EQ(count, std::size_t{5000}); UAM_ASSERT_EQ(tokens, 70000LL);
	UAM_ASSERT(history.CollectStream("unchanged", 0, end, sink).value("complete", false)); UAM_ASSERT_EQ(count, std::size_t{5000});
}


UAM_TEST(UsageOpenCodeStreamingRetainsFutureReceiptsAcrossUnchangedDatabaseRefreshes)
{
	TempDir temp("uam-opencode-streamed-future"); const fs::path path = temp.root / "opencode.db";
	sqlite3* raw = nullptr; UAM_ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE session(id TEXT PRIMARY KEY,time_created INTEGER); INSERT INTO session VALUES('session',1000); CREATE TABLE session_message(id TEXT,session_id TEXT,time_created INTEGER,type TEXT,data TEXT)", nullptr, nullptr, nullptr), SQLITE_OK);
	const std::string message = nlohmann::json({{"role", "assistant"}, {"providerID", "native"}, {"modelID", "model"}, {"tokens", {{"input", 7}, {"output", 3}, {"reasoning", 0}, {"cache", {{"read", 0}, {"write", 0}}}}}}).dump();
	sqlite3_stmt* statement = nullptr; UAM_ASSERT_EQ(sqlite3_prepare_v2(raw, "INSERT INTO session_message VALUES('message','session',3000,'assistant',?1)", -1, &statement, nullptr), SQLITE_OK);
	sqlite3_bind_text(statement, 1, message.c_str(), -1, SQLITE_TRANSIENT); UAM_ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE); sqlite3_finalize(statement); sqlite3_close(raw);
	uam::usage::HistoryService history({{"opencode-cli", {}, {path}}}); uam::usage::Store store(temp.root / "accounting"); std::string error;
	const uam::usage::StreamingAccountingSink sink = [&](const std::string& source, const std::vector<uam::usage::Record>& records, std::size_t position, std::size_t base, std::string& import_error)
	{
		std::vector<uam::usage::SourceRecord> batch; for (std::size_t index = position; index < records.size(); ++index) batch.push_back({&records[index], source, base + index});
		return store.Import(batch, import_error);
	};
	nlohmann::json progress;
	for (int batch = 0; batch < 8; ++batch) { progress = history.CollectStream("before", 0, 2000, sink); UAM_ASSERT(!progress.contains("error")); if (progress.value("complete", false)) break; }
	UAM_ASSERT(progress.value("complete", false)); UAM_ASSERT_EQ(store.ReadLifetimeLedger(2000, error)->Query(0, 2000)->total.tokens.Total(), 0LL);
	progress = history.CollectStream("after", 0, 4000, sink); UAM_ASSERT(progress.value("complete", false)); UAM_ASSERT_EQ(progress["bytesRead"].get<std::size_t>(), std::size_t{0});
	UAM_ASSERT_EQ(store.ReadLifetimeLedger(4000, error)->Query(0, 4000)->total.tokens.Total(), 10LL);
}
