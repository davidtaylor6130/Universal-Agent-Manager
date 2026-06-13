#pragma once

#include "common/provider/provider_profile_constants.h"
#include "common/provider/runtime/provider_build_config.h"

#include <string>
#include <string_view>
#include <vector>

/// <summary>
/// Provider runtime profile used by command/runtime adapters.
/// </summary>
struct ProviderProfile
{
	std::string id;
	std::string title;
	std::string execution_mode = uam::provider_profile_constants::kExecutionModeCli;
	std::string output_mode = uam::provider_profile_constants::kOutputModeStructured;
	std::string interactive_command;
	bool supports_cli = true;
	bool supports_structured = false;
	std::string structured_protocol = uam::provider_profile_constants::kProtocolNone;
	bool supports_interactive = true;
	bool supports_resume = true;
	std::vector<std::string> runtime_flags;
	std::string resume_argument = "-r";
	std::string history_adapter = provider_build_config::DefaultHistoryAdapter();
	std::string prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapPrepend;
	std::string prompt_bootstrap_path;
	std::vector<std::string> user_message_types;
	std::vector<std::string> assistant_message_types;
};

/// <summary>
/// Loads, saves, and resolves provider runtime profiles.
/// </summary>
class ProviderProfileStore
{
  public:
	/// <summary>Returns the built-in default Gemini CLI profile.</summary>
	static ProviderProfile DefaultGeminiProfile();
	/// <summary>Returns the built-in Codex CLI profile.</summary>
	static ProviderProfile DefaultCodexProfile();
	/// <summary>Returns the built-in Claude Code CLI profile.</summary>
	static ProviderProfile DefaultClaudeProfile();
	/// <summary>Returns the built-in OpenCode CLI profile.</summary>
	static ProviderProfile DefaultOpenCodeProfile();
	/// <summary>Returns the built-in GitHub Copilot CLI profile.</summary>
	static ProviderProfile DefaultCopilotProfile();
	/// <summary>Returns the built-in provider profiles for this build.</summary>
	static std::vector<ProviderProfile> BuiltInProfiles();
	/// <summary>Ensures enabled built-in provider profiles exist in the profile list.</summary>
	static void EnsureDefaultProfile(std::vector<ProviderProfile>& profiles);

	/// <summary>Finds a provider profile by id in a read-only collection.</summary>
	static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles, std::string_view id);
	/// <summary>Finds a provider profile by id in a mutable collection.</summary>
	static ProviderProfile* FindById(std::vector<ProviderProfile>& profiles, std::string_view id);
};
