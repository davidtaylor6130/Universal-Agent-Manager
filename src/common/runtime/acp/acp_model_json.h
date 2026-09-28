#pragma once

#include "common/provider/codex/codex_options.h"
#include "common/state/app_state.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

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
		return parsed;
	}


} // namespace uam::acp_models
