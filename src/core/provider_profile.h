#pragma once

#include <filesystem>
#include <string>
#include <vector>

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

class ProviderProfileStore {
 public:
  static std::vector<ProviderProfile> Load(const std::filesystem::path& data_root);
  static bool Save(const std::filesystem::path& data_root, const std::vector<ProviderProfile>& profiles);

  static ProviderProfile DefaultGeminiProfile();
  static void EnsureDefaultProfile(std::vector<ProviderProfile>& profiles);

  static const ProviderProfile* FindById(const std::vector<ProviderProfile>& profiles, const std::string& id);
  static ProviderProfile* FindById(std::vector<ProviderProfile>& profiles, const std::string& id);
};
