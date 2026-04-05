#pragma once

#ifndef UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
#error "UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED must be defined by CMake. Use 0 or 1."
#endif

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

#ifndef UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
#error "UAM_ENABLE_RUNTIME_OPENCODE_LOCAL must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
#error "UAM_ENABLE_RUNTIME_OLLAMA_ENGINE must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_ENGINE_RAG
#error "UAM_ENABLE_ENGINE_RAG must be defined by CMake. Use 0 or 1."
#endif

#ifndef UAM_ENABLE_ANY_GEMINI_PROVIDER
#define UAM_ENABLE_ANY_GEMINI_PROVIDER (UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED || UAM_ENABLE_RUNTIME_GEMINI_CLI)
#endif

namespace provider_build_config
{

	inline constexpr const char* FirstEnabledProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
		return "gemini-structured";
#elif UAM_ENABLE_RUNTIME_GEMINI_CLI
		return "gemini-cli";
#elif UAM_ENABLE_RUNTIME_CODEX_CLI
		return "codex-cli";
#elif UAM_ENABLE_RUNTIME_CLAUDE_CLI
		return "claude-cli";
#elif UAM_ENABLE_RUNTIME_OPENCODE_CLI
		return "opencode-cli";
#elif UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
		return "opencode-local";
#elif UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
		return "ollama-engine";
#else
		return "";
#endif
	}

	inline constexpr const char* DefaultVectorDbBackend()
	{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
		return "ollama-engine";
#else
		return "none";
#endif
	}

	inline constexpr const char* DefaultHistoryAdapter()
	{
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
		return "gemini-cli-json";
#else
		return "local-only";
#endif
	}

	inline constexpr const char* DefaultNativeHistoryProviderId()
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return "gemini-cli";
#elif UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
		return "gemini-structured";
#else
		return "";
#endif
	}

	inline constexpr bool HasNativeHistoryProvider()
	{
#if UAM_ENABLE_ANY_GEMINI_PROVIDER
		return true;
#else
		return false;
#endif
	}

} // namespace provider_build_config
