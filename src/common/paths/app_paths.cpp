#include "common/paths/app_paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	std::string Trim(std::string value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
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

	std::string ToComparableComponent(std::string value)
	{
#if defined(_WIN32)
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
		return value;
	}

	fs::path NormalizePathForCompare(const fs::path& path)
	{
		std::error_code ec;
		const fs::path canonical = fs::weakly_canonical(path, ec);
		return (ec ? path.lexically_normal() : canonical.lexically_normal());
	}

	bool PathComponentEquals(const fs::path& lhs, const fs::path& rhs)
	{
		return ToComparableComponent(lhs.generic_string()) == ToComparableComponent(rhs.generic_string());
	}

	bool PathsEquivalent(const fs::path& lhs, const fs::path& rhs)
	{
		return lhs.has_root_path() == rhs.has_root_path() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](const fs::path& a, const fs::path& b) { return PathComponentEquals(a, b); }) && std::distance(lhs.begin(), lhs.end()) == std::distance(rhs.begin(), rhs.end());
	}

	bool PathHasPrefix(const fs::path& path, const fs::path& prefix)
	{
		auto path_it = path.begin();
		auto prefix_it = prefix.begin();

		for (; prefix_it != prefix.end(); ++prefix_it)
		{
			if (path_it == path.end())
			{
				return false;
			}

			if (!PathComponentEquals(*path_it, *prefix_it))
			{
				return false;
			}

			++path_it;
		}

		return true;
	}

	std::size_t SharedPathDepth(const fs::path& lhs, const fs::path& rhs)
	{
		auto lhs_it = lhs.begin();
		auto rhs_it = rhs.begin();
		std::size_t depth = 0;

		while (lhs_it != lhs.end() && rhs_it != rhs.end() && PathComponentEquals(*lhs_it, *rhs_it))
		{
			++depth;
			++lhs_it;
			++rhs_it;
		}

		return depth;
	}

	std::string JsonUnescape(const std::string& value)
	{
		std::string out;
		out.reserve(value.size());

		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const char ch = value[i];

			if (ch != '\\' || i + 1 >= value.size())
			{
				out.push_back(ch);
				continue;
			}

			const char esc = value[++i];

			switch (esc)
			{
			case '"':
			case '\\':
			case '/':
				out.push_back(esc);
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			case 'u':

				if (i + 4 < value.size())
				{
					i += 4;
				}

				break;
			default:
				out.push_back(esc);
				break;
			}
		}

		return out;
	}

	std::vector<std::pair<fs::path, std::string>> ReadProjectMappings(const fs::path& gemini_home)
	{
		std::vector<std::pair<fs::path, std::string>> mappings;
		const fs::path projects_file = gemini_home / "projects.json";

		if (!fs::exists(projects_file))
		{
			return mappings;
		}

		const std::string text = ReadTextFile(projects_file);

		if (text.empty())
		{
			return mappings;
		}

		const std::regex pair_pattern(R"PAIR("((?:\\.|[^"])*)"\s*:\s*"((?:\\.|[^"])*)")PAIR");

		for (auto it = std::sregex_iterator(text.begin(), text.end(), pair_pattern); it != std::sregex_iterator(); ++it)
		{
			const std::string key = JsonUnescape((*it)[1].str());
			const std::string value = JsonUnescape((*it)[2].str());

			if (key.empty() || value.empty() || key == "projects")
			{
				continue;
			}

			mappings.emplace_back(fs::path(key), value);
		}

		return mappings;
	}

} // namespace

std::filesystem::path AppPaths::SettingsFilePath(const std::filesystem::path& data_root)
{
	return data_root / "settings.txt";
}

std::filesystem::path AppPaths::ChatsRootPath(const std::filesystem::path& data_root)
{
	return data_root / "chats";
}

std::filesystem::path AppPaths::ChatPath(const std::filesystem::path& data_root, const std::string& chat_id)
{
	return ChatsRootPath(data_root) / chat_id;
}

std::filesystem::path AppPaths::DefaultDataRootPath()
{
#if defined(_WIN32)

	if (const char* local_app_data = std::getenv("LOCALAPPDATA"))
	{
		const std::string value = Trim(local_app_data);

		if (!value.empty())
		{
			return std::filesystem::path(value) / "Universal Agent Manager";
		}
	}

	if (const char* app_data = std::getenv("APPDATA"))
	{
		const std::string value = Trim(app_data);

		if (!value.empty())
		{
			return std::filesystem::path(value) / "Universal Agent Manager";
		}
	}

	if (const char* user_profile = std::getenv("USERPROFILE"))
	{
		const std::string value = Trim(user_profile);

		if (!value.empty())
		{
			return std::filesystem::path(value) / "AppData" / "Local" / "Universal Agent Manager";
		}
	}

	if (const char* home_drive = std::getenv("HOMEDRIVE"))
	{
		if (const char* home_path = std::getenv("HOMEPATH"))
		{
			const std::string drive = Trim(home_drive);
			const std::string path = Trim(home_path);

			if (!drive.empty() && !path.empty())
			{
				return std::filesystem::path(drive + path) / "AppData" / "Local" / "Universal Agent Manager";
			}
		}
	}

#endif

	if (const char* home = std::getenv("HOME"))
	{
		const std::string value = Trim(home);

		if (!value.empty())
		{
#if defined(__APPLE__)
			return std::filesystem::path(value) / "Library" / "Application Support" / "Universal Agent Manager";
#else
			return std::filesystem::path(value) / ".universal_agent_manager";
#endif
		}
	}

	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);

	if (!ec)
	{
		return temp / "universal_agent_manager_data";
	}

	return std::filesystem::path("data");
}

std::filesystem::path AppPaths::GeminiHomePath()
{
	if (const char* gemini_cli_home = std::getenv("GEMINI_CLI_HOME"))
	{
		return std::filesystem::path(gemini_cli_home);
	}

	if (const char* gemini_home = std::getenv("GEMINI_HOME"))
	{
		return std::filesystem::path(gemini_home);
	}

#if defined(_WIN32)

	if (const char* user_profile = std::getenv("USERPROFILE"))
	{
		return std::filesystem::path(user_profile) / ".gemini";
	}

	if (const char* home_drive = std::getenv("HOMEDRIVE"))
	{
		if (const char* home_path = std::getenv("HOMEPATH"))
		{
			return std::filesystem::path(std::string(home_drive) + std::string(home_path)) / ".gemini";
		}
	}

#endif

	if (const char* home = std::getenv("HOME"))
	{
		return std::filesystem::path(home) / ".gemini";
	}

	return std::filesystem::current_path() / ".gemini";
}

std::filesystem::path AppPaths::DefaultGeminiUniversalRootPath()
{
#if defined(_WIN32)

	if (const char* user_profile = std::getenv("USERPROFILE"))
	{
		return std::filesystem::path(user_profile) / ".Gemini_universal_agent_manager";
	}

	if (const char* home_drive = std::getenv("HOMEDRIVE"))
	{
		if (const char* home_path = std::getenv("HOMEPATH"))
		{
			return std::filesystem::path(std::string(home_drive) + std::string(home_path)) / ".Gemini_universal_agent_manager";
		}
	}

#endif

	if (const char* home = std::getenv("HOME"))
	{
		return std::filesystem::path(home) / ".Gemini_universal_agent_manager";
	}

	return std::filesystem::current_path() / ".Gemini_universal_agent_manager";
}

std::optional<std::filesystem::path> AppPaths::ResolveGeminiProjectTmpDir(const std::filesystem::path& project_root)
{
	namespace fs = std::filesystem;
	const fs::path gemini_home = GeminiHomePath();
	const fs::path tmp_root = gemini_home / "tmp";

	if (!fs::exists(tmp_root) || !fs::is_directory(tmp_root))
	{
		return std::nullopt;
	}

	const fs::path normalized_project = NormalizePathForCompare(project_root);
	std::optional<fs::path> closest_match;
	std::size_t closest_depth = 0;

	auto consider_candidate = [&](const fs::path& candidate_tmp_path, const fs::path& recorded_project_root)
	{
		const fs::path normalized_recorded = NormalizePathForCompare(recorded_project_root);

		if (PathsEquivalent(normalized_recorded, normalized_project))
		{
			closest_match = candidate_tmp_path;
			closest_depth = static_cast<std::size_t>(-1);
			return true;
		}

		const bool related = PathHasPrefix(normalized_recorded, normalized_project) || PathHasPrefix(normalized_project, normalized_recorded);

		if (!related)
		{
			return false;
		}

		const std::size_t shared_depth = SharedPathDepth(normalized_recorded, normalized_project);

		if (shared_depth > closest_depth)
		{
			closest_depth = shared_depth;
			closest_match = candidate_tmp_path;
		}

		return false;
	};

	std::error_code ec;

	for (const auto& item : fs::directory_iterator(tmp_root, ec))
	{
		if (ec || !item.is_directory())
		{
			continue;
		}

		const fs::path project_root_file = item.path() / ".project_root";

		if (!fs::exists(project_root_file))
		{
			continue;
		}

		const std::string recorded_path_raw = Trim(ReadTextFile(project_root_file));

		if (recorded_path_raw.empty())
		{
			continue;
		}

		if (consider_candidate(item.path(), fs::path(recorded_path_raw)))
		{
			return closest_match;
		}
	}

	const std::vector<std::pair<fs::path, std::string>> project_mappings = ReadProjectMappings(gemini_home);

	for (const auto& entry : project_mappings)
	{
		const fs::path candidate_tmp_path = tmp_root / entry.second;

		if (!fs::exists(candidate_tmp_path) || !fs::is_directory(candidate_tmp_path))
		{
			continue;
		}

		if (consider_candidate(candidate_tmp_path, entry.first))
		{
			return closest_match;
		}
	}

	if (closest_match.has_value())
	{
		return closest_match;
	}

	return std::nullopt;
}
