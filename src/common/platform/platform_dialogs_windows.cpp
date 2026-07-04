#include "platform_services_windows_impl_internal.h"

using namespace uam::platform_windows_impl;

namespace uam::platform_windows_impl
{

class WindowsFileDialogService final : public IPlatformFileDialogService
{
  public:
	bool SupportsNativeDialogs() const override
	{
		return true;
	}

	bool BrowsePath(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr) const override
	{
		return BrowsePathWithNativeDialogWindows(target, initial_path, selected_path_out, error_out);
	}

	bool OpenFolderInFileManager(const std::filesystem::path& folder_path, std::string* error_out = nullptr) const override
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
		if (!uam::paths::CreateDirectoriesNoThrow(folder_path, &ec))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create folder: " + ec.message();
			}

			return false;
		}

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
	}

	bool OpenFolderInEditorPreset(const std::filesystem::path& folder_path, const std::string& editor_preset_id, std::string* error_out = nullptr) const override
	{
		if (folder_path.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Folder path is empty.";
			}
			return false;
		}

		if (!uam::paths::IsDirectoryNoThrow(folder_path))
		{
			if (error_out != nullptr)
			{
				*error_out = "Workspace directory does not exist.";
			}
			return false;
		}

		std::wstring executable = L"Code.exe";
		std::string label = "Visual Studio Code";
		if (editor_preset_id == "visualstudio")
		{
			executable = L"devenv.exe";
			label = "Visual Studio";
		}
		else if (editor_preset_id == "clion")
		{
			executable = L"clion64.exe";
			label = "CLion";
		}
		else if (editor_preset_id == "rider")
		{
			executable = L"rider64.exe";
			label = "Rider";
		}
		else if (editor_preset_id == "webstorm")
		{
			executable = L"webstorm64.exe";
			label = "WebStorm";
		}
		else if (editor_preset_id == "pycharm")
		{
			executable = L"pycharm64.exe";
			label = "PyCharm";
		}
		else if (editor_preset_id == "idea")
		{
			executable = L"idea64.exe";
			label = "IntelliJ IDEA";
		}
		else if (editor_preset_id == "goland")
		{
			executable = L"goland64.exe";
			label = "GoLand";
		}
		else if (editor_preset_id == "rustrover")
		{
			executable = L"rustrover64.exe";
			label = "RustRover";
		}
		else if (editor_preset_id == "xcode")
		{
			if (error_out != nullptr)
			{
				*error_out = "Xcode is not available on Windows.";
			}
			return false;
		}

		const HINSTANCE result = ShellExecuteW(nullptr, L"open", executable.c_str(), folder_path.c_str(), nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<INT_PTR>(result) <= 32)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open workspace in " + label + ".";
			}
			return false;
		}
		return true;
	}

	bool RevealPathInFileManager(const std::filesystem::path& file_path, std::string* error_out = nullptr) const override
	{
		if (file_path.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "File path is empty.";
			}

			return false;
		}

		if (!uam::paths::PathExistsNoThrow(file_path))
		{
			return OpenFolderInFileManager(file_path.parent_path(), error_out);
		}

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
	}
};

IPlatformFileDialogService& GetWindowsFileDialogService()
{
	static WindowsFileDialogService instance;
	return instance;
}

} // namespace uam::platform_windows_impl
