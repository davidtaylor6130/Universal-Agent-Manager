#pragma once

#include "common/platform/platform_services.h"

enum class PathBrowseTarget
{
	Directory,
	File,
};

/// <summary>
/// Opens a native directory/file picker and returns the selected path.
/// </summary>
static bool BrowsePathWithNativeDialog(const PathBrowseTarget target, const std::string& current_value, std::string* selected_path_out, std::string* error_out = nullptr)
{
	if (selected_path_out == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Selected path output is null.";
		}

		return false;
	}

	PlatformServices& platform_services = PlatformServicesFactory::Instance();
	fs::path initial_path = platform_services.path_service.ExpandLeadingTildePath(current_value);

	if (!initial_path.empty())
	{
		std::error_code ec;
		const bool exists = fs::exists(initial_path, ec);

		if (!exists || ec)
		{
			initial_path = initial_path.parent_path();
		}
		else if (target == PathBrowseTarget::File && !fs::is_directory(initial_path, ec))
		{
			initial_path = initial_path.parent_path();
		}
	}

	const PlatformPathBrowseTarget platform_target = (target == PathBrowseTarget::Directory) ? PlatformPathBrowseTarget::Directory : PlatformPathBrowseTarget::File;
	return platform_services.file_dialog_service.BrowsePath(platform_target, initial_path, selected_path_out, error_out);
}

/// <summary>
/// Draws a path input with a folder picker button to its right.
/// </summary>
static bool DrawPathInputWithBrowseButton(const char* label, std::string& value, const char* browse_button_id, const PathBrowseTarget target, const float total_width = -1.0f, bool* input_deactivated_after_edit_out = nullptr, bool* picked_with_dialog_out = nullptr, std::string* error_out = nullptr)
{
	if (input_deactivated_after_edit_out != nullptr)
	{
		*input_deactivated_after_edit_out = false;
	}

	if (picked_with_dialog_out != nullptr)
	{
		*picked_with_dialog_out = false;
	}

	if (error_out != nullptr)
	{
		error_out->clear();
	}

	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const ImVec2 button_size = ScaleUiSize(ImVec2(22.0f, 22.0f));
	const float available_width = (total_width > 0.0f) ? total_width : ImGui::GetContentRegionAvail().x;
	const float input_width = std::max(ScaleUiLength(96.0f), available_width - button_size.x - spacing);

	ImGui::SetNextItemWidth(input_width);
	const bool text_changed = ImGui::InputText(label, &value);
	const bool input_deactivated_after_edit = ImGui::IsItemDeactivatedAfterEdit();

	if (input_deactivated_after_edit_out != nullptr)
	{
		*input_deactivated_after_edit_out = input_deactivated_after_edit;
	}

	ImGui::SameLine(0.0f, spacing);
	bool picked_with_dialog = false;

	if (DrawFolderIconButton(browse_button_id, ImVec2(22.0f, 22.0f)))
	{
		std::string selected_path;
		std::string browse_error;

		if (BrowsePathWithNativeDialog(target, value, &selected_path, &browse_error))
		{
			value = selected_path;
			picked_with_dialog = true;
		}
		else if (error_out != nullptr && !browse_error.empty())
		{
			*error_out = browse_error;
		}
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip(target == PathBrowseTarget::Directory ? "Browse for a folder" : "Browse for a file");
	}

	if (picked_with_dialog_out != nullptr)
	{
		*picked_with_dialog_out = picked_with_dialog;
	}

	return text_changed || picked_with_dialog;
}

/// <summary>
/// Opens a folder path in the native file manager.
/// </summary>
static bool OpenFolderInFileManager(const fs::path& folder_path, std::string* error_out = nullptr)
{
	return PlatformServicesFactory::Instance().file_dialog_service.OpenFolderInFileManager(folder_path, error_out);
}

/// <summary>
/// Reveals a file path in the native file manager.
/// </summary>
static bool RevealPathInFileManager(const fs::path& file_path, std::string* error_out = nullptr)
{
	return PlatformServicesFactory::Instance().file_dialog_service.RevealPathInFileManager(file_path, error_out);
}
