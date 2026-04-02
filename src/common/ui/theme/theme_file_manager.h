#pragma once

#if defined(_WIN32)
#include <shobjidl.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#endif

/// <summary>
/// Cross-platform shell/file-manager helpers used by UI actions.
/// </summary>
static std::string ShellQuotePath(const std::string& path)
{
	std::string escaped = "'";

	for (const char ch : path)
	{
		if (ch == '\'')
		{
			escaped += "'\\''";
		}
		else
		{
			escaped.push_back(ch);
		}
	}

	escaped.push_back('\'');
	return escaped;
}

/// <summary>
/// Executes a shell command and returns success status.
/// </summary>
static bool RunShellCommand(const std::string& command)
{
	return std::system(command.c_str()) == 0;
}

/// <summary>
/// Executes a shell command and captures stdout.
/// </summary>
static bool RunShellCommandCapture(const std::string& command, std::string* output_out = nullptr)
{
#if defined(_WIN32)
	FILE* pipe = _popen(command.c_str(), "r");
#else
	FILE* pipe = popen(command.c_str(), "r");
#endif

	if (pipe == nullptr)
	{
		if (output_out != nullptr)
		{
			output_out->clear();
		}

		return false;
	}

	std::string output;
	std::array<char, 512> buffer{};

	while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
	{
		output.append(buffer.data());
	}

#if defined(_WIN32)
	const int status = _pclose(pipe);
#else
	const int status = pclose(pipe);
#endif

	if (output_out != nullptr)
	{
		*output_out = Trim(output);
	}

	return status == 0;
}

/// <summary>
/// Returns whether a shell command is available on the current platform.
/// </summary>
static bool IsShellCommandAvailable(const std::string& command)
{
#if defined(_WIN32)
	return RunShellCommand("where " + command + " >nul 2>nul");
#else
	return RunShellCommand("command -v " + command + " >/dev/null 2>&1");
#endif
}

enum class PathBrowseTarget
{
	Directory,
	File,
};

/// <summary>
/// Escapes a string for use inside an AppleScript quoted string.
/// </summary>
static std::string EscapeAppleScriptQuotedString(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size());

	for (const char ch : value)
	{
		if (ch == '\\')
		{
			escaped += "\\\\";
		}
		else if (ch == '"')
		{
			escaped += "\\\"";
		}
		else
		{
			escaped.push_back(ch);
		}
	}

	return escaped;
}

#if defined(_WIN32)
/// <summary>
/// Opens a native Windows directory/file picker and returns the selected path.
/// </summary>
static bool BrowsePathWithNativeDialogWindows(const PathBrowseTarget target, const fs::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr)
{
	if (selected_path_out == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Selected path output is null.";
		}

		return false;
	}

	const HRESULT co_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool should_uninitialize = SUCCEEDED(co_init);

	if (FAILED(co_init) && co_init != RPC_E_CHANGED_MODE)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to initialize native file dialog.";
		}

		return false;
	}

	IFileOpenDialog* dialog = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));

	if (FAILED(hr) || dialog == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create native file dialog.";
		}

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	DWORD options = 0;
	hr = dialog->GetOptions(&options);

	if (FAILED(hr))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to configure native file dialog.";
		}

		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;

	if (target == PathBrowseTarget::Directory)
	{
		options |= FOS_PICKFOLDERS;
	}
	else
	{
		options |= FOS_FILEMUSTEXIST;
	}

	dialog->SetOptions(options);

	fs::path initial_folder = initial_path;

	if (!initial_folder.empty())
	{
		std::error_code ec;
		const bool exists = fs::exists(initial_folder, ec);
		const bool is_directory = exists && fs::is_directory(initial_folder, ec);

		if (!exists || ec || !is_directory)
		{
			initial_folder = initial_folder.parent_path();
		}
	}

	if (!initial_folder.empty())
	{
		IShellItem* folder_item = nullptr;
		const std::wstring folder_wide = initial_folder.wstring();

		if (SUCCEEDED(SHCreateItemFromParsingName(folder_wide.c_str(), nullptr, IID_PPV_ARGS(&folder_item))) && folder_item != nullptr)
		{
			dialog->SetDefaultFolder(folder_item);
			dialog->SetFolder(folder_item);
			folder_item->Release();
		}
	}

	hr = dialog->Show(nullptr);

	if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
	{
		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	if (FAILED(hr))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to open native file dialog.";
		}

		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	IShellItem* selected_item = nullptr;
	hr = dialog->GetResult(&selected_item);

	if (FAILED(hr) || selected_item == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "No path was selected.";
		}

		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	PWSTR selected_wide = nullptr;
	hr = selected_item->GetDisplayName(SIGDN_FILESYSPATH, &selected_wide);

	if (FAILED(hr) || selected_wide == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to resolve selected path.";
		}

		selected_item->Release();
		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return false;
	}

	*selected_path_out = fs::path(selected_wide).string();
	CoTaskMemFree(selected_wide);
	selected_item->Release();
	dialog->Release();

	if (should_uninitialize)
	{
		CoUninitialize();
	}

	return true;
}

#endif

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

	fs::path initial_path = ExpandLeadingTildePath(current_value);

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

#if defined(_WIN32)
	return BrowsePathWithNativeDialogWindows(target, initial_path, selected_path_out, error_out);
#elif defined(__APPLE__)

	if (!IsShellCommandAvailable("osascript"))
	{
		if (error_out != nullptr)
		{
			*error_out = "Native path picker is unavailable (missing osascript).";
		}

		return false;
	}

	std::string script = "set selectedPath to POSIX path of (choose " + std::string(target == PathBrowseTarget::Directory ? "folder" : "file") + " with prompt " + std::string(target == PathBrowseTarget::Directory ? "\"Select folder\"" : "\"Select file\"");

	if (!initial_path.empty())
	{
		script += " default location POSIX file \"" + EscapeAppleScriptQuotedString(initial_path.string()) + "\"";
	}

	script += ")";

	std::string selected_path;
	const std::string command = "osascript -e " + ShellQuotePath(script) + " -e " + ShellQuotePath("return selectedPath");

	if (!RunShellCommandCapture(command, &selected_path) || selected_path.empty())
	{
		return false;
	}

	*selected_path_out = selected_path;
	return true;
#else
	const bool has_zenity = IsShellCommandAvailable("zenity");
	const bool has_kdialog = IsShellCommandAvailable("kdialog");

	if (!has_zenity && !has_kdialog)
	{
		if (error_out != nullptr)
		{
			*error_out = "Native path picker is unavailable (install zenity or kdialog).";
		}

		return false;
	}

	std::string command;

	if (has_zenity)
	{
		command = "zenity --file-selection --title=" + ShellQuotePath(target == PathBrowseTarget::Directory ? "Select folder" : "Select file");

		if (target == PathBrowseTarget::Directory)
		{
			command += " --directory";
		}

		if (!initial_path.empty())
		{
			std::string filename_hint = initial_path.string();

			if (target == PathBrowseTarget::Directory && !filename_hint.empty() && filename_hint.back() != '/' && filename_hint.back() != '\\')
			{
				filename_hint.push_back('/');
			}

			command += " --filename=" + ShellQuotePath(filename_hint);
		}
	}
	else
	{
		command = std::string("kdialog ") + (target == PathBrowseTarget::Directory ? "--getexistingdirectory" : "--getopenfilename");

		if (!initial_path.empty())
		{
			command += " " + ShellQuotePath(initial_path.string());
		}
	}

	std::string selected_path;

	if (!RunShellCommandCapture(command, &selected_path) || selected_path.empty())
	{
		return false;
	}

	*selected_path_out = selected_path;
	return true;
#endif
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
	if (folder_path.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Folder path is empty.";
		}

		return false;
	}

	std::error_code ec;
	fs::create_directories(folder_path, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create folder: " + ec.message();
		}

		return false;
	}

#if defined(_WIN32)
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", folder_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

	if (reinterpret_cast<INT_PTR>(result) <= 32)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to open folder in file manager.";
		}

		return false;
	}

	return true;
#elif defined(__APPLE__)
	const std::string command = "open " + ShellQuotePath(folder_path.string());
#else
	const std::string command = "xdg-open " + ShellQuotePath(folder_path.string()) + " >/dev/null 2>&1";
#endif
#if !defined(_WIN32)

	if (!RunShellCommand(command))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to open folder in file manager.";
		}

		return false;
	}

#endif
	return true;
}

/// <summary>
/// Reveals a file path in the native file manager.
/// </summary>
static bool RevealPathInFileManager(const fs::path& file_path, std::string* error_out = nullptr)
{
	if (file_path.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "File path is empty.";
		}

		return false;
	}

	if (!fs::exists(file_path))
	{
		return OpenFolderInFileManager(file_path.parent_path(), error_out);
	}

#if defined(_WIN32)
	const std::wstring params = L"/select,\"" + file_path.wstring() + L"\"";
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);

	if (reinterpret_cast<INT_PTR>(result) <= 32)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to reveal file in file manager.";
		}

		return false;
	}

	return true;
#elif defined(__APPLE__)
	const std::string command = "open -R " + ShellQuotePath(file_path.string());
#else
	return OpenFolderInFileManager(file_path.parent_path(), error_out);
#endif
#if !defined(_WIN32)

	if (!RunShellCommand(command))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to reveal file in file manager.";
		}

		return false;
	}

#endif
	return true;
}
