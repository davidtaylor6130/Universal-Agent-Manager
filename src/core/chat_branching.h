#pragma once

#include "app_models.h"

#include <string>
#include <vector>

class ChatBranching {
 public:
  static void Normalize(std::vector<ChatSession>& chats);
  static void ReparentChildrenAfterDelete(std::vector<ChatSession>& chats, const std::string& deleted_chat_id);
};
