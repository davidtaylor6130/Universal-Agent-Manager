#include "chat_sync.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uam {
namespace {

void ApplyLocalMetadata(ChatSession& native_chat, const ChatSession& local_chat) {
  if (!local_chat.title.empty()) {
    native_chat.title = local_chat.title;
  }
  native_chat.folder_id = local_chat.folder_id;
  native_chat.linked_files = local_chat.linked_files;
  if (!local_chat.created_at.empty()) {
    native_chat.created_at = local_chat.created_at;
  }
  if (!local_chat.updated_at.empty()) {
    native_chat.updated_at = local_chat.updated_at;
  }
}

void SortChatsByRecent(std::vector<ChatSession>& chats) {
  std::sort(chats.begin(), chats.end(), [](const ChatSession& a, const ChatSession& b) {
    return a.updated_at > b.updated_at;
  });
}

}  // namespace

std::vector<ChatSession> MergeNativeAndLocalChats(std::vector<ChatSession> native_chats,
                                                  const std::vector<ChatSession>& local_chats) {
  std::unordered_map<std::string, const ChatSession*> local_by_id;
  local_by_id.reserve(local_chats.size());
  std::unordered_set<std::string> consumed_local_ids;
  consumed_local_ids.reserve(local_chats.size());

  for (const ChatSession& local_chat : local_chats) {
    local_by_id.emplace(local_chat.id, &local_chat);
  }

  std::unordered_set<std::string> native_ids;
  native_ids.reserve(native_chats.size());
  for (ChatSession& native_chat : native_chats) {
    native_ids.insert(native_chat.id);
    const auto local_it = local_by_id.find(native_chat.id);
    if (local_it == local_by_id.end()) {
      continue;
    }
    consumed_local_ids.insert(local_it->second->id);
    ApplyLocalMetadata(native_chat, *local_it->second);
  }

  std::vector<ChatSession> merged = std::move(native_chats);
  for (const ChatSession& local_chat : local_chats) {
    if (consumed_local_ids.find(local_chat.id) != consumed_local_ids.end()) {
      continue;
    }
    if (local_chat.uses_native_session) {
      if (local_chat.native_session_id == local_chat.id && native_ids.find(local_chat.id) == native_ids.end()) {
        merged.push_back(local_chat);
      }
      continue;
    }
    merged.push_back(local_chat);
  }

  SortChatsByRecent(merged);
  return merged;
}

}  // namespace uam
