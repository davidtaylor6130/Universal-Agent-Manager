#include "app/markdown_store_service.h"

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	constexpr std::size_t kTitleMaxChars = 160;
	constexpr std::size_t kMakerMaxChars = 160;
	constexpr std::size_t kReviewMaxChars = 320;
	constexpr std::size_t kTimestampMaxChars = 80;
	constexpr std::size_t kPreviewMaxChars = 320;

	void SetError(std::string* error_out, std::string_view message)
	{
		if (error_out != nullptr)
		{
			*error_out = std::string(message);
		}
	}

	void ClearError(std::string* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}
	}

	std::string StripYamlQuotes(std::string_view value)
	{
		value = uam::strings::TrimAsciiView(value);
		if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
		{
			return std::string(value.substr(1, value.size() - 2));
		}
		return std::string(value);
	}

	std::string_view HeaderValue(const std::map<std::string, std::string>& headers, std::string_view key)
	{
		const auto found = headers.find(uam::strings::ToLowerAscii(std::string(key)));
		return found == headers.end() ? std::string_view{} : std::string_view{found->second};
	}

	struct MarkdownDocumentParts
	{
		std::map<std::string, std::string> headers;
		std::string body;
	};

	std::size_t MarkdownBodyStartAfterFrontMatter(const std::string& text, std::size_t closing_marker)
	{
		std::size_t body_start = closing_marker + 4;
		if (body_start < text.size() && text[body_start] == '\r')
		{
			++body_start;
		}
		if (body_start < text.size() && text[body_start] == '\n')
		{
			++body_start;
		}
		return body_start;
	}

	MarkdownDocumentParts ParseMarkdownDocumentParts(const std::string& text)
	{
		MarkdownDocumentParts parts;
		parts.body = text;
		if (!uam::strings::StartsWith(text, "---"))
		{
			return parts;
		}

		const std::size_t first_line_end = text.find('\n');
		const std::size_t closing = first_line_end == std::string::npos ? std::string::npos : text.find("\n---", first_line_end);
		if (closing == std::string::npos)
		{
			return parts;
		}

		std::istringstream header_lines(text.substr(first_line_end + 1, closing - first_line_end - 1));
		std::string line;
		while (std::getline(header_lines, line))
		{
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos)
			{
				continue;
			}
			parts.headers[uam::strings::TrimAndLowerAscii(line.substr(0, colon))] = StripYamlQuotes(line.substr(colon + 1));
		}
		parts.body = text.substr(MarkdownBodyStartAfterFrontMatter(text, closing));
		return parts;
	}

	std::string FirstHeadingTitle(std::string_view body)
	{
		std::istringstream lines{std::string(body)};
		std::string line;
		while (std::getline(lines, line))
		{
			if (uam::strings::StartsWith(line, "# "))
			{
				return uam::strings::SafeLine(line.substr(2), kTitleMaxChars, true);
			}
		}
		return "";
	}

	std::string PreviewFromBody(std::string_view body)
	{
		std::istringstream lines{std::string(body)};
		std::string line;
		std::string preview;
		while (std::getline(lines, line))
		{
			line = uam::strings::Trim(line);
			if (line.empty() || uam::strings::StartsWith(line, "#"))
			{
				continue;
			}
			if (!preview.empty())
			{
				preview += " ";
			}
			preview += line;
			if (preview.size() >= kPreviewMaxChars)
			{
				preview.resize(kPreviewMaxChars);
				break;
			}
		}
		return preview;
	}

	std::string EntryIdForPath(const fs::path& root, const fs::path& entry_path)
	{
		if (const std::optional<fs::path> relative = uam::paths::RelativePathIfInsideRoot(root, entry_path))
		{
			return uam::paths::PortablePathString(*relative);
		}
		return entry_path.filename().string();
	}

	bool ParseEntryFile(const fs::path& path, MarkdownStoreService::Entry& out_entry)
	{
		out_entry = MarkdownStoreService::Entry{};

		std::string text;
		if (!uam::io::TryReadTextFile(path, text))
		{
			return false;
		}

		const MarkdownDocumentParts parts = ParseMarkdownDocumentParts(text);

		out_entry.title = uam::strings::SafeLine(HeaderValue(parts.headers, "title"), kTitleMaxChars, true);
		if (out_entry.title.empty())
		{
			out_entry.title = FirstHeadingTitle(parts.body);
		}
		if (out_entry.title.empty())
		{
			out_entry.title = path.stem().string();
		}
		out_entry.maker = uam::strings::SafeLine(HeaderValue(parts.headers, "maker"), kMakerMaxChars, true);
		out_entry.review = uam::strings::SafeLine(HeaderValue(parts.headers, "review"), kReviewMaxChars, true);
		out_entry.date_created = uam::strings::SafeLine(HeaderValue(parts.headers, "dateCreated"), kTimestampMaxChars, true);
		out_entry.date_updated = uam::strings::SafeLine(HeaderValue(parts.headers, "dateUpdated"), kTimestampMaxChars, true);
		out_entry.preview = PreviewFromBody(parts.body);
		out_entry.file_path = path;
		return !out_entry.title.empty();
	}

	std::string BuildMarkdown(const MarkdownStoreService::Draft& draft)
	{
		const std::string now = uam::time::TimestampNow();
		std::string markdown;
		markdown.reserve(draft.title.size() + draft.maker.size() + draft.review.size() + draft.body.size() + 160);
		markdown = "---\n";
		markdown += "uamVersion: 1\n";
		markdown += "title: " + uam::strings::SafeLine(draft.title, kTitleMaxChars, true) + "\n";
		markdown += "maker: " + uam::strings::SafeLine(draft.maker, kMakerMaxChars, true) + "\n";
		markdown += "review: " + uam::strings::SafeLine(draft.review, kReviewMaxChars, true) + "\n";
		markdown += "dateCreated: " + now + "\n";
		markdown += "dateUpdated: " + now + "\n";
		markdown += "---\n\n";
		markdown += draft.body + "\n";
		return markdown;
	}

	bool SanitizeDraft(MarkdownStoreService::Draft input, MarkdownStoreService::Draft& draft, std::string* error_out)
	{
		input.title = uam::strings::SafeLine(input.title, kTitleMaxChars, true);
		input.maker = uam::strings::SafeLine(input.maker, kMakerMaxChars, true);
		input.review = uam::strings::SafeLine(input.review, kReviewMaxChars, true);
		input.body = uam::strings::Trim(input.body);
		if (input.title.empty() || input.body.empty())
		{
			SetError(error_out, "Markdown Store entry requires a title and body.");
			return false;
		}

		draft = std::move(input);
		return true;
	}

	fs::path ResolveCreateTarget(const fs::path& root, std::string_view title)
	{
		const std::string slug = uam::strings::AsciiSlug(title, 72, "markdown-store-item");
		fs::path target = root / (slug + ".uam");
		for (int i = 2; uam::paths::PathExistsNoThrow(target); ++i)
		{
			target = root / (slug + "-" + std::to_string(i) + ".uam");
		}
		return target;
	}
} // namespace

fs::path MarkdownStoreService::NormalizeRoot(std::string_view root)
{
	const fs::path expanded = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(root));
	if (expanded.empty())
	{
		return {};
	}
	return uam::paths::NormalizeExistingPath(expanded);
}

bool MarkdownStoreService::IsConfiguredRoot(const fs::path& root, std::string* error_out)
{
	ClearError(error_out);
	if (root.empty())
	{
		SetError(error_out, "Markdown Store directory is not configured.");
		return false;
	}

	if (!uam::paths::PathExistsNoThrow(root))
	{
		SetError(error_out, "Markdown Store directory does not exist.");
		return false;
	}
	if (!uam::paths::IsDirectoryNoThrow(root))
	{
		SetError(error_out, "Markdown Store path is not a directory.");
		return false;
	}
	return true;
}

std::vector<MarkdownStoreService::Entry> MarkdownStoreService::ListEntries(const fs::path& root, std::string* error_out)
{
	ClearError(error_out);
	std::vector<Entry> entries;
	if (!IsConfiguredRoot(root, error_out))
	{
		return entries;
	}

	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		if (!uam::paths::IsRegularFileWithExtensionNoThrow(*it, ".uam"))
		{
			continue;
		}
		Entry entry;
		if (!ParseEntryFile(it->path(), entry))
		{
			continue;
		}
		entry.id = EntryIdForPath(root, it->path());
		entries.push_back(std::move(entry));
	}

	if (ec)
	{
		SetError(error_out, "Failed to enumerate Markdown Store files.");
		return {};
	}

	std::ranges::sort(entries,
	                  [](const Entry& lhs, const Entry& rhs)
	                  {
		                  if (lhs.date_updated != rhs.date_updated)
		                  {
			                  return lhs.date_updated > rhs.date_updated;
		                  }
		                  return lhs.title < rhs.title;
	                  });
	return entries;
}

bool MarkdownStoreService::CreateEntry(const fs::path& root, const Draft& input, Entry* created_entry, std::string* error_out)
{
	ClearError(error_out);
	if (created_entry != nullptr)
	{
		*created_entry = Entry{};
	}

	if (!IsConfiguredRoot(root, error_out))
	{
		return false;
	}

	Draft draft;
	if (!SanitizeDraft(input, draft, error_out))
	{
		return false;
	}

	const fs::path target = ResolveCreateTarget(root, draft.title);
	if (!uam::io::WriteTextFile(target, BuildMarkdown(draft)))
	{
		SetError(error_out, "Failed to write Markdown Store file.");
		return false;
	}

	if (created_entry != nullptr)
	{
		Entry created;
		if (!ParseEntryFile(target, created))
		{
			SetError(error_out, "Markdown Store file was written but could not be reloaded.");
			return false;
		}
		created.id = EntryIdForPath(root, target);
		*created_entry = std::move(created);
	}
	return true;
}

bool MarkdownStoreService::ValidateStoreFilePath(const fs::path& root, std::string_view file_path, fs::path* normalized_path_out, std::string* error_out)
{
	ClearError(error_out);
	if (normalized_path_out != nullptr)
	{
		normalized_path_out->clear();
	}

	if (!IsConfiguredRoot(root, error_out))
	{
		return false;
	}

	const fs::path candidate = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(file_path));
	if (candidate.empty())
	{
		SetError(error_out, "Markdown Store file path is empty.");
		return false;
	}
	if (!uam::paths::IsSameOrInsideRoot(root, candidate))
	{
		SetError(error_out, "Markdown Store file is outside the configured store directory.");
		return false;
	}
	if (candidate.extension() != ".uam")
	{
		SetError(error_out, "Only .uam Markdown Store files can be attached.");
		return false;
	}

	if (!uam::paths::IsRegularFileNoThrow(candidate))
	{
		SetError(error_out, "Markdown Store file does not exist.");
		return false;
	}

	if (normalized_path_out != nullptr)
	{
		*normalized_path_out = uam::paths::NormalizeExistingPath(candidate);
	}
	return true;
}
