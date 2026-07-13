#include "theme_service.h"

#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	constexpr std::size_t kMaxThemeFileBytes = 64 * 1024;
	constexpr auto kColorKeys = std::to_array<std::string_view>({
	    "background",
	    "surface",
	    "surfaceUp",
	    "text",
	    "textMuted",
	    "accent",
	    "sidebar",
	    "userMessage",
	    "assistantMessage",
	    "success",
	    "warning",
	    "error",
	});

	void SetError(std::string* error_out, std::string message)
	{
		if (error_out != nullptr)
		{
			*error_out = std::move(message);
		}
	}

	bool IsSlug(std::string_view value)
	{
		if (value.empty() || value.size() > 48)
		{
			return false;
		}
		return std::ranges::all_of(value, [](unsigned char ch) {
			return std::islower(ch) || std::isdigit(ch) || ch == '-';
		});
	}

	std::optional<std::string> ThemeSlug(std::string_view id)
	{
		constexpr std::string_view prefix = "custom:";
		if (!id.starts_with(prefix))
		{
			return std::nullopt;
		}
		const std::string slug(id.substr(prefix.size()));
		return IsSlug(slug) ? std::optional<std::string>{slug} : std::nullopt;
	}

	bool IsHexColor(std::string_view value)
	{
		if (value.size() != 7 || value.front() != '#')
		{
			return false;
		}
		return std::ranges::all_of(value.substr(1), [](unsigned char ch) { return std::isxdigit(ch); });
	}

	std::optional<nlohmann::json> NormalizeTheme(const nlohmann::json& theme, std::string* error_out)
	{
		if (!theme.is_object())
		{
			SetError(error_out, "Theme must be a JSON object.");
			return std::nullopt;
		}
		const auto version_it = theme.find("version");
		if (version_it == theme.end() || !version_it->is_number_integer() || version_it->get<int>() != 1)
		{
			SetError(error_out, "Theme version must be 1.");
			return std::nullopt;
		}
		const auto id_it = theme.find("id");
		const auto name_it = theme.find("name");
		const auto base_it = theme.find("base");
		if (id_it == theme.end() || !id_it->is_string() || name_it == theme.end() || !name_it->is_string() ||
		    base_it == theme.end() || !base_it->is_string())
		{
			SetError(error_out, "Theme id, name, and base must be strings.");
			return std::nullopt;
		}

		const std::string id = uam::strings::TrimAndLowerAscii(id_it->get<std::string>());
		if (!ThemeSlug(id))
		{
			SetError(error_out, "Theme id must use custom:<lowercase-slug>.");
			return std::nullopt;
		}
		const std::string name = uam::strings::Trim(name_it->get<std::string>());
		if (name.empty() || name.size() > 64)
		{
			SetError(error_out, "Theme name must contain 1 to 64 characters.");
			return std::nullopt;
		}
		const std::string base = uam::strings::TrimAndLowerAscii(base_it->get<std::string>());
		if (base != "dark" && base != "light")
		{
			SetError(error_out, "Theme base must be dark or light.");
			return std::nullopt;
		}

		const auto colors_it = theme.find("colors");
		if (colors_it == theme.end() || !colors_it->is_object())
		{
			SetError(error_out, "Theme colors must be an object.");
			return std::nullopt;
		}

		nlohmann::json colors = nlohmann::json::object();
		for (std::string_view key : kColorKeys)
		{
			const auto value_it = colors_it->find(std::string(key));
			const std::string value = value_it != colors_it->end() && value_it->is_string() ? value_it->get<std::string>() : "";
			if (!IsHexColor(value))
			{
				SetError(error_out, "Theme color '" + std::string(key) + "' must use #RRGGBB.");
				return std::nullopt;
			}
			colors[std::string(key)] = uam::strings::ToLowerAscii(value);
		}

		return nlohmann::json{
		    {"version", 1},
		    {"id", id},
		    {"name", name},
		    {"base", base},
		    {"colors", std::move(colors)},
		};
	}

	fs::path ThemeFilePath(const fs::path& data_root, std::string_view id)
	{
		const std::optional<std::string> slug = ThemeSlug(id);
		return slug ? ThemeService::ThemesRootPath(data_root) / (*slug + ".json") : fs::path{};
	}

	std::optional<nlohmann::json> LoadThemeFile(const fs::path& path)
	{
		const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(path);
		if (!size || *size > kMaxThemeFileBytes)
		{
			return std::nullopt;
		}
		try
		{
			const nlohmann::json parsed = nlohmann::json::parse(uam::io::ReadTextFile(path));
			std::string error;
			std::optional<nlohmann::json> normalized = NormalizeTheme(parsed, &error);
			if (!normalized || ThemeSlug((*normalized)["id"].get<std::string>()).value_or("") != path.stem().string())
			{
				return std::nullopt;
			}
			return normalized;
		}
		catch (const nlohmann::json::exception&)
		{
			return std::nullopt;
		}
	}
} // namespace

fs::path ThemeService::ThemesRootPath(const fs::path& data_root)
{
	return data_root / "themes";
}

nlohmann::json ThemeService::List(const fs::path& data_root)
{
	nlohmann::json themes = nlohmann::json::array();
	std::error_code error;
	fs::directory_iterator iterator(ThemesRootPath(data_root), error);
	for (const fs::directory_entry& entry : iterator)
	{
		if (!uam::paths::IsRegularFileWithExtensionNoThrow(entry, ".json"))
		{
			continue;
		}
		if (std::optional<nlohmann::json> theme = LoadThemeFile(entry.path()))
		{
			themes.push_back(std::move(*theme));
		}
	}
	std::sort(themes.begin(), themes.end(), [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
		return lhs.value("name", "") < rhs.value("name", "");
	});
	return themes;
}

bool ThemeService::Save(const fs::path& data_root, const nlohmann::json& theme, nlohmann::json* normalized_out, std::string* error_out)
{
	std::optional<nlohmann::json> normalized = NormalizeTheme(theme, error_out);
	if (!normalized)
	{
		return false;
	}
	const fs::path file_path = ThemeFilePath(data_root, (*normalized)["id"].get<std::string>());
	if (!uam::io::WriteTextFile(file_path, normalized->dump(2) + "\n"))
	{
		SetError(error_out, "Failed to save theme file.");
		return false;
	}
	if (normalized_out != nullptr)
	{
		*normalized_out = std::move(*normalized);
	}
	return true;
}

bool ThemeService::Delete(const fs::path& data_root, std::string_view id, std::string* error_out)
{
	const fs::path file_path = ThemeFilePath(data_root, id);
	if (file_path.empty())
	{
		SetError(error_out, "Only custom themes can be deleted.");
		return false;
	}
	std::error_code error;
	if (!uam::paths::RemoveFileNoThrow(file_path, &error))
	{
		SetError(error_out, error ? "Failed to delete theme: " + error.message() : "Theme does not exist.");
		return false;
	}
	return true;
}

bool ThemeService::Exists(const fs::path& data_root, std::string_view id)
{
	const fs::path file_path = ThemeFilePath(data_root, id);
	return !file_path.empty() && LoadThemeFile(file_path).has_value();
}

bool ThemeService::IsCustomThemeId(std::string_view id)
{
	return ThemeSlug(id).has_value();
}
