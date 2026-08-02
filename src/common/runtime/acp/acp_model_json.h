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
	struct CodexModelParseOptions
	{
		bool skip_hidden_field = true;
		bool allow_default_non_list_visibility = false;
	};

	struct ParsedCodexModelEntry
	{
		AcpModelState model;
		bool is_default = false;
	};

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

	inline std::optional<ParsedCodexModelEntry> ParseCodexModelEntry(const nlohmann::json& model, const CodexModelParseOptions& options = {})
	{
		if (!model.is_object())
		{
			return std::nullopt;
		}
		const nlohmann::json* hidden = uam::nlohmann_json::FindField(model, "hidden");
		if (options.skip_hidden_field && hidden != nullptr && hidden->is_boolean() && hidden->get<bool>())
		{
			return std::nullopt;
		}

		ParsedCodexModelEntry parsed;
		const nlohmann::json* is_default = uam::nlohmann_json::FindField(model, "isDefault");
		parsed.is_default = is_default != nullptr && is_default->is_boolean() && is_default->get<bool>();
		parsed.model.id = uam::nlohmann_json::TrimmedStringValue(model, {"id", "model", "slug", "modelId"});
		if (parsed.model.id.empty())
		{
			return std::nullopt;
		}

		const std::string visibility = uam::nlohmann_json::TrimmedStringValue(model, {"visibility"});
		if (!visibility.empty() && visibility != "list" && !(options.allow_default_non_list_visibility && parsed.is_default))
		{
			return std::nullopt;
		}

		parsed.model.name = uam::nlohmann_json::TrimmedStringValue(model, {"displayName", "display_name", "name"});
		if (parsed.model.name.empty())
		{
			parsed.model.name = parsed.model.id;
		}
		parsed.model.description = uam::nlohmann_json::TrimmedStringValue(model, {"description"});
		parsed.model.default_reasoning_effort = uam::codex::NormalizeReasoningEffort(uam::nlohmann_json::TrimmedStringValue(model, {"defaultReasoningEffort", "default_reasoning_effort", "defaultReasoningLevel", "default_reasoning_level"}));
		parsed.model.supported_reasoning_efforts = UniqueStringArrayValue(model, {"supportedReasoningEfforts", "supported_reasoning_levels"}, uam::codex::NormalizeReasoningEffort);
		parsed.model.additional_speed_tiers = UniqueStringArrayValue(model, {"additionalSpeedTiers", "additional_speed_tiers", "serviceTiers", "service_tiers"}, NormalizeModelServiceTier);
		return parsed;
	}
} // namespace uam::acp_models
