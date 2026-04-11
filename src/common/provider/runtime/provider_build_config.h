#pragma once

#ifndef UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
#define UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED 0
#endif

#ifndef UAM_ENABLE_RUNTIME_GEMINI_CLI
#define UAM_ENABLE_RUNTIME_GEMINI_CLI 1
#endif

#ifndef UAM_ENABLE_RUNTIME_CODEX_CLI
#define UAM_ENABLE_RUNTIME_CODEX_CLI 0
#endif

#ifndef UAM_ENABLE_RUNTIME_CLAUDE_CLI
#define UAM_ENABLE_RUNTIME_CLAUDE_CLI 0
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_CLI
#define UAM_ENABLE_RUNTIME_OPENCODE_CLI 0
#endif

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
#define UAM_ENABLE_RUNTIME_OPENCODE_LOCAL 0
#endif

#ifndef UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
#define UAM_ENABLE_RUNTIME_OLLAMA_ENGINE 0
#endif

#ifndef UAM_ENABLE_ENGINE_RAG
#define UAM_ENABLE_ENGINE_RAG 0
#endif

#ifndef UAM_ENABLE_ANY_GEMINI_PROVIDER
#define UAM_ENABLE_ANY_GEMINI_PROVIDER UAM_ENABLE_RUNTIME_GEMINI_CLI
#endif

namespace provider_build_config
{

	inline constexpr const char* FirstEnabledProviderId()
	{
		return "gemini-cli";
	}

	inline constexpr const char* DefaultVectorDbBackend()
	{
		return "none";
	}

	inline constexpr const char* DefaultHistoryAdapter()
	{
		return "gemini-cli-json";
	}

	inline constexpr const char* DefaultNativeHistoryProviderId()
	{
		return "gemini-cli";
	}

	inline constexpr bool HasNativeHistoryProvider()
	{
		return true;
	}

} // namespace provider_build_config
