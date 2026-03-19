#pragma once

#include "app_models.h"

#include <filesystem>
#include <string>
#include <vector>

class GeminiTemplateCatalog {
 public:
  static std::filesystem::path CatalogPath(const std::filesystem::path& global_root);
  static bool EnsureCatalogPath(const std::filesystem::path& global_root, std::string* error_out = nullptr);
  static std::vector<TemplateCatalogEntry> List(const std::filesystem::path& global_root);
  static bool HasTemplate(const std::filesystem::path& global_root, const std::string& id);
  static bool ResolveTemplatePath(const std::filesystem::path& global_root,
                                  const std::string& id,
                                  std::filesystem::path& path_out,
                                  std::string* error_out = nullptr);
  static bool ImportMarkdownTemplate(const std::filesystem::path& global_root,
                                     const std::filesystem::path& source_file,
                                     std::string* imported_id_out = nullptr,
                                     std::string* error_out = nullptr);
  static bool RenameTemplate(const std::filesystem::path& global_root,
                             const std::string& id,
                             const std::string& new_name,
                             std::string* new_id_out = nullptr,
                             std::string* error_out = nullptr);
  static bool RemoveTemplate(const std::filesystem::path& global_root,
                             const std::string& id,
                             std::string* error_out = nullptr);
};

