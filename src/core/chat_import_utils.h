#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

/// Identifies provider-generated user events rather than human requests.
bool IsCodexSyntheticUserMessage(std::string_view content);
bool IsInjectedChatTitle(std::string_view title);

std::string BuildImportedChatTitle(const std::vector<Message>& messages, const std::string& created_at, std::size_t max_length = 48);

std::string BuildFolderTitleFromProjectRoot(const std::filesystem::path& project_root);
bool ImportedProjectRootExists(const std::filesystem::path& project_root);
std::filesystem::path ResolveImportedProjectRootOrFallback(const std::filesystem::path& project_root, const std::filesystem::path& fallback_root);

} // namespace uam
