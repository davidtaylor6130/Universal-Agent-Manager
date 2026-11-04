#pragma once

#include "common/models/app_models.h"

#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

/// <summary>
/// Applies branch lineage normalization and reparents branch children after deletions.
/// </summary>
class ChatBranching
{
  public:
	/// <summary>Normalizes parent/root branch metadata across a chat collection.</summary>
	static void Normalize(std::vector<ChatSession>& chats);
	/// <summary>Copies only the identity and lineage fields Normalize reads, never transcripts.</summary>
	static ChatSession LineageOnly(const ChatSession& chat);
	/// <summary>True when Normalize turns a chat's saved parent/root/index into its current lineage in this collection.</summary>
	static bool SavedLineageNormalizesToCurrent(const std::vector<ChatSession>& chats, std::string_view chat_id,
	    const std::string& parent_chat_id, const std::string& branch_root_chat_id, int branch_from_message_index);
	/// <summary>Reparents descendants when a branch node is removed.</summary>
	static void ReparentChildrenAfterDelete(std::vector<ChatSession>& chats, std::string_view deleted_chat_id);
	/// <summary>Reparents survivors to the nearest remaining ancestor before a batch is removed.</summary>
	static void ReparentChildrenAfterDeletes(std::vector<ChatSession>& chats, const std::unordered_set<std::string>& deleted_ids);
};
