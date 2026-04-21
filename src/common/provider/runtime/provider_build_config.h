#pragma once

#ifndef UAM_ENABLE_RUNTIME_GEMINI_CLI
#error "UAM_ENABLE_RUNTIME_GEMINI_CLI must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_CODEX_CLI
#error "UAM_ENABLE_RUNTIME_CODEX_CLI must be defined by CMake. Use 0 or 1."
#endif

namespace provider_build_config
{

	inline constexpr bool GeminiCliEnabled()
	{
		return UAM_ENABLE_RUNTIME_GEMINI_CLI != 0;
	}

	inline constexpr bool CodexCliEnabled()
	{
		return UAM_ENABLE_RUNTIME_CODEX_CLI != 0;
	}

	inline constexpr const char* FirstEnabledProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return "gemini-cli";
#elif UAM_ENABLE_RUNTIME_CODEX_CLI
		return "codex-cli";
#else
		return "";
#endif
	}

	inline constexpr const char* DefaultVectorDbBackend()
	{
		return "none";
	}

	inline constexpr const char* DefaultHistoryAdapter()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return "gemini-cli-json";
#elif UAM_ENABLE_RUNTIME_CODEX_CLI
		return "local-json";
#else
		return "local-json";
#endif
	}

	inline constexpr const char* DefaultNativeHistoryProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return "gemini-cli";
#else
		return "";
#endif
	}

	inline constexpr bool HasNativeHistoryProvider()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return true;
#else
		return false;
#endif
	}

} // namespace provider_build_config
