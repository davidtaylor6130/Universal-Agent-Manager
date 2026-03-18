#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uam {

struct ProviderProfile {
  std::string id;
  std::string display_name;
  std::string command_template;
  bool supports_resume = false;
  std::vector<std::string> runtime_flags;
};

class ProviderProfileStore {
 public:
  static std::filesystem::path ProfilesFilePath(const std::filesystem::path& data_root);

  static ProviderProfile DefaultGeminiProfile();
  static std::vector<ProviderProfile> DefaultProfiles();

  // Loads only the user-editable file content. Defaults are not injected here.
  static std::vector<ProviderProfile> LoadUserProfiles(const std::filesystem::path& file_path,
                                                       std::string* error_out = nullptr);

  // Writes user-editable profiles back to disk in a stable, hand-editable text format.
  static bool SaveUserProfiles(const std::filesystem::path& file_path,
                               const std::vector<ProviderProfile>& profiles,
                               std::string* error_out = nullptr);

  // Loads built-ins plus user-defined overrides. User profiles with a matching id replace defaults.
  static std::vector<ProviderProfile> LoadProfiles(const std::filesystem::path& data_root,
                                                   std::string* error_out = nullptr);

  // Saves the provided profiles to the standard data-root file.
  static bool SaveProfiles(const std::filesystem::path& data_root,
                           const std::vector<ProviderProfile>& profiles,
                           std::string* error_out = nullptr);

  static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles,
                                         const std::string& provider_id);

  static std::vector<ProviderProfile> MergeProfiles(const std::vector<ProviderProfile>& base_profiles,
                                                    const std::vector<ProviderProfile>& overlay_profiles);
};

}  // namespace uam

