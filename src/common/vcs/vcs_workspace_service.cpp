#include "vcs_workspace_service.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

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

	std::string ShellQuote(const std::string& value)
	{
#if defined(_WIN32)
		std::string escaped = "\"";

		for (const char ch : value)
		{
			if (ch == '\"')
			{
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped += "%%";
			}
			else if (ch == '\r' || ch == '\n')
			{
				escaped.push_back(' ');
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('\"');
		return escaped;
#else
		std::string escaped = "'";

		for (const char ch : value)
		{
			if (ch == '\'')
			{
				escaped += "'\\''";
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('\'');
		return escaped;
#endif
	}

	std::string BuildWorkingDirectoryCommand(const std::filesystem::path& working_directory, const std::string& command)
	{
#if defined(_WIN32)
		return "cd /d " + ShellQuote(working_directory.string()) + " && " + command;
#else
		return "cd " + ShellQuote(working_directory.string()) + " && " + command;
#endif
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
#if defined(_WIN32)
			std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(full_command.c_str(), "r"), _pclose);
#else
			std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(full_command.c_str(), "r"), pclose);
#endif

			if (pipe == nullptr)
			{
				result.error = "Failed to launch command.";
				promise.set_value(std::move(result));
				return;
			}

			std::array<char, 4096> buffer{};
			std::size_t captured = 0;

			while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
			{
				const std::string chunk(buffer.data());

				if (captured < output_limit_bytes)
				{
					const std::size_t allowed = output_limit_bytes - captured;

					if (chunk.size() <= allowed)
					{
						result.output += chunk;
						captured += chunk.size();
					}
					else
					{
						result.output.append(chunk.data(), allowed);
						captured += allowed;
						result.truncated = true;
					}
				}
				else
				{
					result.truncated = true;
				}
			}

			const int status = pipe.get_deleter()(pipe.release());
#if defined(_WIN32)
			result.exit_code = status;
			result.ok = (status == 0);
#else
			int exit_code = status;

			if (WIFEXITED(status))
			{
				exit_code = WEXITSTATUS(status);
			}

			result.exit_code = exit_code;
			result.ok = (exit_code == 0);
#endif

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
