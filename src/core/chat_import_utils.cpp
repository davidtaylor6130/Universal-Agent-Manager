#include "chat_import_utils.h"

#include "common/paths/path_utils.h"
#include "common/paths/app_paths.h"
#include "common/paths/workspace_root.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

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
		for (fs::path root = location; !root.empty(); root = root.parent_path())
		{
			const fs::path marker = root / ".git";
			if (uam::paths::PathExistsNoThrow(marker))
			{
				// Stop at the nearest repository boundary, including submodules and ordinary checkouts.
				std::string git_file;
				if (!uam::io::TryReadTextFile(marker, git_file, 16384)) return workspace;
				const std::string_view pointer = uam::strings::TrimAsciiView(git_file);
				if (!pointer.starts_with("gitdir: ")) return workspace;
				const fs::path git_directory = uam::paths::NormalizeExistingOrAbsolutePath(
				    root / uam::paths::PathFromUtf8(uam::strings::TrimAsciiView(pointer.substr(8))));
				std::string common_file;
				std::string backlink;
				if (!uam::io::TryReadTextFile(git_directory / "commondir", common_file, 16384) ||
				    !uam::io::TryReadTextFile(git_directory / "gitdir", backlink, 16384) ||
				    !FolderDirectoryMatches(git_directory / uam::paths::PathFromUtf8(uam::strings::Trim(backlink)), marker)) return workspace;
				const fs::path common = uam::paths::NormalizeExistingOrAbsolutePath(
				    git_directory / uam::paths::PathFromUtf8(uam::strings::Trim(common_file)));
				// A standard main checkout owns the common .git directory. Do not infer bare repositories.
				if (common.filename() != ".git" || !uam::paths::IsDirectoryNoThrow(common)) return workspace;
				const fs::path resolved = common.parent_path() / location.lexically_relative(root);
				return uam::paths::IsDirectoryNoThrow(resolved) ? uam::paths::NormalizeExistingPath(resolved) : workspace;
			}
			if (root == root.parent_path()) break;
		}
		return workspace;
	}

	bool ImportedWorkspaceMatchesFolder(const std::filesystem::path& workspace, const std::filesystem::path& folder)
	{
		return !workspace.empty() && !folder.empty() &&
		    (FolderDirectoryMatches(workspace, folder) ||
		     FolderDirectoryMatches(ResolveImportedWorkspaceFolderDirectory(workspace), folder));
	}

	const ChatFolder* FindImportedWorkspaceFolder(const std::vector<ChatFolder>& folders, const std::filesystem::path& workspace)
	{
		if (workspace.empty()) return nullptr;
		for (const ChatFolder& folder : folders)
		{
			if (uam::paths::IsControllerLocalWorkspace(folder) && !folder.directory.empty() &&
			    FolderDirectoryMatches(uam::paths::PathFromUtf8(folder.directory), workspace)) return &folder;
		}
		const std::filesystem::path resolved = ResolveImportedWorkspaceFolderDirectory(workspace);
		for (const ChatFolder& folder : folders)
		{
			if (uam::paths::IsControllerLocalWorkspace(folder) && !folder.directory.empty() &&
			    FolderDirectoryMatches(uam::paths::PathFromUtf8(folder.directory), resolved)) return &folder;
		}
		return nullptr;
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

	bool ImportedProjectRootExists(const std::filesystem::path& project_root)
	{
		if (project_root.empty())
		{
			return false;
		}
		const std::filesystem::path normalized = uam::paths::LexicallyNormalPath(project_root);
		return uam::paths::IsDirectoryNoThrow(normalized);
	}

	std::filesystem::path ResolveImportedProjectRootOrFallback(const std::filesystem::path& project_root, const std::filesystem::path& fallback_root)
	{
		if (ImportedProjectRootExists(project_root))
		{
			return uam::paths::LexicallyNormalPath(project_root);
		}
		if (fallback_root.empty())
		{
			return {};
		}
		return uam::paths::LexicallyNormalPath(fallback_root);
	}

} // namespace uam
