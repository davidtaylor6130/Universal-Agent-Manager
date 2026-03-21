#pragma once

#include "app_models.h"

#include <string>
#include <vector>

/// <summary>
/// Applies branch lineage normalization and reparents branch children after deletions.
/// </summary>
class ChatBranching {
 public:
  /// <summary>Normalizes parent/root branch metadata across a chat collection.</summary>
  static void Normalize(std::vector<ChatSession>& chats);
  /// <summary>Reparents descendants when a branch node is removed.</summary>
  static void ReparentChildrenAfterDelete(std::vector<ChatSession>& chats, const std::string& deleted_chat_id);
};
