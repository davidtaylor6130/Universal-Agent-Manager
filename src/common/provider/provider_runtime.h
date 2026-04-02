#pragma once

#include "app_models.h"
#include "provider_profile.h"

#include <string>
#include <vector>

/// <summary>
/// Provider-agnostic runtime adapter for prompt/command and history behavior.
/// </summary>
class ProviderRuntime
{
  public:
	/// <summary>Builds a provider prompt from user text and file references.</summary>
	static std::string BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files);

	/// <summary>Builds the provider command for one request.</summary>
	static std::string BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id);

	/// <summary>Builds interactive terminal argv for the active provider.</summary>
	static std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings);

	/// <summary>Maps provider-native message types to app message roles.</summary>
	static MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type);

	/// <summary>Returns true when provider uses Gemini JSON history files.</summary>
	static bool SupportsGeminiJsonHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider persists via local chat storage only.</summary>
	static bool UsesLocalHistory(const ProviderProfile& profile);
	/// <summary>Returns true when provider execution is internal engine backed.</summary>
	static bool UsesInternalEngine(const ProviderProfile& profile);
	/// <summary>Returns true when provider output is fixed to CLI terminal mode.</summary>
	static bool UsesCliOutput(const ProviderProfile& profile);
	/// <summary>Returns true when provider output is fixed to structured chat mode.</summary>
	static bool UsesStructuredOutput(const ProviderProfile& profile);
	/// <summary>Returns true when prompt bootstrap should use @.gemini path injection.</summary>
	static bool UsesGeminiPathBootstrap(const ProviderProfile& profile);
};
