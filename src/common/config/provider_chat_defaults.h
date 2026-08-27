#pragma once

#include "common/config/approval_modes.h"
#include "common/models/app_models.h"
#include "common/memory/memory_levels.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/security/command_safety.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <string_view>

namespace uam::provider_chat_defaults
{
	inline bool IsAllowedModelId(std::string_view value)
	{
		if (value.empty())
		{
			return true;
		}
		if (value.size() > 160 || value.front() == '-')
		{
			return false;
		}
		return std::ranges::all_of(value, [](char ch) {
			return uam::strings::IsAsciiAlnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-' || ch == ':' || ch == '/';
		});
	}

	inline std::string NormalizeReasoningEffort(std::string_view provider_id, std::string_view value)
	{
		const std::string normalized = uam::codex::NormalizeReasoningEffort(value);
		if (!uam::provider_ids::IsCliProviderAliasOf(provider_id, uam::provider_ids::kCopilotCli))
		{
			return normalized;
		}
		return normalized == "ultra" ? "" : normalized;
	}

	inline ProviderChatDefaults Normalize(ProviderChatDefaults defaults, std::string_view provider_id)
	{
		defaults.model_id = uam::strings::Trim(defaults.model_id);
		defaults.reviewer_model_id = uam::strings::Trim(defaults.reviewer_model_id);
		if (!IsAllowedModelId(defaults.model_id))
		{
			defaults.model_id.clear();
		}
		if (!IsAllowedModelId(defaults.reviewer_model_id))
		{
			defaults.reviewer_model_id.clear();
		}
		defaults.feature_preference = defaults.feature_preference == "provider" ? "provider" : "uam";
		defaults.approval_mode = uam::approval_modes::NormalizeIncomingApprovalModeId(defaults.approval_mode);
		if (!uam::approval_modes::IsAppApprovalMode(defaults.approval_mode))
		{
			defaults.approval_mode = uam::approval_modes::kDefaultApprovalMode;
		}
		if (defaults.approval_mode == uam::approval_modes::kAcceptEditsApprovalMode)
		{
			defaults.approval_mode = uam::approval_modes::kDefaultApprovalMode;
			defaults.command_safety_tier = uam::approval_modes::kAcceptEditsApprovalMode;
		}
		defaults.command_safety_tier = uam::command_safety::NormalizeTier(defaults.command_safety_tier);
		defaults.reasoning_effort = NormalizeReasoningEffort(provider_id, defaults.reasoning_effort);
		defaults.service_tier = uam::codex::NormalizeServiceTier(defaults.service_tier);
		defaults.memory_level = uam::memory_levels::Normalize(defaults.memory_level, defaults.memory_enabled);
		defaults.memory_enabled = uam::memory_levels::IsEnabled(defaults.memory_level);
		return defaults;
	}

	inline ProviderChatDefaults ForProvider(const AppSettings& settings, std::string_view provider_id)
	{
		const std::string normalized_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
		const auto found = settings.provider_chat_defaults.find(normalized_provider_id);
		if (found != settings.provider_chat_defaults.end())
		{
			return Normalize(found->second, normalized_provider_id);
		}
		return ProviderChatDefaults{"", uam::approval_modes::kDefaultApprovalMode, "off", settings.memory_enabled_default, "", "", settings.memory_level_default};
	}

	inline void ApplyToChat(ChatSession& chat, ProviderChatDefaults defaults)
	{
		defaults = Normalize(defaults, chat.provider_id);
		if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli))
		{
			defaults.service_tier.clear();
		}
		chat.model_id = defaults.model_id;
		chat.reviewer_model_id = defaults.reviewer_model_id;
		chat.approval_mode = defaults.approval_mode;
		chat.command_safety_tier = defaults.command_safety_tier;
		chat.memory_level = defaults.memory_level;
		chat.memory_enabled = defaults.memory_enabled;
		chat.reasoning_effort = defaults.reasoning_effort;
		chat.service_tier = defaults.service_tier;
		chat.service_tier_explicit = !defaults.service_tier.empty();
		chat.small_model_mode = defaults.small_model_mode;
	}

	inline void ApplyToChat(const AppSettings& settings, ChatSession& chat)
	{
		ApplyToChat(chat, ForProvider(settings, chat.provider_id));
	}
} // namespace uam::provider_chat_defaults
