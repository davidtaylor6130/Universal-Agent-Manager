#include "common/paths/app_paths.h"

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	std::string ToComparableComponent(std::string_view value)
	{
#if defined(_WIN32) || defined(__APPLE__) || defined(__MACH__)
		return uam::strings::ToLowerAscii(value);
#else
		return std::string(value);
#endif
	}

	fs::path NormalizePathForCompare(const fs::path& path)
	{
		const fs::path expanded = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::paths::PortablePathString(path));
		return PlatformServicesFactory::Instance().path_service.CanProbeDirectoryWithoutPrompt(expanded)
		           ? uam::paths::NormalizeExistingPath(expanded)
		           : uam::paths::AbsolutePathNoThrow(expanded).lexically_normal();
	}

	bool PathComponentEquals(const fs::path& lhs, const fs::path& rhs)
	{
		return ToComparableComponent(uam::paths::PortablePathString(lhs)) == ToComparableComponent(uam::paths::PortablePathString(rhs));
	}

	bool PathsEquivalent(const fs::path& lhs, const fs::path& rhs)
	{
		if (lhs.has_root_path() != rhs.has_root_path())
		{
			return false;
		}

		auto lhs_it = lhs.begin();
		auto rhs_it = rhs.begin();
		for (; lhs_it != lhs.end() && rhs_it != rhs.end(); ++lhs_it, ++rhs_it)
		{
			if (!PathComponentEquals(*lhs_it, *rhs_it))
			{
				return false;
			}
		}
		return lhs_it == lhs.end() && rhs_it == rhs.end();
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

	using ProjectMapping = std::pair<fs::path, std::string>;
	using ProjectMappings = std::vector<ProjectMapping>;
	constexpr std::size_t kExactPathMatchDepth = std::numeric_limits<std::size_t>::max();

	void AppendProjectMappingsFromJson(const nlohmann::json& value, ProjectMappings& mappings)
	{
		if (value.is_array())
		{
			for (const nlohmann::json& item : value)
			{
				AppendProjectMappingsFromJson(item, mappings);
			}
			return;
		}

		if (!value.is_object())
		{
			return;
		}

		for (auto it = value.begin(); it != value.end(); ++it)
		{
			const std::string key = it.key();
			const std::optional<std::string> mapped_tmp_name = key == "projects" ? std::nullopt : uam::nlohmann_json::TrimmedStringScalarValue(it.value());
			if (!key.empty() && mapped_tmp_name)
			{
				mappings.emplace_back(fs::path(key), *mapped_tmp_name);
				continue;
			}

			AppendProjectMappingsFromJson(it.value(), mappings);
		}
	}

	ProjectMappings ReadProjectMappings(const fs::path& gemini_home)
	{
		ProjectMappings mappings;
		const fs::path projects_file = gemini_home / "projects.json";

		if (!uam::paths::PathExistsNoThrow(projects_file))
		{
			return mappings;
		}

		const std::string text = uam::io::ReadTextFile(projects_file);

		if (text.empty())
		{
			return mappings;
		}

		const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
		if (root.is_discarded())
		{
			return mappings;
		}

		AppendProjectMappingsFromJson(root, mappings);
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

std::filesystem::path AppPaths::ChatPath(const std::filesystem::path& data_root, std::string_view chat_id)
{
	return ChatsRootPath(data_root) / std::string(chat_id);
}

std::filesystem::path AppPaths::DefaultDataRootPath()
{
#if defined(_WIN32)

	if (const std::optional<std::filesystem::path> local_app_data = uam::env::GetTrimmedPath("LOCALAPPDATA"))
	{
		return *local_app_data / "Universal Agent Manager";
	}

	if (const std::optional<std::filesystem::path> app_data = uam::env::GetTrimmedPath("APPDATA"))
	{
		return *app_data / "Universal Agent Manager";
	}

	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
		return *home / "AppData" / "Local" / "Universal Agent Manager";
	}

#endif

	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
#if defined(__APPLE__)
		return *home / "Library" / "Application Support" / "Universal Agent Manager";
#else
		return *home / ".universal_agent_manager";
#endif
	}

	if (const std::optional<std::filesystem::path> temp = uam::paths::TempDirectoryPathNoThrow())
	{
		return *temp / "universal_agent_manager_data";
	}

	return std::filesystem::path("data");
}

std::filesystem::path AppPaths::GeminiHomePath()
{
	if (const std::optional<std::filesystem::path> gemini_cli_home = uam::env::GetTrimmedPath("GEMINI_CLI_HOME"))
	{
		return *gemini_cli_home;
	}

	if (const std::optional<std::filesystem::path> gemini_home = uam::env::GetTrimmedPath("GEMINI_HOME"))
	{
		return *gemini_home;
	}

	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
		return *home / ".gemini";
	}

	return uam::paths::CurrentPathOrDot() / ".gemini";
}

std::filesystem::path AppPaths::DefaultGeminiUniversalRootPath()
{
	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
		return *home / ".Gemini_universal_agent_manager";
	}

	return uam::paths::CurrentPathOrDot() / ".Gemini_universal_agent_manager";
}

std::optional<std::filesystem::path> AppPaths::ResolveGeminiProjectTmpDir(const std::filesystem::path& project_root)
{
	namespace fs = std::filesystem;
	const fs::path gemini_home = GeminiHomePath();
	const fs::path tmp_root = gemini_home / "tmp";

	if (!uam::paths::IsDirectoryNoThrow(tmp_root))
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
			closest_depth = kExactPathMatchDepth;
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

	for (fs::directory_iterator it(tmp_root, ec), end; !ec && it != end; it.increment(ec))
	{
		const fs::directory_entry& item = *it;
		if (!uam::paths::IsDirectoryEntryNoThrow(item))
		{
			continue;
		}

		const fs::path project_root_file = item.path() / ".project_root";

		if (!uam::paths::PathExistsNoThrow(project_root_file))
		{
			continue;
		}

		const std::string recorded_path_raw = uam::strings::Trim(uam::io::ReadTextFile(project_root_file));

		if (recorded_path_raw.empty())
		{
			continue;
		}

		if (consider_candidate(item.path(), fs::path(recorded_path_raw)))
		{
			return closest_match;
		}
	}

	const ProjectMappings project_mappings = ReadProjectMappings(gemini_home);

	for (const auto& entry : project_mappings)
	{
		const fs::path candidate_tmp_path = tmp_root / entry.second;

		if (!uam::paths::IsDirectoryNoThrow(candidate_tmp_path))
		{
			continue;
		}

		if (consider_candidate(candidate_tmp_path, entry.first))
		{
			return closest_match;
		}
	}

	if (closest_match)
	{
		return closest_match;
	}

	return std::nullopt;
}

std::filesystem::path AppPaths::UamChatsRootPath(const std::filesystem::path& data_root)
{
	return ChatsRootPath(data_root);
}

std::filesystem::path AppPaths::UamChatFilePath(const std::filesystem::path& data_root, std::string_view chat_id)
{
	std::string filename(chat_id);
	filename += ".json";
	return UamChatsRootPath(data_root) / filename;
}

std::filesystem::path AppPaths::UamChatSummariesRootPath(const std::filesystem::path& data_root)
{
	return data_root / "chat-summaries";
}

std::filesystem::path AppPaths::UamChatSummaryFilePath(const std::filesystem::path& data_root, std::string_view chat_id)
{
	std::string filename(chat_id);
	filename += ".json";
	return UamChatSummariesRootPath(data_root) / filename;
}

bool FolderDirectoryMatches(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
	return PathsEquivalent(NormalizePathForCompare(lhs), NormalizePathForCompare(rhs));
}
