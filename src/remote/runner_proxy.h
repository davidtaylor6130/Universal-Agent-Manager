#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::remote
{
	inline constexpr const char* kRemoteProcessSpecEnvironment = "UAM_REMOTE_PROCESS_SPEC";
	inline constexpr std::string_view kRemoteStopControlLine = "\x1eUAM_REMOTE_STOP\n";
	inline constexpr std::string_view kRemoteMcpControlPrefix = "\x1eUAM_REMOTE_MCP ";

	std::string BuildProcessProxySpec(
	    const std::string& session_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment);
	std::filesystem::path PackagedRunnerPath();
	std::vector<std::string> BuildRemoteTerminalSshArgv(
	    const std::string& ssh_alias, const std::string& platform,
	    const std::string& version, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::string& runner_directory = {});
	std::string BuildRemoteMcpControlLine(
	    const std::string& channel_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment);
	int RunProcessProxy(const std::string& ssh_alias, const std::string& platform,
	                    const std::string& version,
	                    const std::string& runner_directory = {});
	int RunTerminalProcess(const std::string& encoded_spec);
	int RunRemoteMcpShim(const std::string& channel_id, const std::filesystem::path& socket_path);
	int RunRemoteMcpShim(const std::string& channel_id);
}
