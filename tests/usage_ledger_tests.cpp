#include "test_harness.h"
#include "common/usage/usage_ledger.h"
#include <cmath>
#include <limits>

using namespace uam_test;

namespace
{
	constexpr std::int64_t kDayMs = 86400000;
	const std::int64_t kAsOf = *uam::usage::TimestampMilliseconds("2026-10-10T12:00:00Z");

	uam::usage::Record LedgerEvent(int age_days, const std::string& identity)
	{
		uam::usage::Record record;
		record.provider_id = uam::provider_ids::kOpenCodeCli;
		record.model_id = "inferdeck/normal";
		record.session_id = "session";
		record.dedupe_key = identity;
		record.timestamp_ms = kAsOf - age_days * kDayMs - 1;
		record.tokens = {10, 5, 2, 20, 7};
		return record;
	}

	void SameTotals(const uam::usage::Totals& actual, const uam::usage::Totals& expected)
	{
		UAM_ASSERT_EQ(actual.tokens.input, expected.tokens.input); UAM_ASSERT_EQ(actual.tokens.cached_input, expected.tokens.cached_input);
		UAM_ASSERT_EQ(actual.tokens.cache_write, expected.tokens.cache_write); UAM_ASSERT_EQ(actual.tokens.output, expected.tokens.output);
		UAM_ASSERT_EQ(actual.tokens.reasoning, expected.tokens.reasoning); UAM_ASSERT_EQ(actual.records, expected.records);
		UAM_ASSERT_EQ(actual.sessions, expected.sessions); UAM_ASSERT_EQ(actual.reported_cost_tokens, expected.reported_cost_tokens);
		UAM_ASSERT_EQ(actual.estimated_cost_tokens, expected.estimated_cost_tokens); UAM_ASSERT_EQ(actual.unpriced_tokens, expected.unpriced_tokens);
		UAM_ASSERT(std::abs(actual.reported_cost_usd - expected.reported_cost_usd) < 1e-10);
		UAM_ASSERT(std::abs(actual.estimated_cost_usd - expected.estimated_cost_usd) < 1e-10);
		UAM_ASSERT(std::abs(actual.estimated_cache_savings_usd - expected.estimated_cache_savings_usd) < 1e-10);
	}
}

UAM_TEST(UsageLedgerCompactsDailyCohortsWithExactRecentAndPricingSessionParity)
{
	std::vector<uam::usage::Record> records;
	for (int age = 0; age < 95; ++age)
		for (int event = 0; event < 12; ++event)
		{
			uam::usage::Record record = LedgerEvent(age, std::to_string(age) + ":" + std::to_string(event));
			record.session_id = event < 6 ? "shared-across-days" : "second-session";
			if (event % 3 == 0) record.reported_cost_usd = 0.01;
			if (event % 3 == 1) record.estimated_cost_usd = 0.02;
			if (event == 11) record.dedupe_key.clear();
			records.push_back(record);
		}
	std::vector<const uam::usage::Record*> references;
	for (const uam::usage::Record& record : records) references.push_back(&record);
	// The same pointer repeated is still a duplicate event, and copied forks cannot move old usage into a new day.
	references.push_back(&records[0]);
	uam::usage::Record fork = records[1000]; fork.timestamp_ms = kAsOf - 2;
	references.push_back(&fork);
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create(references, kAsOf);
	UAM_ASSERT(ledger);
	UAM_ASSERT(ledger->Rows() < references.size() / 2);
	const std::optional<uam::usage::Ledger> restored = uam::usage::Ledger::Decode(ledger->Encode());
	UAM_ASSERT(restored);
	const uam::usage::ModelKey model{uam::provider_ids::kOpenCodeCli, "inferdeck/normal", "standard"};
	for (const std::map<uam::usage::ModelKey, uam::usage::ModelPrices> prices : {
	    std::map<uam::usage::ModelKey, uam::usage::ModelPrices>{}, {{model, {1, 0.1, 2, 4}}}, {{model, {1, std::nullopt, 2, 4}}}})
		for (const int days : {1, 7, 30, 90})
		{
			const std::int64_t start = ledger->WindowStart(days);
			const std::optional<uam::usage::Summary> raw = uam::usage::AggregateReferences(references, start, kAsOf, prices);
			const std::optional<uam::usage::Summary> cached = restored->Query(start, kAsOf, prices);
			UAM_ASSERT(raw && cached);
			SameTotals(cached->total, raw->total);
			SameTotals(cached->providers.at(uam::provider_ids::kOpenCodeCli), raw->providers.at(uam::provider_ids::kOpenCodeCli));
			SameTotals(cached->models.at(model), raw->models.at(model));
			UAM_ASSERT_EQ(cached->duplicates, raw->duplicates);
			UAM_ASSERT_EQ(cached->unidentified_records, raw->unidentified_records);
			UAM_ASSERT_EQ(cached->days.size(), raw->days.size());
			for (const std::pair<const std::int64_t, uam::usage::Totals>& day : raw->days) SameTotals(cached->days.at(day.first), day.second);
			if (days == 1)
				for (const std::pair<const std::int64_t, uam::usage::Totals>& hour : raw->hours) SameTotals(cached->hours.at(hour.first), hour.second);
		}
	UAM_ASSERT(!ledger->Query(kAsOf - 7 * kDayMs, kAsOf));
	UAM_ASSERT(!ledger->Query(ledger->WindowStart(90), kAsOf - 2 * kDayMs));
	UAM_ASSERT(!ledger->Query(ledger->WindowStart(90), kAsOf + 1));
	UAM_ASSERT(!ledger->Query(ledger->WindowStart(90) - kDayMs, kAsOf));
	UAM_ASSERT_EQ(ledger->WindowStart(2), -1LL);
}

UAM_TEST(UsageLedgerRoundTripRejectsMalformedAmountsCountsAndLossyArchiveBoundaries)
{
	const uam::usage::Record record = LedgerEvent(2, "original");
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create({&record}, kAsOf);
	UAM_ASSERT(ledger);
	const nlohmann::json valid = ledger->Encode();
	for (const nlohmann::json value : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("10"), nlohmann::json(9007199254740992ULL)})
	{
		nlohmann::json invalid = valid; invalid["rows"][0][6] = value;
		UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	}
	for (const int column : {11, 12})
	{
		nlohmann::json invalid = valid; invalid["rows"][0][column] = -0.1;
		UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	}
	for (const int column : {5, 13, 14})
	{
		nlohmann::json invalid = valid;
		invalid["rows"][0][column] = column == 5 ? record.timestamp_ms : column == 13 ? 0LL : 2LL;
		UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	}
	nlohmann::json invalid = valid; invalid["rows"][0][10] = 21;
	UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	invalid = valid; invalid["rows"][0][0] = "unknown-provider";
	UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	invalid = valid; invalid["version"] = 2;
	UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	UAM_ASSERT(!uam::usage::Ledger::Create({nullptr}, kAsOf));
	uam::usage::Record bad = record; bad.tokens.input = uam::usage::kMaxExactTokenCount;
	UAM_ASSERT(!uam::usage::Ledger::Create({&bad}, kAsOf));
	bad = record; bad.estimated_cost_usd = std::numeric_limits<double>::infinity();
	UAM_ASSERT(!uam::usage::Ledger::Create({&bad}, kAsOf));
	bad = record; bad.record_count = 0;
	UAM_ASSERT(!uam::usage::Ledger::Create({&bad}, kAsOf));
}

UAM_TEST(UsageLedgerKeepsZeroTokenCostsAndSerializesOnlyAccountingMetadata)
{
	uam::usage::Record record = LedgerEvent(3, "first"); record.tokens = {}; record.reported_cost_usd = 0.05;
	uam::usage::Record second = record; second.dedupe_key = "second";
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create({&record, &second}, kAsOf);
	UAM_ASSERT(ledger); UAM_ASSERT_EQ(ledger->Rows(), std::size_t(1));
	const std::optional<uam::usage::Summary> totals = ledger->Query(ledger->WindowStart(7), kAsOf);
	UAM_ASSERT(totals); UAM_ASSERT_EQ(totals->total.records, std::size_t(2));
	UAM_ASSERT_EQ(totals->total.sessions, std::size_t(1)); UAM_ASSERT_EQ(totals->total.tokens.Total(), 0LL);
	UAM_ASSERT(std::abs(totals->total.reported_cost_usd - 0.1) < 1e-10);
	UAM_ASSERT_EQ(ledger->Encode()["rows"][0][4].get<std::string>(), std::string{});
}

UAM_TEST(UsageLedgerPreservesPartialPriceCoverageWhenEventsUseDifferentTokenBuckets)
{
	uam::usage::Record input = LedgerEvent(2, "input"), cache = LedgerEvent(2, "cache");
	input.tokens = {10, 0, 0, 0, 0}; cache.tokens = {0, 5, 0, 0, 0};
	const uam::usage::ModelKey model{input.provider_id, input.model_id, input.speed};
	const std::map<uam::usage::ModelKey, uam::usage::ModelPrices> prices{{model, {1, std::nullopt, std::nullopt, std::nullopt}}};
	for (const bool saved_estimate : {false, true})
	{
		if (saved_estimate) { input.estimated_cost_usd = 0.001; cache.estimated_cost_usd = 0.002; }
		const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create({&input, &cache}, kAsOf);
		UAM_ASSERT(ledger);
		const std::int64_t start = ledger->WindowStart(7);
		const std::optional<uam::usage::Summary> raw = uam::usage::Aggregate({input, cache}, start, kAsOf, prices);
		const std::optional<uam::usage::Summary> cached = ledger->Query(start, kAsOf, prices);
		UAM_ASSERT(raw && cached);
		SameTotals(cached->total, raw->total);
	}
}

UAM_TEST(UsageLedgerRetainsExactCutoffAndRejectsDuplicatePersistedRows)
{
	uam::usage::Record recent = LedgerEvent(1, "cutoff"); recent.timestamp_ms = kAsOf - kDayMs;
	uam::usage::Record archived = recent; archived.timestamp_ms -= 1; archived.dedupe_key = "older";
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create({&recent, &archived}, kAsOf);
	UAM_ASSERT(ledger);
	UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(1), kAsOf)->total.records, std::size_t(1));
	UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(7), kAsOf)->total.records, std::size_t(2));
	for (const int index : {0, 1})
	{
		nlohmann::json duplicate = ledger->Encode(); duplicate["rows"].push_back(duplicate["rows"][index]);
		UAM_ASSERT(!uam::usage::Ledger::Decode(duplicate));
	}
	uam::usage::Record weighted = recent; weighted.record_count = static_cast<std::size_t>(uam::usage::kMaxExactTokenCount); weighted.dedupe_key = "weighted";
	UAM_ASSERT(!uam::usage::Aggregate({weighted, recent}, 0, kAsOf));
	weighted = recent; weighted.unidentified_records = 2;
	UAM_ASSERT(!uam::usage::Aggregate({weighted}, 0, kAsOf));
}

UAM_TEST(UsageLedgerKeepsProviderModelSpeedAndEmptySessionCohortsDistinct)
{
	std::vector<uam::usage::Record> records;
	for (const char* provider : uam::provider_ids::kAllCliProviderIds)
		for (const std::string speed : {"standard", "fast"})
			for (int event = 0; event < 3; ++event)
			{
				uam::usage::Record record = LedgerEvent(3, std::string(provider) + speed + std::to_string(event));
				record.provider_id = provider; record.speed = speed;
				if (event == 0) record.session_id.clear();
				if (event == 2) record.model_id = "second-model";
				records.push_back(record);
			}
	std::vector<const uam::usage::Record*> references;
	for (const uam::usage::Record& record : records) references.push_back(&record);
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create(references, kAsOf);
	UAM_ASSERT(ledger);
	const std::optional<uam::usage::Summary> cached = ledger->Query(ledger->WindowStart(7), kAsOf);
	const std::optional<uam::usage::Summary> raw = uam::usage::Aggregate(records, ledger->WindowStart(7), kAsOf);
	UAM_ASSERT(raw && cached);
	SameTotals(cached->total, raw->total);
	UAM_ASSERT_EQ(cached->total.sessions, std::size_t(6));
	UAM_ASSERT_EQ(cached->models.size(), std::size_t(24));
	for (const std::pair<const uam::usage::ModelKey, uam::usage::Totals>& model : raw->models) SameTotals(cached->models.at(model.first), model.second);
	for (const std::pair<const std::string, uam::usage::Totals>& provider : raw->providers) SameTotals(cached->providers.at(provider.first), provider.second);
}

UAM_TEST(UsageLedgerStorageSizeGrowsWithDailyCohortsAndRecentEvents)
{
	std::vector<uam::usage::Record> records;
	for (int day = 0; day < 30; ++day)
		for (int event = 0; event < 500; ++event) records.push_back(LedgerEvent(day, std::to_string(day) + ":" + std::to_string(event)));
	std::vector<const uam::usage::Record*> references;
	for (const uam::usage::Record& record : records) references.push_back(&record);
	const std::optional<uam::usage::Ledger> ledger = uam::usage::Ledger::Create(references, kAsOf);
	UAM_ASSERT(ledger);
	const std::size_t bytes = ledger->Encode().dump().size();
	UAM_ASSERT_EQ(ledger->Rows(), std::size_t(529));
	UAM_ASSERT(bytes < 100000);
	UAM_ASSERT_EQ(ledger->Query(ledger->WindowStart(30), kAsOf)->total.records, std::size_t(15000));
	std::cout << "[ledger-size] events=" << records.size() << " rows=" << ledger->Rows() << " bytes=" << bytes << '\n';
}

UAM_TEST(UsageLifetimeLedgerRetainsOldCohortsAndRequiresExplicitSnapshotScope)
{
	const uam::usage::Record old = LedgerEvent(180, "old-native"), recent = LedgerEvent(0, "recent-native");
	uam::usage::Record copy = old;
	copy.timestamp_ms = recent.timestamp_ms;
	const std::vector<const uam::usage::Record*> records{&old, &recent, &copy};
	const std::optional<uam::usage::Ledger> rolling = uam::usage::Ledger::Create(records, kAsOf);
	const std::optional<uam::usage::Ledger> lifetime = uam::usage::Ledger::Create(records, kAsOf, uam::usage::LedgerRange::AllRecorded);
	UAM_ASSERT(rolling && lifetime);
	UAM_ASSERT_EQ(rolling->WindowStart(0), -1LL);
	UAM_ASSERT(!rolling->Query(0, kAsOf));
	UAM_ASSERT_EQ(lifetime->WindowStart(0), 0LL);
	UAM_ASSERT_EQ(lifetime->Query(0, kAsOf)->total.records, std::size_t{2});
	UAM_ASSERT_EQ(lifetime->Query(lifetime->WindowStart(90), kAsOf)->total.records, std::size_t{1});
	const nlohmann::json payload = lifetime->Encode();
	UAM_ASSERT_EQ(payload["version"], nlohmann::json(2));
	UAM_ASSERT_EQ(payload["range"], nlohmann::json("all-recorded"));
	const std::optional<uam::usage::Ledger> restored = uam::usage::Ledger::Decode(payload);
	UAM_ASSERT(restored && restored->IncludesLifetime());
	SameTotals(restored->Query(0, kAsOf)->total, lifetime->Query(0, kAsOf)->total);
	for (const int mutation : {0, 1, 2, 3})
	{
		nlohmann::json invalid = payload;
		if (mutation == 0) invalid.erase("range");
		if (mutation == 1) invalid["range"] = "90-days";
		if (mutation == 2) invalid["version"] = 1;
		if (mutation == 3) invalid["rows"][0][5] = -1;
		UAM_ASSERT(!uam::usage::Ledger::Decode(invalid));
	}
	const std::optional<uam::usage::Ledger> legacy = uam::usage::Ledger::Decode(rolling->Encode());
	UAM_ASSERT(legacy && !legacy->IncludesLifetime());
	UAM_ASSERT(!legacy->Query(0, kAsOf));
}
