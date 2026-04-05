#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
#include "common/provider/codex/cli/codex_cli_provider_runtime.h"
#endif
#include "common/provider/custom/unknown_provider_runtime.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
#include "common/provider/gemini/structured/gemini_structured_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
#include "common/provider/ollama/structured/ollama_engine_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
#include "common/provider/opencode/local/opencode_local_provider_runtime.h"
#endif

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);

#if UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
	if (lower_id == "gemini-structured")
	{
		return GetGeminiStructuredProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	if (lower_id == "gemini-cli")
	{
		return GetGeminiCliProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
	if (lower_id == "codex-cli")
	{
		return GetCodexCliProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	if (lower_id == "claude-cli")
	{
		return GetClaudeCliProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	if (lower_id == "opencode-cli")
	{
		return GetOpenCodeCliProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
	if (lower_id == "opencode-local")
	{
		return GetOpenCodeLocalProviderRuntime();
	}
#endif

#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	if (lower_id == "ollama-engine")
	{
		return GetOllamaEngineProviderRuntime();
	}
#endif

	return GetUnknownProviderRuntime();
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);
	bool known = false;
#if UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
	known = known || (lower_id == "gemini-structured");
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	known = known || (lower_id == "gemini-cli");
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	known = known || (lower_id == "codex-cli");
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	known = known || (lower_id == "claude-cli");
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	known = known || (lower_id == "opencode-cli");
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
	known = known || (lower_id == "opencode-local");
#endif
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	known = known || (lower_id == "ollama-engine");
#endif
	return known;
}
