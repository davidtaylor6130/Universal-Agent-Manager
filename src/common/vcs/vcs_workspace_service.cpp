#include "vcs_workspace_service.h"
#include "common/platform/platform_services.h"

#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <sstream>
#include <string>

namespace
{

	std::string Trim(const std::string& value)
	{
		const std::size_t start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const std::size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
	}

	std::string BuildWorkingDirectoryCommand(const std::filesystem::path& working_directory, const std::string& command)
	{
		return PlatformServicesFactory::Instance().process_service.BuildShellCommandWithWorkingDirectory(working_directory, command);
	}

	VcsCommandResult RunCommand(const std::filesystem::path& working_directory, const std::string& command, const int timeout_ms, const std::size_t output_limit_bytes)
	{
		if (working_directory.empty())
		{
			return VcsCommandResult{false, false, false, -1, "", "Workspace directory is empty."};
		}

		const std::string full_command = BuildWorkingDirectoryCommand(working_directory, command) + " 2>&1";

		std::promise<VcsCommandResult> promise;
		std::future<VcsCommandResult> future = promise.get_future();
		auto run_command_task = [full_command, output_limit_bytes, promise = std::move(promise)]() mutable
		{
			VcsCommandResult result;
			std::string output;
			std::string error;
			int raw_status = -1;
			const IPlatformProcessService& process_service = PlatformServicesFactory::Instance().process_service;

			if (!process_service.CaptureCommandOutput(full_command, &output, &raw_status, &error))
			{
				result.error = error.empty() ? "Failed to launch command." : error;
				promise.set_value(std::move(result));
				return;
			}

			if (output.size() > output_limit_bytes)
			{
				result.output = output.substr(0, output_limit_bytes);
				result.truncated = true;
			}
			else
			{
				result.output = std::move(output);
			}

			result.exit_code = process_service.NormalizeCapturedCommandExitCode(raw_status);
			result.ok = (result.exit_code == 0);

			if (result.truncated)
			{
				result.output += "\n\n[Output truncated due to size limit.]\n";
			}

			if (!result.ok && result.output.empty())
			{
				result.error = "Command failed with no output.";
			}

			promise.set_value(std::move(result));
		};

		std::thread(std::move(run_command_task)).detach();

		const auto wait_status = future.wait_for(std::chrono::milliseconds(timeout_ms));

		if (wait_status != std::future_status::ready)
		{
			VcsCommandResult timed_out;
			timed_out.ok = false;
			timed_out.timed_out = true;
			timed_out.error = "Command timed out.";
			return timed_out;
		}

		return future.get();
	}

	std::string DeriveBranchPath(const std::string& url, const std::string& relative_url)
	{
		if (!relative_url.empty())
		{
			if (StartsWith(relative_url, "^/"))
			{
				return relative_url.substr(1);
			}

			return relative_url;
		}

		const std::array<std::string, 3> markers{"/trunk", "/branches/", "/tags/"};

		for (const std::string& marker : markers)
		{
			const std::size_t pos = url.find(marker);

			if (pos != std::string::npos)
			{
				return url.substr(pos);
			}
		}

		return "";
	}

} // namespace

VcsRepoType VcsWorkspaceService::DetectRepo(const std::filesystem::path& workspace_root)
{
	if (workspace_root.empty())
	{
		return VcsRepoType::None;
	}

	std::error_code ec;

	if (std::filesystem::exists(workspace_root / ".svn", ec) && !ec)
	{
		return VcsRepoType::Svn;
	}

	const VcsCommandResult probe = RunCommand(workspace_root, "svn info --non-interactive", 2000, 32 * 1024);

	if (probe.ok && probe.output.find("Working Copy Root Path:") != std::string::npos)
	{
		return VcsRepoType::Svn;
	}

	return VcsRepoType::None;
}

VcsCommandResult VcsWorkspaceService::ReadSnapshot(const std::filesystem::path& workspace_root, VcsSnapshot& snapshot_out)
{
	snapshot_out = VcsSnapshot{};
	const VcsCommandResult command = RunCommand(workspace_root, "svn info --non-interactive", 4000, 128 * 1024);

	if (!command.ok)
	{
		return command;
	}

	std::istringstream lines(command.output);
	std::string line;
	std::string relative_url;

	while (std::getline(lines, line))
	{
		line = Trim(line);

		if (StartsWith(line, "Working Copy Root Path:"))
		{
			snapshot_out.working_copy_root = Trim(line.substr(std::string("Working Copy Root Path:").size()));
		}
		else if (StartsWith(line, "URL:"))
		{
			snapshot_out.repo_url = Trim(line.substr(std::string("URL:").size()));
		}
		else if (StartsWith(line, "Revision:"))
		{
			snapshot_out.revision = Trim(line.substr(std::string("Revision:").size()));
		}
		else if (StartsWith(line, "Relative URL:"))
		{
			relative_url = Trim(line.substr(std::string("Relative URL:").size()));
		}
	}

	snapshot_out.repo_type = VcsRepoType::Svn;
	snapshot_out.branch_path = DeriveBranchPath(snapshot_out.repo_url, relative_url);
	return command;
}

VcsCommandResult VcsWorkspaceService::ReadStatus(const std::filesystem::path& workspace_root)
{
	return RunCommand(workspace_root, "svn status --non-interactive", 4000, 256 * 1024);
}

VcsCommandResult VcsWorkspaceService::ReadDiff(const std::filesystem::path& workspace_root)
{
	return RunCommand(workspace_root, "svn diff --non-interactive", 6000, 512 * 1024);
}

VcsCommandResult VcsWorkspaceService::ReadLog(const std::filesystem::path& workspace_root)
{
	return RunCommand(workspace_root, "svn log -l 20 --non-interactive", 5000, 384 * 1024);
}
