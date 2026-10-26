#pragma once

#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::command_safety
{

enum class Tier
{
	Off,
	AcceptEdits,
	AiReview,
	Yolo
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
	if (normalized == "off") return Tier::Off;
	if (normalized == "acceptedits") return Tier::AcceptEdits;
	if (normalized == "aireview" || normalized == "low" || normalized == "medium" || normalized == "high") return Tier::AiReview;
	if (normalized == "yolo") return Tier::Yolo;
	return Tier::Off;
}

inline std::string TierName(Tier tier)
{
	if (tier == Tier::Off) return "off";
	if (tier == Tier::AcceptEdits) return "acceptEdits";
	if (tier == Tier::AiReview) return "aiReview";
	if (tier == Tier::Yolo) return "yolo";
	return "off";
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
		"&&",
		"||",
		";",
		"|",
		"&",
		"\n",
		"\r",
		"$(",
		"`",
		">",
		"<",
	});
	if (ContainsAny(normalized, high_risk)) return RiskLevel::WarnHigh;

	const std::size_t separator = normalized.find_first_of(" \t\r\n");
	const std::string_view executable(normalized.data(), separator == std::string::npos ? normalized.size() : separator);
	constexpr auto high_risk_executables = std::to_array<std::string_view>({
		"bash", "cmd", "cmd.exe", "curl", "diskpart", "del", "erase", "invoke-expression", "invoke-webrequest", "node", "node.exe", "npx", "powershell", "powershell.exe", "pwsh", "pwsh.exe", "python", "python.exe", "python3", "remove-item", "rm", "rd", "rmdir", "sh", "sudo", "wget", "zsh",
	});
	for (std::string_view high_risk_executable : high_risk_executables)
	{
		if (executable == high_risk_executable) return RiskLevel::WarnHigh;
	}
	constexpr auto read_commands = std::to_array<std::string_view>({
		"cat",
		"ls",
		"pwd",
		"which",
		"where",
		"grep",
		"head",
		"tail",
		"wc",
		"stat",
		"file",
		"type",
		"get-content",
	});
	if (executable == "git")
	{
		constexpr auto read_git = std::to_array<std::string_view>({
			"git status",
			"git diff",
			"git log",
			"git show",
		});
		for (std::string_view prefix : read_git)
		{
			if ((normalized == prefix || normalized.starts_with(std::string(prefix) + " ")) && !ContainsAny(normalized, std::to_array<std::string_view>({"--ext-diff", "--textconv"})))
			{
				return RiskLevel::Allowed;
			}
		}
		return RiskLevel::WarnHigh;
	}
	if (executable == "svn")
	{
		const std::size_t start = normalized.find_first_not_of(" \t", executable.size());
		const std::string_view rest = start == std::string::npos ? std::string_view{} : std::string_view(normalized).substr(start);
		const std::string_view action = rest.substr(0, rest.find_first_of(" \t"));
		constexpr auto read_svn = std::to_array<std::string_view>({
			"status", "stat", "st", "info", "log", "list", "ls", "cat", "blame", "praise", "annotate", "ann",
			"propget", "pget", "pg", "proplist", "plist", "pl", "diff", "di",
		});
		// A configured or explicit diff-cmd/config override can run another program; only the internal diff is read-only.
		const bool runs_diff = action == "diff" || action == "di" || rest.find("--diff") != std::string_view::npos;
		// Only a parsed switch before "--" counts; an operand such as "notes--internal-diff.txt" must not.
		bool internal_diff = false;
		for (std::size_t begin = 0; begin < rest.size();)
		{
			const std::size_t end = std::min(rest.find_first_of(" \t", begin), rest.size());
			const std::string_view word = rest.substr(begin, end - begin);
			if (word == "--") break;
			internal_diff = internal_diff || word == "--internal-diff";
			begin = end + 1;
		}
		if (std::ranges::find(read_svn, action) != read_svn.end() &&
		    !ContainsAny(normalized, std::to_array<std::string_view>({"--diff-cmd", "--config-option", "--config-dir"})) &&
		    (!runs_diff || internal_diff))
			return RiskLevel::Allowed;
		return RiskLevel::WarnHigh;
	}
	for (std::string_view read_command : read_commands)
	{
		if (executable == uam::strings::ToLowerAscii(read_command))
		{
			return RiskLevel::Allowed;
		}
	}
	return RiskLevel::WarnHigh;
}

inline bool WorkspaceIsVersionControlled(const std::filesystem::path& workspace)
{
	if (workspace.empty()) return false;
	std::error_code ec;
	return std::filesystem::exists(workspace / ".git", ec) || std::filesystem::exists(workspace / ".svn", ec);
}

inline bool RequiresApproval(Tier tier, RiskLevel risk, bool version_controlled_workspace)
{
	(void)risk;
	(void)version_controlled_workspace;
	return tier == Tier::AiReview;
}

} // namespace uam::command_safety
