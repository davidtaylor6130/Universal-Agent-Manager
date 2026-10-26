#include "common/chat/chat_folder_store.h"
#include "common/config/line_value_codec.h"
#include "common/paths/path_utils.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"

#include <sstream>

bool ChatFolderStore::ValidateDataFolderReferences(std::string_view text, const std::filesystem::path& source, std::string& error)
{
	std::string directory, host;
	bool section = false;
	const auto valid = [&]()
	{
		if (!section || directory.empty() || (!host.empty() && host != "local"))
			return true;
		auto path = uam::paths::PathFromUtf8(directory);
		if (directory == "~" || directory.starts_with("~/") || directory.starts_with("~\\"))
		{
			const auto home = uam::env::GetUserHomePath();
			if (!home)
			{
				error = "A workspace home reference cannot be resolved.";
				return false;
			}
			path = directory == "~" ? *home : *home / uam::paths::PathFromUtf8(directory.substr(2));
		}
		std::error_code path_error;
		path = std::filesystem::absolute(path, path_error);
		if (path_error || uam::paths::IsSameOrInsideRoot(source, path))
		{
			error = "A workspace folder points into the current data profile. Its owning workspace service must settle relocation first.";
			return false;
		}
		return true;
	};
	std::istringstream lines{std::string(text)};
	std::string line;
	while (std::getline(lines, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line == "[folder]")
		{
			if (!valid())
				return false;
			section = true;
			directory.clear();
			host.clear();
			continue;
		}
		if (!section)
			continue;
		const auto equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const auto key = uam::strings::TrimAsciiView(std::string_view(line).substr(0, equals));
		const auto value = uam::strings::Trim(uam::DecodeLineValue(std::string_view(line).substr(equals + 1)));
		if (key == "directory")
			directory = value;
		if (key == "execution_host_id")
			host = value;
	}
	return valid();
}
