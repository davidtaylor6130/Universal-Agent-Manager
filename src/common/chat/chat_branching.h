#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>
#include <vector>

/// <summary>
/// Applies branch lineage normalization and reparents branch children after deletions.
/// </summary>
class ChatBranching
{
  public:
	/// <summary>Normalizes parent/root branch metadata across a chat collection.</summary>
	static void Normalize(std::vector<ChatSession>& chats);
	/// <summary>Reparents descendants when a branch node is removed.</summary>
	static void ReparentChildrenAfterDelete(std::vector<ChatSession>& chats, std::string_view deleted_chat_id);
};
