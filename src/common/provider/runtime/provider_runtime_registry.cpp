#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#if UAM_ENABLE_RUNTIME_CODEX_CLI
#include "common/provider/codex/cli/codex_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#endif

namespace
{
	class UnsupportedProviderRuntime final : public IProviderRuntime
	{
	  public:
		const char* RuntimeId() const override { return "unsupported"; }
		bool IsEnabled() const override { return false; }
		const char* DisabledReason() const override { return "Selected provider runtime is not supported in this build."; }
		std::string BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>&) const override { return user_prompt; }
		std::string BuildCommand(const ProviderProfile&, const AppSettings&, const std::string&, const std::vector<std::string>&, const std::string&) const override { return ""; }
		std::vector<std::string> BuildInteractiveArgv(const ProviderProfile&, const ChatSession&, const AppSettings&) const override { return {}; }
		MessageRole RoleFromNativeType(const ProviderProfile&, const std::string&) const override { return MessageRole::System; }
		std::vector<ChatSession> LoadHistory(const ProviderProfile&, const std::filesystem::path&, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const override { return {}; }
		bool SaveHistory(const ProviderProfile&, const std::filesystem::path&, const ChatSession&) const override { return false; }
		bool UsesNativeOverlayHistory(const ProviderProfile&) const override { return false; }
		bool SupportsGeminiJsonHistory(const ProviderProfile&) const override { return false; }
		bool UsesLocalHistory(const ProviderProfile&) const override { return false; }
		bool UsesInternalEngine(const ProviderProfile&) const override { return false; }
		bool UsesCliOutput(const ProviderProfile&) const override { return false; }
		bool UsesGeminiPathBootstrap(const ProviderProfile&) const override { return false; }
	};

	const IProviderRuntime& GetUnsupportedProviderRuntime()
	{
		static const UnsupportedProviderRuntime runtime;
		return runtime;
	}

	std::string NormalizeRuntimeId(std::string provider_id)
	{
		provider_id = provider_runtime_internal::LowerAscii(provider_id);
		return provider_id;
	}
} // namespace

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	const std::string normalized = NormalizeRuntimeId(provider_id);
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	if (normalized == "gemini-cli")
	{
		return GetGeminiCliProviderRuntime();
	}
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	if (normalized == "codex-cli")
	{
		return GetCodexCliProviderRuntime();
	}
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	if (normalized == "claude-cli")
	{
		return GetClaudeCliProviderRuntime();
	}
#endif
	return GetUnsupportedProviderRuntime();
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	const std::string normalized = NormalizeRuntimeId(provider_id);
	return
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    normalized == "gemini-cli" ||
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    normalized == "codex-cli" ||
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    normalized == "claude-cli" ||
#endif
	    false;
}
