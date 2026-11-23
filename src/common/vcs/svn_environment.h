#pragma once

#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"
#include "common/vcs/git_command.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::vcs
{
	/// <summary>Used for svn+ssh when the user has chosen no tunnel command: ssh fails instead of waiting on a password,
	/// passphrase or host-key prompt that nobody can answer from the app.</summary>
	inline constexpr std::string_view kSvnBatchSsh = "ssh -o BatchMode=yes";

	/// <summary>True when Subversion config text defines its own `[tunnels]` `ssh` command. svn's built-in default
	/// (`$SVN_SSH ssh -q`, with or without a trailing `--`) does not count; commented lines never do.</summary>
	inline bool SvnConfigDefinesSshTunnel(std::string_view config)
	{
		bool tunnels = false;
		std::size_t position = 0;
		while (position <= config.size())
		{
			std::size_t end = config.find('\n', position);
			if (end == std::string_view::npos) end = config.size();
			const std::string line = uam::strings::Trim(config.substr(position, end - position));
			position = end + 1;
			if (line.empty() || line.front() == '#' || line.front() == ';') continue;
			if (line.front() == '[')
			{
				const std::size_t close = line.find(']');
				tunnels = close != std::string::npos && uam::strings::Trim(std::string_view(line).substr(1, close - 1)) == "tunnels";
				continue;
			}
			const std::size_t separator = line.find_first_of("=:");
			if (!tunnels || separator == std::string::npos || uam::strings::Trim(std::string_view(line).substr(0, separator)) != "ssh") continue;
			const std::string value = uam::strings::Trim(std::string_view(line).substr(separator + 1));
			return value != "$SVN_SSH ssh -q" && value != "$SVN_SSH ssh -q --";
		}
		return false;
	}

	/// <summary>Whether this machine's user or system Subversion config chooses its own ssh tunnel command.</summary>
	inline bool SvnUserChoseSshTunnel()
	{
		std::vector<std::filesystem::path> files;
#if defined(_WIN32)
		if (const auto app_data = uam::env::GetTrimmedPath("APPDATA")) files.push_back(*app_data / "Subversion" / "config");
		if (const auto program_data = uam::env::GetTrimmedPath("ProgramData")) files.push_back(*program_data / "Subversion" / "config");
#else
		if (const auto home = uam::env::GetUserHomePath()) files.push_back(*home / ".subversion" / "config");
		files.emplace_back("/etc/subversion/config");
#endif
		for (const std::filesystem::path& file : files)
		{
			std::ifstream stream(file, std::ios::binary);
			if (!stream) continue;
			std::string text;
			text.resize(256 * 1024);
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			text.resize(static_cast<std::size_t>(stream.gcount()));
			if (SvnConfigDefinesSshTunnel(text)) return true;
		}
		return false;
	}

	/// <summary>Environment every svn process UAM starts receives. `platform` is empty for this machine, otherwise the SSH
	/// host's platform. LC_ALL gives UTF-8 path encoding and parseable messages whatever the launch locale (a Finder launch
	/// has none, and svn then cannot read a non-ASCII path); Windows svn ignores it. SVN_SSH is set only on this machine and
	/// only when the user has neither set SVN_SSH nor chosen a `[tunnels]` ssh command in their Subversion config, because a
	/// set SVN_SSH replaces a configured `$SVN_SSH ...` tunnel whole. An SSH host's environment is left as it is.</summary>
	inline std::vector<std::pair<std::string, std::string>> SvnEnvironment(std::string_view platform)
	{
		std::vector<std::pair<std::string, std::string>> environment;
		if (platform.empty() && !uam::env::GetNonEmptyString("SVN_SSH") && !SvnUserChoseSshTunnel()) environment.emplace_back("SVN_SSH", std::string(kSvnBatchSsh));
		if (IsWindowsHostPlatform(platform)) return environment;
#if defined(__APPLE__)
		if (platform.empty())
		{
			environment.emplace_back("LC_ALL", "en_US.UTF-8");
			return environment;
		}
#elif defined(_WIN32)
		if (platform.empty()) return environment;
#endif
		environment.emplace_back("LC_ALL", "C.UTF-8");
		return environment;
	}
} // namespace uam::vcs
