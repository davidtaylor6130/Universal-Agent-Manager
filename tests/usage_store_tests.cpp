#include "test_harness.h"
#include "common/usage/usage_store.h"
#include <sqlite3.h>

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
