#pragma once

#include "common/utils/string_utils.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::command_safety
{

enum class Tier
{
	Low,
	Medium,
	High
};

enum class RiskLevel
{
	Allowed,
	Warn,
	WarnHigh
};

inline Tier ParseTier(std::string_view value)
{
	const std::string normalized = uam::strings::ToLowerAscii(uam::strings::Trim(value));
	if (normalized == "low") return Tier::Low;
	if (normalized == "high") return Tier::High;
	return Tier::Medium;
}

inline std::string TierName(Tier tier)
{
	if (tier == Tier::Low) return "low";
	if (tier == Tier::High) return "high";
	return "medium";
}

inline std::string NormalizeTier(std::string_view value)
{
	return TierName(ParseTier(value));
}

inline std::string RiskLevelName(RiskLevel risk)
{
	if (risk == RiskLevel::Allowed) return "allowed";
	if (risk == RiskLevel::WarnHigh) return "warn_high";
	return "warn";
}

inline bool ContainsAny(std::string_view value, const auto& needles)
{
	for (std::string_view needle : needles)
	{
		if (value.find(needle) != std::string_view::npos) return true;
	}
	return false;
}

inline RiskLevel ClassifyCommand(std::string_view command)
{
	const std::string normalized = uam::strings::ToLowerAscii(uam::strings::Trim(command));
	if (normalized.empty()) return RiskLevel::WarnHigh;

	constexpr auto high_risk = std::to_array<std::string_view>({
	    "curl ", "wget ", "invoke-webrequest", "npm install", "npm i ", "npx ", "pip install",
	    "pip3 install", "brew install", "apt install", "apt-get install", "winget install", "choco install",
	    "git clone", "sudo ", "chmod ", "chown ", "reg add", "reg delete", "set-executionpolicy",
	    "| bash", "| sh", "| zsh", "| powershell", "format ", "diskpart",
	});
	if (ContainsAny(normalized, high_risk)) return RiskLevel::WarnHigh;

	const std::size_t separator = normalized.find_first_of(" \t\r\n");
	const std::string_view executable(normalized.data(), separator == std::string::npos ? normalized.size() : separator);
	constexpr auto high_risk_executables = std::to_array<std::string_view>({
	    "curl", "wget", "invoke-webrequest", "npx", "sudo", "chmod", "chown", "diskpart",
	});
	for (std::string_view high_risk_executable : high_risk_executables)
	{
		if (executable == high_risk_executable) return RiskLevel::WarnHigh;
	}
	constexpr auto read_commands = std::to_array<std::string_view>({
	    "cat", "ls", "pwd", "env", "printenv", "which", "where", "rg", "grep", "head", "tail",
	    "sed", "awk", "wc", "stat", "file", "type", "Get-Content",
	});
	if (ContainsAny(normalized, std::to_array<std::string_view>({">", " --delete", " -delete", " -exec"})))
	{
		return RiskLevel::Warn;
	}
	if (ContainsAny(normalized, std::to_array<std::string_view>({"&&", "||", ";", "|", "\n", "\r"})))
	{
		return RiskLevel::Warn;
	}
	if (executable == "git")
	{
		constexpr auto read_git = std::to_array<std::string_view>({"git status", "git diff", "git log", "git show"});
		for (std::string_view prefix : read_git)
		{
			if (normalized == prefix || normalized.starts_with(std::string(prefix) + " ")) return RiskLevel::Allowed;
		}
		return RiskLevel::Warn;
	}
	for (std::string_view read_command : read_commands)
	{
		if (executable == uam::strings::ToLowerAscii(read_command)) return RiskLevel::Allowed;
	}
	return RiskLevel::Warn;
}

inline bool WorkspaceIsVersionControlled(const std::filesystem::path& workspace)
{
	if (workspace.empty()) return false;
	std::error_code ec;
	return std::filesystem::exists(workspace / ".git", ec) || std::filesystem::exists(workspace / ".svn", ec);
}

inline bool RequiresApproval(Tier tier, RiskLevel risk, bool version_controlled_workspace)
{
	if (risk == RiskLevel::Allowed) return false;
	if (risk == RiskLevel::WarnHigh || tier == Tier::Low) return true;
	// ponytail: workspace-level VCS detection; parse individual command targets if cross-root writes become common.
	return !version_controlled_workspace;
}

} // namespace uam::command_safety
