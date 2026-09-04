#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "remote/runner_protocol.h"

namespace uam::remote
{
	inline constexpr const char* kRemoteProcessSpecEnvironment = "UAM_REMOTE_PROCESS_SPEC";
	inline constexpr std::string_view kRemoteStopControlLine = "\x1eUAM_REMOTE_STOP\n";
	inline constexpr std::string_view kRemoteMcpControlPrefix = "\x1eUAM_REMOTE_MCP ";
	inline constexpr std::string_view kRemoteOutputMarkerPrefix = "\x1eUAM_REMOTE_OUTPUT ";
	inline constexpr std::string_view kRemoteOutputAckPrefix = "\x1eUAM_REMOTE_OUTPUT_ACK ";
	inline constexpr std::string_view kRemoteSourceExitPrefix = "\x1eUAM_REMOTE_SOURCE_EXIT ";
	inline constexpr std::string_view kRemoteSourceExitAckPrefix = "\x1eUAM_REMOTE_SOURCE_EXIT_ACK ";
	inline constexpr std::string_view kRemoteInputDeliveryPrefix = "\x1eUAM_REMOTE_INPUT ";
	inline constexpr std::string_view kRemoteInputReceiptPrefix = "\x1eUAM_REMOTE_INPUT_ACK ";
	inline bool UsesDurableRemoteOutputHandshake(int protocol_version,
	                                            std::string_view delivery_token)
	{
		return protocol_version >= 3 && !delivery_token.empty();
	}

	std::string BuildProcessProxySpec(
	    const std::string& session_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment,
	    bool attach_only = false,
	    const std::string& delivery_token = {},
	    std::uintmax_t delivered_stdout_cursor = 0,
	    std::uintmax_t delivered_stderr_cursor = 0);
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
	std::string BuildRemoteInputDeliveryLine(
	    std::string_view delivery_token, std::string_view delivery_id,
	    std::string_view payload);
	int RunProcessProxy(const std::string& ssh_alias, const std::string& platform,
	                    const std::string& version,
	                    const std::string& runner_directory = {},
	                    int protocol_version = kRunnerProtocolVersion);
	int RunTerminalProcess(const std::string& encoded_spec);
	int RunRemoteMcpShim(const std::string& channel_id, const std::filesystem::path& socket_path);
	int RunRemoteMcpShim(const std::string& channel_id);
}
