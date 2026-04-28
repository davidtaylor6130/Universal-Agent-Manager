#include "app/markdown_store_service.h"

#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
	std::string Trimmed(const std::string& value)
	{
		return uam::strings::Trim(value);
	}

	std::string LowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	std::string SafeLine(std::string value, const std::size_t max_chars = 240)
	{
		value = Trimmed(value);
		std::replace(value.begin(), value.end(), '\r', ' ');
		std::replace(value.begin(), value.end(), '\n', ' ');
		if (value.size() > max_chars)
		{
			value = value.substr(0, max_chars);
		}
		return value;
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
		return out.empty() ? "markdown-store-item" : out;
	}

	std::string StripYamlQuotes(std::string value)
	{
		value = Trimmed(value);
		if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
		{
			return value.substr(1, value.size() - 2);
		}
		return value;
	}

	std::string HeaderValue(const std::map<std::string, std::string>& headers, const std::string& key)
	{
		const auto found = headers.find(LowerAscii(key));
		return found == headers.end() ? "" : found->second;
	}

	std::string FirstHeadingTitle(const std::string& body)
	{
		std::istringstream lines(body);
		std::string line;
		while (std::getline(lines, line))
		{
			if (line.rfind("# ", 0) == 0)
			{
				return SafeLine(line.substr(2), 160);
			}
		}
		return "";
	}

	std::string PreviewFromBody(const std::string& body)
	{
		std::istringstream lines(body);
		std::string line;
		std::string preview;
		while (std::getline(lines, line))
		{
			line = Trimmed(line);
			if (line.empty() || line.rfind("#", 0) == 0)
			{
				continue;
			}
			if (!preview.empty())
			{
				preview += " ";
			}
			preview += line;
			if (preview.size() >= 320)
			{
				preview.resize(320);
				break;
			}
		}
		return preview;
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

	bool ParseEntryFile(const fs::path& path, MarkdownStoreService::Entry& out_entry)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in.good())
		{
			return false;
		}

		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::map<std::string, std::string> headers;
		std::string body = text;
		if (text.rfind("---", 0) == 0)
		{
			const std::size_t first_line_end = text.find('\n');
			const std::size_t closing = first_line_end == std::string::npos ? std::string::npos : text.find("\n---", first_line_end);
			if (closing != std::string::npos)
			{
				std::istringstream header_lines(text.substr(first_line_end + 1, closing - first_line_end - 1));
				std::string line;
				while (std::getline(header_lines, line))
				{
					const std::size_t colon = line.find(':');
					if (colon == std::string::npos)
					{
						continue;
					}
					headers[LowerAscii(Trimmed(line.substr(0, colon)))] = StripYamlQuotes(line.substr(colon + 1));
				}
				std::size_t body_start = closing + 4;
				if (body_start < text.size() && text[body_start] == '\r')
				{
					++body_start;
				}
				if (body_start < text.size() && text[body_start] == '\n')
				{
					++body_start;
				}
				body = text.substr(body_start);
			}
		}

		out_entry.title = SafeLine(HeaderValue(headers, "title"), 160);
		if (out_entry.title.empty())
		{
			out_entry.title = FirstHeadingTitle(body);
		}
		if (out_entry.title.empty())
		{
			out_entry.title = path.stem().string();
		}
		out_entry.maker = SafeLine(HeaderValue(headers, "maker"), 160);
		out_entry.review = SafeLine(HeaderValue(headers, "review"), 320);
		out_entry.date_created = SafeLine(HeaderValue(headers, "dateCreated"), 80);
		out_entry.date_updated = SafeLine(HeaderValue(headers, "dateUpdated"), 80);
		out_entry.preview = PreviewFromBody(body);
		out_entry.file_path = path;
		return !out_entry.title.empty();
	}

	std::string BuildMarkdown(const MarkdownStoreService::Draft& draft)
	{
		const std::string now = uam::time::TimestampNow();
		std::ostringstream out;
		out << "---\n";
		out << "uamVersion: 1\n";
		out << "title: " << SafeLine(draft.title, 160) << "\n";
		out << "maker: " << SafeLine(draft.maker, 160) << "\n";
		out << "review: " << SafeLine(draft.review, 320) << "\n";
		out << "dateCreated: " << now << "\n";
		out << "dateUpdated: " << now << "\n";
		out << "---\n\n";
		out << draft.body << "\n";
		return out.str();
	}
}

fs::path MarkdownStoreService::NormalizeRoot(const std::string& root)
{
	const fs::path expanded = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(Trimmed(root));
	if (expanded.empty())
	{
		return {};
	}
	std::error_code ec;
	return fs::weakly_canonical(expanded, ec).lexically_normal();
}

bool MarkdownStoreService::IsConfiguredRoot(const fs::path& root, std::string* error_out)
{
	if (root.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store directory is not configured.";
		}
		return false;
	}

	std::error_code ec;
	if (!fs::exists(root, ec) || ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store directory does not exist.";
		}
		return false;
	}
	if (!fs::is_directory(root, ec) || ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store path is not a directory.";
		}
		return false;
	}
	return true;
}

std::vector<MarkdownStoreService::Entry> MarkdownStoreService::ListEntries(const fs::path& root, std::string* error_out)
{
	std::vector<Entry> entries;
	if (!IsConfiguredRoot(root, error_out))
	{
		return entries;
	}

	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
	{
		if (!it->is_regular_file() || it->path().extension() != ".uam")
		{
			continue;
		}
		Entry entry;
		if (!ParseEntryFile(it->path(), entry))
		{
			continue;
		}
		std::error_code relative_ec;
		entry.id = fs::relative(it->path(), root, relative_ec).generic_string();
		if (relative_ec || entry.id.empty())
		{
			entry.id = it->path().filename().string();
		}
		entries.push_back(std::move(entry));
	}

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to enumerate Markdown Store files.";
		}
		return {};
	}

	std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
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
	if (!IsConfiguredRoot(root, error_out))
	{
		return false;
	}

	Draft draft = input;
	draft.title = SafeLine(draft.title, 160);
	draft.maker = SafeLine(draft.maker, 160);
	draft.review = SafeLine(draft.review, 320);
	draft.body = Trimmed(draft.body);
	if (draft.title.empty() || draft.body.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store entry requires a title and body.";
		}
		return false;
	}

	fs::path target = root / (Slug(draft.title) + ".uam");
	for (int i = 2; fs::exists(target); ++i)
	{
		target = root / (Slug(draft.title) + "-" + std::to_string(i) + ".uam");
	}

	if (!uam::io::WriteTextFile(target, BuildMarkdown(draft)))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to write Markdown Store file.";
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
				*error_out = "Markdown Store file was written but could not be reloaded.";
			}
			return false;
		}
		std::error_code relative_ec;
		created.id = fs::relative(target, root, relative_ec).generic_string();
		if (relative_ec || created.id.empty())
		{
			created.id = target.filename().string();
		}
		*created_entry = std::move(created);
	}
	return true;
}

bool MarkdownStoreService::ValidateStoreFilePath(const fs::path& root, const std::string& file_path, fs::path* normalized_path_out, std::string* error_out)
{
	if (!IsConfiguredRoot(root, error_out))
	{
		return false;
	}

	const fs::path candidate = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(Trimmed(file_path));
	if (candidate.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store file path is empty.";
		}
		return false;
	}
	if (!IsPathInsideRoot(root, candidate))
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store file is outside the configured store directory.";
		}
		return false;
	}
	if (candidate.extension() != ".uam")
	{
		if (error_out != nullptr)
		{
			*error_out = "Only .uam Markdown Store files can be attached.";
		}
		return false;
	}

	std::error_code ec;
	if (!fs::exists(candidate, ec) || ec || !fs::is_regular_file(candidate, ec))
	{
		if (error_out != nullptr)
		{
			*error_out = "Markdown Store file does not exist.";
		}
		return false;
	}

	if (normalized_path_out != nullptr)
	{
		*normalized_path_out = fs::weakly_canonical(candidate, ec).lexically_normal();
		if (ec)
		{
			*normalized_path_out = candidate.lexically_normal();
		}
	}
	return true;
}
