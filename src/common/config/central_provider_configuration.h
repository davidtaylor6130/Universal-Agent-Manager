#pragma once

#include "common/models/app_models.h"
#include "common/paths/path_utils.h"
#include "common/utils/string_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string_view>

namespace uam::central_configuration
{
	inline nlohmann::json Serialize(const CentralProviderConfiguration& configuration)
	{
		return {{"enabled", configuration.enabled}, {"instructions", configuration.instructions},
		        {"instructionFiles", configuration.instruction_files}, {"skillDirectories", configuration.skill_directories}, {"defaultSkills", configuration.default_skills}, {"defaultAgents", configuration.default_agents},
		        {"defaultAgentId", configuration.default_agent_id}, {"uamControlEnabled", configuration.uam_control_enabled}};
	}

	/// <summary>Reject malformed settings instead of silently discarding an enabled launch requirement.</summary>
	inline bool Parse(const nlohmann::json& value, CentralProviderConfiguration& configuration, std::string& error)
	{
		if (!value.is_object()) { error = "Central configuration must be an object."; return false; }
		CentralProviderConfiguration parsed;
		for (const char* key : {"enabled", "uamControlEnabled"})
			if (value.contains(key) && !value[key].is_boolean()) { error = "Central configuration switches must be booleans."; return false; }
		for (const char* key : {"instructions", "defaultAgentId"})
			if (value.contains(key) && !value[key].is_string()) { error = "Central instructions and agent id must be text."; return false; }
		parsed.enabled = value.value("enabled", false);
		parsed.uam_control_enabled = value.value("uamControlEnabled", true);
		parsed.instructions = value.value("instructions", std::string{});
		parsed.default_agent_id = uam::strings::TrimAndLowerAscii(value.value("defaultAgentId", std::string{"build"}));
		if (parsed.instructions.size() > 256 * 1024 || parsed.instructions.find('\0') != std::string::npos || parsed.default_agent_id.empty() || parsed.default_agent_id.size() > 128 || parsed.default_agent_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
		{ error = "Central instructions are too large or the default agent id is invalid."; return false; }
		for (const char* key : {"instructionFiles", "skillDirectories"})
		{
			if (!value.contains(key)) continue;
			if (!value[key].is_array() || value[key].size() > 64) { error = "Use at most 64 paths for each central resource list."; return false; }
			std::vector<std::string>& paths = std::string_view(key) == "instructionFiles" ? parsed.instruction_files : parsed.skill_directories;
			for (const nlohmann::json& entry : value[key])
			{
				if (!entry.is_string()) { error = "Central resource paths must be text."; return false; }
				const std::string path = uam::strings::Trim(entry.get<std::string>());
				if (path.size() > 4096 || path.find('\0') != std::string::npos || !uam::paths::PathFromUtf8(path).is_absolute())
				{ error = "Central resource paths must be absolute paths on this computer."; return false; }
				if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
			}
		}
		if (value.contains("defaultSkills"))
		{
			if (!value["defaultSkills"].is_array() || value["defaultSkills"].size() > 256) { error = "Use at most 256 default skills."; return false; }
			for (const nlohmann::json& entry : value["defaultSkills"])
			{
				if (!entry.is_string() || entry.get<std::string>().empty() || entry.get<std::string>().size() > 256 || entry.get<std::string>().find_first_of(std::string_view("/\\\0", 3)) != std::string::npos)
				{ error = "Default skills must be skill names."; return false; }
				if (std::find(parsed.default_skills.begin(), parsed.default_skills.end(), entry.get<std::string>()) == parsed.default_skills.end()) parsed.default_skills.push_back(entry.get<std::string>());
			}
		}
		if (value.contains("defaultAgents"))
		{
			if (!value["defaultAgents"].is_array() || value["defaultAgents"].size() > 256) { error = "Use at most 256 default agents."; return false; }
			for (const nlohmann::json& entry : value["defaultAgents"])
			{
				if (!entry.is_string() || entry.get<std::string>().empty() || entry.get<std::string>().size() > 256 || entry.get<std::string>().find_first_of(std::string_view("/\\\0", 3)) != std::string::npos)
				{ error = "Default skills must be agent names."; return false; }
				if (std::find(parsed.default_agents.begin(), parsed.default_agents.end(), entry.get<std::string>()) == parsed.default_agents.end()) parsed.default_agents.push_back(entry.get<std::string>());
			}
		}
		configuration = std::move(parsed);
		return true;
	}
}
