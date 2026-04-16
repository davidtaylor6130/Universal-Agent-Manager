#include "common/models/app_models.h"

std::string RoleToString(const MessageRole role)
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

MessageRole RoleFromString(const std::string& value)
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

std::string ViewModeToString(const CenterViewMode mode)
{
	(void)mode;
	return "cli";
}

CenterViewMode ViewModeFromString(const std::string& value)
{
	(void)value;
	return CenterViewMode::CliConsole;
}
