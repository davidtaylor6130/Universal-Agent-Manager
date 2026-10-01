#pragma once

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <stop_token>
#include <string>

namespace uam::remote
{
	/// <summary>Reads only the selected target. Host instructions precede project memories.</summary>
	inline bool ResolveHostContext(IPlatformProcessService& service, const ExecutionHost& host,
	    const std::string& workspace, int memory_budget, std::string& context,
	    std::string& error, std::stop_token stop_token = {})
	{
		context.clear();
		if (host.id == execution_hosts::kLocalHostId) return true;
		if (!execution_hosts::IsAbsoluteRemotePath(host.platform, workspace))
		{
			error = "The remote workspace must be an absolute path on this host.";
			return false;
		}
		if (host.instruction_file.empty() && memory_budget <= 0) return true;
		RunnerClient client(service, SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version,
		    host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
		if (!client.Connect(&error, stop_token)) return false;
		std::string text;
		if (!host.instruction_file.empty())
		{
			if (!execution_hosts::IsAbsoluteRemotePath(host.platform, host.instruction_file))
			{
				error = "Host instruction file: select an absolute path on this host.";
				return false;
			}
			if (!client.ReadTextFile(paths::PathFromUtf8(host.instruction_file), text, &error, stop_token))
			{
				error = "Host instruction file: " + error;
				return false;
			}
			context = "--- BEGIN HOST INSTRUCTIONS ---\n" + text + "\n--- END HOST INSTRUCTIONS ---\n\n";
		}
		if (memory_budget > 0)
		{
			if (!client.ReadProjectMemory(paths::PathFromUtf8(workspace), std::clamp(memory_budget, 512, 65536), text, &error, stop_token))
			{
				error = "Project memory: " + error;
				return false;
			}
			if (!text.empty()) context += "Relevant memories from this remote project:\n" + text + "\n";
		}
		return true;
	}
}
