#pragma once

#include "common/paths/path_utils.h"

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace uam::staged_attachment_folder
{
	/// True for an absolute `<workspace>/.UAM/attachments/<chat id>` folder whose chat id is one plain name. A `.` or
	/// `..` anywhere is refused, so the folder checked is the folder swept even after a linked parent.
	inline bool IsChatFolder(const std::filesystem::path& folder)
	{
		for (const std::filesystem::path& part : folder)
			if (part == "." || part == "..") return false;
		const std::string id = uam::paths::Utf8PathString(folder.filename());
		return folder.is_absolute() && !id.empty() && id.size() <= 256 &&
		       id.find_first_of("/\\:") == std::string::npos && uam::paths::Utf8PathString(folder.parent_path().filename()) == "attachments" &&
		       uam::paths::Utf8PathString(folder.parent_path().parent_path().filename()) == ".UAM";
	}

	/// Removes the regular files a deleted chat staged in its own folder, except the names in `keep` (still referenced
	/// by a surviving chat), then the folder once empty. Never follows a link or enters a subfolder. Returns false only
	/// for a path that is not a chat folder or a folder that could not be read; `left` counts entries not removed,
	/// kept names aside.
	inline bool Sweep(const std::filesystem::path& folder, const std::set<std::string>& keep, std::size_t& left)
	{
		left = 0;
		if (!IsChatFolder(folder)) return false;
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(folder, error);
		if (error == std::errc::no_such_file_or_directory || (!error && !std::filesystem::exists(status))) return true;
		if (error || !std::filesystem::is_directory(status)) return false;
		std::vector<std::filesystem::directory_entry> entries;
		for (std::filesystem::directory_iterator iterator(folder, error), end; !error && iterator != end; iterator.increment(error))
			entries.push_back(*iterator);
		if (error) return false;
		std::size_t kept = 0;
		for (const std::filesystem::directory_entry& entry : entries)
		{
			if (keep.contains(uam::paths::Utf8PathString(entry.path().filename()))) { ++kept; continue; }
			std::error_code entry_error;
			const std::filesystem::file_status kind = entry.symlink_status(entry_error);
			if (entry_error || !std::filesystem::is_regular_file(kind) || !std::filesystem::remove(entry.path(), entry_error) || entry_error) ++left;
		}
		if (left == 0 && kept == 0) std::filesystem::remove(folder, error);
		return true;
	}
} // namespace uam::staged_attachment_folder
