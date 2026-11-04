#pragma once

#include "common/config/provider_chat_defaults.h"
#include "common/memory/memory_levels.h"
#include "common/models/app_models.h"

namespace uam::memory_policy
{
	/// <summary>Resolve explicit chat policy, environment-scoped provider policy, then the current concrete app default.</summary>
	inline std::string Resolve(const AppSettings& settings, const ChatSession& chat)
	{
		const std::string selected = memory_levels::NormalizeSelection(chat.memory_level, chat.memory_enabled);
		if (selected != memory_levels::kDefault) return selected;
		const ProviderChatDefaults defaults = provider_chat_defaults::ForProvider(settings, chat.provider_id, chat.execution_host_id);
		const std::string provider_level = memory_levels::NormalizeSelection(defaults.memory_level, defaults.memory_enabled);
		if (provider_level != memory_levels::kDefault) return provider_level;
		return memory_levels::Normalize(settings.memory_level_default, settings.memory_enabled_default);
	}
} // namespace uam::memory_policy
