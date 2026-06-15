#pragma once

#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace uam::provider_profile_constants
{
	inline constexpr const char* kNone = "none";

	inline constexpr const char* kExecutionModeCli = "cli";
	inline constexpr const char* kOutputModeCli = "cli";
	inline constexpr const char* kOutputModeStructured = "structured";

	inline constexpr const char* kProtocolNone = kNone;
	inline constexpr const char* kProtocolGeminiAcp = "gemini-acp";
	inline constexpr const char* kProtocolCodexAppServer = "codex-app-server";
	inline constexpr const char* kProtocolClaudeCodeStreamJson = "claude-code-stream-json";
	inline constexpr const char* kProtocolOpenCodeAcp = "opencode-acp";
	inline constexpr const char* kProtocolCopilotAcp = "copilot-acp";

	inline constexpr const char* kHistoryAdapterGeminiCliJson = "gemini-cli-json";
	inline constexpr const char* kHistoryAdapterLocalJson = "local-json";

	inline constexpr const char* kPromptBootstrapPrepend = "prepend";
	inline constexpr const char* kPromptBootstrapNone = kNone;
	inline constexpr const char* kPromptBootstrapGeminiAtPath = "gemini-at-path";
	inline constexpr const char* kGeminiPromptBootstrapPath = "@.gemini/gemini.md";

	inline std::string StructuredProtocolOrGemini(std::string_view protocol)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(protocol);
		return trimmed.empty() ? std::string(kProtocolGeminiAcp) : std::string(trimmed);
	}

	inline bool IsGeminiJsonHistoryAdapter(std::string_view adapter)
	{
		return uam::strings::TrimmedEqualsIgnoreCase(adapter, kHistoryAdapterGeminiCliJson);
	}
} // namespace uam::provider_profile_constants
