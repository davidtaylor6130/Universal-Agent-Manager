#include "markdown_template_catalog.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
	namespace fs = std::filesystem;

	std::string Trim(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool IsMarkdownPath(const fs::path& path)
	{
		return ToLower(path.extension().string()) == ".md";
	}

	bool IsValidTemplateId(const std::string& id)
	{
		if (id.empty())
		{
			return false;
		}

		const fs::path as_path(id);

		if (as_path.has_parent_path())
		{
			return false;
		}

		if (as_path.filename() != as_path)
		{
			return false;
		}

		return IsMarkdownPath(as_path);
	}

	std::string DisplayNameForFile(const fs::path& path)
	{
		const std::string stem = Trim(path.stem().string());
		return stem.empty() ? path.filename().string() : stem;
	}

	std::string FormatFileTimestamp(const fs::path& path)
	{
		std::error_code ec;
		const auto file_time = fs::last_write_time(path, ec);

		if (ec)
		{
			return "";
		}

		const auto system_now = std::chrono::system_clock::now();
		const auto file_now = fs::file_time_type::clock::now();
		const auto adjusted = std::chrono::time_point_cast<std::chrono::system_clock::duration>(file_time - file_now + system_now);
		const std::time_t tt = std::chrono::system_clock::to_time_t(adjusted);

		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &tt);
#else
		localtime_r(&tt, &tm_snapshot);
#endif
		std::ostringstream out;
		out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
		return out.str();
	}

	fs::path BuildUniqueTemplatePath(const fs::path& catalog_dir, const std::string& preferred_base_name)
	{
		std::string base = Trim(preferred_base_name);

		if (base.empty())
		{
			base = "template";
		}

		for (char& ch : base)
		{
			if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
			{
				ch = '_';
			}
		}

		fs::path candidate = catalog_dir / (base + ".md");

		if (!fs::exists(candidate))
		{
			return candidate;
		}

		for (int i = 1; i < 5000; ++i)
		{
			candidate = catalog_dir / (base + "_" + std::to_string(i) + ".md");

			if (!fs::exists(candidate))
			{
				return candidate;
			}
		}

		return catalog_dir / (base + "_" + std::to_string(static_cast<long long>(std::time(nullptr))) + ".md");
	}

} // namespace

std::filesystem::path MarkdownTemplateCatalog::CatalogPath(const std::filesystem::path& global_root)
{
	return global_root / "Markdown_Templates";
}

bool MarkdownTemplateCatalog::EnsureCatalogPath(const std::filesystem::path& global_root, std::string* error_out)
{
	std::error_code ec;
	fs::create_directories(CatalogPath(global_root), ec);

	if (ec && error_out != nullptr)
	{
		*error_out = "Failed to create template catalog directory: " + ec.message();
	}

	return !ec;
}

std::vector<TemplateCatalogEntry> MarkdownTemplateCatalog::List(const std::filesystem::path& global_root)
{
	std::vector<TemplateCatalogEntry> entries;
	std::error_code ec;

	if (!EnsureCatalogPath(global_root, nullptr))
	{
		return entries;
	}

	const fs::path catalog_dir = CatalogPath(global_root);

	for (const auto& item : fs::directory_iterator(catalog_dir, ec))
	{
		if (ec || !item.is_regular_file())
		{
			continue;
		}

		const fs::path file_path = item.path();

		if (!IsMarkdownPath(file_path))
		{
			continue;
		}

		TemplateCatalogEntry entry;
		entry.id = file_path.filename().string();
		entry.display_name = DisplayNameForFile(file_path);
		entry.absolute_path = file_path.string();
		entry.updated_at = FormatFileTimestamp(file_path);
		entries.push_back(std::move(entry));
	}

	auto sort_entries_by_display_name_then_id = [](const TemplateCatalogEntry& lhs, const TemplateCatalogEntry& rhs)
	{
		const std::string l_name = ToLower(lhs.display_name);
		const std::string r_name = ToLower(rhs.display_name);

		if (l_name == r_name)
		{
			return ToLower(lhs.id) < ToLower(rhs.id);
		}

		return l_name < r_name;
	};
	std::sort(entries.begin(), entries.end(), sort_entries_by_display_name_then_id);
	return entries;
}

bool MarkdownTemplateCatalog::HasTemplate(const std::filesystem::path& global_root, const std::string& id)
{
	std::filesystem::path resolved;
	return ResolveTemplatePath(global_root, id, resolved, nullptr);
}

bool MarkdownTemplateCatalog::ResolveTemplatePath(const std::filesystem::path& global_root, const std::string& id, std::filesystem::path& path_out, std::string* error_out)
{
	if (!IsValidTemplateId(id))
	{
		if (error_out != nullptr)
		{
			*error_out = "Invalid template id.";
		}

		return false;
	}

	const fs::path candidate = CatalogPath(global_root) / id;

	if (!fs::exists(candidate) || !fs::is_regular_file(candidate))
	{
		if (error_out != nullptr)
		{
			*error_out = "Template file not found in catalog.";
		}

		return false;
	}

	path_out = candidate;
	return true;
}

bool MarkdownTemplateCatalog::ImportMarkdownTemplate(const std::filesystem::path& global_root, const std::filesystem::path& source_file, std::string* imported_id_out, std::string* error_out)
{
	if (source_file.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Provide a source template file path.";
		}

		return false;
	}

	if (!fs::exists(source_file) || !fs::is_regular_file(source_file))
	{
		if (error_out != nullptr)
		{
			*error_out = "Source template file does not exist.";
		}

		return false;
	}

	if (!IsMarkdownPath(source_file))
	{
		if (error_out != nullptr)
		{
			*error_out = "Only .md files can be imported.";
		}

		return false;
	}

	if (!EnsureCatalogPath(global_root, error_out))
	{
		return false;
	}

	const fs::path catalog_dir = CatalogPath(global_root);
	const fs::path target = BuildUniqueTemplatePath(catalog_dir, source_file.stem().string());
	std::error_code ec;
	fs::copy_file(source_file, target, fs::copy_options::overwrite_existing, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to import template: " + ec.message();
		}

		return false;
	}

	if (imported_id_out != nullptr)
	{
		*imported_id_out = target.filename().string();
	}

	return true;
}

bool MarkdownTemplateCatalog::RenameTemplate(const std::filesystem::path& global_root, const std::string& id, const std::string& new_name, std::string* new_id_out, std::string* error_out)
{
	fs::path current_path;

	if (!ResolveTemplatePath(global_root, id, current_path, error_out))
	{
		return false;
	}

	const std::string trimmed_name = Trim(new_name);

	if (trimmed_name.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "New template name cannot be empty.";
		}

		return false;
	}

	fs::path proposed_name(trimmed_name);

	if (proposed_name.has_parent_path())
	{
		if (error_out != nullptr)
		{
			*error_out = "Template name cannot include folders.";
		}

		return false;
	}

	if (ToLower(proposed_name.extension().string()) != ".md")
	{
		proposed_name += ".md";
	}

	const fs::path catalog_dir = CatalogPath(global_root);

	if (ToLower(current_path.filename().string()) == ToLower(proposed_name.filename().string()))
	{
		if (new_id_out != nullptr)
		{
			*new_id_out = current_path.filename().string();
		}

		return true;
	}

	const fs::path desired_path = BuildUniqueTemplatePath(catalog_dir, proposed_name.stem().string());
	std::error_code ec;
	fs::rename(current_path, desired_path, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to rename template: " + ec.message();
		}

		return false;
	}

	if (new_id_out != nullptr)
	{
		*new_id_out = desired_path.filename().string();
	}

	return true;
}

bool MarkdownTemplateCatalog::RemoveTemplate(const std::filesystem::path& global_root, const std::string& id, std::string* error_out)
{
	fs::path existing_path;

	if (!ResolveTemplatePath(global_root, id, existing_path, error_out))
	{
		return false;
	}

	std::error_code ec;
	fs::remove(existing_path, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to delete template: " + ec.message();
		}

		return false;
	}

	return true;
}
