#pragma once

#include "common/memory/memory_categories.h"
#include "common/memory/memory_library_store.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/sensitive_text.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/// <summary>The one on-disk format for durable memory entries (`<category>/<slug>.md`). The memory worker's staged batch
/// and the Memory Library store (manual entries and SSH-host workers through the runner) both build, match and count
/// entries here, so the same title always lands in the same file whichever path wrote it first.</summary>
namespace uam::memory
{
	inline constexpr std::size_t kEntryTitleMaxChars = 160; // Bytes, cut at a UTF-8 character boundary, like every limit here.
	inline constexpr std::size_t kEntryMemoryMaxChars = 1400;
	inline constexpr std::size_t kEntryEvidenceMaxChars = 900;
	inline constexpr std::size_t kEntryConfidenceMaxChars = 80;
	inline constexpr std::size_t kEntrySourceChatIdMaxChars = 120;
	inline constexpr std::size_t kEntryExtractionKeyMaxChars = 256;
	inline constexpr std::size_t kEntryMaxExtractionKeys = 32;

	/// <summary>Caps and trims every field; refuses an entry without a supported category, title or body, or one that looks
	/// sensitive. Worker-only quality rules (confidence vocabulary, required evidence) stay with the worker.</summary>
	inline bool SanitizeEntryDraft(MemoryLibraryStore::Draft input, MemoryLibraryStore::Draft& draft, std::string* error_out)
	{
		input.category = strings::Trim(input.category);
		input.title = strings::SafeLine(input.title, kEntryTitleMaxChars);
		input.memory = strings::SafeLine(input.memory, kEntryMemoryMaxChars);
		input.evidence = strings::SafeLine(input.evidence, kEntryEvidenceMaxChars);
		input.confidence = strings::SafeLine(strings::NonEmptyOrFallback(input.confidence, "medium"), kEntryConfidenceMaxChars);
		input.source_chat_id = strings::SafeLine(input.source_chat_id, kEntrySourceChatIdMaxChars);
		input.extraction_key = strings::SafeLine(input.extraction_key, kEntryExtractionKeyMaxChars);
		if (!IsSupportedCategory(input.category) || input.title.empty() || input.memory.empty())
		{
			if (error_out != nullptr) *error_out = "Memory entry is missing a valid category, title, or body.";
			return false;
		}
		if (sensitive::LooksSensitiveText(input.title + "\n" + input.memory + "\n" + input.evidence))
		{
			if (error_out != nullptr) *error_out = "Memory entry appears to contain sensitive content.";
			return false;
		}
		draft = std::move(input);
		return true;
	}

	/// <summary>Same-title key: ASCII letters and digits, lowercased, with other runs as single spaces. A saved title longer
	/// than the cap (older worker entries kept up to 700 bytes) compares by its first 160 bytes, so a new capped draft of
	/// it still finds that file.</summary>
	inline std::string EntryTitleKey(std::string_view title)
	{
		return strings::NormalizeComparableKey(strings::SafeLine(title, kEntryTitleMaxChars));
	}

	/// <summary>A saved entry's title: its first `# ` line, or empty.</summary>
	inline std::string ReadEntryTitle(const std::filesystem::path& path)
	{
		std::ifstream in;
		if (!io::OpenReadFile(path, in)) return "";
		std::string line;
		while (std::getline(in, line))
			if (strings::StartsWith(line, "# ")) return line.substr(2);
		return "";
	}

	/// <summary>Title key to entry file for one category folder, over the files `accept` admits (all when empty). A file
	/// whose whole title is the key outranks a longer legacy title that only shares its first 160 bytes; otherwise the
	/// first file in directory order wins.</summary>
	using EntryTitleIndex = std::map<std::string, std::filesystem::path>;

	inline EntryTitleIndex IndexEntryTitles(const std::filesystem::path& category_path, const std::function<bool(const std::filesystem::path&)>& accept = {})
	{
		EntryTitleIndex index;
		if (!paths::IsDirectoryNoThrow(category_path)) return index;
		std::set<std::string> whole_title_keys;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(category_path, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!paths::IsRegularFileWithExtensionNoThrow(*it, ".md") || (accept && !accept(it->path()))) continue;
			const std::string title = ReadEntryTitle(it->path());
			const std::string key = EntryTitleKey(title);
			const bool whole_title = key == strings::NormalizeComparableKey(title);
			const auto [entry, inserted] = index.try_emplace(key, it->path());
			if (whole_title && whole_title_keys.insert(key).second && !inserted) entry->second = it->path();
		}
		return index;
	}

	inline std::filesystem::path FindEntryFile(const EntryTitleIndex& index, std::string_view title)
	{
		const std::string wanted = EntryTitleKey(title);
		if (wanted.empty()) return {};
		const auto found = index.find(wanted);
		return found == index.end() ? std::filesystem::path{} : found->second;
	}

	/// <summary>A fresh `<slug>.md`, `<slug>-2.md`, ... path that `taken` does not report as in use.</summary>
	inline std::filesystem::path NewEntryPath(const std::filesystem::path& category_path, std::string_view title, const std::function<bool(const std::filesystem::path&)>& taken)
	{
		const std::string slug = strings::AsciiSlug(title, 72, "memory");
		std::filesystem::path target = category_path / (slug + ".md");
		for (int suffix = 2; taken(target); ++suffix) target = category_path / (slug + "-" + std::to_string(suffix) + ".md");
		return target;
	}

	/// <summary>Occurrence count for one more observation of a saved entry: an existing file counts as at least one
	/// occurrence whatever its header says. Empty when the saved count cannot grow.</summary>
	inline std::optional<int> NextOccurrenceCount(std::string_view existing_text)
	{
		constexpr std::string_view marker = "Occurrence count: ";
		int previous = 1;
		if (const std::size_t at = existing_text.find(marker); at != std::string_view::npos)
		{
			const std::size_t start = at + marker.size();
			const std::size_t end = existing_text.find_first_of("\r\n", start);
			previous = std::max(1, parse::IntOr(existing_text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start), 1));
		}
		if (previous == std::numeric_limits<int>::max()) return std::nullopt;
		return previous + 1;
	}

	/// <summary>Adds this extraction's receipt and keeps the newest receipts only.</summary>
	inline void AppendExtractionKey(std::vector<std::string>& keys, const std::string& key)
	{
		if (!key.empty()) keys.push_back(key);
		if (keys.size() > kEntryMaxExtractionKeys) keys.erase(keys.begin(), keys.end() - static_cast<std::ptrdiff_t>(kEntryMaxExtractionKeys));
	}

	/// <summary>Entry text for an already sanitized draft. Readers accept receipts anywhere before `## Memory`.</summary>
	inline std::string BuildEntryMarkdown(const MemoryLibraryStore::Draft& draft, std::string_view scope, int occurrence_count, const std::vector<std::string>& extraction_keys)
	{
		std::string markdown;
		markdown.reserve(draft.title.size() + draft.category.size() + draft.confidence.size() + draft.source_chat_id.size() + draft.memory.size() + draft.evidence.size() + 180);
		markdown += "# " + draft.title + "\n\n";
		markdown += "Scope: ";
		markdown.append(scope);
		markdown += "\n";
		markdown += "Category: " + draft.category + "\n";
		markdown += "Confidence: " + draft.confidence + "\n";
		markdown += "Source chat: " + draft.source_chat_id + "\n";
		for (const std::string& key : extraction_keys) markdown += "Extraction key: " + key + "\n";
		markdown += "Last observed: " + time::TimestampNow() + "\n";
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
}
