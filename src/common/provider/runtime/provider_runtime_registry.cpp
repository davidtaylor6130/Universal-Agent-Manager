#include "common/provider/provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"

#if UAM_ENABLE_RUNTIME_CODEX_CLI
#include "common/provider/codex/cli/codex_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"
#endif

namespace
{
	class UnsupportedProviderRuntime final : public IProviderRuntime
	{
	  public:
		const char* RuntimeId() const override
		{
			return "unsupported";
		}
		bool IsEnabled() const override
		{
			return false;
		}
		const char* DisabledReason() const override
		{
			return "Selected provider runtime is not supported in this build.";
		}
		std::string BuildPrompt(const ProviderProfile&, std::string_view user_prompt, const std::vector<std::string>&, const Goal*, int64_t, int64_t) const override
		{
			return std::string(user_prompt);
		}
		std::string BuildCommand(const ProviderProfile&, const AppSettings&, std::string_view, const std::vector<std::string>&, const std::string&, const ChatSession*) const override
		{
			return "";
		}
		std::vector<std::string> BuildInteractiveArgv(const ProviderProfile&, const ChatSession&, const AppSettings&) const override
		{
			return {};
		}
		MessageRole RoleFromNativeType(const ProviderProfile&, std::string_view) const override
		{
			return MessageRole::System;
		}
		std::vector<ChatSession> LoadHistory(const ProviderProfile&, const std::filesystem::path&, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const override
		{
			return {};
		}
		bool SaveHistory(const ProviderProfile&, const std::filesystem::path&, const ChatSession&) const override
		{
			return false;
		}
		bool UsesNativeOverlayHistory(const ProviderProfile&) const override
		{
			return false;
		}
		bool SupportsGeminiJsonHistory(const ProviderProfile&) const override
		{
			return false;
		}
		bool UsesLocalHistory(const ProviderProfile&) const override
		{
			return false;
		}
		bool UsesInternalEngine(const ProviderProfile&) const override
		{
			return false;
		}
		bool UsesCliOutput(const ProviderProfile&) const override
		{
			return false;
		}
		bool UsesGeminiPathBootstrap(const ProviderProfile&) const override
		{
			return false;
		}
	};

	const IProviderRuntime& GetUnsupportedProviderRuntime()
	{
		static const UnsupportedProviderRuntime runtime;
		return runtime;
	}

	const IProviderRuntime* RuntimeForEnabledId(std::string_view normalized)
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		if (normalized == uam::provider_ids::kGeminiCli)
		{
			return &GetGeminiCliProviderRuntime();
		}
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		if (normalized == uam::provider_ids::kCodexCli)
		{
			return &GetCodexCliProviderRuntime();
		}
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		if (normalized == uam::provider_ids::kClaudeCli)
		{
			return &GetClaudeCliProviderRuntime();
		}
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		if (normalized == uam::provider_ids::kOpenCodeCli)
		{
			return &GetOpenCodeCliProviderRuntime();
		}
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		if (normalized == uam::provider_ids::kCopilotCli)
		{
			return &GetCopilotCliProviderRuntime();
		}
#endif
		return nullptr;
	}

	const IProviderRuntime* RuntimeForProviderId(std::string_view provider_id)
	{
		const std::string normalized = uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
		return RuntimeForEnabledId(normalized);
	}
} // namespace

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(std::string_view provider_id)
{
	if (const IProviderRuntime* runtime = RuntimeForProviderId(provider_id))
	{
		return *runtime;
	}

	return GetUnsupportedProviderRuntime();
}

bool ProviderRuntimeRegistry::IsEnabledRuntimeId(std::string_view provider_id)
{
	return RuntimeForProviderId(provider_id) != nullptr;
}
