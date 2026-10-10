#include "test_harness.h"
#include "common/usage/usage_history_service.h"

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
