#include "app/memory_library_service.h"

#include "app/chat_domain_service.h"
#include "app/memory_service.h"
#include "common/memory/memory_categories.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/sensitive_text.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
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
	constexpr std::size_t kMemoryMaxChars = 1400;
	constexpr std::size_t kEvidenceMaxChars = 900;
	constexpr std::size_t kConfidenceMaxChars = 80;
	constexpr std::size_t kSourceChatIdMaxChars = 120;
	constexpr std::size_t kPreviewMaxChars = 320;

	enum class MemoryEntrySection
	{
		Header,
		Memory,
		Evidence,
	};

	std::string CanonicalRootKey(const fs::path& root)
	{
		return uam::paths::NormalizeExistingPath(root).string();
	}

	std::string HexEncode(std::string_view value)
	{
		static constexpr char kDigits[] = "0123456789abcdef";
		std::string out;
		out.reserve(value.size() * 2);
		for (const unsigned char ch : value)
		{
			out.push_back(kDigits[(ch >> 4) & 0x0f]);
			out.push_back(kDigits[ch & 0x0f]);
		}
		return out;
	}

	bool HexDecode(std::string_view value, std::string& out)
	{
		out.clear();
		if (value.size() % 2 != 0)
		{
			return false;
		}

		std::string decoded;
		decoded.reserve(value.size() / 2);
		for (std::size_t i = 0; i < value.size(); i += 2)
		{
			const int high = uam::strings::HexDigitValue(static_cast<unsigned char>(value[i]));
			const int low = uam::strings::HexDigitValue(static_cast<unsigned char>(value[i + 1]));
			if (high < 0 || low < 0)
			{
				return false;
			}
			decoded.push_back(static_cast<char>((high << 4) | low));
		}
		out = std::move(decoded);
		return true;
	}

	std::string BuildAggregateEntryId(const fs::path& root, std::string_view relative_id)
	{
		std::string id = "all/";
		id += HexEncode(CanonicalRootKey(root));
		id.push_back('/');
		id.append(relative_id);
		return id;
	}

	bool ParseAggregateEntryId(std::string_view entry_id, std::string& root_key, std::string& relative_id)
	{
		constexpr const char* kPrefix = "all/";
		const std::string trimmed = uam::strings::Trim(entry_id);
		if (!uam::strings::StartsWith(trimmed, kPrefix))
		{
			return false;
		}

		const std::size_t encoded_start = std::char_traits<char>::length(kPrefix);
		const std::size_t separator = trimmed.find('/', encoded_start);
		if (separator == std::string::npos || separator == encoded_start || separator + 1 >= trimmed.size())
		{
			return false;
		}

		const std::string_view trimmed_view(trimmed);
		if (!HexDecode(trimmed_view.substr(encoded_start, separator - encoded_start), root_key))
		{
			return false;
		}
		relative_id = trimmed.substr(separator + 1);
		return !root_key.empty() && !relative_id.empty();
	}

	std::string WorkspaceLabel(const fs::path& workspace_root, const ChatSession& chat)
	{
		const std::string filename = workspace_root.filename().string();
		const std::string fallback = uam::strings::NonEmptyOrFallback(filename, "Project memory");
		return uam::strings::TrimOrFallback(chat.title, fallback);
	}

	fs::path FolderWorkspaceRoot(const ChatFolder& folder)
	{
		return PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(folder.directory));
	}

	void SetError(std::string* error_out, std::string_view message)
	{
		if (error_out != nullptr)
		{
			error_out->assign(message);
		}
	}

	void ClearError(std::string* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}
	}

	void AddUniqueMemoryLibraryRoot(std::vector<MemoryLibraryService::Root>& roots,
	                                std::set<std::string>& seen,
	                                std::string scope_type,
	                                std::string folder_id,
	                                std::string label,
	                                const fs::path& root_path)
	{
		if (root_path.empty())
		{
			return;
		}

		const std::string key = CanonicalRootKey(root_path);
		if (!seen.insert(key).second)
		{
			return;
		}

		MemoryLibraryService::Root root;
		root.scope_type = std::move(scope_type);
		root.folder_id = std::move(folder_id);
		root.label = uam::strings::TrimOrFallback(label, "Project memory");
		root.root_path = root_path;
		roots.push_back(std::move(root));
	}

	std::vector<MemoryLibraryService::Root> CollectAllMemoryRoots(const uam::AppState& app)
	{
		std::vector<MemoryLibraryService::Root> roots;
		std::set<std::string> seen;

		if (!app.data_root.empty())
		{
			AddUniqueMemoryLibraryRoot(roots, seen, "global", "", "Global memory", MemoryService::GlobalMemoryRoot(app.data_root));
		}

		for (const ChatFolder& folder : app.folders)
		{
			const fs::path workspace_root = FolderWorkspaceRoot(folder);
			AddUniqueMemoryLibraryRoot(roots, seen, "folder", folder.id, uam::strings::TrimOrFallback(folder.title, "Project memory"), MemoryService::LocalMemoryRoot(workspace_root));
		}

		for (const ChatSession& chat : app.chats)
		{
			const ChatFolder* folder = uam::paths::FindWorkspaceFolderById(app, chat.folder_id);
			if (uam::strings::IsBlank(chat.workspace_directory) &&
			    !uam::paths::HasGitWorktreeDirectory(chat) &&
			    (folder == nullptr || uam::strings::IsBlank(folder->directory)))
			{
				continue;
			}
			const fs::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
			AddUniqueMemoryLibraryRoot(roots, seen, "folder", chat.folder_id, WorkspaceLabel(workspace_root, chat), MemoryService::LocalMemoryRoot(workspace_root));
		}

		return roots;
	}

	std::string ReadHeaderValue(const std::map<std::string, std::string>& headers, const std::string& key, const std::string& fallback = "")
	{
		const auto found = headers.find(key);
		return found == headers.end() ? fallback : found->second;
	}

	bool ParseEntryFile(const fs::path& path, MemoryLibraryService::Entry& out_entry)
	{
		out_entry = MemoryLibraryService::Entry{};

		std::string text;
		if (!uam::io::TryReadTextFile(path, text))
		{
			return false;
		}

		std::istringstream in(text);
		std::string line;
		std::map<std::string, std::string> headers;
		MemoryEntrySection section = MemoryEntrySection::Header;
		std::vector<std::string> memory_lines;

		while (std::getline(in, line))
		{
			const std::string trimmed = uam::strings::Trim(line);
			if (out_entry.title.empty() && uam::strings::StartsWith(line, "# "))
			{
				out_entry.title = uam::strings::Trim(line.substr(2));
				continue;
			}

			if (trimmed == "## Memory")
			{
				section = MemoryEntrySection::Memory;
				continue;
			}

			if (trimmed == "## Evidence")
			{
				section = MemoryEntrySection::Evidence;
				continue;
			}

			if (section == MemoryEntrySection::Header)
			{
				const std::size_t colon = line.find(':');
				if (colon != std::string::npos)
				{
					headers[uam::strings::Trim(line.substr(0, colon))] = uam::strings::Trim(line.substr(colon + 1));
				}
				continue;
			}

			if (section == MemoryEntrySection::Memory)
			{
				memory_lines.push_back(line);
			}
		}

		if (out_entry.title.empty())
		{
			return false;
		}

		out_entry.category = ReadHeaderValue(headers, "Category");
		out_entry.scope = uam::strings::ToLowerAscii(ReadHeaderValue(headers, "Scope", "local"));
		out_entry.confidence = ReadHeaderValue(headers, "Confidence", "medium");
		out_entry.source_chat_id = ReadHeaderValue(headers, "Source chat");
		out_entry.last_observed = ReadHeaderValue(headers, "Last observed");
		out_entry.occurrence_count = std::max(1, uam::parse::IntOr(ReadHeaderValue(headers, "Occurrence count"), 1));
		out_entry.preview = uam::strings::SafeLine(uam::strings::Join(memory_lines, "\n"), kPreviewMaxChars);
		out_entry.file_path = path;
		return uam::memory::IsSupportedCategory(out_entry.category);
	}

	fs::path FindExistingMemoryFile(const fs::path& category_path, std::string_view title)
	{
		const std::string wanted = uam::strings::NormalizeComparableKey(title);
		if (wanted.empty() || !uam::paths::IsDirectoryNoThrow(category_path))
		{
			return {};
		}

		std::error_code ec;
		for (fs::directory_iterator it(category_path, ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::directory_entry& item = *it;
			if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".md"))
			{
				continue;
			}

			MemoryLibraryService::Entry existing;
			if (ParseEntryFile(item.path(), existing) && uam::strings::NormalizeComparableKey(existing.title) == wanted)
			{
				return item.path();
			}
		}
		return {};
	}

	struct MemoryEntryTarget
	{
		fs::path path;
		int occurrence_count = 1;
	};

	MemoryEntryTarget ResolveMemoryEntryTarget(const fs::path& category_path, std::string_view title)
	{
		MemoryEntryTarget target;
		target.path = FindExistingMemoryFile(category_path, title);
		if (!target.path.empty())
		{
			MemoryLibraryService::Entry existing;
			if (ParseEntryFile(target.path, existing))
			{
				target.occurrence_count = existing.occurrence_count + 1;
			}
			return target;
		}

		const std::string slug = uam::strings::AsciiSlug(title, 72, "memory");
		target.path = category_path / (slug + ".md");
		for (int i = 2; uam::paths::PathExistsNoThrow(target.path); ++i)
		{
			target.path = category_path / (slug + "-" + std::to_string(i) + ".md");
		}
		return target;
	}

	std::string PersistedScopeFor(const MemoryLibraryService::Scope& scope)
	{
		return scope.scope_type == "global" ? "global" : "local";
	}

	std::string BuildMemoryMarkdown(const MemoryLibraryService::Draft& draft, std::string_view scope, int occurrence_count)
	{
		std::string markdown;
		markdown.reserve(draft.title.size() + draft.category.size() + draft.confidence.size() + draft.source_chat_id.size() + draft.memory.size() + draft.evidence.size() + 180);
		markdown = "# " + draft.title + "\n\n";
		markdown += "Scope: ";
		markdown.append(scope);
		markdown += "\n";
		markdown += "Category: " + draft.category + "\n";
		markdown += "Confidence: " + draft.confidence + "\n";
		markdown += "Source chat: " + draft.source_chat_id + "\n";
		markdown += "Last observed: " + uam::time::TimestampNow() + "\n";
		markdown += "Occurrence count: " + std::to_string(std::max(1, occurrence_count)) + "\n\n";
		markdown += "## Memory\n";
		markdown += draft.memory + "\n\n";
		if (!draft.evidence.empty())
		{
			markdown += "## Evidence\n";
			markdown += draft.evidence + "\n";
		}
		return markdown;
	}

	std::string EntryIdForPath(const fs::path& root_path, const fs::path& entry_path)
	{
		if (const std::optional<fs::path> relative = uam::paths::RelativePathIfInsideRoot(root_path, entry_path))
		{
			return uam::paths::PortablePathString(*relative);
		}
		return entry_path.filename().string();
	}

	void PopulateEntryScope(MemoryLibraryService::Entry& entry, const MemoryLibraryService::Root& root, bool aggregate)
	{
		entry.id = EntryIdForPath(root.root_path, entry.file_path);
		if (aggregate)
		{
			entry.id = BuildAggregateEntryId(root.root_path, entry.id);
		}
		entry.scope_type = root.scope_type;
		entry.folder_id = root.folder_id;
		entry.scope_label = root.label;
		entry.root_path = root.root_path;
	}

	void PopulateEntryScope(MemoryLibraryService::Entry& entry, const MemoryLibraryService::Scope& scope)
	{
		PopulateEntryScope(entry, MemoryLibraryService::Root{scope.scope_type, scope.folder_id, scope.label, scope.root_path}, false);
	}

	MemoryLibraryService::Scope ScopeFromRoot(const MemoryLibraryService::Root& root)
	{
		MemoryLibraryService::Scope scope;
		scope.scope_type = root.scope_type;
		scope.folder_id = root.folder_id;
		scope.label = root.label;
		scope.root_path = root.root_path;
		scope.roots = {root};
		return scope;
	}

	std::vector<MemoryLibraryService::Root> EffectiveRootsForScope(const MemoryLibraryService::Scope& scope)
	{
		std::vector<MemoryLibraryService::Root> roots = scope.roots;
		if (roots.empty() && !scope.root_path.empty())
		{
			roots.push_back(MemoryLibraryService::Root{scope.scope_type, scope.folder_id, scope.label, scope.root_path});
		}
		return roots;
	}

	bool AppendEntriesFromRoot(const MemoryLibraryService::Root& root, bool aggregate, std::vector<MemoryLibraryService::Entry>& entries, std::string* error_out)
	{
		if (root.root_path.empty())
		{
			return true;
		}

		for (const std::string& category : uam::memory::SupportedCategories())
		{
			const fs::path category_path = MemoryService::CategoryPath(root.root_path, category);
			if (!uam::paths::IsDirectoryNoThrow(category_path))
			{
				continue;
			}

			std::error_code ec;
			for (fs::directory_iterator it(category_path, ec), end; !ec && it != end; it.increment(ec))
			{
				const fs::directory_entry& item = *it;
				if (!uam::paths::IsRegularFileWithExtensionNoThrow(item, ".md"))
				{
					continue;
				}

				MemoryLibraryService::Entry entry;
				if (ParseEntryFile(item.path(), entry))
				{
					PopulateEntryScope(entry, root, aggregate);
					entries.push_back(std::move(entry));
				}
			}
			if (ec)
			{
				SetError(error_out, "Failed to enumerate memory files.");
				return false;
			}
		}
		return true;
	}

	bool MemoryEntryOrderLess(const MemoryLibraryService::Entry& lhs, const MemoryLibraryService::Entry& rhs)
	{
		if (lhs.scope_label != rhs.scope_label)
		{
			return lhs.scope_label < rhs.scope_label;
		}
		if (lhs.category != rhs.category)
		{
			return lhs.category < rhs.category;
		}
		if (lhs.title != rhs.title)
		{
			return lhs.title < rhs.title;
		}
		return lhs.file_path.filename() < rhs.file_path.filename();
	}

	bool SanitizeDraft(MemoryLibraryService::Draft input, MemoryLibraryService::Draft& draft, std::string* error_out)
	{
		input.category = uam::strings::Trim(input.category);
		input.title = uam::strings::SafeLine(input.title, kTitleMaxChars);
		input.memory = uam::strings::SafeLine(input.memory, kMemoryMaxChars);
		input.evidence = uam::strings::SafeLine(input.evidence, kEvidenceMaxChars);
		input.confidence = uam::strings::SafeLine(uam::strings::NonEmptyOrFallback(input.confidence, "medium"), kConfidenceMaxChars);
		input.source_chat_id = uam::strings::SafeLine(input.source_chat_id, kSourceChatIdMaxChars);

		if (!uam::memory::IsSupportedCategory(input.category) || input.title.empty() || input.memory.empty())
		{
			SetError(error_out, "Memory entry is missing a valid category, title, or body.");
			return false;
		}

		if (uam::sensitive::LooksSensitiveText(input.title + "\n" + input.memory + "\n" + input.evidence))
		{
			SetError(error_out, "Memory entry appears to contain sensitive content.");
			return false;
		}

		draft = std::move(input);
		return true;
	}

	bool DeleteAggregateEntry(const MemoryLibraryService::Scope& scope, std::string_view entry_id, std::string* error_out)
	{
		std::string root_key;
		std::string relative_id;
		if (!ParseAggregateEntryId(entry_id, root_key, relative_id))
		{
			SetError(error_out, "Invalid aggregate memory entry id.");
			return false;
		}

		for (const MemoryLibraryService::Root& root : scope.roots)
		{
			if (CanonicalRootKey(root.root_path) != root_key)
			{
				continue;
			}

			return MemoryLibraryService::DeleteEntry(ScopeFromRoot(root), relative_id, error_out);
		}

		SetError(error_out, "Aggregate memory root is no longer known.");
		return false;
	}

	bool ResolveDeletableEntryPath(const MemoryLibraryService::Scope& scope, std::string_view entry_id, fs::path& entry_path, std::string* error_out)
	{
		if (scope.root_path.empty())
		{
			SetError(error_out, "Memory root is unavailable.");
			return false;
		}

		const std::string trimmed_id = uam::strings::Trim(entry_id);
		if (trimmed_id.empty())
		{
			SetError(error_out, "Memory entry id is required.");
			return false;
		}

		entry_path = scope.root_path / fs::path(trimmed_id);
		if (!uam::paths::IsSameOrInsideRoot(scope.root_path, entry_path))
		{
			SetError(error_out, "Memory entry path is outside the memory root.");
			return false;
		}

		if (!uam::paths::PathExistsNoThrow(entry_path))
		{
			SetError(error_out, "Memory entry not found: " + trimmed_id);
			return false;
		}

		if (entry_path.extension() != ".md")
		{
			SetError(error_out, "Only markdown memory files can be deleted.");
			return false;
		}

		return true;
	}
} // namespace

bool MemoryLibraryService::ResolveScope(const uam::AppState& app, std::string_view scope_type, std::string_view folder_id, Scope& out_scope, std::string* error_out)
{
	ClearError(error_out);
	out_scope = Scope{};
	const std::string normalized_scope = uam::strings::TrimAndLowerAscii(scope_type);
	if (normalized_scope == "all")
	{
		out_scope.scope_type = "all";
		out_scope.folder_id.clear();
		out_scope.label = "All memory";
		out_scope.root_path.clear();
		out_scope.roots = CollectAllMemoryRoots(app);
		return true;
	}

	if (normalized_scope == "global")
	{
		out_scope.scope_type = "global";
		out_scope.folder_id.clear();
		out_scope.label = "Global memory";
		out_scope.root_path = MemoryService::GlobalMemoryRoot(app.data_root);
		out_scope.roots = {Root{"global", "", out_scope.label, out_scope.root_path}};
		return true;
	}

	if (normalized_scope == "folder" || normalized_scope == "local")
	{
		const std::string normalized_folder_id = uam::strings::Trim(folder_id);
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, normalized_folder_id);
		if (folder == nullptr)
		{
			SetError(error_out, "Folder not found: " + normalized_folder_id);
			return false;
		}

		const fs::path workspace_root = FolderWorkspaceRoot(*folder);
		if (workspace_root.empty())
		{
			SetError(error_out, "Folder has no workspace directory.");
			return false;
		}

		out_scope.scope_type = "folder";
		out_scope.folder_id = folder->id;
		out_scope.label = uam::strings::TrimOrFallback(folder->title, "Project memory");
		out_scope.root_path = MemoryService::LocalMemoryRoot(workspace_root);
		out_scope.roots = {Root{"folder", folder->id, out_scope.label, out_scope.root_path}};
		return true;
	}

	SetError(error_out, "Unsupported memory scope.");
	return false;
}

std::vector<MemoryLibraryService::Entry> MemoryLibraryService::ListEntries(const Scope& scope, std::string* error_out)
{
	ClearError(error_out);
	std::vector<Entry> entries;
	const std::vector<Root> roots = EffectiveRootsForScope(scope);
	if (roots.empty())
	{
		SetError(error_out, "Memory root is unavailable.");
		return entries;
	}

	const bool aggregate = scope.scope_type == "all";
	for (const Root& root : roots)
	{
		if (!AppendEntriesFromRoot(root, aggregate, entries, error_out))
		{
			return {};
		}
	}

	std::ranges::sort(entries, MemoryEntryOrderLess);
	return entries;
}

bool MemoryLibraryService::CreateEntry(const Scope& scope, const Draft& input, Entry* created_entry, std::string* error_out)
{
	ClearError(error_out);
	if (created_entry != nullptr)
	{
		*created_entry = Entry{};
	}

	if (scope.scope_type == "all")
	{
		SetError(error_out, "All memory scope requires a concrete target.");
		return false;
	}

	if (scope.root_path.empty())
	{
		SetError(error_out, "Memory root is unavailable.");
		return false;
	}

	Draft draft;
	if (!SanitizeDraft(input, draft, error_out))
	{
		return false;
	}

	if (!MemoryService::EnsureMemoryLayout(scope.root_path))
	{
		SetError(error_out, "Failed to create memory directory layout.");
		return false;
	}

	const fs::path category_path = MemoryService::CategoryPath(scope.root_path, draft.category);
	const MemoryEntryTarget target = ResolveMemoryEntryTarget(category_path, draft.title);
	if (!uam::io::WriteTextFile(target.path, BuildMemoryMarkdown(draft, PersistedScopeFor(scope), target.occurrence_count)))
	{
		SetError(error_out, "Failed to write memory file.");
		return false;
	}

	if (created_entry != nullptr)
	{
		Entry created;
		if (!ParseEntryFile(target.path, created))
		{
			SetError(error_out, "Memory file was written but could not be reloaded.");
			return false;
		}
		PopulateEntryScope(created, scope);
		*created_entry = std::move(created);
	}
	return true;
}

bool MemoryLibraryService::DeleteEntry(const Scope& scope, std::string_view entry_id, std::string* error_out)
{
	ClearError(error_out);
	if (scope.scope_type == "all")
	{
		return DeleteAggregateEntry(scope, entry_id, error_out);
	}

	fs::path entry_path;
	if (!ResolveDeletableEntryPath(scope, entry_id, entry_path, error_out))
	{
		return false;
	}

	std::error_code ec;
	if (!uam::paths::RemoveFileNoThrow(entry_path, &ec))
	{
		SetError(error_out, ec ? "Failed to delete memory file." : "Memory file no longer exists.");
		return false;
	}
	return true;
}
