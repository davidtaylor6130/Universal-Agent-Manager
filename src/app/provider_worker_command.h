#pragma once

#include "common/paths/path_utils.h"
#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/utils/env_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct AppState;

	inline constexpr auto kBaseProviderWorkerPathEntries = std::to_array<std::string_view>({
	    "/opt/homebrew/bin",
	    "/opt/homebrew/sbin",
	    "/usr/local/bin",
	    "/usr/local/sbin",
	    "/usr/bin",
	    "/bin",
	    "/usr/sbin",
	    "/sbin",
	});
	inline constexpr std::size_t kBaseProviderWorkerPathEntryCount = kBaseProviderWorkerPathEntries.size();

	enum class ProviderWorkerPathMode
	{
		BasePath,
		IncludeNvmNodeVersions
	};

	struct ProviderWorkerInvocation
	{
		bool direct_process = false;
		std::vector<std::string> argv;
		std::string standard_input;
		std::string shell_command;
		std::string command_preview;
		std::vector<std::pair<std::string, std::string>> environment_overrides;
		std::shared_ptr<const std::filesystem::path> isolated_working_directory;

		bool Empty() const
		{
			return direct_process ? argv.empty() : shell_command.empty();
		}
	};

	inline void AppendUniqueProviderWorkerPathEntry(std::vector<std::string>& values, std::string_view value)
	{
		uam::ranges::PushUniqueTrimmedNonEmptyString(values, value);
	}

	inline void AppendProviderWorkerNvmNodeVersionBins(std::vector<std::string>& entries, const std::filesystem::path& home_path)
	{
		const std::filesystem::path nvm_versions_dir = home_path / ".nvm" / "versions" / "node";
		std::error_code ec;
		if (!uam::paths::IsDirectoryNoThrow(nvm_versions_dir))
		{
			return;
		}

		std::vector<std::string> discovered_bins;
		for (std::filesystem::directory_iterator it(nvm_versions_dir, ec), end; !ec && it != end; it.increment(ec))
		{
			const std::filesystem::directory_entry& entry = *it;
			if (!uam::paths::IsDirectoryEntryNoThrow(entry))
			{
				continue;
			}

			const std::filesystem::path bin_dir = entry.path() / "bin";
			if (uam::paths::IsDirectoryNoThrow(bin_dir))
			{
				discovered_bins.push_back(bin_dir.string());
			}
		}

		std::ranges::sort(discovered_bins);
		for (const std::string& bin_dir : discovered_bins)
		{
			AppendUniqueProviderWorkerPathEntry(entries, bin_dir);
		}
	}

	inline void AppendProviderWorkerUserToolchainPaths(std::vector<std::string>& entries, const std::filesystem::path& home_path, ProviderWorkerPathMode path_mode)
	{
		AppendUniqueProviderWorkerPathEntry(entries, (home_path / ".volta" / "bin").string());
		AppendUniqueProviderWorkerPathEntry(entries, (home_path / ".asdf" / "shims").string());
		AppendUniqueProviderWorkerPathEntry(entries, (home_path / ".fnm").string());

		if (path_mode == ProviderWorkerPathMode::IncludeNvmNodeVersions)
		{
			AppendProviderWorkerNvmNodeVersionBins(entries, home_path);
		}
	}

	inline std::vector<std::string> ProviderWorkerPathEntries(ProviderWorkerPathMode path_mode)
	{
		std::vector<std::string> entries;
		entries.reserve(kBaseProviderWorkerPathEntries.size());
		for (const std::string_view dir : kBaseProviderWorkerPathEntries)
		{
			AppendUniqueProviderWorkerPathEntry(entries, dir);
		}

		if (const std::optional<std::filesystem::path> home_path = uam::env::GetTrimmedPath("HOME"))
		{
			AppendProviderWorkerUserToolchainPaths(entries, *home_path, path_mode);
		}

		return entries;
	}

	inline std::string JoinProviderWorkerPathEntries(const std::vector<std::string>& entries)
	{
		return uam::strings::JoinNonEmpty(entries, ":");
	}

	inline std::string WithProviderWorkerPathEnvironment(std::string_view command, ProviderWorkerPathMode path_mode)
	{
#if defined(_WIN32)
		(void)path_mode;
		return std::string(command);
#else
		const std::string path_prefix = JoinProviderWorkerPathEntries(ProviderWorkerPathEntries(path_mode));
		if (path_prefix.empty())
		{
			return std::string(command);
		}

		std::string wrapped = "PATH=" + uam::shell::EscapeArg(path_prefix) + "${PATH:+\":${PATH}\"} ";
		wrapped.append(command);
		return wrapped;
#endif
	}

	inline std::string BuildProviderWorkerShellCommand(const std::vector<std::string>& argv, ProviderWorkerPathMode path_mode)
	{
		return WithProviderWorkerPathEnvironment(uam::shell::JoinEscapedArgs(argv), path_mode);
	}

	ProviderWorkerInvocation BuildProviderWorkerInvocation(const AppState& app, const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id, ProviderWorkerPathMode path_mode, std::string* error_out = nullptr);
	ProcessExecutionResult ExecuteProviderWorkerInvocation(const ProviderWorkerInvocation& invocation, const std::filesystem::path& working_directory, int timeout_ms = -1, std::stop_token stop_token = {});
} // namespace uam
