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

std::string ModelDecisionSource(std::string_view model_id)
{
	return std::string(kDecisionSourceModelPrefix).append(model_id);
}

std::string NormalizeDecisionSource(std::string_view source)
{
	const bool known = source.empty() || source == kDecisionSourceFilter || source == kDecisionSourceFallback ||
	                   source == kDecisionSourceUnknown || source.starts_with(kDecisionSourceModelPrefix);
	return known ? std::string(source) : std::string(kDecisionSourceUnknown);
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
