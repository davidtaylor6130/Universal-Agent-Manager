#include "app/svn_branch_service.h"
#include "app/vcs_commit_service.h"

#include <filesystem>
#include <system_error>

namespace uam
{
	SvnBranchRunner CreateSvnBranchRunner(std::stop_token stop)
	{
		return [stop](const std::vector<std::string>& args, const int timeout_ms, std::string& output, std::string& failure)
		{
			// Every target is absolute or a URL, so the directory only has to exist; RunSvn adds the shared SVN environment.
			std::error_code error;
			std::filesystem::path directory = std::filesystem::temp_directory_path(error);
			if (error) directory = std::filesystem::current_path(error);
			// Stopping the switch itself would leave a locked, half-switched working copy; only the reads before it stop.
			const bool switching = !args.empty() && args.front() == "switch";
			if (switching && stop.stop_requested())
			{
				output.clear();
				failure = "Canceled.";
				return false;
			}
			const ProcessExecutionResult result = RunSvn(directory, args, timeout_ms, switching ? std::stop_token{} : stop);
			output = result.output;
			const bool succeeded = result.ok && result.exit_code == 0 && !result.timed_out && !result.canceled && !result.output_truncated && result.error.empty();
			if (succeeded) failure.clear();
			else
			{
				failure = SvnFailureMessage(result, {});
				LogSvnFailure(result, args.empty() ? "command" : args.front());
			}
			return succeeded;
		};
	}
}
