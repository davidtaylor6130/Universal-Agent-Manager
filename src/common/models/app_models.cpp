#include "common/models/app_models.h"

std::string RoleToString(MessageRole role)
{
	switch (role)
	{
	case MessageRole::User:
		return "user";
	case MessageRole::Assistant:
		return "assistant";
	case MessageRole::System:
		return "system";
	}

	return "user";
}

MessageRole RoleFromString(std::string_view value)
{
	if (value == "assistant")
	{
		return MessageRole::Assistant;
	}

	if (value == "system")
	{
		return MessageRole::System;
	}

	return MessageRole::User;
}

std::string ViewModeToString(CenterViewMode mode)
{
	(void)mode;
	return "cli";
}

CenterViewMode ViewModeFromString(std::string_view value)
{
	(void)value;
	return CenterViewMode::CliConsole;
}

std::string GoalStatusToString(GoalStatus status)
{
	switch (status)
	{
	case GoalStatus::Active:
		return "active";
	case GoalStatus::Complete:
		return "complete";
	case GoalStatus::Blocked:
		return "blocked";
	case GoalStatus::Paused:
		return "paused";
	}
	return "active";
}

GoalStatus GoalStatusFromString(std::string_view value)
{
	if (value == "complete")
	{
		return GoalStatus::Complete;
	}

	if (value == "blocked")
	{
		return GoalStatus::Blocked;
	}

	if (value == "paused")
	{
		return GoalStatus::Paused;
	}

	return GoalStatus::Active;
}
