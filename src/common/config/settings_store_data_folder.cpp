#include "common/config/settings_store.h"
#include "common/config/line_value_codec.h"
#include "common/paths/data_folder_reference.h"

bool SettingsStore::RelocateDataFolderReferences(std::string_view original, const std::filesystem::path& source, const std::filesystem::path& destination, std::string& relocated, std::string& error)
{
	relocated.clear();
	while (!original.empty())
	{
		const auto newline = original.find('\n');
		const auto length = newline == std::string_view::npos ? original.size() : newline;
		const auto line = original.substr(0, length);
		const auto equals = line.find('=');
		std::string next(line);
		if (equals != std::string_view::npos)
		{
			const auto key = line.substr(0, equals);
			const std::string decoded = uam::DecodeLineValue(line.substr(equals + 1));
			const auto path = uam::paths::PathFromUtf8(decoded);
			if (key != "markdown_store_directory" && uam::paths::EmbedsDataFolderReference(decoded, source))
			{
				error = "An unsupported settings field embeds an old data-folder reference.";
				return false;
			}
			if (path.is_absolute() && uam::paths::IsSameOrInsideRoot(source, path))
			{
				if (key != "markdown_store_directory")
				{
					error = "An unsupported settings reference points into the old data folder.";
					return false;
				}
				std::error_code path_error;
				const auto canonical = std::filesystem::weakly_canonical(path, path_error);
				if (path_error)
				{
					error = "The settings reference cannot be resolved safely.";
					return false;
				}
				const auto canonical_source = std::filesystem::canonical(source, path_error);
				if (path_error)
				{
					error = "The settings reference cannot be resolved safely.";
					return false;
				}
				const auto relative = canonical.lexically_relative(canonical_source);
				for (const auto& component : relative)
					if (component == "..")
					{
						error = "A settings reference escapes its owner.";
						return false;
					}
				if (relative.empty() || relative.is_absolute())
					return false;
				next = std::string(key) + "=" + uam::EncodeLineValue(uam::paths::Utf8PathString((destination / relative).lexically_normal()));
			}
		}
		relocated += next;
		if (newline == std::string_view::npos)
			break;
		relocated += '\n';
		original.remove_prefix(length + 1);
	}
	return true;
}
