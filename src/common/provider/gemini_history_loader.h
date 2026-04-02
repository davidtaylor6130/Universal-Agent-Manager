#pragma once

#include "provider_runtime.h"

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options);
