#pragma once

#include "common/utils/env_utils.h"
#include "common/paths/path_utils.h"

inline std::filesystem::path CopilotSessionStatePath()
{
	if (const std::optional<std::filesystem::path> copilot_home = uam::env::GetTrimmedPath("COPILOT_HOME"))
	{
		return *copilot_home / "session-state";
	}
	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
		return *home / ".copilot" / "session-state";
	}
	return uam::paths::CurrentPathOrDot() / ".copilot" / "session-state";
}
