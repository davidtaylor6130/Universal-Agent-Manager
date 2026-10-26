#include "app/svn_branch_service.h"
#include "common/platform/platform_services.h"
#include "common/utils/shell_escape.h"

namespace uam
{
	SvnBranchRunner CreateSvnBranchRunner()
	{
		return [](const std::vector<std::string>& args, int timeout, std::string& output)
		{
			const auto result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("svn " + shell::JoinEscapedArgs(args), timeout);
			output = result.output;
			return result.ok && result.exit_code == 0 && !result.timed_out && !result.canceled && !result.output_truncated && result.error.empty();
		};
	}
}
