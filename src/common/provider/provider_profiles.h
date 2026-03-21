#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uam {

/// <summary>
/// Legacy user-editable provider profile model.
/// </summary>
struct ProviderProfile {
  std::string id;
  std::string display_name;
  std::string command_template;
  bool supports_resume = false;
  std::vector<std::string> runtime_flags;
};

/// <summary>
/// Legacy provider profile store used for profile-file workflows.
/// </summary>
class ProviderProfileStore {
 public:
  /// <summary>Returns the provider profile file path for a data root.</summary>
  static std::filesystem::path ProfilesFilePath(const std::filesystem::path& data_root);

  /// <summary>Returns the built-in default Gemini profile.</summary>
  static ProviderProfile DefaultGeminiProfile();
  /// <summary>Returns all built-in provider profiles.</summary>
  static std::vector<ProviderProfile> DefaultProfiles();

  /// <summary>Loads only user-editable profile records from disk.</summary>
  static std::vector<ProviderProfile> LoadUserProfiles(const std::filesystem::path& file_path,
                                                       std::string* error_out = nullptr);

  /// <summary>Saves user-editable profile records to disk.</summary>
  static bool SaveUserProfiles(const std::filesystem::path& file_path,
                               const std::vector<ProviderProfile>& profiles,
                               std::string* error_out = nullptr);

  /// <summary>Loads defaults plus user overrides merged by id.</summary>
  static std::vector<ProviderProfile> LoadProfiles(const std::filesystem::path& data_root,
                                                   std::string* error_out = nullptr);

  /// <summary>Saves merged profiles into the standard data-root profile file.</summary>
  static bool SaveProfiles(const std::filesystem::path& data_root,
                           const std::vector<ProviderProfile>& profiles,
                           std::string* error_out = nullptr);

  /// <summary>Finds a profile by id in a collection.</summary>
  static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles,
                                         const std::string& provider_id);

  /// <summary>Merges overlay profiles onto base profiles by id.</summary>
  static std::vector<ProviderProfile> MergeProfiles(const std::vector<ProviderProfile>& base_profiles,
                                                    const std::vector<ProviderProfile>& overlay_profiles);
};

}  // namespace uam
