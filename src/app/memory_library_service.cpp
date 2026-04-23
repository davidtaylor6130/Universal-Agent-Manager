#include "app/memory_library_service.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/memory_service.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kFailuresAi = "Failures/AI_Failures";
	constexpr const char* kFailuresUser = "Failures/User_Failures";
	constexpr const char* kLessonsAi = "Lessons/AI_Lessons";
	constexpr const char* kLessonsUser = "Lessons/User_Lessons";

	const std::vector<std::string>& SupportedCategories()
	{
		static const std::vector<std::string> kCategories = {
			kFailuresAi,
			kFailuresUser,
			kLessonsAi,
			kLessonsUser,
		};
		return kCategories;
	}

	std::string Trimmed(const std::string& value)
	{
		return uam::strings::Trim(value);
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool IsSupportedCategory(const std::string& category)
	{
		return std::find(SupportedCategories().begin(), SupportedCategories().end(), category) != SupportedCategories().end();
	}

	bool LooksSensitive(const std::string& text)
	{
		const std::string lowered = LowerAscii(text);
		return lowered.find("api_key") != std::string::npos ||
		       lowered.find("apikey") != std::string::npos ||
		       lowered.find("password") != std::string::npos ||
		       lowered.find("secret") != std::string::npos ||
		       lowered.find("token=") != std::string::npos ||
		       lowered.find("bearer ") != std::string::npos ||
		       lowered.find("-----begin ") != std::string::npos;
	}

	std::string Slug(std::string value)
	{
		value = LowerAscii(value);
		std::string out;
		bool previous_dash = false;
		for (const unsigned char ch : value)
		{
			if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
			{
				out.push_back(static_cast<char>(ch));
				previous_dash = false;
			}
			else if (!previous_dash && !out.empty())
			{
				out.push_back('-');
				previous_dash = true;
			}
			if (out.size() >= 72)
			{
				break;
			}
		}
		while (!out.empty() && out.back() == '-')
		{
			out.pop_back();
		}
		return out.empty() ? "memory" : out;
	}

	std::string SafeLine(std::string value, const std::size_t max_chars = 700)
	{
		value = Trimmed(value);
		std::replace(value.begin(), value.end(), '\r', ' ');
		if (value.size() > max_chars)
		{
			value = value.substr(0, max_chars);
		}
		return value;
	}

	std::string NormalizeComparable(std::string value)
	{
		value = LowerAscii(value);
		std::string out;
		for (const unsigned char ch : value)
		{
			if (std::isalnum(ch))
			{
				out.push_back(static_cast<char>(ch));
			}
			else if (!out.empty() && out.back() != ' ')
			{
				out.push_back(' ');
			}
		}
		return Trimmed(out);
	}

	std::string CanonicalRootKey(const fs::path& root)
	{
		std::error_code ec;
		const fs::path canonical = fs::weakly_canonical(root, ec).lexically_normal();
		return (ec ? root.lexically_normal() : canonical).string();
	}

	std::string HexEncode(const std::string& value)
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

	int HexValue(const char ch)
	{
		if (ch >= '0' && ch <= '9')
		{
			return ch - '0';
		}
		if (ch >= 'a' && ch <= 'f')
		{
			return 10 + (ch - 'a');
		}
		if (ch >= 'A' && ch <= 'F')
		{
			return 10 + (ch - 'A');
		}
		return -1;
	}

	bool HexDecode(const std::string& value, std::string& out)
	{
		if (value.size() % 2 != 0)
		{
			return false;
		}

		std::string decoded;
		decoded.reserve(value.size() / 2);
		for (std::size_t i = 0; i < value.size(); i += 2)
		{
			const int high = HexValue(value[i]);
			const int low = HexValue(value[i + 1]);
			if (high < 0 || low < 0)
			{
				return false;
			}
			decoded.push_back(static_cast<char>((high << 4) | low));
		}
		out = std::move(decoded);
		return true;
	}

	std::string BuildAggregateEntryId(const fs::path& root, const std::string& relative_id)
	{
		return "all/" + HexEncode(CanonicalRootKey(root)) + "/" + relative_id;
	}

	bool ParseAggregateEntryId(const std::string& entry_id, std::string& root_key, std::string& relative_id)
	{
		constexpr const char* kPrefix = "all/";
		const std::string trimmed = Trimmed(entry_id);
		if (trimmed.rfind(kPrefix, 0) != 0)
		{
			return false;
		}

		const std::size_t encoded_start = std::char_traits<char>::length(kPrefix);
		const std::size_t separator = trimmed.find('/', encoded_start);
		if (separator == std::string::npos || separator == encoded_start || separator + 1 >= trimmed.size())
		{
			return false;
		}

		if (!HexDecode(trimmed.substr(encoded_start, separator - encoded_start), root_key))
		{
			return false;
		}
		relative_id = trimmed.substr(separator + 1);
		return !root_key.empty() && !relative_id.empty();
	}

	std::string WorkspaceLabel(const fs::path& workspace_root, const ChatSession& chat)
	{
		const std::string title = Trimmed(chat.title);
		if (!title.empty())
		{
			return title;
		}

		const std::string filename = workspace_root.filename().string();
		return filename.empty() ? "Project memory" : filename;
	}

	std::vector<MemoryLibraryService::Root> CollectAllMemoryRoots(const uam::AppState& app)
	{
		std::vector<MemoryLibraryService::Root> roots;
		std::set<std::string> seen;
		auto add_root = [&](std::string scope_type, std::string folder_id, std::string label, const fs::path& root_path)
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
			root.label = Trimmed(label).empty() ? "Project memory" : Trimmed(label);
			root.root_path = root_path;
			roots.push_back(std::move(root));
		};

		if (!app.data_root.empty())
		{
			add_root("global", "", "Global memory", MemoryService::GlobalMemoryRoot(app.data_root));
		}

		for (const ChatFolder& folder : app.folders)
		{
			const fs::path workspace_root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder.directory);
			add_root("folder", folder.id, Trimmed(folder.title).empty() ? "Project memory" : Trimmed(folder.title), MemoryService::LocalMemoryRoot(workspace_root));
		}

		for (const ChatSession& chat : app.chats)
		{
			const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
			add_root("folder", chat.folder_id, WorkspaceLabel(workspace_root, chat), MemoryService::LocalMemoryRoot(workspace_root));
		}

		return roots;
	}

	std::string ReadHeaderValue(const std::map<std::string, std::string>& headers, const std::string& key, const std::string& fallback = "")
	{
		const auto found = headers.find(key);
		return found == headers.end() ? fallback : found->second;
	}

	int ParseInt(const std::string& value, const int fallback)
	{
		try
		{
			return std::stoi(Trimmed(value));
		}
		catch (...)
		{
			return fallback;
		}
	}

	bool IsPathInsideRoot(const fs::path& root, const fs::path& candidate)
	{
		std::error_code root_ec;
		std::error_code candidate_ec;
		const fs::path normalized_root = fs::weakly_canonical(root, root_ec).lexically_normal();
		const fs::path normalized_candidate = fs::weakly_canonical(candidate, candidate_ec).lexically_normal();
		const fs::path& safe_root = root_ec ? root.lexically_normal() : normalized_root;
		const fs::path& safe_candidate = candidate_ec ? candidate.lexically_normal() : normalized_candidate;

		auto root_it = safe_root.begin();
		auto candidate_it = safe_candidate.begin();
		for (; root_it != safe_root.end(); ++root_it, ++candidate_it)
		{
			if (candidate_it == safe_candidate.end() || *root_it != *candidate_it)
			{
				return false;
			}
		}
		return true;
	}

	bool ParseEntryFile(const fs::path& path, MemoryLibraryService::Entry& out_entry)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.good())
		{
			return false;
		}

		std::string line;
		std::map<std::string, std::string> headers;
		bool in_memory_section = false;
		bool in_evidence_section = false;
		std::ostringstream memory_body;

		while (std::getline(in, line))
		{
			const std::string trimmed = Trimmed(line);
			if (out_entry.title.empty() && line.rfind("# ", 0) == 0)
			{
				out_entry.title = Trimmed(line.substr(2));
				continue;
			}

			if (trimmed == "## Memory")
			{
				in_memory_section = true;
				in_evidence_section = false;
				continue;
			}

			if (trimmed == "## Evidence")
			{
				in_memory_section = false;
				in_evidence_section = true;
				continue;
			}

			if (!in_memory_section && !in_evidence_section)
			{
				const std::size_t colon = line.find(':');
				if (colon != std::string::npos)
				{
					headers[Trimmed(line.substr(0, colon))] = Trimmed(line.substr(colon + 1));
				}
				continue;
			}

			if (in_memory_section)
			{
				if (!memory_body.str().empty())
				{
					memory_body << '\n';
				}
				memory_body << line;
			}
		}

		if (out_entry.title.empty())
		{
			return false;
		}

		out_entry.category = ReadHeaderValue(headers, "Category");
		out_entry.scope = LowerAscii(ReadHeaderValue(headers, "Scope", "local"));
		out_entry.confidence = ReadHeaderValue(headers, "Confidence", "medium");
		out_entry.source_chat_id = ReadHeaderValue(headers, "Source chat");
		out_entry.last_observed = ReadHeaderValue(headers, "Last observed");
		out_entry.occurrence_count = std::max(1, ParseInt(ReadHeaderValue(headers, "Occurrence count"), 1));
		out_entry.preview = SafeLine(memory_body.str(), 320);
		out_entry.file_path = path;
		return IsSupportedCategory(out_entry.category);
	}

	fs::path FindExistingMemoryFile(const fs::path& category_path, const std::string& title)
	{
		const std::string wanted = NormalizeComparable(title);
		if (wanted.empty() || !fs::exists(category_path))
		{
			return {};
		}

		std::error_code ec;
		for (const fs::directory_entry& item : fs::directory_iterator(category_path, ec))
		{
			if (ec || !item.is_regular_file() || item.path().extension() != ".md")
			{
				continue;
			}

			MemoryLibraryService::Entry existing;
			if (ParseEntryFile(item.path(), existing) && NormalizeComparable(existing.title) == wanted)
			{
				return item.path();
			}
		}
		return {};
	}

	std::string BuildMemoryMarkdown(const MemoryLibraryService::Draft& draft, const std::string& scope, const int occurrence_count)
	{
		std::ostringstream out;
		out << "# " << draft.title << "\n\n";
		out << "Scope: " << scope << "\n";
		out << "Category: " << draft.category << "\n";
		out << "Confidence: " << draft.confidence << "\n";
		out << "Source chat: " << draft.source_chat_id << "\n";
		out << "Last observed: " << uam::time::TimestampNow() << "\n";
		out << "Occurrence count: " << std::max(1, occurrence_count) << "\n\n";
		out << "## Memory\n";
		out << draft.memory << "\n\n";
		if (!draft.evidence.empty())
		{
			out << "## Evidence\n";
			out << draft.evidence << "\n";
		}
		return out.str();
	}
}

bool MemoryLibraryService::ResolveScope(const uam::AppState& app, const std::string& scope_type, const std::string& folder_id, Scope& out_scope, std::string* error_out)
{
	const std::string normalized_scope = LowerAscii(Trimmed(scope_type));
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
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, folder_id);
		if (folder == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Folder not found: " + folder_id;
			}
			return false;
		}

		const fs::path workspace_root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder->directory);
		if (workspace_root.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Folder has no workspace directory.";
			}
			return false;
		}

		out_scope.scope_type = "folder";
		out_scope.folder_id = folder->id;
		out_scope.label = Trimmed(folder->title).empty() ? "Project memory" : Trimmed(folder->title);
		out_scope.root_path = MemoryService::LocalMemoryRoot(workspace_root);
		out_scope.roots = {Root{"folder", folder->id, out_scope.label, out_scope.root_path}};
		return true;
	}

	if (error_out != nullptr)
	{
		*error_out = "Unsupported memory scope.";
	}
	return false;
}

std::vector<MemoryLibraryService::Entry> MemoryLibraryService::ListEntries(const Scope& scope, std::string* error_out)
{
	std::vector<Entry> entries;
	std::vector<Root> roots = scope.roots;
	if (roots.empty() && !scope.root_path.empty())
	{
		roots.push_back(Root{scope.scope_type, scope.folder_id, scope.label, scope.root_path});
	}

	if (roots.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory root is unavailable.";
		}
		return entries;
	}

	const bool aggregate = scope.scope_type == "all";
	for (const Root& root : roots)
	{
		if (root.root_path.empty())
		{
			continue;
		}

		for (const std::string& category : SupportedCategories())
		{
			const fs::path category_path = MemoryService::CategoryPath(root.root_path, category);
			if (!fs::exists(category_path))
			{
				continue;
			}

			std::error_code ec;
			for (const fs::directory_entry& item : fs::directory_iterator(category_path, ec))
			{
				if (ec)
				{
					if (error_out != nullptr)
					{
						*error_out = "Failed to enumerate memory files.";
					}
					return {};
				}
				if (!item.is_regular_file() || item.path().extension() != ".md")
				{
					continue;
				}

				Entry entry;
				if (ParseEntryFile(item.path(), entry))
				{
					std::error_code relative_ec;
					const std::string relative_id = fs::relative(item.path(), root.root_path, relative_ec).generic_string();
					entry.id = (relative_ec || relative_id.empty()) ? item.path().filename().string() : relative_id;
					if (aggregate)
					{
						entry.id = BuildAggregateEntryId(root.root_path, entry.id);
					}
					entry.scope_type = root.scope_type;
					entry.folder_id = root.folder_id;
					entry.scope_label = root.label;
					entry.root_path = root.root_path;
					entries.push_back(std::move(entry));
				}
			}
		}
	}

	std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
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
		return lhs.file_path.filename().string() < rhs.file_path.filename().string();
	});
	return entries;
}

bool MemoryLibraryService::CreateEntry(const Scope& scope, const Draft& input, Entry* created_entry, std::string* error_out)
{
	if (scope.scope_type == "all")
	{
		if (error_out != nullptr)
		{
			*error_out = "All memory scope requires a concrete target.";
		}
		return false;
	}

	if (scope.root_path.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory root is unavailable.";
		}
		return false;
	}

	Draft draft = input;
	draft.category = Trimmed(draft.category);
	draft.title = SafeLine(draft.title, 160);
	draft.memory = SafeLine(draft.memory, 1400);
	draft.evidence = SafeLine(draft.evidence, 900);
	draft.confidence = SafeLine(draft.confidence.empty() ? "medium" : draft.confidence, 80);
	draft.source_chat_id = SafeLine(draft.source_chat_id, 120);

	if (!IsSupportedCategory(draft.category) || draft.title.empty() || draft.memory.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory entry is missing a valid category, title, or body.";
		}
		return false;
	}

	if (LooksSensitive(draft.title + "\n" + draft.memory + "\n" + draft.evidence))
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory entry appears to contain sensitive content.";
		}
		return false;
	}

	if (!MemoryService::EnsureMemoryLayout(scope.root_path))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create memory directory layout.";
		}
		return false;
	}

	const fs::path category_path = MemoryService::CategoryPath(scope.root_path, draft.category);
	fs::path target = FindExistingMemoryFile(category_path, draft.title);
	int occurrence_count = 1;
	if (!target.empty())
	{
		Entry existing;
		if (ParseEntryFile(target, existing))
		{
			occurrence_count = existing.occurrence_count + 1;
		}
	}
	else
	{
		target = category_path / (Slug(draft.title) + ".md");
		for (int i = 2; fs::exists(target); ++i)
		{
			target = category_path / (Slug(draft.title) + "-" + std::to_string(i) + ".md");
		}
	}

	const std::string persisted_scope = scope.scope_type == "global" ? "global" : "local";
	if (!uam::io::WriteTextFile(target, BuildMemoryMarkdown(draft, persisted_scope, occurrence_count)))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write memory file.";
		}
		return false;
	}

	if (created_entry != nullptr)
	{
		Entry created;
		if (!ParseEntryFile(target, created))
		{
			if (error_out != nullptr)
			{
				*error_out = "Memory file was written but could not be reloaded.";
			}
			return false;
		}
		std::error_code relative_ec;
		created.id = fs::relative(target, scope.root_path, relative_ec).generic_string();
		if (relative_ec || created.id.empty())
		{
			created.id = target.filename().string();
		}
		created.scope_type = scope.scope_type;
		created.folder_id = scope.folder_id;
		created.scope_label = scope.label;
		created.root_path = scope.root_path;
		*created_entry = std::move(created);
	}
	return true;
}

bool MemoryLibraryService::DeleteEntry(const Scope& scope, const std::string& entry_id, std::string* error_out)
{
	if (scope.scope_type == "all")
	{
		std::string root_key;
		std::string relative_id;
		if (!ParseAggregateEntryId(entry_id, root_key, relative_id))
		{
			if (error_out != nullptr)
			{
				*error_out = "Invalid aggregate memory entry id.";
			}
			return false;
		}

		for (const Root& root : scope.roots)
		{
			if (CanonicalRootKey(root.root_path) != root_key)
			{
				continue;
			}

			Scope concrete_scope;
			concrete_scope.scope_type = root.scope_type;
			concrete_scope.folder_id = root.folder_id;
			concrete_scope.label = root.label;
			concrete_scope.root_path = root.root_path;
			concrete_scope.roots = {root};
			return DeleteEntry(concrete_scope, relative_id, error_out);
		}

		if (error_out != nullptr)
		{
			*error_out = "Aggregate memory root is no longer known.";
		}
		return false;
	}

	if (scope.root_path.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory root is unavailable.";
		}
		return false;
	}

	const std::string trimmed_id = Trimmed(entry_id);
	if (trimmed_id.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory entry id is required.";
		}
		return false;
	}

	const fs::path candidate = scope.root_path / fs::path(trimmed_id);
	if (!IsPathInsideRoot(scope.root_path, candidate))
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory entry path is outside the memory root.";
		}
		return false;
	}

	if (!fs::exists(candidate))
	{
		if (error_out != nullptr)
		{
			*error_out = "Memory entry not found: " + trimmed_id;
		}
		return false;
	}

	if (candidate.extension() != ".md")
	{
		if (error_out != nullptr)
		{
			*error_out = "Only markdown memory files can be deleted.";
		}
		return false;
	}

	std::error_code ec;
	if (!fs::remove(candidate, ec))
	{
		if (error_out != nullptr)
		{
			*error_out = ec ? "Failed to delete memory file." : "Memory file no longer exists.";
		}
		return false;
	}
	return true;
}
