#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/shell_escape.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uam::vcs
{
	inline constexpr bool kLocalHostIsWindows = uam::paths::kHostUsesWindowsPathRules;

	inline bool IsWindowsHostPlatform(std::string_view platform)
	{
		return platform == "windows" || platform == "Windows";
	}

	/// <summary>Global options every Git process UAM starts receives: repository configuration cannot start a file-system
	/// monitor, hooks, an external diff or a pager for these commands. Other configured programs, such as filter drivers,
	/// are not covered. No value contains a space or shell metacharacter, so the shell form needs no escaping.</summary>
	inline std::vector<std::string> GitSafetyOptions(bool windows_host)
	{
		return {"-c", "core.fsmonitor=false", "-c", windows_host ? "core.hooksPath=NUL" : "core.hooksPath=/dev/null",
		    "-c", "diff.external=", "-c", "core.pager=cat"};
	}

	/// <summary>Subcommands that render patches also take these, so attribute-selected text conversion and external diff
	/// drivers never run. `diff.external=` alone makes an unflagged `git diff` fail rather than run anything.</summary>
	inline bool GitSubcommandRendersPatches(std::string_view subcommand)
	{
		return subcommand == "diff" || subcommand == "show";
	}

	inline constexpr std::string_view kGitPatchSafetyFlags = "--no-ext-diff --no-textconv";

	/// <summary>The only way UAM builds a Git argv: `git`, the safety options, then `arguments` (everything after `git`),
	/// with the patch flags right after a diff or show subcommand.</summary>
	inline std::vector<std::string> HardenedGitArgv(const std::vector<std::string>& arguments, bool windows_host)
	{
		std::vector<std::string> argv{"git"};
		const std::vector<std::string> safety = GitSafetyOptions(windows_host);
		argv.insert(argv.end(), safety.begin(), safety.end());
		bool subcommand_seen = false;
		for (std::size_t index = 0; index < arguments.size(); ++index)
		{
			argv.push_back(arguments[index]);
			if (subcommand_seen) continue;
			if ((arguments[index] == "-C" || arguments[index] == "-c") && index + 1 < arguments.size())
			{
				argv.push_back(arguments[++index]);
				continue;
			}
			if (arguments[index].starts_with('-')) continue;
			subcommand_seen = true;
			if (GitSubcommandRendersPatches(arguments[index]))
			{
				argv.emplace_back("--no-ext-diff");
				argv.emplace_back("--no-textconv");
			}
		}
		return argv;
	}

	namespace detail
	{
		/// <summary>End of the shell word starting at `start`: a space outside single or double quotes, where a backslash
		/// escapes the next character outside single quotes, which covers both EscapeArg forms.</summary>
		inline std::size_t ShellWordEnd(std::string_view text, std::size_t start)
		{
			char quote = 0;
			std::size_t index = start;
			while (index < text.size() && (quote != 0 || text[index] != ' '))
			{
				const char value = text[index];
				if (value == '\\' && quote != '\'' && index + 1 < text.size()) { index += 2; continue; }
				if (quote == 0 && (value == '\'' || value == '"')) quote = value;
				else if (value == quote) quote = 0;
				++index;
			}
			return index;
		}

		/// <summary>Adds the patch flags after the subcommand of an already shell-escaped argument string.</summary>
		inline std::string WithPatchSafetyFlags(std::string_view arguments)
		{
			std::size_t position = 0;
			bool option_value = false;
			while (position < arguments.size())
			{
				const std::size_t start = arguments.find_first_not_of(' ', position);
				if (start == std::string_view::npos) break;
				const std::size_t end = ShellWordEnd(arguments, start);
				const std::string_view token = arguments.substr(start, end - start);
				position = end;
				if (option_value) { option_value = false; continue; }
				if (token == "-C" || token == "-c") { option_value = true; continue; }
				if (token.starts_with('-')) continue;
				if (!GitSubcommandRendersPatches(token)) break;
				return std::string(arguments.substr(0, end)) + " " + std::string(kGitPatchSafetyFlags) + std::string(arguments.substr(end));
			}
			return std::string(arguments);
		}

		inline std::string GitShellPrefix()
		{
			std::string command = "git";
			for (const std::string& option : GitSafetyOptions(kLocalHostIsWindows)) command += " " + option;
			return command;
		}
	} // namespace detail

	/// <summary>Local shell form of HardenedGitArgv. `arguments` is everything after `git`, already shell-escaped.</summary>
	inline std::string HardenedGitShellCommand(std::string_view arguments)
	{
		return detail::GitShellPrefix() + " " + detail::WithPatchSafetyFlags(arguments);
	}

	/// <summary>HardenedGitShellCommand run in `cwd` through `git -C`.</summary>
	inline std::string HardenedGitShellCommandInDirectory(const std::filesystem::path& cwd, std::string_view arguments)
	{
		return detail::GitShellPrefix() + " -C " + uam::shell::EscapeArg(uam::paths::Utf8PathString(cwd)) + " " + detail::WithPatchSafetyFlags(arguments);
	}
} // namespace uam::vcs
