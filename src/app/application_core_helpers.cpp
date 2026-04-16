#include "application_core_helpers.h"

#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using uam::AppState;

namespace
{
	const ChatFolder* FindFolderById(const AppState& app, const std::string& folder_id)
	{
		for (const ChatFolder& folder : app.folders)
		{
			if (folder.id == folder_id)
			{
				return &folder;
			}
		}

		return nullptr;
	}
}

std::string Trim(const std::string& value)
{
	const auto start = value.find_first_not_of(" \t\r\n");
	if (start == std::string::npos)
	{
		return "";
	}

	const auto end = value.find_last_not_of(" \t\r\n");
	return value.substr(start, end - start + 1);
}

std::string TimestampNow()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm tm_snapshot{};

	if (!PlatformServicesFactory::Instance().process_service.PopulateLocalTime(tt, &tm_snapshot))
	{
		return "";
	}

	std::ostringstream out;
	out << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
	return out.str();
}

std::string NewSessionId()
{
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> hex_digit(0, 15);
	std::ostringstream id;
	id << "chat-" << epoch_ms << "-";

	for (int i = 0; i < 6; ++i)
	{
		id << std::hex << hex_digit(rng);
	}

	return id.str();
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

bool WriteTextFile(const fs::path& path, const std::string& content)
{
	return uam::io::WriteTextFile(path, content);
}

fs::path ResolveWorkspaceRootPath(const AppState& app, const ChatSession& chat)
{
	fs::path workspace_root;

	if (!Trim(chat.workspace_directory).empty())
	{
		workspace_root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(chat.workspace_directory);
	}

	if (const ChatFolder* folder = FindFolderById(app, chat.folder_id); folder != nullptr && workspace_root.empty())
	{
		workspace_root = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(folder->directory);
	}

	if (workspace_root.empty())
	{
		workspace_root = fs::current_path();
	}

	std::error_code ec;
	const fs::path absolute_root = fs::absolute(workspace_root, ec);
	return ec ? workspace_root.lexically_normal() : absolute_root.lexically_normal();
}

std::uint64_t Fnv1a64(const std::string& text)
{
	std::uint64_t hash = 1469598103934665603ULL;

	for (const unsigned char ch : text)
	{
		hash ^= static_cast<std::uint64_t>(ch);
		hash *= 1099511628211ULL;
	}

	return hash;
}

std::string Hex64(const std::uint64_t value)
{
	std::ostringstream out;
	out << std::hex << value;
	return out.str();
}

std::string ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

std::filesystem::path NormalizeAbsolutePath(const fs::path& path)
{
	if (path.empty())
	{
		return {};
	}

	std::error_code ec;
	const fs::path absolute = fs::absolute(path, ec);
	return ec ? path.lexically_normal() : absolute.lexically_normal();
}
