#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

/// Identifies provider-generated user events rather than human requests.
/// Removes only complete generated policy wrappers, preserving authored and malformed text.
std::string CodexVisibleUserMessage(std::string_view content);
bool IsCodexSyntheticUserMessage(std::string_view content);
bool IsInjectedChatTitle(std::string_view title);

std::string BuildImportedChatTitle(const std::vector<Message>& messages, const std::string& created_at, std::size_t max_length = 48);

std::string BuildFolderTitleFromProjectRoot(const std::filesystem::path& project_root);
bool ImportedProjectRootExists(const std::filesystem::path& project_root);
std::filesystem::path ResolveImportedProjectRootOrFallback(const std::filesystem::path& project_root, const std::filesystem::path& fallback_root);

/// <summary>Resolve linked worktrees to main-checkout folders for organisation; unavailable metadata retains the original location.</summary>
std::filesystem::path ResolveImportedWorkspaceFolderDirectory(const std::filesystem::path& workspace);
/// <summary>Find a local folder, giving explicit worktree folders priority over main-checkout aliases.</summary>
const ChatFolder* FindImportedWorkspaceFolder(const std::vector<ChatFolder>& folders, const std::filesystem::path& workspace);
bool ImportedWorkspaceMatchesFolder(const std::filesystem::path& workspace, const std::filesystem::path& folder);

} // namespace uam
