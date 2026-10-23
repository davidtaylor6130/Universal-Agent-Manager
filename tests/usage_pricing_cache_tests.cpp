#include "test_harness.h"
#include "common/usage/usage_pricing_cache.h"

using namespace uam_test;

namespace
{
	constexpr std::int64_t kPricingAsOf = 1791633600000;
	std::string RateDocument(double input = 0.000002)
	{
		return nlohmann::json({{"test-model", {{"input_cost_per_token", input}, {"output_cost_per_token", 0.000010}}}}).dump();
	}
}

UAM_TEST(UsagePricingCacheRestoresRatesOfflineAndDoesNotFetchFreshCatalogsAgain)
{
	TempDir temp("uam-usage-rates-cache");
	int requests = 0;
	uam::usage::PricingCache cache(temp.root, [&](std::stop_token) -> std::optional<std::string> { ++requests; return RateDocument(); });
	cache.Refresh(kPricingAsOf);
	UAM_ASSERT_EQ(requests, 1); UAM_ASSERT_EQ(cache.Metadata(kPricingAsOf)["status"].get<std::string>(), std::string("fresh"));
	UAM_ASSERT_EQ(*cache.Catalog()->Resolve({"codex-cli", "test-model", "standard"})->prices.input_per_million, 2.0);
	cache.Refresh(kPricingAsOf + 30000); UAM_ASSERT_EQ(requests, 1);
	UAM_ASSERT(fs::exists(temp.root / "usage" / "rates-v1.json"));
	uam::usage::PricingCache restored(temp.root, [&](std::stop_token) -> std::optional<std::string> { ++requests; return std::nullopt; });
	restored.Refresh(kPricingAsOf + 60000);
	UAM_ASSERT_EQ(requests, 1); UAM_ASSERT_EQ(restored.Metadata(kPricingAsOf + 60000)["status"].get<std::string>(), std::string("cached"));
	UAM_ASSERT_EQ(*restored.Catalog()->Resolve({"codex-cli", "test-model", "standard"})->prices.input_per_million, 2.0);
}

UAM_TEST(UsagePricingCacheRetainsStaleRatesDuringFailuresAndBoundsRetryFrequency)
{
	TempDir temp("uam-usage-rates-retry");
	int requests = 0;
	uam::usage::PricingCache cache(temp.root, [&](std::stop_token) -> std::optional<std::string>
	{
		++requests;
		if (requests == 2) throw std::runtime_error("fixture failure");
		return RateDocument(requests == 1 ? 0.000002 : 0.000004);
	});
	cache.Refresh(kPricingAsOf); cache.Refresh(kPricingAsOf + 86400000);
	UAM_ASSERT_EQ(requests, 2);
	UAM_ASSERT(!cache.Metadata(kPricingAsOf + 86400000)["error"].get<std::string>().empty());
	UAM_ASSERT_EQ(cache.Metadata(kPricingAsOf + 86400000)["status"].get<std::string>(), std::string("cached"));
	UAM_ASSERT_EQ(*cache.Catalog()->Resolve({"codex-cli", "test-model", "standard"})->prices.input_per_million, 2.0);
	cache.Refresh(kPricingAsOf + 86430000); UAM_ASSERT_EQ(requests, 2);
	cache.Refresh(kPricingAsOf + 87000001); UAM_ASSERT_EQ(requests, 3);
	UAM_ASSERT(cache.Metadata(kPricingAsOf + 87000001)["error"].get<std::string>().empty());
	UAM_ASSERT_EQ(*cache.Catalog()->Resolve({"codex-cli", "test-model", "standard"})->prices.input_per_million, 4.0);
}

UAM_TEST(UsagePricingCacheRejectsOversizeAndCancelledDownloadsAndKeepsExplicitMappingsOnInvalidEdits)
{
	TempDir temp("uam-usage-rates-validation");
	fs::create_directories(temp.root / "usage");
	const fs::path path = temp.root / "usage" / "reference-models-v1.json";
	UAM_ASSERT(uam::io::WriteTextFile(path, nlohmann::json({{"version", 1}, {"aliases", nlohmann::json::array({{{"providerId", "opencode-cli"}, {"modelId", "example-provider/normal"}, {"referenceModel", "qwen3.8-27b"}}})}}).dump()));
	int requests = 0;
	uam::usage::PricingCache cache(temp.root, [&](std::stop_token) -> std::optional<std::string> { ++requests; return std::string(4 * 1024 * 1024 + 1, 'x'); });
	std::stop_source stop; stop.request_stop(); cache.Refresh(kPricingAsOf, stop.get_token()); UAM_ASSERT_EQ(requests, 0);
	cache.Refresh(kPricingAsOf); UAM_ASSERT_EQ(requests, 1); UAM_ASSERT(!fs::exists(temp.root / "usage" / "rates-v1.json"));
	UAM_ASSERT(cache.Catalog()->Resolve({"opencode-cli", "example-provider/normal", "standard"}, cache.Aliases()));
	UAM_ASSERT(uam::io::WriteTextFile(path, "corrupt")); cache.Refresh(kPricingAsOf + 30000);
	UAM_ASSERT(cache.Catalog()->Resolve({"opencode-cli", "example-provider/normal", "standard"}, cache.Aliases()));
	UAM_ASSERT(!cache.Metadata(kPricingAsOf + 30000)["aliasError"].get<std::string>().empty());
	UAM_ASSERT(fs::remove(path)); cache.Refresh(kPricingAsOf + 60000); UAM_ASSERT(cache.Aliases().empty());
	UAM_ASSERT_EQ(requests, 1);
}
