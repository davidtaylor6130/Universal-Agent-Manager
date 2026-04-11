#pragma once

#include "common/provider/runtime/provider_build_config.h"

#include <filesystem>
#include <string>
#include <vector>

/// <summary>
/// Provider runtime profile used by command/runtime adapters.
/// </summary>
struct ProviderProfile
{
	std::string id;
	std::string title;
	std::string execution_mode = "cli";
	std::string output_mode = "structured";
	std::string command_template;
	std::string interactive_command;
	bool supports_interactive = true;
	bool supports_resume = true;
	std::vector<std::string> runtime_flags;
	std::string resume_argument = "-r";
	std::string history_adapter = provider_build_config::DefaultHistoryAdapter();
	std::string prompt_bootstrap = "prepend";
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
	/// <summary>Returns the built-in provider profiles for this build.</summary>
	static std::vector<ProviderProfile> BuiltInProfiles();
	/// <summary>Ensures the default Gemini CLI profile exists in the profile list.</summary>
	static void EnsureDefaultProfile(std::vector<ProviderProfile>& profiles);

	/// <summary>Finds a provider profile by id in a read-only collection.</summary>
	static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles, const std::string& id);
	/// <summary>Finds a provider profile by id in a mutable collection.</summary>
	static ProviderProfile* FindById(std::vector<ProviderProfile>& profiles, const std::string& id);
};
