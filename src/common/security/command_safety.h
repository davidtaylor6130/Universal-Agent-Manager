#pragma once

#include "common/security/read_only_command.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

/// <summary>The permission level name the composer shows for a tier.</summary>
inline std::string TierDisplayName(std::string_view value)
{
	const Tier tier = ParseTier(value);
	if (tier == Tier::AcceptEdits) return "Accept Edits";
	if (tier == Tier::AiReview) return "AI Review";
	if (tier == Tier::Yolo) return "YOLO";
	return "Default";
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

/// <summary>Shell startup files: writing one runs code in every later shell, so no automatic path may approve it.</summary>
inline constexpr auto kShellStartupFiles = std::to_array<std::string_view>({
	".bashrc", ".bash_profile", ".bash_login", ".bash_logout", ".profile", ".zshrc", ".zshenv", ".zprofile", ".zlogin", ".zlogout",
	".cshrc", ".tcshrc", ".kshrc", ".mkshrc", "config.fish", "microsoft.powershell_profile.ps1", "profile.ps1",
});

inline bool IsShellStartupFile(const std::filesystem::path& path)
{
	const std::string name = uam::strings::ToLowerAscii(uam::paths::PortablePathString(path.filename()));
	return std::ranges::find(kShellStartupFiles, name) != kShellStartupFiles.end();
}

/// <summary>Admitted literal reads are Allowed. Chaining, redirection, network, deletion, shells, interpreters,
/// unadmitted VCS commands and paths outside the workspace are WarnHigh and always go to the user;
/// other commands are Warn and may go to the AI reviewer.</summary>
inline RiskLevel ClassifyCommand(std::string_view command, const ReadScope& scope)
{
	if (IsReadOnlyCommand(command, scope)) return RiskLevel::Allowed;
	const std::string normalized = uam::strings::ToLowerAscii(uam::strings::Trim(command));
	if (normalized.empty()) return RiskLevel::WarnHigh;

	constexpr auto high_risk = std::to_array<std::string_view>({
		"&&", "||", ";", "|", "&", "\n", "\r", "$(", "`", ">", "<", "://",
	});
	if (ContainsAny(normalized, high_risk) || ContainsAny(normalized, kShellStartupFiles)) return RiskLevel::WarnHigh;

	std::vector<std::string_view> words;
	for (std::size_t begin = normalized.find_first_not_of(" \t"); begin != std::string::npos; begin = normalized.find_first_not_of(" \t", begin))
	{
		const std::size_t end = std::min(normalized.find_first_of(" \t", begin), normalized.size());
		std::string_view word = std::string_view(normalized).substr(begin, end - begin);
		while (!word.empty() && (word.front() == '"' || word.front() == '\'')) word.remove_prefix(1);
		if (!word.empty()) words.push_back(word);
		begin = end;
	}
	if (words.empty()) return RiskLevel::WarnHigh;
	// Home, absolute, parent and drive paths leave the workspace.
	for (std::string_view word : words)
	{
		if (word.starts_with('/') || word.starts_with('~') || word.find("..") != std::string_view::npos ||
		    word.find("=~") != std::string_view::npos || (word.size() > 1 && word[1] == ':')) return RiskLevel::WarnHigh;
	}

	std::string_view executable = words.front();
	if (const std::size_t slash = executable.find_last_of("/\\"); slash != std::string_view::npos) executable.remove_prefix(slash + 1);
	if (executable.ends_with(".exe")) executable.remove_suffix(4);
	constexpr auto high_risk_executables = std::to_array<std::string_view>({
		// Shells, interpreters and indirection.
		"bash", "sh", "zsh", "dash", "ksh", "csh", "tcsh", "fish", "nu", "cmd", "powershell", "pwsh", "wsl",
		"python", "python2", "python3", "py", "node", "nodejs", "deno", "bun", "bunx", "npx", "uvx", "pipx", "perl", "ruby", "php", "lua", "tclsh", "rscript",
		"osascript", "awk", "gawk", "sed", "env", "xargs", "eval", "exec", "source", "find", "nohup", "open", "start", "start-process",
		"invoke-expression", "iex", "invoke-command", "sudo", "su", "doas", "runas",
		// Network.
		"curl", "wget", "ssh", "scp", "sftp", "rsync", "nc", "ncat", "netcat", "telnet", "ftp", "gh",
		"invoke-webrequest", "iwr", "invoke-restmethod", "irm",
		// Deletion and ownership.
		"rm", "rmdir", "rd", "del", "erase", "remove-item", "ri", "shred", "unlink", "diskpart", "format", "mkfs", "dd", "chmod", "chown",
		// Unadmitted version-control commands can push, rewrite history or run repository hooks.
		"git", "svn",
	});
	if (std::ranges::find(high_risk_executables, executable) != high_risk_executables.end()) return RiskLevel::WarnHigh;

	// Package installation downloads and runs third-party code.
	constexpr auto package_managers = std::to_array<std::string_view>({
		"npm", "pnpm", "yarn", "pip", "pip3", "uv", "poetry", "gem", "bundle", "cargo", "go", "brew", "apt", "apt-get", "dnf", "yum", "choco", "winget", "composer",
	});
	constexpr auto package_installs = std::to_array<std::string_view>({"install", "i", "add", "ci", "update", "upgrade", "get", "publish", "exec", "dlx", "x"});
	if (words.size() > 1 && std::ranges::find(package_managers, executable) != package_managers.end() &&
	    std::ranges::find(package_installs, words[1]) != package_installs.end()) return RiskLevel::WarnHigh;
	return RiskLevel::Warn;
}

inline bool WorkspaceIsVersionControlled(const std::filesystem::path& workspace)
{
	if (workspace.empty()) return false;
	std::error_code ec;
	return std::filesystem::exists(workspace / ".git", ec) || std::filesystem::exists(workspace / ".svn", ec);
}

/// <summary>AI Review always needs a decision from the reviewer or the user.</summary>
inline bool RequiresApproval(Tier tier)
{
	return tier == Tier::AiReview;
}

/// <summary>High-risk requests skip the AI reviewer: only the user may approve them.</summary>
inline bool RequiresUserDecision(Tier tier, RiskLevel risk)
{
	return tier == Tier::AiReview && risk == RiskLevel::WarnHigh;
}

} // namespace uam::command_safety
