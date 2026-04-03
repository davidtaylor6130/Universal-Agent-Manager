#include "common/provider/provider_runtime.h"

#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#include "common/provider/codex/cli/codex_cli_provider_runtime.h"
#include "common/provider/custom/unknown_provider_runtime.h"
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#include "common/provider/gemini/structured/gemini_structured_provider_runtime.h"
#include "common/provider/ollama/structured/ollama_engine_provider_runtime.h"
#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#include "common/provider/opencode/local/opencode_local_provider_runtime.h"
#include "common/provider/runtime/provider_runtime_internal.h"

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);

	if (lower_id == "gemini-structured")
	{
		return GetGeminiStructuredProviderRuntime();
	}

	if (lower_id == "gemini-cli")
	{
		return GetGeminiCliProviderRuntime();
	}

	if (lower_id == "codex-cli")
	{
		return GetCodexCliProviderRuntime();
	}

	if (lower_id == "claude-cli")
	{
		return GetClaudeCliProviderRuntime();
	}

	if (lower_id == "opencode-cli")
	{
		return GetOpenCodeCliProviderRuntime();
	}

	if (lower_id == "opencode-local")
	{
		return GetOpenCodeLocalProviderRuntime();
	}

	if (lower_id == "ollama-engine")
	{
		return GetOllamaEngineProviderRuntime();
	}

	return GetUnknownProviderRuntime();
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);
	return lower_id == "gemini-structured" || lower_id == "gemini-cli" || lower_id == "codex-cli" || lower_id == "claude-cli" || lower_id == "opencode-cli" || lower_id == "opencode-local" || lower_id == "ollama-engine";
}
