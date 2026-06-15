#include "chat_import_utils.h"

#include "common/paths/path_utils.h"
#include "common/utils/string_utils.h"

#include <string_view>

namespace
{
	std::string StripPromptWrappers(std::string_view raw_value)
	{
		std::string_view value = uam::strings::TrimAsciiView(raw_value);
		if (value.empty())
		{
			return {};
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
