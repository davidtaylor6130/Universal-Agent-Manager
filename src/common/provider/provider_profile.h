#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// <summary>
/// Provider runtime profile used by command/runtime adapters.
/// </summary>
struct ProviderProfile {
  std::string id;
  std::string title;
  std::string command_template;
  std::string interactive_command;
  bool supports_resume = true;
  std::vector<std::string> runtime_flags;
  std::string resume_argument = "--resume";
  std::string history_adapter = "gemini-cli-json";
  std::vector<std::string> user_message_types;
  std::vector<std::string> assistant_message_types;
};

/// <summary>
/// Loads, saves, and resolves provider runtime profiles.
/// </summary>
class ProviderProfileStore {
 public:
  /// <summary>Loads provider profiles from disk for the current data root.</summary>
  static std::vector<ProviderProfile> Load(const std::filesystem::path& data_root);
  /// <summary>Saves provider profiles to disk for the current data root.</summary>
  static bool Save(const std::filesystem::path& data_root, const std::vector<ProviderProfile>& profiles);

  /// <summary>Returns the built-in default Gemini profile.</summary>
  static ProviderProfile DefaultGeminiProfile();
  /// <summary>Ensures the default Gemini profile exists in the profile list.</summary>
  static void EnsureDefaultProfile(std::vector<ProviderProfile>& profiles);

  /// <summary>Finds a provider profile by id in a read-only collection.</summary>
  static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles, const std::string& id);
  /// <summary>Finds a provider profile by id in a mutable collection.</summary>
  static ProviderProfile* FindById(std::vector<ProviderProfile>& profiles, const std::string& id);
};
