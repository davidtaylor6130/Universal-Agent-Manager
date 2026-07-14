#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::provider_ids
{
	inline constexpr const char* kGeminiCli = "gemini-cli";
	inline constexpr const char* kCodexCli = "codex-cli";
	inline constexpr const char* kClaudeCli = "claude-cli";
	inline constexpr const char* kOpenCodeCli = "opencode-cli";
	inline constexpr const char* kCopilotCli = "copilot-cli";

	inline constexpr auto kAllCliProviderIds = std::to_array<const char*>({
	    kGeminiCli, kCodexCli, kClaudeCli, kOpenCodeCli, kCopilotCli,
	});

	inline constexpr auto kVersionManagedCliProviderIds = std::to_array<const char*>({
	    kGeminiCli,
	    kCodexCli,
	    kClaudeCli,
	    kOpenCodeCli,
	    kCopilotCli,
	});

	struct ProviderAlias
	{
		std::string_view alias;
		std::string_view provider_id;
	};

	inline constexpr auto kCliProviderAliases = std::to_array<ProviderAlias>({
	    ProviderAlias{"gemini", kGeminiCli},
	    ProviderAlias{kGeminiCli, kGeminiCli},
	    ProviderAlias{"codex", kCodexCli},
	    ProviderAlias{kCodexCli, kCodexCli},
	    ProviderAlias{"claude", kClaudeCli},
	    ProviderAlias{"claude-code", kClaudeCli},
	    ProviderAlias{kClaudeCli, kClaudeCli},
	    ProviderAlias{"opencode", kOpenCodeCli},
	    ProviderAlias{"open-code", kOpenCodeCli},
	    ProviderAlias{kOpenCodeCli, kOpenCodeCli},
	    ProviderAlias{"copilot", kCopilotCli},
	    ProviderAlias{"github-copilot", kCopilotCli},
	    ProviderAlias{kCopilotCli, kCopilotCli},
	});

	template <std::size_t N> inline bool ContainsProviderId(const std::array<const char*, N>& provider_ids, std::string_view provider_id)
	{
		return uam::ranges::Contains(provider_ids, provider_id);
	}

	inline bool IsKnownCliProviderId(std::string_view provider_id)
	{
		return ContainsProviderId(kAllCliProviderIds, provider_id);
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

	inline std::string NormalizeVersionManagedCliProviderId(std::string_view provider_id, std::string_view fallback = kGeminiCli)
	{
		const std::string normalized = NormalizeCliProviderAlias(provider_id);
		if (IsVersionManagedCliProviderId(normalized))
		{
			return normalized;
		}

		const std::string normalized_fallback = NormalizeCliProviderAlias(fallback);
		return IsVersionManagedCliProviderId(normalized_fallback) ? normalized_fallback : std::string(kGeminiCli);
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
