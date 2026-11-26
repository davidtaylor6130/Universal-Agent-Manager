#pragma once

#include "common/models/app_models.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

/// <summary>Resolve linked worktrees to main-checkout folders for organisation; unavailable metadata retains the original location.</summary>
std::filesystem::path ResolveImportedWorkspaceFolderDirectory(const std::filesystem::path& workspace);
/// <summary>Find the deepest registered local owner without crossing another repository; explicit worktrees precede validated aliases. A path inside a UAM-managed worktree bucket, even a removed one, belongs to its registered source.</summary>
const ChatFolder* FindImportedWorkspaceFolder(const std::vector<ChatFolder>& folders, const std::filesystem::path& workspace);

/// <summary>FindImportedWorkspaceFolder for many workspaces in one pass: each registered local folder is resolved on disk
/// once, and each distinct workspace once, instead of once per lookup. Valid only within one pass over unchanged folders
/// and directories. It refers to the folder vector by position, so the vector may grow (Refresh indexes appended folders)
/// but must not otherwise change or be destroyed while the index is used. Not thread-safe.</summary>
class ImportedWorkspaceFolderIndex
{
  public:
	explicit ImportedWorkspaceFolderIndex(const std::vector<ChatFolder>& folders);
	/// <summary>Reuses what `prefix` resolved when `folders` starts with the same folders (identity and directory),
	/// so a plan that appends folders to a list resolves only the appended ones; otherwise resolves every folder.</summary>
	ImportedWorkspaceFolderIndex(const std::vector<ChatFolder>& folders, const ImportedWorkspaceFolderIndex& prefix);
	const ChatFolder* Find(const std::filesystem::path& workspace) const;
	/// <summary>Indexes folders appended since the last call and forgets earlier answers, which an appended folder may change.
	/// A list that shrank is indexed again from the start.</summary>
	void Refresh();
	const std::vector<ChatFolder>& Folders() const { return m_folders; }

  private:
	struct RegisteredFolder
	{
		std::size_t position;
		std::filesystem::path key;
		std::size_t depth;
	};
	const std::vector<ChatFolder>& m_folders;
	std::size_t m_indexed = 0;
	std::vector<RegisteredFolder> m_registered;
	mutable std::unordered_map<std::string, std::optional<std::size_t>> m_found;
};

/// <summary>Registered folders ImportedWorkspaceFolderIndex resolved on this thread; tests read the difference around a pass.</summary>
std::size_t& RegisteredWorkspaceFolderResolutionsOnThisThread();
bool ImportedWorkspaceMatchesFolder(const std::filesystem::path& workspace, const std::filesystem::path& folder);

} // namespace uam
