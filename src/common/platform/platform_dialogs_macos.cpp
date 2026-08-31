#include "platform_services_macos_impl_internal.h"
#include "common/platform/platform_application_macos.h"

using namespace uam::platform_macos_impl;

namespace uam::platform_macos_impl
{

class MacFileDialogService final : public IPlatformFileDialogService
{
  public:
	bool SupportsNativeDialogs() const override
	{
		return true;
	}

	bool BrowsePath(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr) const override
	{
		return uam::platform::BrowsePath(target == PlatformPathBrowseTarget::Directory, initial_path, selected_path_out, error_out);
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

		std::string launch_error;
		if (!uam::platform::OpenPath(folder_path, &launch_error))
		{
			if (error_out != nullptr)
			{
				*error_out = ErrorWithOptionalDetail("Failed to open folder in file manager.", launch_error);
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

		std::string app_name;
		std::string app_bundle_id;
		if (editor_preset_id == "xcode")
		{
			app_name = "Xcode";
			app_bundle_id = "com.apple.dt.Xcode";
		}
		else if (editor_preset_id == "clion")
		{
			app_name = "CLion";
			app_bundle_id = "com.jetbrains.CLion";
		}
		else if (editor_preset_id == "rider")
		{
			app_name = "Rider";
			app_bundle_id = "com.jetbrains.rider";
		}
		else if (editor_preset_id == "visualstudio")
		{
			app_name = "Visual Studio";
			app_bundle_id = "com.microsoft.visual-studio";
		}
		else if (editor_preset_id == "webstorm")
		{
			app_name = "WebStorm";
			app_bundle_id = "com.jetbrains.WebStorm";
		}
		else if (editor_preset_id == "pycharm")
		{
			app_name = "PyCharm";
			app_bundle_id = "com.jetbrains.PyCharm";
		}
		else if (editor_preset_id == "idea")
		{
			app_name = "IntelliJ IDEA";
			app_bundle_id = "com.jetbrains.intellij";
		}
		else if (editor_preset_id == "goland")
		{
			app_name = "GoLand";
			app_bundle_id = "com.jetbrains.goland";
		}
		else if (editor_preset_id == "rustrover")
		{
			app_name = "RustRover";
			app_bundle_id = "com.jetbrains.rustrover";
		}
		else
		{
			app_name = "Visual Studio Code";
			app_bundle_id = "com.microsoft.VSCode";
		}

		std::string launch_error;
		if (!uam::platform::OpenPathWithApplication(folder_path, app_bundle_id, &launch_error))
		{
			if (error_out != nullptr)
			{
				*error_out = ErrorWithOptionalDetail("Failed to open workspace in " + app_name + ".", launch_error);
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

		std::string launch_error;
		if (!uam::platform::RevealPath(file_path, &launch_error))
		{
			if (error_out != nullptr)
			{
				*error_out = ErrorWithOptionalDetail("Failed to reveal file in file manager.", launch_error);
			}

			return false;
		}

		return true;
	}

	bool OpenFileInTextEditor(const std::filesystem::path& file_path, std::string* error_out = nullptr) const override
	{
		if (file_path.empty() || !uam::paths::IsRegularFileNoThrow(file_path))
		{
			if (error_out != nullptr)
			{
				*error_out = "File does not exist.";
			}
			return false;
		}

		std::string launch_error;
		if (!uam::platform::OpenPath(file_path, &launch_error))
		{
			if (error_out != nullptr)
			{
				*error_out = ErrorWithOptionalDetail("Failed to open file in text editor.", launch_error);
			}
			return false;
		}
		return true;
	}
};

IPlatformFileDialogService& GetMacFileDialogService()
{
	static MacFileDialogService instance;
	return instance;
}

} // namespace uam::platform_macos_impl
