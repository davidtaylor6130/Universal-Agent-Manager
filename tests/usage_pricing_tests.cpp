#include "test_harness.h"
#include "common/usage/usage_pricing.h"
#include <cmath>

using namespace uam_test;

namespace
{
	nlohmann::json PriceRow(double input = 0.000002, double output = 0.000010)
	{
		return {{"input_cost_per_token", input}, {"output_cost_per_token", output}};
	}
}

UAM_TEST(UsagePricingConvertsTokenRatesAndKeepsFreeAndUnknownModelsDistinct)
{
	const uam::usage::RateCatalog catalog = uam::usage::RateCatalog::Parse({{"model", PriceRow()}, {"free", PriceRow(0, 0)},
	    {"missing", {{"input_cost_per_token", 1}}}, {"negative", PriceRow(-1)}, {"string", {{"input_cost_per_token", "2"}, {"output_cost_per_token", 1}}}});
	const std::optional<uam::usage::ResolvedPrice> standard = catalog.Resolve({"codex-cli", " MODEL ", "standard"});
	UAM_ASSERT(standard); UAM_ASSERT_EQ(*standard->prices.input_per_million, 2.0); UAM_ASSERT_EQ(*standard->prices.output_per_million, 10.0);
	UAM_ASSERT_EQ(*standard->prices.cached_input_per_million, 2.0); UAM_ASSERT(standard->cache_fallback);
	UAM_ASSERT_EQ(*catalog.Resolve({"copilot-cli", "free", "standard"})->prices.input_per_million, 0.0);
	for (const std::string& model : {"missing", "negative", "string", "unknown", "sonnet", "default", "<synthetic>"}) UAM_ASSERT(!catalog.Resolve({"claude-cli", model, "standard"}));
	UAM_ASSERT(!catalog.Resolve({"removed-provider", "model", "standard"}));
	UAM_ASSERT(!catalog.Resolve({"codex-cli", "model", "wrong-speed"}));
}

UAM_TEST(UsagePricingUsesPublishedSpeedTiersAndPreservesCacheRatiosForMissingTierFields)
{
	nlohmann::json row = PriceRow();
	row["cache_read_input_token_cost"] = 0.0000002; row["cache_creation_input_token_cost"] = 0.0000025;
	row["input_cost_per_token_priority"] = 0.000004; row["output_cost_per_token_priority"] = 0.000020;
	row["input_cost_per_token_ultrafast"] = 0.000006; row["output_cost_per_token_ultrafast"] = 0.000030;
	nlohmann::json multiplied = row; multiplied["provider_specific_entry"] = {{"fast", 3}};
	const uam::usage::RateCatalog catalog = uam::usage::RateCatalog::Parse({{"model", row}, {"multiplied", multiplied}, {"fallback", PriceRow()}});
	const std::optional<uam::usage::ResolvedPrice> fast = catalog.Resolve({"codex-cli", "model", "fast"});
	UAM_ASSERT(fast); UAM_ASSERT(!fast->speed_fallback); UAM_ASSERT_EQ(*fast->prices.input_per_million, 4.0);
	UAM_ASSERT(std::abs(*fast->prices.cached_input_per_million - 0.4) < 1e-12);
	UAM_ASSERT_EQ(*fast->prices.cache_write_per_million, 5.0);
	UAM_ASSERT_EQ(*catalog.Resolve({"codex-cli", "model", "ultrafast"})->prices.input_per_million, 6.0);
	UAM_ASSERT_EQ(*catalog.Resolve({"claude-cli", "multiplied", "fast"})->prices.input_per_million, 6.0);
	UAM_ASSERT(catalog.Resolve({"codex-cli", "fallback", "fast"})->speed_fallback);
}

UAM_TEST(UsagePricingRejectsAmbiguousBareAliasesButHonorsCanonicalAndProviderScopedModelReferences)
{
	const uam::usage::RateCatalog catalog = uam::usage::RateCatalog::Parse({{"one/shared", PriceRow()}, {"two/shared", PriceRow(0.000003)},
	    {"one/same", PriceRow()}, {"two/same", PriceRow()}, {"gpt", PriceRow()}, {"gemini/gemini-model", PriceRow()},
	    {"MODEL", PriceRow()}, {"model", PriceRow(0.000003)}});
	UAM_ASSERT(!catalog.Resolve({"codex-cli", "shared", "standard"}));
	UAM_ASSERT(!catalog.Resolve({"codex-cli", "model", "standard"}));
	UAM_ASSERT(catalog.Resolve({"copilot-cli", "same", "standard"}));
	UAM_ASSERT(catalog.Resolve({"opencode-cli", "openai/gpt", "standard"}));
	UAM_ASSERT(catalog.Resolve({"gemini-cli", "gemini-model", "standard"}));
	UAM_ASSERT(!catalog.Resolve({"opencode-cli", "unknown/gpt", "standard"}));
	const uam::usage::ModelAliases aliases{{{"opencode-cli", "inferdeck/normal"}, "one/shared"}};
	UAM_ASSERT(catalog.Resolve({"opencode-cli", "inferdeck/normal", "standard"}, aliases));
	UAM_ASSERT(!catalog.Resolve({"opencode-cli", "other/normal", "standard"}, aliases));
	UAM_ASSERT(!catalog.Resolve({"claude-cli", "inferdeck/normal", "standard"}, aliases));
}

UAM_TEST(UsagePricingValidatesAliasFilesAndPricesLocalQwenAsExplicitHostedReferences)
{
	const nlohmann::json document{{"version", 1}, {"aliases", nlohmann::json::array({
	    {{"providerId", "opencode-cli"}, {"modelId", "inferdeck/normal"}, {"referenceModel", "qwen3.8-27b"}},
	    {{"providerId", "opencode-cli"}, {"modelId", "inferdeck/fast"}, {"referenceModel", "qwen3.6-35b-a3b"}}})}};
	const std::optional<uam::usage::ModelAliases> aliases = uam::usage::RateCatalog::ParseAliases(document);
	UAM_ASSERT(aliases);
	uam::usage::RateCatalog catalog; catalog.AddVerifiedReferences();
	const std::optional<uam::usage::ResolvedPrice> normal = catalog.Resolve({"opencode-cli", "inferdeck/normal", "standard"}, *aliases), fast = catalog.Resolve({"opencode-cli", "inferdeck/fast", "standard"}, *aliases);
	UAM_ASSERT(normal && fast); UAM_ASSERT_EQ(*normal->prices.input_per_million, 0.5); UAM_ASSERT_EQ(*normal->prices.output_per_million, 3.0);
	UAM_ASSERT_EQ(*fast->prices.input_per_million, 0.375); UAM_ASSERT_EQ(*fast->prices.output_per_million, 2.25); UAM_ASSERT(fast->cache_fallback);
	UAM_ASSERT_EQ(normal->verified_date, std::string("2026-10-10")); UAM_ASSERT(normal->source.starts_with("https://www.alibabacloud.com/"));
	nlohmann::json duplicate = document; duplicate["aliases"].push_back(document["aliases"][0]); UAM_ASSERT(!uam::usage::RateCatalog::ParseAliases(duplicate));
	nlohmann::json unsupported = document; unsupported["version"] = 2; UAM_ASSERT(!uam::usage::RateCatalog::ParseAliases(unsupported));
	nlohmann::json bad = document; bad["aliases"][0]["referenceModel"] = false; UAM_ASSERT(!uam::usage::RateCatalog::ParseAliases(bad));
	uam::usage::RateCatalog newer = uam::usage::RateCatalog::Parse({{"qwen3.8-27b", PriceRow()}}); newer.AddVerifiedReferences();
	UAM_ASSERT_EQ(*newer.Resolve({"opencode-cli", "qwen3.8-27b", "standard"})->prices.input_per_million, 2.0);
}
