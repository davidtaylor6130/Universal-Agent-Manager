#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

class ThemeService
{
  public:
	static std::filesystem::path ThemesRootPath(const std::filesystem::path& data_root);
	static nlohmann::json List(const std::filesystem::path& data_root);
	static bool Save(const std::filesystem::path& data_root, const nlohmann::json& theme, nlohmann::json* normalized_out = nullptr, std::string* error_out = nullptr);
	static bool Delete(const std::filesystem::path& data_root, std::string_view id, std::string* error_out = nullptr);
	static bool Exists(const std::filesystem::path& data_root, std::string_view id);
	static bool IsCustomThemeId(std::string_view id);
};
