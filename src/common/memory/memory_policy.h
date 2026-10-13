#pragma once

#include "common/memory/memory_levels.h"
#include "common/models/app_models.h"
#include "common/provider/provider_ids.h"

namespace uam::memory_policy
{
	/// <summary>Resolve explicit chat policy, provider policy, then the current concrete app default.</summary>
	inline std::string Resolve(const AppSettings& settings, const ChatSession& chat)
	{
		const std::string selected = memory_levels::NormalizeSelection(chat.memory_level, chat.memory_enabled);
		if (selected != memory_levels::kDefault) return selected;
		const std::string provider = provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
		const std::map<std::string, ProviderChatDefaults>::const_iterator found = settings.provider_chat_defaults.find(provider);
		if (found != settings.provider_chat_defaults.end())
		{
			const std::string provider_level = memory_levels::NormalizeSelection(found->second.memory_level, found->second.memory_enabled);
			if (provider_level != memory_levels::kDefault) return provider_level;
		}
		return memory_levels::Normalize(settings.memory_level_default, settings.memory_enabled_default);
	}
} // namespace uam::memory_policy
