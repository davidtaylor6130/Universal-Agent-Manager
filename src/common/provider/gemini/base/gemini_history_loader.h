#pragma once

#include "common/provider/provider_runtime.h"

#include <stop_token>

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir,
                                                         const ProviderProfile& profile,
                                                         const ProviderRuntimeHistoryLoadOptions& options,
                                                         std::stop_token stop_token = {});
