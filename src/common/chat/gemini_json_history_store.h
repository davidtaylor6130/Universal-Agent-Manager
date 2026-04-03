#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <vector>

/// <summary>
/// Optional guardrails used while parsing native Gemini history files.
/// </summary>
struct GeminiJsonHistoryStoreOptions
{
	std::uintmax_t max_file_bytes = 0;
	std::size_t max_messages = 0;
};

/// <summary>
/// Shared Gemini-native session history store.
/// </summary>
class GeminiJsonHistoryStore
{
  public:
	/// <summary>Parses one Gemini native session file into an app chat model.</summary>
	static std::optional<ChatSession> ParseFile(const std::filesystem::path& file_path, const ProviderProfile& provider, const GeminiJsonHistoryStoreOptions& options = {});

	/// <summary>Loads all Gemini native sessions from a chats directory.</summary>
	static std::vector<ChatSession> Load(const std::filesystem::path& chats_dir,
	                                     const ProviderProfile& provider,
	                                     const GeminiJsonHistoryStoreOptions& options = {},
	                                     std::stop_token stop_token = {});
};
