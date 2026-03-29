#pragma once

#include "app_models.h"
#include "provider_profile.h"

#include <filesystem>

namespace uam {

struct ImportSummary {
  bool ok = false;
  int chats_imported = 0;
  int chats_updated = 0;
  int chats_skipped = 0;
  int folders_imported = 0;
  int folders_updated = 0;
  int providers_imported = 0;
  int templates_imported = 0;
  std::string message;
};

std::optional<ChatSession> ParseGeminiSessionFileForImport(const std::filesystem::path& file_path,
                                                           const ProviderProfile& provider);

ImportSummary ImportGeminiChatsIntoLocalData(const std::filesystem::path& source_path,
                                             const std::filesystem::path& destination_data_root,
                                             const std::string& destination_provider_id);

ImportSummary ImportLegacyUamDataIntoLocalData(const std::filesystem::path& legacy_data_root,
                                               const std::filesystem::path& destination_data_root,
                                               const std::filesystem::path& destination_prompt_root);

}  // namespace uam
