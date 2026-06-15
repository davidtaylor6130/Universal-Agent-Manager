#pragma once

#include "common/models/app_models.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uam::editor_file_associations
{
	inline constexpr int kDefaultGroupsVersion = 1;
	inline constexpr const char* kDefaultEditorPresetId = "vscode";
	inline constexpr auto kKnownEditorPresetIds = std::to_array<std::string_view>({
	    "vscode", "xcode", "visualstudio", "clion", "rider", "webstorm", "pycharm", "idea", "goland", "rustrover",
	});

	inline bool IsSafeFileExtensionChar(char ch)
	{
		const unsigned char value = static_cast<unsigned char>(ch);
		return !uam::strings::IsAsciiSpace(value) && ch != '/' && ch != '\\';
	}

	inline std::string NormalizeFileExtension(std::string_view value)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(value);
		if (trimmed.empty() || trimmed == ".")
		{
			return "";
		}

		for (const char ch : trimmed)
		{
			if (!IsSafeFileExtensionChar(ch))
			{
				return "";
			}
		}

		std::string normalized;
		normalized.reserve(trimmed.size() + (trimmed.front() == '.' ? 0 : 1));
		if (trimmed.front() != '.')
		{
			normalized.push_back('.');
		}
		for (const char ch : trimmed)
		{
			normalized.push_back(uam::strings::ToLowerAsciiChar(static_cast<unsigned char>(ch)));
		}
		return normalized;
	}

	inline bool IsKnownEditorPresetId(std::string_view value)
	{
		return uam::ranges::Contains(kKnownEditorPresetIds, value);
	}

	inline std::vector<std::string> NormalizeFileExtensions(const std::vector<std::string>& extensions)
	{
		std::vector<std::string> normalized;
		normalized.reserve(extensions.size());
		std::unordered_set<std::string> seen_extensions;
		for (const std::string& extension : extensions)
		{
			const std::string normalized_extension = NormalizeFileExtension(extension);
			if (!normalized_extension.empty() && seen_extensions.insert(normalized_extension).second)
			{
				normalized.push_back(normalized_extension);
			}
		}
		return normalized;
	}

	inline std::string NormalizeEditorPresetId(std::string_view value, std::string_view fallback = kDefaultEditorPresetId)
	{
		const std::string normalized = uam::strings::TrimAndLowerAscii(value);
		if (IsKnownEditorPresetId(normalized))
		{
			return normalized;
		}

		const std::string normalized_fallback = uam::strings::TrimAndLowerAscii(fallback);
		return IsKnownEditorPresetId(normalized_fallback) ? normalized_fallback : std::string(kDefaultEditorPresetId);
	}

	inline std::vector<EditorFileAssociation> DefaultEditorFileAssociations()
	{
		return {
		    EditorFileAssociation{"cpp", "C++", {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}, "clion"},
		    EditorFileAssociation{"csharp", "C#", {".cs", ".csx", ".csproj", ".sln"}, "rider"},
		    EditorFileAssociation{"python", "Python", {".py", ".pyw", ".ipynb"}, "pycharm"},
		    EditorFileAssociation{"javascript", "JavaScript", {".js", ".mjs", ".cjs"}, "webstorm"},
		    EditorFileAssociation{"react-typescript", "React / TypeScript", {".jsx", ".ts", ".tsx", ".mts", ".cts"}, "webstorm"},
		    EditorFileAssociation{"rust", "Rust", {".rs"}, "rustrover"},
		    EditorFileAssociation{"go", "Go", {".go"}, "goland"},
		    EditorFileAssociation{"java-kotlin", "Java / Kotlin", {".java", ".kt", ".kts"}, "idea"},
		    EditorFileAssociation{"swift-apple", "Swift / Apple", {".swift"}, "xcode"},
		    EditorFileAssociation{"powershell", "PowerShell", {".ps1", ".psm1", ".psd1"}, "vscode"},
		    EditorFileAssociation{"shell", "Bash / Shell", {".sh", ".bash", ".zsh", ".fish"}, "vscode"},
		    EditorFileAssociation{"web-styles", "Web Styles / Templates", {".html", ".css", ".scss", ".sass", ".less"}, "webstorm"},
		};
	}

	inline std::optional<EditorFileAssociation> NormalizeEditorFileAssociation(EditorFileAssociation association)
	{
		association.id = uam::strings::TrimAndLowerAscii(association.id);
		association.name = uam::strings::Trim(association.name);
		association.extensions = NormalizeFileExtensions(association.extensions);
		if (association.id.empty() || association.name.empty() || association.extensions.empty())
		{
			return std::nullopt;
		}

		association.editor_preset_id = NormalizeEditorPresetId(association.editor_preset_id);
		return association;
	}

	inline void NormalizeEditorFileAssociations(std::vector<EditorFileAssociation>& associations)
	{
		std::vector<EditorFileAssociation> normalized;
		normalized.reserve(associations.size());
		std::unordered_set<std::string> seen_ids;
		for (EditorFileAssociation& association : associations)
		{
			std::optional<EditorFileAssociation> normalized_association = NormalizeEditorFileAssociation(std::move(association));
			if (!normalized_association)
			{
				continue;
			}
			if (!seen_ids.insert(normalized_association->id).second)
			{
				continue;
			}
			normalized.push_back(std::move(*normalized_association));
		}
		associations = std::move(normalized);
	}

	inline void AppendMissingDefaultEditorGroups(AppSettings& settings)
	{
		if (settings.editor_default_groups_version >= kDefaultGroupsVersion)
		{
			return;
		}

		std::unordered_set<std::string> existing_ids;
		for (const EditorFileAssociation& association : settings.editor_file_associations)
		{
			uam::ranges::InsertTrimmedNonEmptyString(existing_ids, association.id);
		}

		const std::vector<EditorFileAssociation> default_associations = DefaultEditorFileAssociations();
		settings.editor_file_associations.reserve(settings.editor_file_associations.size() + default_associations.size());
		for (const EditorFileAssociation& default_association : default_associations)
		{
			if (existing_ids.insert(default_association.id).second)
			{
				settings.editor_file_associations.push_back(default_association);
			}
		}

		settings.editor_default_groups_version = kDefaultGroupsVersion;
	}
} // namespace uam::editor_file_associations
