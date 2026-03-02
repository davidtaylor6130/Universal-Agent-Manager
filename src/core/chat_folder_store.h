#pragma once

#include "app_models.h"

#include <filesystem>
#include <vector>

class ChatFolderStore {
 public:
  static std::vector<ChatFolder> Load(const std::filesystem::path& data_root);
  static bool Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
};
