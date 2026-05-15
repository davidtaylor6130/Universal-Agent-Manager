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
