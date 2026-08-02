#include "common/chat/chat_folder_store.h"
#include "common/config/line_value_codec.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <sstream>
#include <string>
#include <string_view>

namespace
{
	namespace fs = std::filesystem;

	constexpr std::string_view kFoldersFileName = "folders.txt";
	constexpr std::string_view kFolderSection = "[folder]";
	constexpr std::string_view kFolderIdKey = "id";
	constexpr std::string_view kFolderTitleKey = "title";
	constexpr std::string_view kFolderDirectoryKey = "directory";
	constexpr std::string_view kFolderCollapsedKey = "collapsed";

	fs::path FolderFilePath(const fs::path& data_root)
	{
		return data_root / kFoldersFileName;
	}

	std::string_view StripCarriageReturn(std::string_view line)
	{
		if (!line.empty() && line.back() == '\r')
		{
			return line.substr(0, line.size() - 1);
		}
		return line;
	}

	std::size_t CountFolderEntries(std::string_view text)
	{
		std::istringstream lines{std::string(text)};
		std::string line;
		std::size_t count = 0;

		while (std::getline(lines, line))
		{
			if (StripCarriageReturn(line) == kFolderSection)
			{
				++count;
			}
		}

		return count;
	}

	void ApplyFolderField(ChatFolder& folder, std::string_view key, std::string_view value)
	{
		const std::string_view normalized_key = uam::strings::TrimAsciiView(key);
		if (normalized_key == kFolderIdKey)
		{
			folder.id = uam::strings::Trim(value);
			std::erase_if(folder.id, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		}
		else if (normalized_key == kFolderTitleKey)
		{
			folder.title.assign(value);
		}
		else if (normalized_key == kFolderDirectoryKey)
		{
			folder.directory.assign(value);
		}
		else if (normalized_key == kFolderCollapsedKey)
		{
			folder.collapsed = uam::parse::BoolOr(value, folder.collapsed);
		}
	}

	void WriteEncodedFolderField(std::ostringstream& out, std::string_view key, std::string_view value)
	{
		out << key << '=' << uam::EncodeLineValue(value) << '\n';
	}

	void WriteBoolFolderField(std::ostringstream& out, std::string_view key, bool value)
	{
		out << key << '=' << (value ? "1" : "0") << '\n';
	}

} // namespace

std::vector<ChatFolder> ChatFolderStore::Load(const std::filesystem::path& data_root)
{
	std::vector<ChatFolder> folders;
	const fs::path file = FolderFilePath(data_root);
	const fs::path backup = uam::io::MakeBackupPath(file);

	if (!uam::paths::PathExistsNoThrow(file) && !uam::paths::PathExistsNoThrow(backup))
	{
		return folders;
	}

	const std::string primary_text = uam::io::ReadTextFile(file);
	const std::string text = (CountFolderEntries(primary_text) > 0 || !uam::paths::PathExistsNoThrow(backup)) ? primary_text : uam::io::ReadTextFile(backup);
	std::istringstream lines(text);
	std::string line;
	ChatFolder current;
	bool in_folder = false;

	while (std::getline(lines, line))
	{
		const std::string_view trimmed_line = StripCarriageReturn(line);

		if (trimmed_line == kFolderSection)
		{
			if (in_folder && !current.id.empty())
			{
				folders.push_back(current);
			}

			current = ChatFolder{};
			in_folder = true;
			continue;
		}

		if (!in_folder || trimmed_line.empty())
		{
			continue;
		}

		const auto eq = trimmed_line.find('=');

		if (eq == std::string_view::npos)
		{
			continue;
		}

		const std::string_view key = trimmed_line.substr(0, eq);
		const std::string value = uam::DecodeLineValue(trimmed_line.substr(eq + 1));
		ApplyFolderField(current, key, value);
	}

	if (in_folder && !current.id.empty())
	{
		folders.push_back(current);
	}

	return folders;
}

bool ChatFolderStore::Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders)
{
	std::ostringstream out;

	for (const ChatFolder& folder : folders)
	{
		std::string folder_id = uam::strings::Trim(folder.id);
		std::erase_if(folder_id, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		if (folder_id.empty())
		{
			continue;
		}

		out << kFolderSection << '\n';
		WriteEncodedFolderField(out, kFolderIdKey, folder_id);
		WriteEncodedFolderField(out, kFolderTitleKey, folder.title);
		WriteEncodedFolderField(out, kFolderDirectoryKey, folder.directory);
		WriteBoolFolderField(out, kFolderCollapsedKey, folder.collapsed);
		out << '\n';
	}

	return uam::io::WriteTextFile(FolderFilePath(data_root), out.str());
}
