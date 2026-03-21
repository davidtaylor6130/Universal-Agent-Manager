#include "chat_branching.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::unordered_map<std::string, std::size_t> BuildIndexById(const std::vector<ChatSession>& chats) {
  std::unordered_map<std::string, std::size_t> index_by_id;
  index_by_id.reserve(chats.size());
  for (std::size_t i = 0; i < chats.size(); ++i) {
    if (!chats[i].id.empty()) {
      index_by_id[chats[i].id] = i;
    }
  }
  return index_by_id;
}

std::string ResolveRootId(const std::vector<ChatSession>& chats,
                          const std::unordered_map<std::string, std::size_t>& index_by_id,
                          const std::size_t start_index) {
  std::size_t cursor = start_index;
  std::unordered_set<std::string> seen;
  while (cursor < chats.size() && !chats[cursor].parent_chat_id.empty()) {
    const std::string& parent_id = chats[cursor].parent_chat_id;
    if (!seen.insert(parent_id).second) {
      break;
    }
    const auto it = index_by_id.find(parent_id);
    if (it == index_by_id.end()) {
      break;
    }
    cursor = it->second;
  }
  return (cursor < chats.size()) ? chats[cursor].id : chats[start_index].id;
}

void SetBranchRootForSubtree(std::vector<ChatSession>& chats,
                             const std::string& subtree_root_id,
                             const std::string& new_root_id) {
  if (subtree_root_id.empty() || new_root_id.empty()) {
    return;
  }

  std::vector<std::string> stack;
  stack.push_back(subtree_root_id);
  std::unordered_set<std::string> visited;
  while (!stack.empty()) {
    const std::string current_id = stack.back();
    stack.pop_back();
    if (!visited.insert(current_id).second) {
      continue;
    }

    for (ChatSession& chat : chats) {
      if (chat.id == current_id) {
        chat.branch_root_chat_id = new_root_id;
        if (chat.parent_chat_id.empty()) {
          chat.branch_from_message_index = -1;
        }
      }
      if (chat.parent_chat_id == current_id && !chat.id.empty()) {
        stack.push_back(chat.id);
      }
    }
  }
}

}  // namespace

void ChatBranching::Normalize(std::vector<ChatSession>& chats) {
  std::unordered_map<std::string, std::size_t> index_by_id = BuildIndexById(chats);

  for (ChatSession& chat : chats) {
    if (chat.id.empty()) {
      continue;
    }
    if (chat.parent_chat_id == chat.id) {
      chat.parent_chat_id.clear();
    }
    if (!chat.parent_chat_id.empty() && index_by_id.find(chat.parent_chat_id) == index_by_id.end()) {
      chat.parent_chat_id.clear();
    }
  }

  index_by_id = BuildIndexById(chats);
  for (std::size_t i = 0; i < chats.size(); ++i) {
    ChatSession& chat = chats[i];
    if (chat.id.empty()) {
      continue;
    }
    if (chat.parent_chat_id.empty()) {
      chat.branch_root_chat_id = chat.id;
      chat.branch_from_message_index = -1;
      continue;
    }

    const std::string resolved_root = ResolveRootId(chats, index_by_id, i);
    chat.branch_root_chat_id = resolved_root.empty() ? chat.id : resolved_root;
    if (chat.branch_from_message_index < 0) {
      chat.branch_from_message_index = 0;
    }
  }
}

void ChatBranching::ReparentChildrenAfterDelete(std::vector<ChatSession>& chats, const std::string& deleted_chat_id) {
  if (deleted_chat_id.empty()) {
    return;
  }
  Normalize(chats);

  std::unordered_map<std::string, std::size_t> index_by_id = BuildIndexById(chats);
  const auto deleted_it = index_by_id.find(deleted_chat_id);
  if (deleted_it == index_by_id.end()) {
    return;
  }

  const ChatSession& deleted_chat = chats[deleted_it->second];
  const std::string parent_id = deleted_chat.parent_chat_id;

  std::vector<std::string> direct_child_ids;
  for (const ChatSession& chat : chats) {
    if (chat.parent_chat_id == deleted_chat_id && !chat.id.empty()) {
      direct_child_ids.push_back(chat.id);
    }
  }

  for (const std::string& child_id : direct_child_ids) {
    const auto child_it = index_by_id.find(child_id);
    if (child_it == index_by_id.end()) {
      continue;
    }
    ChatSession& child = chats[child_it->second];
    child.parent_chat_id = parent_id;

    if (parent_id.empty()) {
      child.branch_from_message_index = -1;
      SetBranchRootForSubtree(chats, child.id, child.id);
      continue;
    }

    const auto parent_it = index_by_id.find(parent_id);
    if (parent_it != index_by_id.end()) {
      const ChatSession& parent_chat = chats[parent_it->second];
      child.branch_root_chat_id = parent_chat.branch_root_chat_id.empty()
                                      ? parent_chat.id
                                      : parent_chat.branch_root_chat_id;
    }
  }

  Normalize(chats);
}
