#pragma once

#include "common/usage/usage_aggregation.h"
#include <nlohmann/json.hpp>

namespace uam::usage
{
	inline constexpr const char* kUsageRatesUrl = "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json";
	using ModelAliases = std::map<std::pair<std::string, std::string>, std::string>;

	struct ReferenceRate
	{
		ModelPrices standard;
		std::optional<ModelPrices> fast;
		std::optional<ModelPrices> ultrafast;
		std::string source;
		std::string verified_date;
		bool cache_fallback = false;
	};

	struct ResolvedPrice
	{
		ModelPrices prices;
		std::string reference_model;
		std::string source;
		std::string verified_date;
		bool cache_fallback = false;
		bool speed_fallback = false;
	};

	/// <summary>Pure reference-rate lookup. Ambiguous aliases stay unknown; billing receipts remain authoritative.</summary>
	class RateCatalog
	{
	public:
		static RateCatalog Parse(const nlohmann::json& document);
		static std::optional<ModelAliases> ParseAliases(const nlohmann::json& document);
		void AddVerifiedReferences();
		std::optional<ResolvedPrice> Resolve(const ModelKey& model, const ModelAliases& aliases = {}) const;
		std::size_t Size() const { return m_rates.size(); }

	private:
		std::map<std::string, ReferenceRate> m_rates;
	};
}
