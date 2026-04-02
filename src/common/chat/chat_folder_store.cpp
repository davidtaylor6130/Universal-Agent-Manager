#include "chat_folder_store.h"

#include <fstream>
#include <sstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path FolderFilePath(const fs::path& data_root)
	{
		return data_root / "folders.txt";
	}

	bool WriteTextFile(const fs::path& path, const std::string& content)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);

		if (!out.good())
		{
			return false;
		}

		out << content;
		return out.good();
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

} // namespace

std::vector<ChatFolder> ChatFolderStore::Load(const std::filesystem::path& data_root)
{
	std::vector<ChatFolder> folders;
	const fs::path file = FolderFilePath(data_root);

	if (!fs::exists(file))
	{
		return folders;
	}

	std::istringstream lines(ReadTextFile(file));
	std::string line;
	ChatFolder current;
	bool in_folder = false;

	while (std::getline(lines, line))
	{
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
		const std::string value = line.substr(eq + 1);

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
			current.collapsed = (value == "1" || value == "true" || value == "on");
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
		out << "id=" << folder.id << "\n";
		out << "title=" << folder.title << "\n";
		out << "directory=" << folder.directory << "\n";
		out << "collapsed=" << (folder.collapsed ? "1" : "0") << "\n\n";
	}

	return WriteTextFile(FolderFilePath(data_root), out.str());
}
