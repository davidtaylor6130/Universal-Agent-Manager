#pragma once

#include "app_models.h"

#include <filesystem>
#include <vector>

namespace uam {

std::vector<ChatSession> MergeNativeAndLocalChats(std::vector<ChatSession> native_chats,
                                                  const std::vector<ChatSession>& local_chats);

}  // namespace uam
