#include "common/usage/usage_pricing.h"
#include "common/utils/string_utils.h"
#include <cmath>
#include <set>

namespace uam::usage
{
	namespace
	{
		std::optional<double> Number(const nlohmann::json& row, const std::string& key)
		{
			const nlohmann::json::const_iterator field = row.find(key);
			if (field == row.end() || !field->is_number()) return std::nullopt;
			const double value = field->get<double>();
			return std::isfinite(value) && value >= 0 && value <= 1000 ? std::optional<double>{value} : std::nullopt;
		}
		std::optional<ModelPrices> Rates(const nlohmann::json& row, const std::string& suffix, bool& fallback, const ModelPrices* base = nullptr)
		{
			const std::optional<double> input = Number(row, "input_cost_per_token" + suffix), output = Number(row, "output_cost_per_token" + suffix);
			if (!input || !output) return std::nullopt;
			const std::optional<double> read = Number(row, "cache_read_input_token_cost" + suffix), write = Number(row, "cache_creation_input_token_cost" + suffix);
			fallback = fallback || !read || !write;
			const double read_rate = read ? *read * 1000000 : base && base->input_per_million.value_or(0) > 0 ? base->cached_input_per_million.value_or(0) / *base->input_per_million * *input * 1000000 : *input * 1000000;
			const double write_rate = write ? *write * 1000000 : base && base->input_per_million.value_or(0) > 0 ? base->cache_write_per_million.value_or(0) / *base->input_per_million * *input * 1000000 : *input * 1000000;
			return ModelPrices{*input * 1000000, read_rate, write_rate, *output * 1000000};
		}
		bool Same(const ModelPrices& left, const ModelPrices& right)
		{
			return left.input_per_million == right.input_per_million && left.cached_input_per_million == right.cached_input_per_million &&
			    left.cache_write_per_million == right.cache_write_per_million && left.output_per_million == right.output_per_million;
		}
		bool Same(const std::optional<ModelPrices>& left, const std::optional<ModelPrices>& right)
		{
			return left.has_value() == right.has_value() && (!left || Same(*left, *right));
		}
		bool Same(const ReferenceRate& left, const ReferenceRate& right)
		{
			return Same(left.standard, right.standard) && Same(left.fast, right.fast) && Same(left.ultrafast, right.ultrafast);
		}
		std::string Bare(const std::string& model)
		{
			const std::size_t slash = model.rfind('/'); return slash == std::string::npos ? model : model.substr(slash + 1);
		}
	}

	RateCatalog RateCatalog::Parse(const nlohmann::json& document)
	{
		RateCatalog catalog;
		if (!document.is_object() || document.size() > 20000) return catalog;
		std::set<std::string> conflicts;
		for (nlohmann::json::const_iterator entry = document.begin(); entry != document.end(); ++entry)
		{
			if (!entry.value().is_object() || entry.key().size() > 512) continue;
			const std::string key = strings::TrimAndLowerAscii(entry.key());
			if (key.empty() || conflicts.contains(key)) continue;
			ReferenceRate rate;
			const std::optional<ModelPrices> standard = Rates(entry.value(), "", rate.cache_fallback);
			if (!standard) continue;
			rate.standard = *standard; rate.fast = Rates(entry.value(), "_priority", rate.cache_fallback, &rate.standard); rate.ultrafast = Rates(entry.value(), "_ultrafast", rate.cache_fallback, &rate.standard);
			rate.source = kUsageRatesUrl;
			const nlohmann::json::const_iterator specific = entry.value().find("provider_specific_entry");
			const std::optional<double> multiple = specific != entry.value().end() && specific->is_object() ? Number(*specific, "fast") : std::nullopt;
			if (multiple && *multiple > 0)
			{
				ModelPrices fast = rate.standard;
				for (std::optional<double>* amount : {&fast.input_per_million, &fast.cached_input_per_million, &fast.cache_write_per_million, &fast.output_per_million}) *amount = **amount * *multiple;
				rate.fast = fast;
			}
			const std::map<std::string, ReferenceRate>::const_iterator previous = catalog.m_rates.find(key);
			if (previous != catalog.m_rates.end() && !Same(previous->second, rate)) { catalog.m_rates.erase(key); conflicts.insert(key); }
			else catalog.m_rates.emplace(key, std::move(rate));
		}
		std::map<std::string, std::optional<ReferenceRate>> aliases;
		for (const std::pair<const std::string, ReferenceRate>& rate : catalog.m_rates)
		{
			const std::string bare = Bare(rate.first);
			if (bare.empty() || bare == rate.first || catalog.m_rates.contains(bare) || conflicts.contains(bare)) continue;
			const std::pair<std::map<std::string, std::optional<ReferenceRate>>::iterator, bool> found = aliases.emplace(bare, rate.second);
			if (!found.second && found.first->second && !Same(*found.first->second, rate.second)) found.first->second.reset();
		}
		for (const std::pair<const std::string, std::optional<ReferenceRate>>& alias : aliases)
			if (alias.second) catalog.m_rates.emplace(alias.first, *alias.second);
		return catalog;
	}

	std::optional<ModelAliases> RateCatalog::ParseAliases(const nlohmann::json& document)
	{
		if (!document.is_object() || !document.contains("version") || !document["version"].is_number_integer() || document["version"] != 1 || !document.contains("aliases") || !document["aliases"].is_array() || document["aliases"].size() > 200) return std::nullopt;
		ModelAliases aliases;
		for (const nlohmann::json& row : document["aliases"])
		{
			if (!row.is_object() || !row.contains("providerId") || !row["providerId"].is_string() || !row.contains("modelId") || !row["modelId"].is_string() || !row.contains("referenceModel") || !row["referenceModel"].is_string()) return std::nullopt;
			const std::string provider = row["providerId"].get<std::string>(), model = row["modelId"].get<std::string>(), reference = strings::TrimAndLowerAscii(row["referenceModel"].get_ref<const std::string&>());
			if (!provider_ids::IsKnownCliProviderId(provider) || model.empty() || model.size() > 512 || reference.empty() || reference.size() > 512 || !aliases.emplace(std::pair{provider, model}, reference).second) return std::nullopt;
		}
		return aliases;
	}

	void RateCatalog::AddVerifiedReferences()
	{
		// Published Singapore/international reference rates checked 2026-10-10; a newer catalog entry takes precedence.
		m_rates.try_emplace("qwen3.8-27b", ReferenceRate{{0.5, 0.1, 0.625, 3.0}, {}, {}, "https://www.alibabacloud.com/help/en/model-studio/qwen3-8-27b", "2026-10-10", false});
		m_rates.try_emplace("qwen3.6-35b-a3b", ReferenceRate{{0.375, 0.375, 0.375, 2.25}, {}, {}, "https://www.alibabacloud.com/help/en/model-studio/qwen3-6-35b-a3b", "2026-10-10", true});
	}

	std::optional<ResolvedPrice> RateCatalog::Resolve(const ModelKey& model, const ModelAliases& aliases) const
	{
		const std::string& provider = std::get<0>(model), original = std::get<1>(model), speed = std::get<2>(model);
		if (!provider_ids::IsKnownCliProviderId(provider) || (speed != "standard" && speed != "fast" && speed != "ultrafast")) return std::nullopt;
		const ModelAliases::const_iterator alias = aliases.find({provider, original});
		std::string key = strings::TrimAndLowerAscii(alias == aliases.end() ? original : alias->second);
		const std::size_t variant = key.find('['); if (variant != std::string::npos) key.resize(variant);
		const std::string bare = Bare(key);
		if (bare.empty() || bare == "synthetic" || bare == "<synthetic>" || bare == "opus" || bare == "sonnet" || bare == "haiku" || bare == "fable" || bare == "default") return std::nullopt;
		std::map<std::string, ReferenceRate>::const_iterator rate = m_rates.find(key);
		if (rate == m_rates.end() && alias == aliases.end())
		{
			if (provider == provider_ids::kGeminiCli) rate = m_rates.find("gemini/" + key);
			else if (provider == provider_ids::kOpenCodeCli && (key.starts_with("openai/") || key.starts_with("anthropic/"))) rate = m_rates.find(bare);
		}
		if (rate == m_rates.end()) return std::nullopt;
		const std::optional<ModelPrices>& tier = speed == "fast" ? rate->second.fast : rate->second.ultrafast;
		return ResolvedPrice{speed == "standard" || !tier ? rate->second.standard : *tier, rate->first, rate->second.source, rate->second.verified_date, rate->second.cache_fallback, speed != "standard" && !tier};
	}
}
