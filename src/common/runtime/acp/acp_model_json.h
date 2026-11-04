#pragma once

#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/state/app_state.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace uam::acp_models
{
	inline std::string ModelStringListItemValue(const nlohmann::json& item)
	{
		if (item.is_string())
		{
			return uam::strings::Trim(item.get_ref<const std::string&>());
		}
		if (item.is_object())
		{
			return uam::nlohmann_json::TrimmedStringValue(item, {"reasoningEffort", "reasoning_effort", "effort", "id"});
		}

		return {};
	}

	template <typename NormalizeValue> inline std::vector<std::string> UniqueStringArrayValue(const nlohmann::json& object, const char* key, NormalizeValue normalize_value)
	{
		std::vector<std::string> values;
		const nlohmann::json* array = uam::nlohmann_json::FindArrayField(object, key);
		if (array == nullptr)
		{
			return values;
		}

		values.reserve(array->size());
		for (const nlohmann::json& item : *array)
		{
			uam::ranges::PushUniqueNonEmptyString(values, normalize_value(ModelStringListItemValue(item)));
		}

		return values;
	}

	template <typename NormalizeValue> inline std::vector<std::string> UniqueStringArrayValue(const nlohmann::json& object, std::initializer_list<const char*> keys, NormalizeValue normalize_value)
	{
		std::vector<std::string> merged;
		for (const char* key : keys)
		{
			for (const std::string& value : UniqueStringArrayValue(object, key, normalize_value))
			{
				uam::ranges::PushUniqueNonEmptyString(merged, value);
			}
		}
		return merged;
	}

	inline std::string NormalizeModelServiceTier(std::string_view value)
	{
		const std::string normalized = uam::codex::NormalizeServiceTier(value);
		return !normalized.empty() ? normalized : uam::strings::TrimmedEqualsIgnoreCase(value, "priority") ? "fast" : "";
	}

	inline std::optional<std::int64_t> ModelTokenLimit(const nlohmann::json& model, std::initializer_list<const char*> keys)
	{
		for (const char* key : keys)
		{
			const nlohmann::json* value = uam::nlohmann_json::FindField(model, key);
			if (value == nullptr) continue;
			if (!value->is_number_integer()) return std::nullopt;
			constexpr std::int64_t max_exact = 9007199254740991LL;
			if (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(max_exact)) return std::nullopt;
			const std::int64_t count = value->get<std::int64_t>();
			return count >= 0 && count <= max_exact ? std::optional<std::int64_t>{count} : std::nullopt;
		}
		return std::nullopt;
	}

	inline std::optional<AcpModelState> ParseAcpModelState(const nlohmann::json& model)
	{
		if (!model.is_object())
		{
			return std::nullopt;
		}

		AcpModelState parsed;
		parsed.id = uam::nlohmann_json::TrimmedStringValue(model, {"modelId", "id"});
		if (parsed.id.empty())
		{
			return std::nullopt;
		}

		parsed.name = uam::nlohmann_json::TrimmedStringValue(model, {"name", "displayName", "display_name"});
		if (parsed.name.empty())
		{
			parsed.name = parsed.id;
		}
		parsed.description = uam::nlohmann_json::TrimmedStringValue(model, {"description"});
		parsed.default_reasoning_effort = uam::codex::NormalizeReasoningEffort(uam::nlohmann_json::TrimmedStringValue(model, {"defaultReasoningEffort", "default_reasoning_effort", "defaultReasoningLevel", "default_reasoning_level"}));
		parsed.supported_reasoning_efforts = UniqueStringArrayValue(model, {"supportedReasoningEfforts", "supported_reasoning_levels"}, uam::codex::NormalizeReasoningEffort);
		parsed.additional_speed_tiers = UniqueStringArrayValue(model, {"additionalSpeedTiers", "additional_speed_tiers", "serviceTiers", "service_tiers"}, NormalizeModelServiceTier);
		parsed.context_window_tokens = ModelTokenLimit(model, {"contextWindowTokens"});
		parsed.max_output_tokens = ModelTokenLimit(model, {"maxOutputTokens"});
		return parsed;
	}

	/// <summary>Return exact native model-choice values; display labels and current selection are not advertised IDs.</summary>
	inline std::vector<std::string> ConfigModelIds(const AcpConfigOptionState& option)
	{
		std::vector<std::string> ids;
		for (const AcpConfigOptionChoiceState& choice : option.choices)
			uam::ranges::PushUniqueNonEmptyString(ids, choice.value);
		return ids;
	}

	/// <summary>Keep native IDs exact; repair only a unique advertised OpenCode casing alias.</summary>
	inline std::optional<std::string> ResolveAdvertisedModelId(const AcpSessionState& session, std::string_view requested, std::string& error)
	{
		error.clear();
		const std::string model_id = uam::strings::Trim(requested);
		const bool opencode = session.provider_id == uam::provider_ids::kOpenCodeCli;
		if (model_id.empty() || (!opencode && model_id == session.current_model_id)) return model_id;
		const auto option = std::ranges::find_if(session.available_config_options, [](const AcpConfigOptionState& value)
		{
			return value.category == "model" || value.id == "model";
		});
		std::vector<std::string> advertised;
		if (option != session.available_config_options.end())
		{
			advertised = ConfigModelIds(*option);
		}
		else
		{
			if (!opencode) return model_id;
			for (const AcpModelState& model : session.available_models) uam::ranges::PushUniqueNonEmptyString(advertised, model.id);
			if (advertised.empty()) return model_id;
		}
		if (std::ranges::find(advertised, model_id) != advertised.end()) return model_id;
		if (opencode)
		{
			std::optional<std::string> alias;
			for (const std::string& candidate : advertised)
			{
				if (!uam::strings::TrimmedEqualsIgnoreCase(candidate, model_id)) continue;
				if (alias)
				{
					error = "The saved model '" + model_id + "' matches multiple provider models. Select an exact available model before retrying.";
					return std::nullopt;
				}
				alias = candidate;
			}
			if (alias) return alias;
		}
		error = "The provider no longer offers the saved model '" + model_id + "'. Refresh models in Settings or restart UAM, then select an available model before retrying.";
		return std::nullopt;
	}


} // namespace uam::acp_models
