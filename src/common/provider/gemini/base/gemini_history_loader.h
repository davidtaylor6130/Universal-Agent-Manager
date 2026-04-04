#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <vector>

struct GeminiJsonHistoryStoreOptions
{
	std::uintmax_t max_file_bytes = 0;
	std::size_t max_messages = 0;
};

class GeminiJsonHistoryStore
{
  public:
	static std::optional<ChatSession> ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});

	static std::vector<ChatSession> Load(const std::filesystem::path& chats_dir, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {}, std::stop_token stop_token = {});
};

std::vector<ChatSession> LoadGeminiJsonHistoryForRuntime(const std::filesystem::path& chats_dir, const ProviderProfile& profile, const ProviderRuntimeHistoryLoadOptions& options, std::stop_token stop_token = {});