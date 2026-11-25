#pragma once

#include "common/models/app_models.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cctype>
#include <stop_token>
#include <string>
#include <string_view>

/// <summary>Process helpers shared by the Git worktree and VCS commit services. Each text helper's name says which
/// streams it returns: Git and SVN put their failure detail on stderr, so error messages use the OutputAndError forms.</summary>
namespace uam::vcs_command
{
	inline constexpr int kVcsCommandTimeoutMs = 120000;

	inline ProcessExecutionResult RunCommand(const std::string& command, int timeout_ms = kVcsCommandTimeoutMs, std::stop_token stop_token = {})
	{
		return PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout_ms, stop_token);
	}

	inline bool CommandSucceeded(const ProcessExecutionResult& result)
	{
		return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
	}

	/// <summary>Trimmed stdout, then trimmed stderr on its own line.</summary>
	inline std::string CommandOutputAndError(const ProcessExecutionResult& result)
	{
		std::string detail = strings::Trim(result.output);
		const std::string error = strings::Trim(result.error);
		if (!error.empty())
		{
			if (!detail.empty()) detail += "\n";
			detail += error;
		}
		return detail;
	}

	/// <summary>CommandOutputAndError, or the fallback when both streams are empty.</summary>
	inline std::string CommandOutputAndErrorOr(const ProcessExecutionResult& result, const std::string& fallback)
	{
		return strings::NonEmptyOrFallback(CommandOutputAndError(result), fallback);
	}

	/// <summary>Trimmed stdout only, or the fallback when it is empty; stderr is dropped.</summary>
	inline std::string CommandStdoutOr(const ProcessExecutionResult& result, const std::string& fallback)
	{
		return strings::NonEmptyOrFallback(strings::Trim(result.output), fallback);
	}

	/// <summary>A full SHA-1 or SHA-256 Git object id.</summary>
	inline bool IsGitObjectId(std::string_view value)
	{
		return (value.size() == 40 || value.size() == 64) && std::ranges::all_of(value, [](unsigned char ch) { return std::isxdigit(ch) != 0; });
	}
}
