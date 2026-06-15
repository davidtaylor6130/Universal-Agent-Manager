#pragma once

#ifndef UAM_ENABLE_RUNTIME_GEMINI_CLI
#error "UAM_ENABLE_RUNTIME_GEMINI_CLI must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_CODEX_CLI
#error "UAM_ENABLE_RUNTIME_CODEX_CLI must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_CLAUDE_CLI
#error "UAM_ENABLE_RUNTIME_CLAUDE_CLI must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_CLI
#error "UAM_ENABLE_RUNTIME_OPENCODE_CLI must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_COPILOT_CLI
#error "UAM_ENABLE_RUNTIME_COPILOT_CLI must be defined by CMake. Use 0 or 1."
#endif

#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile_constants.h"
#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace provider_build_config
{

	inline constexpr bool ProviderEnabled(std::string_view provider_id)
	{
		return
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		    provider_id == uam::provider_ids::kGeminiCli ||
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		    provider_id == uam::provider_ids::kCodexCli ||
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		    provider_id == uam::provider_ids::kClaudeCli ||
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		    provider_id == uam::provider_ids::kOpenCodeCli ||
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		    provider_id == uam::provider_ids::kCopilotCli ||
#endif
		    false;
	}

	inline constexpr const char* FirstEnabledProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return uam::provider_ids::kGeminiCli;
#elif UAM_ENABLE_RUNTIME_CODEX_CLI
		return uam::provider_ids::kCodexCli;
#elif UAM_ENABLE_RUNTIME_CLAUDE_CLI
		return uam::provider_ids::kClaudeCli;
#elif UAM_ENABLE_RUNTIME_OPENCODE_CLI
		return uam::provider_ids::kOpenCodeCli;
#elif UAM_ENABLE_RUNTIME_COPILOT_CLI
		return uam::provider_ids::kCopilotCli;
#else
		return "";
#endif
	}

	inline constexpr const char* DefaultHistoryAdapter()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return uam::provider_profile_constants::kHistoryAdapterGeminiCliJson;
#else
		return uam::provider_profile_constants::kHistoryAdapterLocalJson;
#endif
}

inline constexpr const char* DefaultNativeHistoryProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return uam::provider_ids::kGeminiCli;
#else
		return "";
#endif
	}

	inline std::string EnabledCliProviderIdOrFirst(std::string_view provider_id)
	{
		const std::string normalized = uam::provider_ids::NormalizeCliProviderAlias(provider_id);
		if (!normalized.empty() && ProviderEnabled(normalized))
		{
			return normalized;
		}

		return FirstEnabledProviderId();
	}

	inline std::string NativeHistoryProviderIdOrFirst()
	{
		const std::string native_provider_id = DefaultNativeHistoryProviderId();
		return uam::strings::NonEmptyOrFallback(native_provider_id, FirstEnabledProviderId());
	}

} // namespace provider_build_config
