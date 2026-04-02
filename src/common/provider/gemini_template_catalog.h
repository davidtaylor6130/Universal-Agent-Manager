#pragma once

#include "app_models.h"

#include <filesystem>
#include <string>
#include <vector>

/// <summary>
/// Manages Gemini markdown template discovery and lifecycle operations.
/// </summary>
class GeminiTemplateCatalog
{
  public:
	/// <summary>Returns the markdown catalog directory for a global root.</summary>
	static std::filesystem::path CatalogPath(const std::filesystem::path& global_root);
	/// <summary>Ensures the markdown catalog directory exists.</summary>
	static bool EnsureCatalogPath(const std::filesystem::path& global_root, std::string* error_out = nullptr);
	/// <summary>Lists all markdown templates from the catalog directory.</summary>
	static std::vector<TemplateCatalogEntry> List(const std::filesystem::path& global_root);
	/// <summary>Checks if a template id exists in the catalog.</summary>
	static bool HasTemplate(const std::filesystem::path& global_root, const std::string& id);
	/// <summary>Resolves a template id to an absolute file path.</summary>
	static bool ResolveTemplatePath(const std::filesystem::path& global_root, const std::string& id, std::filesystem::path& path_out, std::string* error_out = nullptr);
	/// <summary>Imports a markdown file into the catalog.</summary>
	static bool ImportMarkdownTemplate(const std::filesystem::path& global_root, const std::filesystem::path& source_file, std::string* imported_id_out = nullptr, std::string* error_out = nullptr);
	/// <summary>Renames an existing template entry.</summary>
	static bool RenameTemplate(const std::filesystem::path& global_root, const std::string& id, const std::string& new_name, std::string* new_id_out = nullptr, std::string* error_out = nullptr);
	/// <summary>Deletes a template entry by id.</summary>
	static bool RemoveTemplate(const std::filesystem::path& global_root, const std::string& id, std::string* error_out = nullptr);
};
