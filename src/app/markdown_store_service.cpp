#include "app/markdown_store_service.h"

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
	constexpr std::size_t kCommandMaxChars = 80;
	constexpr std::size_t kGroupMaxChars = 160;
	constexpr std::uintmax_t kImportMaxBytes = 2U * 1024U * 1024U;

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

	bool HeaderBool(const std::map<std::string, std::string>& headers, std::string_view key)
	{
		return uam::strings::TrimAndLowerAscii(std::string(HeaderValue(headers, key))) == "true";
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
				preview = uam::strings::SafeLine(preview, kPreviewMaxChars);
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

	std::string LegacyCommandName(const fs::path& path, std::string_view title)
	{
		std::uint64_t hash = 1469598103934665603ULL;
		for (const unsigned char ch : uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(path)))
		{
			hash ^= ch;
			hash *= 1099511628211ULL;
		}
		std::ostringstream suffix;
		suffix << std::hex << (hash & 0xffffffffULL);
		return uam::strings::AsciiSlug(title, kCommandMaxChars - 10, "skill") + "-" + suffix.str();
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
		out_entry.body = parts.body;
		out_entry.favorite = HeaderBool(parts.headers, "favorite");
		out_entry.source_provider = uam::strings::SafeLine(HeaderValue(parts.headers, "sourceProvider"), kMakerMaxChars, true);
		out_entry.source_path = uam::strings::SafeLine(HeaderValue(parts.headers, "sourcePath"), 4096, true);
		out_entry.command_name = uam::strings::AsciiSlug(HeaderValue(parts.headers, "commandName"), kCommandMaxChars, "");
		out_entry.group = uam::strings::SafeLine(HeaderValue(parts.headers, "group"), kGroupMaxChars, true);
		if (out_entry.command_name.empty()) out_entry.command_name = LegacyCommandName(path, out_entry.title);
		out_entry.file_path = path;
		return !out_entry.title.empty();
	}

	struct PersistedMetadata
	{
		std::string date_created;
		bool favorite = false;
		std::string source_provider;
		std::string source_path;
		std::string command_name;
	};

	std::string BuildMarkdown(const MarkdownStoreService::Draft& draft, const PersistedMetadata& metadata)
	{
		const std::string now = uam::time::TimestampNow();
		std::string markdown;
		markdown.reserve(draft.title.size() + draft.maker.size() + draft.review.size() + draft.body.size() + metadata.source_path.size() + 300);
		markdown = "---\n";
		markdown += "uamVersion: 1\n";
		markdown += "title: " + uam::strings::SafeLine(draft.title, kTitleMaxChars, true) + "\n";
		markdown += "maker: " + uam::strings::SafeLine(draft.maker, kMakerMaxChars, true) + "\n";
		markdown += "review: " + uam::strings::SafeLine(draft.review, kReviewMaxChars, true) + "\n";
		markdown += "dateCreated: " + (metadata.date_created.empty() ? now : metadata.date_created) + "\n";
		markdown += "dateUpdated: " + now + "\n";
		markdown += std::string("favorite: ") + (metadata.favorite ? "true\n" : "false\n");
		markdown += "sourceProvider: " + uam::strings::SafeLine(metadata.source_provider, kMakerMaxChars, true) + "\n";
		markdown += "sourcePath: " + uam::strings::SafeLine(metadata.source_path, 4096, true) + "\n";
		markdown += "commandName: " + uam::strings::AsciiSlug(metadata.command_name, kCommandMaxChars, "skill") + "\n";
		if (!draft.group.empty()) markdown += "group: " + uam::strings::SafeLine(draft.group, kGroupMaxChars, true) + "\n";
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
		input.group = uam::strings::SafeLine(input.group, kGroupMaxChars, true);
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

	std::string UniqueCommandName(const fs::path& root, std::string_view title, const fs::path& ignored_path = {})
	{
		std::set<std::string> used;
		std::error_code ec;
		for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!uam::paths::IsRegularFileWithExtensionNoThrow(*it, ".uam") || (!ignored_path.empty() && uam::paths::NormalizeExistingPath(it->path()) == uam::paths::NormalizeExistingPath(ignored_path)))
			{
				continue;
			}
			MarkdownStoreService::Entry entry;
			if (ParseEntryFile(it->path(), entry))
			{
				used.insert(entry.command_name.empty() ? uam::strings::AsciiSlug(entry.title, kCommandMaxChars, "skill") : entry.command_name);
			}
		}

		const std::string base = uam::strings::AsciiSlug(title, kCommandMaxChars, "skill");
		std::string candidate = base;
		for (int suffix = 2; used.contains(candidate); ++suffix)
		{
			candidate = uam::strings::AsciiSlug(base, kCommandMaxChars - 8, "skill") + "-" + std::to_string(suffix);
		}
		return candidate;
	}

	bool IsSupportedImportFile(const fs::path& path)
	{
		const std::string extension = uam::strings::ToLowerAscii(path.extension().string());
		return extension == ".md" || extension == ".markdown" || extension == ".uam";
	}

	std::string CandidateId(std::string_view provider, const fs::path& path)
	{
		return uam::strings::AsciiSlug(provider, 40, "manual") + ":" + uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(path));
	}

	std::optional<MarkdownStoreService::Draft> ReadImportDraft(const fs::path& path, std::string* error_out)
	{
		std::error_code ec;
		if (!uam::paths::IsRegularFileNoThrow(path))
		{
			SetError(error_out, "Import source is not a regular file.");
			return std::nullopt;
		}
		if (!IsSupportedImportFile(path))
		{
			SetError(error_out, "Unsupported file type; expected .md, .markdown, or .uam.");
			return std::nullopt;
		}
		const std::uintmax_t size = fs::file_size(path, ec);
		if (ec || size > kImportMaxBytes)
		{
			SetError(error_out, ec ? "Could not inspect import source size." : "Import source exceeds the 2 MiB limit.");
			return std::nullopt;
		}

		std::string text;
		if (!uam::io::TryReadTextFile(path, text))
		{
			SetError(error_out, "Could not read import source.");
			return std::nullopt;
		}
		const MarkdownDocumentParts parts = ParseMarkdownDocumentParts(text);
		MarkdownStoreService::Draft draft;
		draft.title = uam::strings::SafeLine(HeaderValue(parts.headers, "title"), kTitleMaxChars, true);
		if (draft.title.empty()) draft.title = FirstHeadingTitle(parts.body);
		if (draft.title.empty()) draft.title = path.stem().string();
		draft.maker = uam::strings::SafeLine(HeaderValue(parts.headers, "maker"), kMakerMaxChars, true);
		draft.review = uam::strings::SafeLine(HeaderValue(parts.headers, "review"), kReviewMaxChars, true);
		draft.body = uam::strings::Trim(parts.body);
		draft.group = uam::strings::SafeLine(HeaderValue(parts.headers, "group"), kGroupMaxChars, true);
		MarkdownStoreService::Draft sanitized;
		if (!SanitizeDraft(std::move(draft), sanitized, error_out))
		{
			return std::nullopt;
		}
		return sanitized;
	}

	std::vector<fs::path> SourceFiles(const fs::path& source)
	{
		if (uam::paths::IsRegularFileNoThrow(source)) return {source};
		std::vector<fs::path> files;
		if (!uam::paths::IsDirectoryNoThrow(source)) return files;
		std::error_code ec;
		for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->is_regular_file(ec) && !it->is_symlink(ec)) files.push_back(it->path());
		}
		std::ranges::sort(files);
		return files;
	}

	std::filesystem::path FindCollision(const std::vector<MarkdownStoreService::Entry>& entries, std::string_view provider, const fs::path& source_path, std::string_view title)
	{
		const std::string normalized_title = uam::strings::TrimAndLowerAscii(std::string(title));
		const std::string normalized_source = uam::paths::Utf8PathString(uam::paths::NormalizeExistingPath(source_path));
		for (const auto& entry : entries)
		{
			if ((!entry.source_path.empty() && entry.source_provider == provider && entry.source_path == normalized_source) ||
			    uam::strings::TrimAndLowerAscii(entry.title) == normalized_title)
			{
				return entry.file_path;
			}
		}
		return {};
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

fs::path MarkdownStoreService::BundledRootForExecutable(const fs::path& executable)
{
	if (executable.empty()) return {};
	const fs::path executable_directory = executable.parent_path();
	for (const fs::path& candidate : {
	         executable_directory / "markdown-store",
	         executable_directory.parent_path() / "Resources" / "markdown-store",
	     })
	{
		if (uam::paths::IsDirectoryNoThrow(candidate)) return uam::paths::NormalizeExistingPath(candidate);
	}
	return {};
}

bool MarkdownStoreService::SeedBundledEntries(const fs::path& bundled_root, const fs::path& destination_root, std::string* error_out)
{
	ClearError(error_out);
	if (!IsConfiguredRoot(bundled_root, error_out)) return false;
	if (destination_root.empty())
	{
		SetError(error_out, "Markdown Store destination is unavailable.");
		return false;
	}

	std::error_code ec;
	if (!uam::paths::CreateDirectoriesNoThrow(destination_root, &ec))
	{
		SetError(error_out, "Failed to create the Markdown Store directory.");
		return false;
	}
	const fs::path source_root = uam::paths::NormalizeExistingPath(bundled_root);
	const fs::path target_root = uam::paths::NormalizeExistingPath(destination_root);
	if (source_root == target_root) return true;

	std::string list_error;
	const std::vector<Entry> bundled_entries = ListEntries(source_root, &list_error);
	if (!list_error.empty())
	{
		SetError(error_out, list_error);
		return false;
	}
	const std::vector<Entry> existing_entries = ListEntries(target_root, &list_error);
	if (!list_error.empty())
	{
		SetError(error_out, list_error);
		return false;
	}

	std::set<std::string> titles;
	std::set<std::string> commands;
	for (const Entry& entry : existing_entries)
	{
		titles.insert(uam::strings::TrimAndLowerAscii(entry.title));
		commands.insert(uam::strings::TrimAndLowerAscii(entry.command_name));
	}
	for (const Entry& entry : bundled_entries)
	{
		const std::string title = uam::strings::TrimAndLowerAscii(entry.title);
		const std::string command = uam::strings::TrimAndLowerAscii(entry.command_name);
		if (titles.contains(title) || commands.contains(command)) continue;

		const std::optional<fs::path> relative = uam::paths::RelativePathIfInsideRoot(source_root, entry.file_path);
		if (!relative)
		{
			SetError(error_out, "Bundled Markdown Store entry escaped its source directory.");
			return false;
		}
		const fs::path target = target_root / *relative;
		if (uam::paths::PathExistsNoThrow(target)) continue;
		if (!uam::paths::CreateDirectoriesNoThrow(target.parent_path(), &ec) ||
		    !fs::copy_file(entry.file_path, target, fs::copy_options::none, ec))
		{
			SetError(error_out, "Failed to import bundled Markdown Store entries.");
			return false;
		}
		titles.insert(title);
		commands.insert(command);
	}
	return true;
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

	std::ranges::sort(entries, {}, [](const Entry& entry) { return uam::paths::Utf8PathString(entry.file_path); });
	std::set<std::string> command_names;
	for (Entry& entry : entries)
	{
		std::string base = entry.command_name.empty() ? uam::strings::AsciiSlug(entry.title, kCommandMaxChars, "skill") : entry.command_name;
		std::string candidate = base;
		for (int suffix = 2; command_names.contains(candidate); ++suffix)
		{
			candidate = uam::strings::AsciiSlug(base, kCommandMaxChars - 8, "skill") + "-" + std::to_string(suffix);
		}
		entry.command_name = std::move(candidate);
		command_names.insert(entry.command_name);
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
	PersistedMetadata metadata;
	metadata.command_name = UniqueCommandName(root, draft.title);
	if (!uam::io::WriteTextFile(target, BuildMarkdown(draft, metadata)))
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

bool MarkdownStoreService::UpdateEntry(const fs::path& root, std::string_view file_path, const Draft& input, Entry* updated_entry, std::string* error_out)
{
	ClearError(error_out);
	if (updated_entry != nullptr) *updated_entry = Entry{};
	fs::path target;
	if (!ValidateStoreFilePath(root, file_path, &target, error_out)) return false;

	Draft draft;
	if (!SanitizeDraft(input, draft, error_out)) return false;
	Entry existing;
	if (!ParseEntryFile(target, existing))
	{
		SetError(error_out, "Markdown Store entry could not be loaded.");
		return false;
	}
	PersistedMetadata metadata{existing.date_created, existing.favorite, existing.source_provider, existing.source_path,
	                           existing.command_name.empty() ? UniqueCommandName(root, existing.title, target) : existing.command_name};
	if (!uam::io::WriteTextFile(target, BuildMarkdown(draft, metadata)))
	{
		SetError(error_out, "Failed to atomically update Markdown Store file.");
		return false;
	}
	if (updated_entry != nullptr)
	{
		if (!ParseEntryFile(target, *updated_entry))
		{
			SetError(error_out, "Markdown Store file was updated but could not be reloaded.");
			return false;
		}
		updated_entry->id = EntryIdForPath(root, target);
	}
	return true;
}

bool MarkdownStoreService::SetFavorite(const fs::path& root, std::string_view file_path, bool favorite, Entry* updated_entry, std::string* error_out)
{
	ClearError(error_out);
	if (updated_entry != nullptr) *updated_entry = Entry{};
	fs::path target;
	if (!ValidateStoreFilePath(root, file_path, &target, error_out)) return false;
	Entry existing;
	if (!ParseEntryFile(target, existing))
	{
		SetError(error_out, "Markdown Store entry could not be loaded.");
		return false;
	}
	Draft draft{existing.title, existing.maker, existing.review, existing.body, existing.group};
	PersistedMetadata metadata{existing.date_created, favorite, existing.source_provider, existing.source_path,
	                           existing.command_name.empty() ? UniqueCommandName(root, existing.title, target) : existing.command_name};
	if (!uam::io::WriteTextFile(target, BuildMarkdown(draft, metadata)))
	{
		SetError(error_out, "Failed to atomically update Markdown Store favorite.");
		return false;
	}
	if (updated_entry != nullptr)
	{
		if (!ParseEntryFile(target, *updated_entry))
		{
			SetError(error_out, "Markdown Store favorite was updated but could not be reloaded.");
			return false;
		}
		updated_entry->id = EntryIdForPath(root, target);
	}
	return true;
}

std::vector<MarkdownStoreService::ImportSource> MarkdownStoreService::DefaultImportSources()
{
	std::vector<ImportSource> sources;
	auto add = [&sources](std::string provider, const fs::path& base, std::initializer_list<const char*> children)
	{
		for (const char* child : children)
		{
			const fs::path path = base / child;
			if (uam::paths::PathExistsNoThrow(path)) sources.push_back({provider, path});
		}
	};
	const std::optional<fs::path> home = PlatformServicesFactory::Instance().path_service.ResolveUserHomePath();
	if (const auto configured = uam::env::GetTrimmedPath("CODEX_HOME")) add("codex", *configured, {"skills", "prompts"});
	else if (home) add("codex", *home / ".codex", {"skills", "prompts"});
	if (const auto configured = uam::env::GetTrimmedPath("CLAUDE_CONFIG_DIR")) add("claude-code", *configured, {"commands", "skills"});
	else if (home) add("claude-code", *home / ".claude", {"commands", "skills"});
	if (const auto configured = uam::env::GetTrimmedPath("GEMINI_CLI_HOME")) add("gemini-cli", *configured, {"commands", "skills"});
	else if (const auto configured = uam::env::GetTrimmedPath("GEMINI_HOME")) add("gemini-cli", *configured, {"commands", "skills"});
	else if (home) add("gemini-cli", *home / ".gemini", {"commands", "skills"});
	if (const auto configured = uam::env::GetTrimmedPath("XDG_CONFIG_HOME")) add("opencode", *configured / "opencode", {"commands", "skills"});
	else if (home) add("opencode", *home / ".config" / "opencode", {"commands", "skills"});
	if (const auto configured = uam::env::GetTrimmedPath("COPILOT_HOME")) add("github-copilot", *configured, {"commands", "prompts", "skills"});
	else if (home) add("github-copilot", *home / ".copilot", {"commands", "prompts", "skills"});
	return sources;
}

std::vector<MarkdownStoreService::ImportCandidate> MarkdownStoreService::PreviewImports(const fs::path& root, const std::vector<ImportSource>& sources, std::string* error_out)
{
	ClearError(error_out);
	if (!IsConfiguredRoot(root, error_out)) return {};
	std::string list_error;
	const std::vector<Entry> entries = ListEntries(root, &list_error);
	if (!list_error.empty())
	{
		SetError(error_out, list_error);
		return {};
	}

	std::vector<ImportCandidate> candidates;
	std::set<std::string> seen;
	for (const ImportSource& source : sources)
	{
		const std::vector<fs::path> files = SourceFiles(source.path);
		if (files.empty() && !uam::paths::PathExistsNoThrow(source.path)) continue;
		for (const fs::path& path : files)
		{
			const std::string provider = uam::strings::SafeLine(source.provider, kMakerMaxChars, true).empty() ? "manual" : uam::strings::SafeLine(source.provider, kMakerMaxChars, true);
			const std::string id = CandidateId(provider, path);
			if (!seen.insert(id).second) continue;
			ImportCandidate candidate;
			candidate.id = id;
			candidate.source_provider = provider;
			candidate.source_path = uam::paths::NormalizeExistingPath(path);
			if (uam::paths::IsSameOrInsideRoot(root, candidate.source_path))
			{
				candidate.validation_error = "Source is already inside the managed Markdown Store.";
			}
			else
			{
				std::string validation_error;
				if (const auto draft = ReadImportDraft(path, &validation_error))
				{
					candidate.supported = true;
					candidate.title = draft->title;
					candidate.maker = draft->maker;
					candidate.review = draft->review;
					candidate.preview = PreviewFromBody(draft->body);
					candidate.collision_path = FindCollision(entries, provider, path, draft->title);
				}
				else candidate.validation_error = validation_error;
			}
			candidates.push_back(std::move(candidate));
		}
	}
	return candidates;
}

std::vector<MarkdownStoreService::ImportResult> MarkdownStoreService::ImportEntries(const fs::path& root, const std::vector<ImportRequest>& requests, std::string* error_out)
{
	ClearError(error_out);
	if (!IsConfiguredRoot(root, error_out)) return {};
	std::string list_error;
	std::vector<Entry> entries = ListEntries(root, &list_error);
	if (!list_error.empty())
	{
		SetError(error_out, list_error);
		return {};
	}

	std::vector<ImportResult> results;
	for (const ImportRequest& request : requests)
	{
		ImportResult result;
		result.source_path = uam::paths::NormalizeExistingPath(request.source_path);
		const std::string provider = uam::strings::SafeLine(request.source_provider, kMakerMaxChars, true).empty() ? "manual" : uam::strings::SafeLine(request.source_provider, kMakerMaxChars, true);
		if (uam::paths::IsSameOrInsideRoot(root, result.source_path))
		{
			result.status = "error";
			result.message = "Source is already inside the managed Markdown Store.";
			results.push_back(std::move(result));
			continue;
		}
		std::string import_error;
		const auto draft = ReadImportDraft(result.source_path, &import_error);
		if (!draft)
		{
			result.status = "error";
			result.message = import_error;
			results.push_back(std::move(result));
			continue;
		}

		const fs::path collision = FindCollision(entries, provider, result.source_path, draft->title);
		if (!collision.empty() && request.conflict_action == ImportConflictAction::Skip)
		{
			result.status = "skipped";
			result.message = "Skipped because a matching entry already exists.";
			results.push_back(std::move(result));
			continue;
		}
		const fs::path target = !collision.empty() && request.conflict_action == ImportConflictAction::Replace ? collision : ResolveCreateTarget(root, draft->title);
		Entry existing;
		if (target == collision) (void)ParseEntryFile(target, existing);
		PersistedMetadata metadata{existing.date_created, existing.favorite, provider, uam::paths::Utf8PathString(result.source_path),
		                           existing.command_name.empty() ? UniqueCommandName(root, draft->title, target) : existing.command_name};
		if (!uam::io::WriteTextFile(target, BuildMarkdown(*draft, metadata)))
		{
			result.status = "error";
			result.message = "Failed to atomically write imported .uam file.";
			results.push_back(std::move(result));
			continue;
		}
		ParseEntryFile(target, result.entry);
		result.entry.id = EntryIdForPath(root, target);
		result.status = "imported";
		result.message = target == collision ? "Replaced the matching managed entry." : "Imported as a new managed entry.";
		entries.erase(std::remove_if(entries.begin(), entries.end(), [&target](const Entry& entry) { return uam::paths::NormalizeExistingPath(entry.file_path) == uam::paths::NormalizeExistingPath(target); }), entries.end());
		entries.push_back(result.entry);
		results.push_back(std::move(result));
	}
	return results;
}

bool MarkdownStoreService::LoadEntry(const fs::path& root, std::string_view file_path, Entry* entry_out, std::string* error_out)
{
	ClearError(error_out);
	if (entry_out != nullptr)
		*entry_out = Entry{};

	fs::path normalized_path;
	if (!ValidateStoreFilePath(root, file_path, &normalized_path, error_out))
	{
		return false;
	}
	const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(normalized_path);
	if (!size || *size > kImportMaxBytes)
	{
		SetError(error_out, size ? "Markdown Store file exceeds the 2 MiB limit." : "Could not inspect Markdown Store file size.");
		return false;
	}

	Entry entry;
	if (!ParseEntryFile(normalized_path, entry))
	{
		SetError(error_out, "Could not read Markdown Store file.");
		return false;
	}
	entry.id = EntryIdForPath(root, normalized_path);
	if (entry_out != nullptr)
		*entry_out = std::move(entry);
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
