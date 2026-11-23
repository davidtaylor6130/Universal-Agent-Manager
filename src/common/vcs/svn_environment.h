#pragma once

#include "common/utils/env_utils.h"
#include "common/vcs/git_command.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::vcs
{
	/// <summary>Used for svn+ssh when the user has not set SVN_SSH: ssh fails instead of waiting on a password, passphrase or
	/// host-key prompt that nobody can answer from the app.</summary>
	inline constexpr std::string_view kSvnBatchSsh = "ssh -o BatchMode=yes";

	/// <summary>Environment every svn process UAM starts receives. `platform` is empty for this machine, otherwise the SSH
	/// host's platform. LC_ALL gives UTF-8 path encoding and parseable messages whatever the launch locale (a Finder launch
	/// has none, and svn then cannot read a non-ASCII path); Windows svn ignores it. SVN_SSH is set only on this machine and
	/// only when it is not already set, so a user's own tunnel command wins; an SSH host's environment is left as it is.</summary>
	inline std::vector<std::pair<std::string, std::string>> SvnEnvironment(std::string_view platform)
	{
		std::vector<std::pair<std::string, std::string>> environment;
		if (platform.empty() && !uam::env::GetNonEmptyString("SVN_SSH")) environment.emplace_back("SVN_SSH", std::string(kSvnBatchSsh));
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
