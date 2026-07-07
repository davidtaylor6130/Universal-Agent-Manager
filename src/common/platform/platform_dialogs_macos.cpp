#include "platform_services_macos_impl_internal.h"

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
		if (selected_path_out != nullptr)
		{
			selected_path_out->clear();
		}

		if (!IsExecutableFile("/usr/bin/osascript"))
		{
			if (error_out != nullptr)
			{
				*error_out = "Native path picker is unavailable (missing osascript).";
			}

			return false;
		}

		const bool choosing_directory = target == PlatformPathBrowseTarget::Directory;
		const std::string chooser_kind = choosing_directory ? "folder" : "file";
		const std::string prompt = choosing_directory ? "\"Select folder\"" : "\"Select file\"";

		std::string script = "set selectedPath to POSIX path of (choose " + chooser_kind + " with prompt " + prompt;

		if (!initial_path.empty())
		{
			script += " default location POSIX file \"" + EscapeAppleScriptQuotedString(initial_path.string()) + "\"";
		}

		script += ")";
		std::string selected_path;

		if (!RunProgramAndCapture({"/usr/bin/osascript", "-e", script, "-e", "return selectedPath"}, &selected_path) || selected_path.empty())
		{
			return false;
		}

		if (selected_path_out != nullptr)
		{
			*selected_path_out = selected_path;
		}

		return true;
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
		if (!RunProgramAndWait({"/usr/bin/open", folder_path.string()}, &launch_error))
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
		if (editor_preset_id == "xcode")
		{
			app_name = "Xcode";
		}
		else if (editor_preset_id == "clion")
		{
			app_name = "CLion";
		}
		else if (editor_preset_id == "rider")
		{
			app_name = "Rider";
		}
		else if (editor_preset_id == "visualstudio")
		{
			app_name = "Visual Studio";
		}
		else if (editor_preset_id == "webstorm")
		{
			app_name = "WebStorm";
		}
		else if (editor_preset_id == "pycharm")
		{
			app_name = "PyCharm";
		}
		else if (editor_preset_id == "idea")
		{
			app_name = "IntelliJ IDEA";
		}
		else if (editor_preset_id == "goland")
		{
			app_name = "GoLand";
		}
		else if (editor_preset_id == "rustrover")
		{
			app_name = "RustRover";
		}
		else
		{
			app_name = "Visual Studio Code";
		}

		std::string launch_error;
		if (!RunProgramAndWait({"/usr/bin/open", "-a", app_name, folder_path.string()}, &launch_error))
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
		if (!RunProgramAndWait({"/usr/bin/open", "-R", file_path.string()}, &launch_error))
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
		if (!RunProgramAndWait({"/usr/bin/open", "-t", file_path.string()}, &launch_error))
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
