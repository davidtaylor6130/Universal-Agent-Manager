#pragma once

#include "common/config/build_features.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::provider_ids
{
	// ponytail: a provider excluded by its build flag gets an id that no chat, setting or native
	// record can carry. Shared comparisons stay valid, saved data cannot reactivate the provider and
	// its real id never reaches the binary. The tables below list only included providers.
	inline constexpr const char* kExcludedCliProviderId = "\x01";
	inline constexpr const char* kGeminiCli = UAM_ENABLE_RUNTIME_GEMINI_CLI ? "gemini-cli" : kExcludedCliProviderId;
	inline constexpr const char* kCodexCli = UAM_ENABLE_RUNTIME_CODEX_CLI ? "codex-cli" : kExcludedCliProviderId;
	inline constexpr const char* kClaudeCli = UAM_ENABLE_RUNTIME_CLAUDE_CLI ? "claude-cli" : kExcludedCliProviderId;
	inline constexpr const char* kOpenCodeCli = UAM_ENABLE_RUNTIME_OPENCODE_CLI ? "opencode-cli" : kExcludedCliProviderId;
	inline constexpr const char* kCopilotCli = UAM_ENABLE_RUNTIME_COPILOT_CLI ? "copilot-cli" : kExcludedCliProviderId;
	inline constexpr const char* kAntigravityCli = UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI ? "antigravity-cli" : kExcludedCliProviderId;

	/// <summary>Included providers in canonical order; the first is the default provider.</summary>
	inline constexpr auto kAllCliProviderIds = std::to_array<const char*>({
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    kGeminiCli,
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    kCodexCli,
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    kClaudeCli,
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	    kOpenCodeCli,
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	    kCopilotCli,
#endif
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	    kAntigravityCli,
#endif
	});
	static_assert(!kAllCliProviderIds.empty(), "At least one provider runtime must be enabled.");
	inline constexpr const char* kDefaultCliProviderId = kAllCliProviderIds.front();

	inline constexpr auto kVersionManagedCliProviderIds = std::to_array<const char*>({
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    kGeminiCli,
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    kCodexCli,
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    kClaudeCli,
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	    kOpenCodeCli,
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	    kCopilotCli,
#endif
#if !(UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_CODEX_CLI || UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_RUNTIME_OPENCODE_CLI || UAM_ENABLE_RUNTIME_COPILOT_CLI)
	    kExcludedCliProviderId, // Antigravity-only builds manage no CLI versions.
#endif
	});

	struct ProviderAlias
	{
		std::string_view alias;
		std::string_view provider_id;
	};

	inline constexpr auto kCliProviderAliases = std::to_array<ProviderAlias>({
#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
	    ProviderAlias{"agy", kAntigravityCli},
	    ProviderAlias{"antigravity", kAntigravityCli},
	    ProviderAlias{kAntigravityCli, kAntigravityCli},
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    ProviderAlias{"gemini", kGeminiCli},
	    ProviderAlias{kGeminiCli, kGeminiCli},
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    ProviderAlias{"codex", kCodexCli},
	    ProviderAlias{kCodexCli, kCodexCli},
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	    ProviderAlias{"claude", kClaudeCli},
	    ProviderAlias{"claude-code", kClaudeCli},
	    ProviderAlias{kClaudeCli, kClaudeCli},
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	    ProviderAlias{"opencode", kOpenCodeCli},
	    ProviderAlias{"open-code", kOpenCodeCli},
	    ProviderAlias{kOpenCodeCli, kOpenCodeCli},
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	    ProviderAlias{"copilot", kCopilotCli},
	    ProviderAlias{"github-copilot", kCopilotCli},
	    ProviderAlias{kCopilotCli, kCopilotCli},
#endif
	});

	template <std::size_t N> inline bool ContainsProviderId(const std::array<const char*, N>& provider_ids, std::string_view provider_id)
	{
		return uam::ranges::Contains(provider_ids, provider_id);
	}

	inline bool IsKnownCliProviderId(std::string_view provider_id)
	{
		return ContainsProviderId(kAllCliProviderIds, provider_id);
	}

	/// <summary>True only in a build that excludes providers, for a well-formed provider id it does not include. Saved data
	/// for such a provider stays on disk as inert data: runtime lookups use included ids, so it is never matched or used, and a
	/// fuller build finds it again. Full builds keep rejecting unknown ids.</summary>
	inline bool IsRetainedExcludedProviderId(std::string_view provider_id)
	{
#if UAM_ALL_PROVIDERS_INCLUDED
		(void)provider_id;
		return false;
#else
		return !provider_id.empty() && provider_id.size() <= 128 && provider_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string_view::npos &&
		    !IsKnownCliProviderId(provider_id);
#endif
	}

	inline bool IsVersionManagedCliProviderId(std::string_view provider_id)
	{
		return ContainsProviderId(kVersionManagedCliProviderIds, provider_id);
	}

	inline std::string NormalizeCliProviderAlias(std::string_view value)
	{
		const std::string normalized = uam::strings::TrimAndLowerAscii(value);
		for (const ProviderAlias& alias : kCliProviderAliases)
		{
			if (alias.alias == normalized)
			{
				return std::string(alias.provider_id);
			}
		}

		return "";
	}

	inline std::string NormalizeVersionManagedCliProviderId(std::string_view provider_id, std::string_view fallback = kDefaultCliProviderId)
	{
		const std::string normalized = NormalizeCliProviderAlias(provider_id);
		if (IsVersionManagedCliProviderId(normalized))
		{
			return normalized;
		}

		const std::string normalized_fallback = NormalizeCliProviderAlias(fallback);
		return IsVersionManagedCliProviderId(normalized_fallback) ? normalized_fallback : std::string(kVersionManagedCliProviderIds.front());
	}

	inline std::string NormalizeCliProviderAliasOrSelf(std::string_view value)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(value);
		const std::string normalized = NormalizeCliProviderAlias(trimmed);
		return normalized.empty() ? std::string(trimmed) : normalized;
	}

	inline std::string CanonicalCliProviderLookupId(std::string_view value)
	{
		return uam::strings::ToLowerAscii(NormalizeCliProviderAliasOrSelf(value));
	}

	inline bool IsCliProviderAliasOf(std::string_view value, std::string_view canonical_provider_id)
	{
		const std::string normalized_provider_id = NormalizeCliProviderAlias(value);
		const std::string normalized_target_provider_id = NormalizeCliProviderAlias(canonical_provider_id);
		return !normalized_provider_id.empty() && normalized_provider_id == normalized_target_provider_id;
	}

	inline bool IsLegacyOpenCodeLocalHistoryProviderId(std::string_view provider_id, std::string_view terminal_provider_id)
	{
		const std::string normalized_terminal = NormalizeCliProviderAliasOrSelf(terminal_provider_id);
		if (normalized_terminal != kOpenCodeCli)
		{
			return IsCliProviderAliasOf(provider_id, terminal_provider_id);
		}

		// Legacy opencode local history: blank provider_id is treated as opencode
		const std::string normalized_chat = NormalizeCliProviderAliasOrSelf(provider_id);
		return normalized_chat.empty() || normalized_chat == kOpenCodeCli;
	}

	inline void NormalizeLegacyLocalHistoryChatProvider(std::string& provider_id, std::string_view terminal_provider_id)
	{
		const std::string normalized_terminal = NormalizeCliProviderAliasOrSelf(terminal_provider_id);
		if (normalized_terminal != kOpenCodeCli)
		{
			return;
		}

		// Legacy opencode local history: blank provider_id becomes opencode-cli
		const std::string normalized_chat = NormalizeCliProviderAliasOrSelf(provider_id);
		if (normalized_chat.empty() || normalized_chat == kOpenCodeCli)
		{
			provider_id = kOpenCodeCli;
		}
	}

} // namespace uam::provider_ids
