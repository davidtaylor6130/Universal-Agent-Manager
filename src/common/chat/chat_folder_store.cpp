#include "common/chat/chat_folder_store.h"
#include "common/config/line_value_codec.h"
#include "common/utils/io_utils.h"

#include <sstream>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	fs::path FolderFilePath(const fs::path& data_root)
	{
		return data_root / "folders.txt";
	}

	std::string ReadTextFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);

		if (!in.good())
		{
			return "";
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	std::string StripCarriageReturn(const std::string& line)
	{
		if (!line.empty() && line.back() == '\r')
		{
			return line.substr(0, line.size() - 1);
		}
		return line;
	}

	std::string ReadFolderFileText(const fs::path& path)
	{
		const std::string text = ReadTextFile(path);
		if (!text.empty())
		{
			return text;
		}

		const fs::path backup = fs::path(path.string() + ".bak");
		if (fs::exists(backup))
		{
			return ReadTextFile(backup);
		}

		return text;
	}

	std::size_t CountFolderEntries(const std::string& text)
	{
		std::istringstream lines(text);
		std::string line;
		std::size_t count = 0;

		while (std::getline(lines, line))
		{
			line = StripCarriageReturn(line);
			if (line == "[folder]")
			{
				++count;
			}
		}

		return count;
	}

} // namespace

std::vector<ChatFolder> ChatFolderStore::Load(const std::filesystem::path& data_root)
{
	std::vector<ChatFolder> folders;
	const fs::path file = FolderFilePath(data_root);

	if (!fs::exists(file))
	{
		return folders;
	}

	const fs::path backup = fs::path(file.string() + ".bak");
	const std::string primary_text = ReadTextFile(file);
	const std::string text = (CountFolderEntries(primary_text) > 0 || !fs::exists(backup)) ? primary_text : ReadTextFile(backup);
	std::istringstream lines(text);
	std::string line;
	ChatFolder current;
	bool in_folder = false;

	while (std::getline(lines, line))
	{
		// Strip carriage return for Windows compatibility
		line = StripCarriageReturn(line);
		
		if (line == "[folder]")
		{
			if (in_folder && !current.id.empty())
			{
				folders.push_back(current);
			}

			current = ChatFolder{};
			in_folder = true;
			continue;
		}

		if (!in_folder || line.empty())
		{
			continue;
		}

		const auto eq = line.find('=');

		if (eq == std::string::npos)
		{
			continue;
		}

		const std::string key = line.substr(0, eq);
		const std::string value = uam::DecodeLineValue(line.substr(eq + 1));

		if (key == "id")
		{
			current.id = value;
		}
		else if (key == "title")
		{
			current.title = value;
		}
			else if (key == "directory")
			{
				current.directory = value;
			}
			else if (key == "collapsed")
			{
				current.collapsed = (value == "1" || value == "true");
			}
		}

	if (in_folder && !current.id.empty())
	{
		folders.push_back(current);
	}

	return folders;
}

bool ChatFolderStore::Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders)
{
	std::error_code ec;
	fs::create_directories(data_root, ec);

	std::ostringstream out;

	for (const ChatFolder& folder : folders)
	{
		if (folder.id.empty())
		{
			continue;
		}

		out << "[folder]\n";
			out << "id=" << uam::EncodeLineValue(folder.id) << "\n";
			out << "title=" << uam::EncodeLineValue(folder.title) << "\n";
			out << "directory=" << uam::EncodeLineValue(folder.directory) << "\n";
			out << "collapsed=" << (folder.collapsed ? "1" : "0") << "\n";
			out << "\n";
		}

	return uam::io::WriteTextFile(FolderFilePath(data_root), out.str());
}
