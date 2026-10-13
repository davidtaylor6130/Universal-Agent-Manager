#include "test_harness.h"
#include "common/usage/usage_store.h"
#include "common/usage/usage_history_service.h"
#include "common/usage/antigravity_accounting.h"
#include <sqlite3.h>
#include <unordered_set>

using namespace uam_test;

namespace
{
	const std::int64_t kStoreAsOf = *uam::usage::TimestampMilliseconds("2026-10-10T12:00:00Z");

	uam::usage::Record StoreEvent(const std::string& identity)
	{
		uam::usage::Record record;
		record.provider_id = uam::provider_ids::kOpenCodeCli; record.model_id = "qwen3.8-27b"; record.session_id = "session";
		record.dedupe_key = identity; record.timestamp_ms = kStoreAsOf - 3LL * 86400000;
		record.tokens = {10, 5, 0, 20, 7}; record.estimated_cost_usd = 0.01;
		return record;
	}
}

UAM_TEST(UsageStorePersistsIdempotentAccountingAndUpdatesMutableMessagesWithoutForkBilling)
{
	TempDir temp("uam-usage-store");
	uam::usage::Store store(temp.root);
	std::string error;
	uam::usage::Record original = StoreEvent("message");
	UAM_ASSERT(store.Import({{&original, "source", 0}}, error));
	UAM_ASSERT(store.Import({{&original, "source", 0}}, error));
	const std::optional<uam::usage::Ledger> restored = uam::usage::Store(temp.root).Read(kStoreAsOf, error);
	UAM_ASSERT(restored); UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(restored->Query(restored->WindowStart(7), kStoreAsOf)->total.records, std::size_t(1));
	UAM_ASSERT_EQ(restored->Query(restored->WindowStart(7), kStoreAsOf)->total.tokens.Total(), 35LL);
	original.tokens.output = 40; original.estimated_cost_usd = 0.02;
	UAM_ASSERT(store.Import({{&original, "source", 0}}, error));
	uam::usage::Record copied = original; copied.timestamp_ms += 86400000; copied.tokens.output = 999;
	UAM_ASSERT(store.Import({{&copied, "fork", 0}}, error));
	const std::optional<uam::usage::Ledger> updated = store.Read(kStoreAsOf, error);
	UAM_ASSERT(updated);
	const std::optional<uam::usage::Summary> totals = updated->Query(updated->WindowStart(7), kStoreAsOf);
	UAM_ASSERT(totals); UAM_ASSERT_EQ(totals->total.records, std::size_t(1)); UAM_ASSERT_EQ(totals->total.tokens.Total(), 55LL);
	UAM_ASSERT_EQ(totals->total.estimated_cost_usd, 0.02);
	// An empty source scan, including removed provider transcripts, retains already recorded accounting.
	UAM_ASSERT(store.Import({}, error));
	UAM_ASSERT_EQ(store.Read(kStoreAsOf, error)->Query(updated->WindowStart(7), kStoreAsOf)->total.tokens.Total(), 55LL);
}

UAM_TEST(UsageStoreKeepsUnidentifiedOccurrencesAndProviderScopedIdentitiesSeparate)
{
	TempDir temp("uam-usage-store-identities");
	uam::usage::Store store(temp.root); std::string error;
	uam::usage::Record first = StoreEvent("shared"), second = first, unidentified = first;
	second.provider_id = uam::provider_ids::kClaudeCli; unidentified.dedupe_key.clear();
	const std::vector<uam::usage::SourceRecord> entries{{&first, "file", 0}, {&second, "file", 0}, {&unidentified, "file", 1}, {&unidentified, "file", 2}};
	UAM_ASSERT(store.Import(entries, error)); UAM_ASSERT(store.Import(entries, error));
	const std::optional<uam::usage::Ledger> ledger = store.Read(kStoreAsOf, error);
	UAM_ASSERT(ledger);
	const std::optional<uam::usage::Summary> summary = ledger->Query(ledger->WindowStart(7), kStoreAsOf);
	UAM_ASSERT(summary); UAM_ASSERT_EQ(summary->total.records, std::size_t(4));
	UAM_ASSERT_EQ(summary->total.tokens.Total(), 140LL); UAM_ASSERT_EQ(summary->unidentified_records, std::size_t(2));
	UAM_ASSERT_EQ(summary->providers.size(), std::size_t(2));
}

UAM_TEST(UsageStoreRejectsInvalidBatchesAndRollsBackCancellationWithoutErasingRecordedCosts)
{
	TempDir temp("uam-usage-store-cancel");
	uam::usage::Store store(temp.root); std::string error;
	const uam::usage::Record original = StoreEvent("recorded"), next = StoreEvent("next");
	UAM_ASSERT(store.Import({{&original, "file", 0}}, error));
	int cancellation_checks = 0;
	UAM_ASSERT(!store.Import({{&next, "file", 1}, {&next, "file", 1}}, error, [&]() { return ++cancellation_checks >= 3; }));
	UAM_ASSERT(!store.Import({{nullptr, "file", 0}}, error));
	UAM_ASSERT(!store.Import(std::vector<uam::usage::SourceRecord>(2049, {&next, "file", 1}), error));
	UAM_ASSERT(!uam::usage::Store({}).Read(kStoreAsOf, error));
	const std::optional<uam::usage::Ledger> ledger = store.Read(kStoreAsOf, error);
	UAM_ASSERT(ledger); UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(7), kStoreAsOf)->total.records, std::size_t(1));
	UAM_ASSERT(!store.Read(kStoreAsOf, error, [] { return true; }));
}

UAM_TEST(UsageStorePrunesOutsideTheCalendarWindowAndRejectsUnsupportedOrCorruptStorage)
{
	TempDir temp("uam-usage-store-retention");
	uam::usage::Store store(temp.root); std::string error;
	uam::usage::Record old = StoreEvent("old"), recent = StoreEvent("recent");
	old.timestamp_ms = kStoreAsOf - 91LL * 86400000;
	UAM_ASSERT(store.Import({{&old, "file", 0}, {&recent, "file", 1}}, error));
	const std::optional<uam::usage::Ledger> ledger = store.Read(kStoreAsOf, error);
	UAM_ASSERT(ledger); UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(90), kStoreAsOf)->total.records, std::size_t(1));
	sqlite3* database = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &database), SQLITE_OK);
	sqlite3_stmt* count = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM facts", -1, &count, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_step(count), SQLITE_ROW);
	UAM_ASSERT_EQ(sqlite3_column_int(count, 0), 1);
	UAM_ASSERT_EQ(sqlite3_finalize(count), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(database, "UPDATE facts SET data='[1,2]'", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
	UAM_ASSERT(!store.Read(kStoreAsOf, error));
	UAM_ASSERT(error.find("corrupt") != std::string::npos);
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &database), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(database, "PRAGMA user_version=99", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
	UAM_ASSERT(!store.Read(kStoreAsOf, error));
	UAM_ASSERT(error.find("unsupported") != std::string::npos);
}

UAM_TEST(UsageStoreArchivesOldCanonicalReceiptsBeforeRetentionPruning)
{
	TempDir temp("uam-usage-archive-retention");
	uam::usage::Store store(temp.root);
	std::string error;
	uam::usage::Record old = StoreEvent("archived-native"), recent = StoreEvent("recent-native");
	old.timestamp_ms = kStoreAsOf - 180LL * 86400000;
	UAM_ASSERT(store.Import({{&old, "private-old-source", 0}, {&recent, "private-recent-source", 0}}, error));
	UAM_ASSERT(store.Read(kStoreAsOf, error));
	sqlite3* database = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &database), SQLITE_OK);
	struct Cleanup { sqlite3* database; ~Cleanup() { sqlite3_close(database); } } cleanup{database};
	sqlite3_stmt* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(database, "SELECT identity,created,data FROM archived_facts", -1, &raw, nullptr), SQLITE_OK);
	std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(raw, sqlite3_finalize);
	UAM_ASSERT_EQ(sqlite3_step(row.get()), SQLITE_ROW);
	UAM_ASSERT_EQ(sqlite3_column_int64(row.get(), 1), old.timestamp_ms);
	const std::string identity(reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 0)));
	const nlohmann::json data = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 2)));
	const std::optional<uam::usage::AccountingRecord> archived = uam::usage::DecodeAccountingRecord(nlohmann::json::array({identity, old.timestamp_ms, data}));
	UAM_ASSERT(archived.has_value());
	UAM_ASSERT_EQ(archived->record.dedupe_key, old.dedupe_key);
	UAM_ASSERT_EQ(archived->record.tokens.output, old.tokens.output);
	UAM_ASSERT_EQ(sqlite3_step(row.get()), SQLITE_DONE);
}

UAM_TEST(UsageArchiveMigrationPreservesMutableIdentityWithoutRecentCopyBilling)
{
	TempDir temp("uam-usage-archive-migration");
	uam::usage::Store store(temp.root);
	std::string error;
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(store.Path().parent_path()));
	sqlite3* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &raw), SQLITE_OK);
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
	UAM_ASSERT_EQ(sqlite3_exec(database.get(), "CREATE TABLE facts(identity TEXT PRIMARY KEY,created INTEGER NOT NULL,data TEXT NOT NULL) WITHOUT ROWID; CREATE INDEX facts_time ON facts(created); PRAGMA user_version=1", nullptr, nullptr, nullptr), SQLITE_OK);
	uam::usage::Record old = StoreEvent("migration-native");
	old.timestamp_ms = kStoreAsOf - 180LL * 86400000;
	UAM_ASSERT(store.Import({{&old, "source", 0}}, error));
	UAM_ASSERT(store.Read(kStoreAsOf, error));
	old.tokens.output = 45;
	old.estimated_cost_usd = 0.025;
	UAM_ASSERT(uam::usage::Store(temp.root).Import({{&old, "source", 0}}, error));
	uam::usage::Record copy = old;
	copy.timestamp_ms = kStoreAsOf - 86400000;
	copy.tokens.output = 999;
	UAM_ASSERT(store.Import({{&copy, "fork", 0}}, error));
	const std::optional<uam::usage::Ledger> recent = store.Read(kStoreAsOf, error);
	UAM_ASSERT(recent);
	UAM_ASSERT_EQ(recent->Query(recent->WindowStart(90), kStoreAsOf)->total.records, std::size_t{0});
	sqlite3_stmt* statement_raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), "SELECT created,data,(SELECT COUNT(*) FROM facts),(SELECT user_version FROM pragma_user_version) FROM archived_facts", -1, &statement_raw, nullptr), SQLITE_OK);
	std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(statement_raw, sqlite3_finalize);
	UAM_ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
	UAM_ASSERT_EQ(sqlite3_column_int64(statement.get(), 0), old.timestamp_ms);
	const nlohmann::json data = nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1)));
	UAM_ASSERT_EQ(data[8].get<int>(), 45);
	UAM_ASSERT_EQ(data[11].get<double>(), 0.025);
	UAM_ASSERT_EQ(sqlite3_column_int(statement.get(), 2), 0);
	UAM_ASSERT_EQ(sqlite3_column_int(statement.get(), 3), 2);
	UAM_ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_DONE);
}

UAM_TEST(UsageArchiveCancellationRollsBackCopyAndPruneTogether)
{
	TempDir temp("uam-usage-archive-cancel");
	uam::usage::Store store(temp.root);
	std::string error;
	uam::usage::Record old = StoreEvent("cancelled-archive");
	old.timestamp_ms = kStoreAsOf - 180LL * 86400000;
	UAM_ASSERT(store.Import({{&old, "source", 0}}, error));
	int checks = 0;
	UAM_ASSERT(!store.ReadRecords(kStoreAsOf, error, [&]() { return ++checks >= 2; }));
	UAM_ASSERT_EQ(checks, 2);
	sqlite3* raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &raw), SQLITE_OK);
	std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
	sqlite3_stmt* statement_raw = nullptr;
	UAM_ASSERT_EQ(sqlite3_prepare_v2(database.get(), "SELECT (SELECT COUNT(*) FROM facts),(SELECT COUNT(*) FROM archived_facts)", -1, &statement_raw, nullptr), SQLITE_OK);
	std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(statement_raw, sqlite3_finalize);
	UAM_ASSERT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
	UAM_ASSERT_EQ(sqlite3_column_int(statement.get(), 0), 1);
	UAM_ASSERT_EQ(sqlite3_column_int(statement.get(), 1), 0);
	statement.reset();
	UAM_ASSERT(store.ReadRecords(kStoreAsOf, error));
}

UAM_TEST(UsageStoreBusyWriterDoesNotLoseAccountingAndRecoversAfterRelease)
{
	TempDir temp("uam-usage-store-busy");
	uam::usage::Store store(temp.root); std::string error;
	const uam::usage::Record original = StoreEvent("original"), next = StoreEvent("next");
	UAM_ASSERT(store.Import({{&original, "file", 0}}, error));
	sqlite3* database = nullptr;
	UAM_ASSERT_EQ(sqlite3_open(store.Path().string().c_str(), &database), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT(!store.Import({{&next, "file", 1}}, error));
	UAM_ASSERT(error.find("busy") != std::string::npos);
	UAM_ASSERT_EQ(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
	UAM_ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
	const std::optional<uam::usage::Ledger> retained = store.Read(kStoreAsOf, error);
	UAM_ASSERT(retained);
	UAM_ASSERT_EQ(retained->Query(retained->WindowStart(7), kStoreAsOf)->total.records, std::size_t(1));
	UAM_ASSERT(store.Import({{&next, "file", 1}}, error));
	const std::optional<uam::usage::Ledger> recovered = store.Read(kStoreAsOf, error);
	UAM_ASSERT(recovered);
	UAM_ASSERT_EQ(recovered->Query(recovered->WindowStart(7), kStoreAsOf)->total.records, std::size_t(2));
}

UAM_TEST(UsageStoreMeasuresPersistentIndexAndCompactLedgerForThirtyDays)
{
	TempDir temp("uam-usage-store-size");
	uam::usage::Store store(temp.root); std::string error;
	std::vector<uam::usage::Record> records;
	records.reserve(15000);
	for (std::size_t index = 0; index < 15000; ++index)
	{
		uam::usage::Record record = StoreEvent("message-" + std::to_string(index));
		record.timestamp_ms = kStoreAsOf - 30LL * 86400000 + static_cast<std::int64_t>(index) * 172800;
		records.push_back(std::move(record));
	}
	std::vector<uam::usage::SourceRecord> batch;
	for (std::size_t index = 0; index < records.size(); ++index)
	{
		batch.push_back({&records[index], "file", index});
		if (batch.size() == 2048 || index + 1 == records.size())
		{
			UAM_ASSERT(store.Import(batch, error));
			batch.clear();
		}
	}
	const std::optional<uam::usage::Ledger> ledger = store.Read(kStoreAsOf, error);
	UAM_ASSERT(ledger);
	UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(90), kStoreAsOf)->total.records, records.size());
	const std::uintmax_t index_bytes = std::filesystem::file_size(store.Path());
	const std::size_t ledger_bytes = ledger->Encode().dump().size();
	std::cout << "[usage-store-size] events=" << records.size() << " indexBytes=" << index_bytes << " ledgerBytes=" << ledger_bytes << '\n';
	UAM_ASSERT(index_bytes < 8 * 1024 * 1024);
	UAM_ASSERT(ledger_bytes < 128 * 1024);
}

UAM_TEST(UsageStoreAccountingExportPreservesNativeAndUnidentifiedIdentityAcrossRestart)
{
	TempDir temp("uam-usage-accounting-records");
	uam::usage::Store store(temp.root);
	std::string error;
	uam::usage::Record native = StoreEvent("native-receipt"), unidentified = native;
	unidentified.dedupe_key.clear();
	UAM_ASSERT(store.Import({{&native, "provider-file", 0}, {&unidentified, "provider-file", 1}, {&unidentified, "provider-file", 2}}, error));
	const std::optional<std::vector<uam::usage::AccountingRecord>> records = uam::usage::Store(temp.root).ReadRecords(kStoreAsOf, error);
	UAM_ASSERT(records); UAM_ASSERT_EQ(records->size(), std::size_t{3});
	std::unordered_set<std::string> identities;
	for (const uam::usage::AccountingRecord& fact : *records)
	{
		const nlohmann::json wire = uam::usage::EncodeAccountingRecord(fact);
		const std::optional<uam::usage::AccountingRecord> decoded = uam::usage::DecodeAccountingRecord(wire);
		UAM_ASSERT(decoded); UAM_ASSERT_EQ(decoded->identity, fact.identity);
		UAM_ASSERT_EQ(decoded->record.tokens.Total(), 35LL);
		UAM_ASSERT_EQ(decoded->record.dedupe_key, fact.record.dedupe_key);
		UAM_ASSERT(identities.insert(decoded->identity).second);
	}
}

UAM_TEST(UsageAccountingCodecRejectsMalformedIdentitiesCountsAndCosts)
{
	const uam::usage::Record original = StoreEvent("native");
	const nlohmann::json valid = uam::usage::EncodeAccountingRecord({nlohmann::json::array({"native", original.provider_id, original.dedupe_key}).dump(), original});
	UAM_ASSERT(uam::usage::DecodeAccountingRecord(valid));
	for (int mutation = 0; mutation < 7; ++mutation)
	{
		nlohmann::json row = valid;
		if (mutation == 0) row[0] = "[1,2]";
		if (mutation == 1) row[1] = -1;
		if (mutation == 2) row[2][0] = "unknown-provider";
		if (mutation == 3) row[2][5] = -1;
		if (mutation == 4) row[2][9] = 999;
		if (mutation == 5) row[2][10] = "unpriced";
		if (mutation == 6) row[0] = nlohmann::json::array({"native", original.provider_id, ""}).dump();
		UAM_ASSERT(!uam::usage::DecodeAccountingRecord(row));
	}
}

UAM_TEST(UsageLifetimeReadCombinesArchiveAndRecentReceiptsAcrossRestart)
{
	TempDir temp("uam-usage-lifetime-read");
	uam::usage::Store store(temp.root);
	std::string error;
	uam::usage::Record old = StoreEvent("lifetime-old"), recent = StoreEvent("lifetime-recent"), future = StoreEvent("lifetime-future");
	old.timestamp_ms = kStoreAsOf - 180LL * 86400000;
	future.timestamp_ms = kStoreAsOf + 86400000;
	UAM_ASSERT(store.Import({{&old, "source", 0}, {&recent, "source", 1}, {&future, "source", 2}}, error));
	UAM_ASSERT_EQ(store.ReadRecords(kStoreAsOf, error)->size(), std::size_t{1});
	const std::optional<std::vector<uam::usage::AccountingRecord>> lifetime = uam::usage::Store(temp.root).ReadLifetimeRecords(kStoreAsOf, error);
	UAM_ASSERT(lifetime);
	UAM_ASSERT_EQ(lifetime->size(), std::size_t{2});
	UAM_ASSERT_EQ((*lifetime)[0].record.dedupe_key, old.dedupe_key);
	UAM_ASSERT_EQ((*lifetime)[1].record.dedupe_key, recent.dedupe_key);
	UAM_ASSERT(!store.ReadLifetimeRecords(kStoreAsOf, error, [] { return true; }));
	UAM_ASSERT_EQ(store.ReadRecords(kStoreAsOf, error)->size(), std::size_t{1});
	UAM_ASSERT_EQ(store.ReadLifetimeRecords(kStoreAsOf + 2LL * 86400000, error)->size(), std::size_t{3});
}

UAM_TEST(UsageStoreStreamedSnapshotExceedsDocumentCapWithoutLosingOldNativeReceipts)
{
	TempDir temp("uam-usage-streamed-large"); uam::usage::Store store(temp.root); std::string error;
	for (int batch_index = 0; batch_index < 5; ++batch_index)
	{
		std::vector<uam::usage::Record> records;
		for (int index = 0; index < 1800; ++index)
		{
			uam::usage::Record record = StoreEvent(std::string(8000, 'r') + std::to_string(batch_index * 1800 + index)); record.timestamp_ms = kStoreAsOf - 180LL * 86400000;
			records.push_back(std::move(record));
		}
		std::vector<uam::usage::SourceRecord> references;
		for (const uam::usage::Record& record : records) references.push_back({&record, "fixture", 0});
		UAM_ASSERT(store.Import(references, error));
	}
	const std::optional<uam::usage::Ledger> ledger = store.ReadLifetimeLedger(kStoreAsOf, error);
	UAM_ASSERT(ledger); UAM_ASSERT_EQ(ledger->Rows(), std::size_t{1});
	const std::optional<uam::usage::Summary> totals = ledger->Query(0, kStoreAsOf);
	UAM_ASSERT(totals); UAM_ASSERT_EQ(totals->total.records, std::size_t{9000}); UAM_ASSERT_EQ(totals->total.tokens.Total(), 315000LL);
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> snapshot = store.WriteReceiptSnapshot(kStoreAsOf, uam::usage::EmptyHistoryMetadata(), error);
	UAM_ASSERT(snapshot); UAM_ASSERT(snapshot->bytes > 64 * 1024 * 1024);
	std::size_t count = 0; UAM_ASSERT(snapshot->Visit([&](const uam::usage::AccountingRecord&) { ++count; return true; }, error)); UAM_ASSERT_EQ(count, std::size_t{9000});
	UAM_ASSERT(!store.ReadLifetimeLedger(kStoreAsOf, error, [] { return true; }));
	UAM_ASSERT_EQ(store.ReadLifetimeLedger(kStoreAsOf, error)->Query(0, kStoreAsOf)->total.records, std::size_t{9000});
	const fs::path snapshot_path = snapshot->path;
	UAM_ASSERT(fs::exists(snapshot_path));
}

UAM_TEST(UsageStorePeerMergeRetainsUnidentifiedHostRowsAndFirstEqualTimeNativeReceipt)
{
	TempDir temp("uam-usage-streamed-merge"); uam::usage::Store store(temp.root); std::string error;
	uam::usage::Record native = StoreEvent("same");
	uam::usage::Record unidentified = native; unidentified.dedupe_key.clear();
	const uam::usage::AccountingRecord first{nlohmann::json::array({"native", native.provider_id, native.dedupe_key}).dump(), native};
	const uam::usage::AccountingRecord unknown{nlohmann::json::array({"source", unidentified.provider_id, "same-source", 0, unidentified.timestamp_ms}).dump(), unidentified};
	UAM_ASSERT(store.ImportAccounting({first, unknown}, "host-one", error));
	uam::usage::AccountingRecord copied = first; copied.record.tokens.output = 999;
	UAM_ASSERT(store.ImportAccounting({copied, unknown}, "host-two", error));
	const std::optional<uam::usage::Ledger> ledger = store.ReadLifetimeLedger(kStoreAsOf, error); UAM_ASSERT(ledger);
	const std::optional<uam::usage::Summary> totals = ledger->Query(0, kStoreAsOf); UAM_ASSERT(totals);
	UAM_ASSERT_EQ(totals->total.records, std::size_t{3}); UAM_ASSERT_EQ(totals->total.tokens.Total(), 105LL); UAM_ASSERT_EQ(totals->unidentified_records, std::size_t{2}); UAM_ASSERT_EQ(totals->duplicates, std::size_t{1});
	copied.record.timestamp_ms -= 86400000; copied.record.tokens.output = 40;
	UAM_ASSERT(store.ImportAccounting({copied}, "host-three", error));
	UAM_ASSERT_EQ(store.ReadLifetimeLedger(kStoreAsOf, error)->Query(0, kStoreAsOf)->total.tokens.Total(), 125LL);
}

UAM_TEST(UsageStoreDiskAliasReductionMatchesExistingCohortsAndPriceProvenance)
{
	TempDir temp("uam-usage-disk-aliases"); uam::usage::Store store(temp.root); std::string error;
	std::vector<uam::usage::Record> records;
	for (int index = 0; index < 80; ++index)
	{
		uam::usage::AntigravityUsageCandidate candidate;
		candidate.record.provider_id = "antigravity-cli"; candidate.record.model_id = index % 4 ? "model" : "antigravity-unknown";
		candidate.record.session_id = "session-" + std::to_string(index % 5); candidate.record.timestamp_ms = kStoreAsOf - (100LL + index % 9) * 86400000 + index;
		candidate.record.tokens = {index + 1, index % 3, index % 2, 10, 2}; candidate.timestamp_quality = index % 3; candidate.is_step = index % 2;
		candidate.location_id = nlohmann::json::array({"antigravity", candidate.record.session_id, candidate.is_step ? "step" : "generation", index % 7, 0}).dump();
		if (index % 6) candidate.aliases = {nlohmann::json::array({12, "request-" + std::to_string(index % 11)}).dump()};
		if (index % 10 == 1) candidate.aliases.push_back(nlohmann::json::array({7, "bridge-" + std::to_string(index % 3)}).dump());
		std::sort(candidate.aliases.begin(), candidate.aliases.end());
		const std::optional<uam::usage::Record> record = uam::usage::EncodeAntigravityAccountingRecord(candidate); UAM_ASSERT(record); records.push_back(*record);
	}
	for (int index = 0; index < 4; ++index)
	{
		uam::usage::Record record = StoreEvent("ordinary-" + std::to_string(index)); record.timestamp_ms -= 180LL * 86400000;
		record.tokens.input = index % 2 ? 0 : 10; record.tokens.cached_input = index % 2 ? 5 : 0;
		if (index < 2) record.reported_cost_usd = 0.125; else if (index == 2) record.estimated_cost_usd.reset();
		records.push_back(record);
	}
	std::vector<uam::usage::SourceRecord> references;
	for (const uam::usage::Record& record : records) references.push_back({&record, "fixture", 0});
	UAM_ASSERT(store.Import(references, error));
	const std::optional<std::vector<uam::usage::AccountingRecord>> prior = store.ReadLifetimeRecords(kStoreAsOf, error); UAM_ASSERT(prior);
	std::vector<const uam::usage::Record*> expected_rows; for (const uam::usage::AccountingRecord& entry : *prior) expected_rows.push_back(&entry.record);
	const std::optional<uam::usage::Ledger> expected = uam::usage::Ledger::Create(expected_rows, kStoreAsOf, uam::usage::LedgerRange::AllRecorded);
	const std::optional<uam::usage::Ledger> actual = store.ReadLifetimeLedger(kStoreAsOf, error); UAM_ASSERT(expected); UAM_ASSERT(actual);
	UAM_ASSERT_EQ(actual->Encode(), expected->Encode());
	std::map<uam::usage::ModelKey, uam::usage::ModelPrices> prices;
	prices[{"opencode-cli", "qwen3.8-27b", "standard"}] = {2.0, std::nullopt, std::nullopt, 5.0};
	const std::optional<uam::usage::Summary> first = expected->Query(0, kStoreAsOf, prices), second = actual->Query(0, kStoreAsOf, prices);
	UAM_ASSERT(first); UAM_ASSERT(second); UAM_ASSERT_EQ(first->total.reported_cost_usd, second->total.reported_cost_usd); UAM_ASSERT_EQ(first->total.estimated_cost_usd, second->total.estimated_cost_usd); UAM_ASSERT_EQ(first->total.unpriced_tokens, second->total.unpriced_tokens); UAM_ASSERT_EQ(first->total.sessions, second->total.sessions);
}

UAM_TEST(UsageStreamedReceiptsRejectDuplicateIdentitiesAndMalformedCoverage)
{
	TempDir temp("uam-usage-streamed-validation"); uam::usage::Store store(temp.root); std::string error; uam::usage::Record record = StoreEvent("same");
	UAM_ASSERT(store.Import({{&record, "fixture", 0}}, error));
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> original = store.WriteReceiptSnapshot(kStoreAsOf, uam::usage::EmptyHistoryMetadata(), error); UAM_ASSERT(original);
	std::string bytes = ReadFile(original->path); const std::size_t first = bytes.find('\n'); UAM_ASSERT(first != std::string::npos);
	const fs::path duplicate = temp.root / "duplicate.jsonl"; UAM_ASSERT(uam::io::WriteTextFile(duplicate, bytes + bytes.substr(first + 1)));
	const std::shared_ptr<const uam::usage::ReceiptSnapshot> duplicated = uam::usage::ReadReceiptSnapshot(duplicate, error); UAM_ASSERT(duplicated);
	UAM_ASSERT(!duplicated->Visit([](const uam::usage::AccountingRecord&) { return true; }, error)); UAM_ASSERT(error.find("duplicate") != std::string::npos);
	nlohmann::json header = nlohmann::json::parse(bytes.substr(0, first)); header["metadata"]["providers"][0]["providerId"] = "unknown-provider";
	UAM_ASSERT(uam::io::WriteTextFile(duplicate, header.dump() + "\n" + bytes.substr(first + 1))); UAM_ASSERT(!uam::usage::ReadReceiptSnapshot(duplicate, error));
	UAM_ASSERT(original->Visit([](const uam::usage::AccountingRecord&) { return true; }, error));
}
