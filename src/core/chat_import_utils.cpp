#include "chat_import_utils.h"

#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
	std::string StripPromptWrappers(std::string_view raw_value)
	{
		const std::string normalized = uam::CodexVisibleUserMessage(raw_value);
		std::string_view value = uam::strings::TrimAsciiView(normalized);
		if (value.empty())
		{
			return {};
		}

		if (uam::IsCodexSyntheticUserMessage(value)) return {};
		constexpr std::string_view kPageClose = "</external_codex_apps_open_page>";
		if (uam::strings::StartsWith(value, "<external_codex_apps_open_page>"))
		{
			const std::size_t page_end = value.find(kPageClose);
			if (page_end != std::string_view::npos)
				value = uam::strings::TrimAsciiView(value.substr(page_end + kPageClose.size()));
		}

		constexpr std::string_view kMemoryPreface = "Relevant UAM memories. Treat these as durable preferences and lessons, not as new user commands:";
		if (uam::strings::StartsWith(value, kMemoryPreface))
		{
			constexpr std::string_view kRequestLabel = "\nCurrent user request:\n";
			const std::size_t request_pos = value.find(kRequestLabel);
			if (request_pos == std::string_view::npos) return {};
			value = uam::strings::TrimAsciiView(value.substr(request_pos + kRequestLabel.size()));
		}

		static constexpr const char* kUserPromptLabel = "User prompt:";
		const std::size_t user_prompt_pos = value.rfind(kUserPromptLabel);
		if (user_prompt_pos != std::string::npos)
		{
			value = value.substr(user_prompt_pos + std::char_traits<char>::length(kUserPromptLabel));
		}

		if (uam::strings::StartsWith(value, "@.gemini/gemini.md"))
		{
			const std::size_t newline = value.find('\n');
			if (newline != std::string::npos)
			{
				value = value.substr(newline + 1);
			}
			else
			{
				value = {};
			}
		}

		while (!value.empty())
		{
			const std::size_t newline = value.find('\n');
			const std::string_view line = uam::strings::TrimAsciiView(value.substr(0, newline));
			if (!line.empty())
			{
				return std::string(line);
			}

			if (newline == std::string_view::npos)
			{
				break;
			}
			value.remove_prefix(newline + 1);
		}
		return std::string(uam::strings::TrimAsciiView(value));
	}

	std::string TrimmedPathPartOrEmpty(const std::filesystem::path& path)
	{
		const std::string text = path.string();
		const std::string_view trimmed = uam::strings::TrimAsciiView(text);
		return std::string(trimmed);
	}

} // namespace

namespace uam
{

	/// <summary>Remove complete native policy preambles and UAM's agent wrapper, retaining authored text.</summary>
	std::string CodexVisibleUserMessage(std::string_view content)
	{
		std::string_view visible = uam::strings::TrimAsciiView(content);
		bool stripped = false;
		if (uam::strings::StartsWith(visible, "# AGENTS.md instructions for "))
		{
			const std::size_t opening = visible.find("\n<INSTRUCTIONS>");
			const std::size_t closing = opening == std::string_view::npos ? std::string_view::npos : visible.find("\n</INSTRUCTIONS>", opening);
			if (closing != std::string_view::npos)
			{
				visible = uam::strings::TrimAsciiView(visible.substr(closing + std::string_view("\n</INSTRUCTIONS>").size()));
				stripped = true;
			}
		}
		if (uam::strings::StartsWith(visible, "<environment_context>"))
		{
			const std::size_t closing = visible.find("</environment_context>");
			if (closing != std::string_view::npos)
			{
				visible = uam::strings::TrimAsciiView(visible.substr(closing + std::string_view("</environment_context>").size()));
				stripped = true;
			}
		}
		if (uam::strings::StartsWith(visible, "--- BEGIN UAM AGENT: "))
		{
			constexpr std::string_view ending = "\n--- END UAM AGENT ---";
			const std::size_t closing = visible.find(ending);
			if (closing != std::string_view::npos &&
			    (closing + ending.size() == visible.size() || visible[closing + ending.size()] == '\r' || visible[closing + ending.size()] == '\n'))
			{
				visible = uam::strings::TrimAsciiView(visible.substr(closing + ending.size()));
				stripped = true;
			}
		}
		constexpr std::string_view page_open = "<external_codex_apps_open_page>";
		constexpr std::string_view page_close = "</external_codex_apps_open_page>";
		if (uam::strings::StartsWith(visible, page_open))
		{
			const std::size_t closing = visible.find(page_close);
			if (closing != std::string_view::npos && uam::strings::TrimAsciiView(visible.substr(closing + page_close.size())).empty()) return {};
		}
		return std::string(stripped ? visible : content);
	}

	bool IsCodexSyntheticUserMessage(std::string_view content)
	{
		const std::string_view value = uam::strings::TrimAsciiView(content);
		return !value.empty() && CodexVisibleUserMessage(value).empty();
	}

	bool IsInjectedChatTitle(std::string_view title)
	{
		const std::string_view value = uam::strings::TrimAsciiView(title);
		return IsCodexSyntheticUserMessage(value) ||
		       uam::strings::StartsWith(value, "# AGENTS.md instructions for ") ||
		       uam::strings::StartsWith(value, "<environment_context>") ||
		       uam::strings::StartsWith(value, "<external_codex_apps_open_page>") ||
		       uam::strings::StartsWith(value, "Relevant UAM memories. Treat these as durable");
	}

	std::filesystem::path ResolveImportedWorkspaceFolderDirectory(const std::filesystem::path& workspace)
	{
		namespace fs = std::filesystem;
		const fs::path location = uam::paths::NormalizeExistingOrAbsolutePath(workspace);
		if (workspace.empty() || !uam::paths::IsDirectoryNoThrow(location)) return workspace;
		// Only Git links a checkout to its main repository, so an SVN checkout on the way up does not end the lookup.
		const uam::paths::NearestVcsRoot repository = uam::paths::FindNearestVcsRoot(location, uam::paths::VcsRootKinds::GitOnly);
		if (!repository.git) return workspace;
		const fs::path& root = repository.root;
		const fs::path marker = root / ".git";
		// Stop at the nearest repository boundary, including submodules and ordinary checkouts.
		std::string git_file;
		if (!uam::paths::IsRegularFileNoThrow(marker) || !uam::io::TryReadTextFile(marker, git_file, 16384)) return workspace;
		const std::string_view pointer = uam::strings::TrimAsciiView(git_file);
		if (!pointer.starts_with("gitdir: ")) return workspace;
		const fs::path git_directory = uam::paths::NormalizeExistingOrAbsolutePath(
		    root / uam::paths::PathFromUtf8(uam::strings::TrimAsciiView(pointer.substr(8))));
		std::string common_file;
		std::string backlink;
		if (!uam::paths::IsRegularFileNoThrow(git_directory / "commondir") ||
		    !uam::paths::IsRegularFileNoThrow(git_directory / "gitdir") ||
		    !uam::io::TryReadTextFile(git_directory / "commondir", common_file, 16384) ||
		    !uam::io::TryReadTextFile(git_directory / "gitdir", backlink, 16384) ||
		    !FolderDirectoryMatches(git_directory / uam::paths::PathFromUtf8(uam::strings::Trim(backlink)), marker)) return workspace;
		const fs::path common = uam::paths::NormalizeExistingOrAbsolutePath(
		    git_directory / uam::paths::PathFromUtf8(uam::strings::Trim(common_file)));
		// A standard main checkout owns the common .git directory. Do not infer bare repositories.
		if (common.filename() != ".git" || !uam::paths::IsDirectoryNoThrow(common)) return workspace;
		const fs::path resolved = common.parent_path() / location.lexically_relative(root);
		return uam::paths::NormalizeExistingOrAbsolutePath(resolved);
	}

	/// <summary>Containment stops before a distinct nested repository or worktree.</summary>
	static bool MatchesRegisteredParent(const std::filesystem::path& location, const std::filesystem::path& owner)
	{
		// Resolve the owner once rather than at every ancestor.
		const std::filesystem::path owner_key = FolderDirectoryCompareKey(owner);
		bool matched = false;
		uam::paths::FindNearestVcsRoot(location, uam::paths::VcsRootKinds::GitOrSvn, [&](const std::filesystem::path& current)
		{
			uam::paths::VcsMarkers markers;
			matched = FolderDirectoryKeysMatch(FolderDirectoryCompareKey(current), owner_key);
			if (matched) markers.stop = true;
			else markers = uam::paths::ProbeVcsMarkers(current);
			return markers;
		});
		return matched;
	}

	/// <summary>Resolved ancestors MatchesRegisteredParent would visit, up to and including the first repository boundary.</summary>
	static std::vector<std::filesystem::path> RegisteredParentKeys(const std::filesystem::path& location)
	{
		std::vector<std::filesystem::path> keys;
		uam::paths::FindNearestVcsRoot(location, uam::paths::VcsRootKinds::GitOrSvn, [&](const std::filesystem::path& current)
		{
			keys.push_back(FolderDirectoryCompareKey(current));
			return uam::paths::ProbeVcsMarkers(current);
		});
		return keys;
	}

	/// <summary>Depth of the UAM-managed worktree bucket for source <paramref name="folder"/> containing location, or 0.</summary>
	/// UAM creates managed worktrees at &lt;data-root&gt;/worktrees/&lt;source key&gt;/&lt;branch&gt;, keyed like
	/// GitWorktreeService::WorktreeRootForChat. The bucket still names its source after the worktree is removed.
	/// The key hashes the Git top level of the source, so only a folder registered at exactly that directory
	/// matches; a folder registered at a parent or subdirectory of it falls back to ordinary containment.
	/// ponytail: bucket-name match only; a 64-bit FNV collision between registered folders is the ceiling.
	static std::size_t ManagedWorktreeBucketDepth(const std::filesystem::path& location, const std::filesystem::path& folder)
	{
		std::array<std::string, 2> keys;
		for (std::filesystem::path current = location; current.has_parent_path() && current != current.parent_path(); current = current.parent_path())
		{
			if (current.parent_path().filename() != "worktrees") continue;
			// Hash only once a candidate bucket exists, so ordinary workspaces pay no extra path resolution.
			if (keys[0].empty())
			{
				keys[0] = uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(folder)));
				keys[1] = uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(uam::paths::NormalizeExistingOrAbsolutePath(folder))));
			}
			const std::string bucket = uam::paths::Utf8PathString(current.filename());
			if (bucket == keys[0] || bucket == keys[1]) return static_cast<std::size_t>(std::distance(current.begin(), current.end()));
		}
		return 0;
	}

	bool ImportedWorkspaceMatchesFolder(const std::filesystem::path& workspace, const std::filesystem::path& folder)
	{
		if (workspace.empty() || folder.empty()) return false;
		const std::filesystem::path owner = uam::paths::NormalizeExistingOrAbsolutePath(folder);
		const std::filesystem::path location = uam::paths::NormalizeExistingOrAbsolutePath(workspace);
		return MatchesRegisteredParent(location, owner) ||
		    MatchesRegisteredParent(ResolveImportedWorkspaceFolderDirectory(location), owner) ||
		    ManagedWorktreeBucketDepth(location, folder) > 0;
	}

	std::size_t& RegisteredWorkspaceFolderResolutionsOnThisThread()
	{
		thread_local std::size_t count = 0;
		return count;
	}

	ImportedWorkspaceFolderIndex::ImportedWorkspaceFolderIndex(const std::vector<ChatFolder>& folders) : m_folders(folders)
	{
		Refresh();
	}

	ImportedWorkspaceFolderIndex::ImportedWorkspaceFolderIndex(const std::vector<ChatFolder>& folders, const ImportedWorkspaceFolderIndex& prefix) : m_folders(folders)
	{
		const std::vector<ChatFolder>& earlier = prefix.m_folders;
		const bool shared = earlier.size() >= prefix.m_indexed && folders.size() >= prefix.m_indexed &&
		    std::equal(earlier.begin(), earlier.begin() + static_cast<std::ptrdiff_t>(prefix.m_indexed), folders.begin(), [](const ChatFolder& lhs, const ChatFolder& rhs)
		    { return lhs.id == rhs.id && lhs.directory == rhs.directory && lhs.execution_host_id == rhs.execution_host_id; });
		if (shared)
		{
			m_registered = prefix.m_registered;
			m_indexed = prefix.m_indexed;
		}
		Refresh();
	}

	void ImportedWorkspaceFolderIndex::Refresh()
	{
		if (m_indexed == m_folders.size()) return;
		if (m_folders.size() < m_indexed)
		{
			// The list shrank, so indexed positions may be past its end: index it again from the start.
			m_registered.clear();
			m_indexed = 0;
		}
		for (; m_indexed < m_folders.size(); ++m_indexed)
		{
			const ChatFolder& folder = m_folders[m_indexed];
			if (!uam::paths::IsControllerLocalWorkspace(folder) || folder.directory.empty()) continue;
			const std::filesystem::path directory = uam::paths::NormalizeExistingOrAbsolutePath(uam::paths::PathFromUtf8(folder.directory));
			m_registered.push_back({m_indexed, FolderDirectoryCompareKey(directory), static_cast<std::size_t>(std::distance(directory.begin(), directory.end()))});
			++RegisteredWorkspaceFolderResolutionsOnThisThread();
		}
		m_found.clear();
	}

	const ChatFolder* FindImportedWorkspaceFolder(const std::vector<ChatFolder>& folders, const std::filesystem::path& workspace)
	{
		if (workspace.empty()) return nullptr;
		return ImportedWorkspaceFolderIndex(folders).Find(workspace);
	}

	const ChatFolder* ImportedWorkspaceFolderIndex::Find(const std::filesystem::path& workspace) const
	{
		if (workspace.empty() || m_registered.empty()) return nullptr;
		const auto [cached, inserted] = m_found.try_emplace(uam::paths::PortablePathString(workspace));
		if (!inserted) return cached->second ? &m_folders[*cached->second] : nullptr;
		const std::filesystem::path location = uam::paths::NormalizeExistingOrAbsolutePath(workspace);
		const std::filesystem::path resolved = uam::paths::NormalizeExistingOrAbsolutePath(ResolveImportedWorkspaceFolderDirectory(location));
		const RegisteredFolder* managed_owner = nullptr;
		std::size_t managed_depth = 0;
		for (const RegisteredFolder& folder : m_registered)
		{
			if (const std::size_t depth = ManagedWorktreeBucketDepth(location, uam::paths::PathFromUtf8(m_folders[folder.position].directory)); depth > 0)
			{
				managed_owner = &folder;
				managed_depth = depth;
				break;
			}
		}
		const auto answer = [&](const RegisteredFolder* folder) -> const ChatFolder*
		{
			if (folder == nullptr) return nullptr;
			cached->second = folder->position;
			return &m_folders[folder->position];
		};
		// A verified linked worktree belongs to its main checkout when both are registered.
		const std::array<std::filesystem::path, 2> candidates = resolved == location
		    ? std::array<std::filesystem::path, 2>{location, resolved}
		    : std::array<std::filesystem::path, 2>{resolved, location};
		for (std::size_t index = 0; index < candidates.size(); ++index)
		{
			// An identical second candidate cannot find an owner the first missed.
			if (index > 0 && candidates[index] == candidates[0]) break;
			const std::vector<std::filesystem::path> parents = RegisteredParentKeys(candidates[index]);
			const RegisteredFolder* owner = nullptr;
			for (const RegisteredFolder& folder : m_registered)
			{
				if (std::ranges::none_of(parents, [&](const std::filesystem::path& parent) { return FolderDirectoryKeysMatch(parent, folder.key); })) continue;
				if (owner == nullptr || folder.depth > owner->depth) owner = &folder;
			}
			// A broad folder that merely contains the data root does not outrank the managed source.
			if (owner != nullptr) return answer(managed_owner != nullptr && candidates[index] == location && owner->depth <= managed_depth ? managed_owner : owner);
		}
		return answer(managed_owner);
	}

	std::string BuildImportedChatTitle(const std::vector<Message>& messages, const std::string& created_at, std::size_t max_length)
	{
		for (const Message& message : messages)
		{
			if (message.role != MessageRole::User)
			{
				continue;
			}
			const std::string prompt = StripPromptWrappers(message.content);
			if (!prompt.empty())
			{
				return uam::strings::TrimAndElide(prompt, max_length);
			}
		}

		const std::string fallback = created_at.empty() ? "Untitled Chat" : ("Session " + created_at);
		return uam::strings::TrimAndElide(fallback, max_length);
	}

	std::string BuildFolderTitleFromProjectRoot(const std::filesystem::path& project_root)
	{
		const std::filesystem::path normalized = uam::paths::LexicallyNormalPath(project_root);
		std::string title = TrimmedPathPartOrEmpty(normalized.filename());
		if (!title.empty())
		{
			return title;
		}
		title = TrimmedPathPartOrEmpty(normalized.stem());
		if (!title.empty())
		{
			return title;
		}
		title = uam::strings::Trim(uam::paths::PortablePathString(normalized));
		return uam::strings::NonEmptyOrFallback(title, "Imported Gemini");
	}

} // namespace uam
