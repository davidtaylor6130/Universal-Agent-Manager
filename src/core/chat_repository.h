#pragma once

#include "app_models.h"

#include <filesystem>
#include <vector>

class ChatRepository {
 public:
  static bool SaveChat(const std::filesystem::path& data_root, const ChatSession& chat);
  static std::vector<ChatSession> LoadLocalChats(const std::filesystem::path& data_root);
};
