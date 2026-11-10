#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <stop_token>
#include <vector>

#include "remote/runner_protocol.h"

namespace uam::remote
{
	inline constexpr const char* kRemoteProcessSpecEnvironment = "UAM_REMOTE_PROCESS_SPEC";
	inline constexpr std::string_view kRemoteStopOutcomePrefix = "UAM_REMOTE_STOP_OUTCOME ";
	/// Last stderr line of an attach whose bridge failed on a reachable host where no helper of that version is running,
	/// so no helper owns the process any more.
	inline constexpr std::string_view kRemoteHelperNotRunningLine = "The remote helper is not running on this host.";
	/// The host-side check behind that line; empty for a platform it cannot check. It prints kRemoteHelperNotRunningToken
	/// only after listing this account's processes and finding no helper of that version, deleted binary included.
	inline constexpr std::string_view kRemoteHelperNotRunningToken = "UAM_HELPER_NOT_RUNNING";
	std::string RemoteHelperNotRunningCommand(const std::string& platform, const std::string& version);
	// Only the proxy emits this before attempting to create a provider process.
	inline constexpr int kRemoteConfigurationStartupFailureExitCode = 71;
	inline constexpr std::string_view kRemoteStartupFailurePrefix = "UAM_REMOTE_STARTUP_FAILURE ";
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
	    std::uintmax_t delivered_stderr_cursor = 0,
	    const std::string& context_provider_id = {},
	    const std::string& context_directory = {},
	    const std::string& configuration_provider = {}, const std::string& configuration_directory = {}, const nlohmann::json& session_mcp_servers = nlohmann::json::array(), bool transient_process = false);
	std::filesystem::path PackagedRunnerPath();
	using PackagedRunnerChecksums = std::map<std::string, std::string, std::less<>>;
	/// Reads `<root>/<platform>-<architecture>/uam-runner.sha256` for each packaged helper target.
	PackagedRunnerChecksums ReadPackagedRunnerChecksums(const std::filesystem::path& root);
	/// Packaged helper SHA-256 for a host platform and architecture, read once; empty when none is packaged.
	/// Installation plans and update offers both use this cache, so they agree.
	std::string PackagedRunnerSha256(std::string_view platform, std::string_view architecture);
	/// Test seam: replaces the process-wide cache and returns the previous values.
	PackagedRunnerChecksums SetPackagedRunnerChecksumsForTesting(PackagedRunnerChecksums checksums);
	std::vector<std::string> BuildRemoteTerminalSshArgv(
	    const std::string& ssh_alias, const std::string& platform,
	    const std::string& version, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::string& runner_directory = {},
	    const std::string& launch_channel_id = {});
	std::string BuildRemoteMcpControlLine(
	    const std::string& channel_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment);
	std::string BuildRemoteInputDeliveryLine(
	    std::string_view delivery_token, std::string_view delivery_id,
	    std::string_view payload);
	nlohmann::json BuildRemoteMcpServer(const std::string& channel, const std::string& platform, const std::string& version, const std::string& directory, int protocol);
	void RunLocalMcpRelayFromSpec(std::stop_token stop, const std::string& alias, const std::string& platform, const std::string& version, const std::string& runner_directory, int protocol, const std::string& encoded_spec);
	int RunProcessProxy(const std::string& ssh_alias, const std::string& platform,
	                    const std::string& version,
	                    const std::string& runner_directory = {},
	                    int protocol_version = kRunnerProtocolVersion);
	int RunTerminalProcess(const std::string& encoded_spec);
	/// <summary>Consumes a private one-use launch before taking over the native terminal.</summary>
	int RunTerminalProcessFromChannel(const std::string& channel_id,
	                                  const std::filesystem::path& socket_path = {});
	int RunRemoteMcpShim(const std::string& channel_id, const std::filesystem::path& socket_path);
	int RunRemoteMcpShim(const std::string& channel_id);
}
