#pragma once

#include "common/config/approval_modes.h"
#include "common/models/app_models.h"
#include "common/memory/memory_levels.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
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

	inline ProviderChatDefaults Normalize(ProviderChatDefaults defaults)
	{
		defaults.model_id = uam::strings::Trim(defaults.model_id);
		if (!IsAllowedModelId(defaults.model_id))
		{
			defaults.model_id.clear();
		}
		defaults.approval_mode = uam::approval_modes::NormalizeIncomingApprovalModeId(defaults.approval_mode);
		if (!uam::approval_modes::IsAppApprovalMode(defaults.approval_mode))
		{
			defaults.approval_mode = uam::approval_modes::kDefaultApprovalMode;
		}
		defaults.reasoning_effort = uam::codex::NormalizeReasoningEffort(defaults.reasoning_effort);
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
			return Normalize(found->second);
		}
		return ProviderChatDefaults{"", uam::approval_modes::kDefaultApprovalMode, false, settings.memory_enabled_default, "", "", settings.memory_level_default};
	}

	inline void ApplyToChat(ChatSession& chat, ProviderChatDefaults defaults)
	{
		if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kCodexCli))
		{
			defaults.reasoning_effort.clear();
			defaults.service_tier.clear();
		}
		chat.model_id = defaults.model_id;
		chat.approval_mode = defaults.approval_mode == uam::approval_modes::kAcceptEditsApprovalMode ? uam::approval_modes::kDefaultApprovalMode : defaults.approval_mode;
		if (defaults.approval_mode == uam::approval_modes::kAcceptEditsApprovalMode) chat.command_safety_tier = uam::approval_modes::kAcceptEditsApprovalMode;
		chat.auto_approve_commands = defaults.auto_approve_commands;
		chat.memory_level = defaults.memory_level;
		chat.memory_enabled = defaults.memory_enabled;
		chat.reasoning_effort = defaults.reasoning_effort;
		chat.service_tier = defaults.service_tier;
	}

	inline void ApplyToChat(const AppSettings& settings, ChatSession& chat)
	{
		ApplyToChat(chat, ForProvider(settings, chat.provider_id));
	}
} // namespace uam::provider_chat_defaults
