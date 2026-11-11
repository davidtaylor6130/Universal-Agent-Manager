#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <stop_token>
#include <vector>

#include "remote/runner_protocol.h"

namespace uam::remote
{
	inline constexpr const char* kRemoteProcessSpecEnvironment = "UAM_REMOTE_PROCESS_SPEC";
	/// Set for a UAM tool server that replaces an earlier one while its provider keeps running (remote reattach, relay
	/// restart): the provider finished its MCP handshake long ago, so the server records the connection itself.
	inline constexpr const char* kUamControlResumedEnvironment = "UAM_CONTROL_RESUMED";
	/// Provider-side deadline for one UAM tool call, where UAM sets it (Codex, OpenCode). A question or approval card
	/// waits for the user, so this is the default Tool call limit rather than a short transport budget; every other
	/// UAM tool is bounded by the tool server itself.
	inline constexpr int kUamControlToolDeadlineSeconds = 7200;
	/// How long the desktop may stay silent on a UAM tool channel before the host-side shim answers for it.
	inline constexpr std::int64_t kRemoteMcpDesktopSilenceLimitMs = 30000;

	/// <summary>
	/// Host-side gate of the remote UAM tool pipe. It follows which provider calls are waiting on the desktop, so a
	/// call gets a clear error instead of waiting for the provider's own timeout when the desktop is not connected.
	/// </summary>
	/// <remarks>
	/// A question or approval card waits for the user, so a silent desktop (a sleeping laptop) does not fail it; a
	/// replaced desktop tool server does, because the card it showed is gone.
	/// </remarks>
	class RemoteMcpShimFilter
	{
	  public:
		/// Provider bytes. Returns what to send to the desktop; a call refused while the desktop is silent is answered in replies.
		std::string FromProvider(std::string_view bytes, bool desktop_live, std::string& replies);
		/// The calls returned by the last FromProvider reached the channel while `epoch` was the desktop tool server.
		void Sent(const std::string& epoch);
		/// Desktop bytes. Returns what the provider may read: late replies to calls already failed here are dropped.
		std::string FromDesktop(std::string_view bytes);
		/// Applies the latest desktop state, failing calls that can no longer be answered into replies.
		/// Returns true when the caller must fence the channel so those calls cannot reach a desktop later.
		bool Observe(bool desktop_live, const std::string& epoch, std::string& replies);

	  private:
		struct Pending
		{
			nlohmann::json id;
			std::string epoch;
			bool tool_call = false;
			bool card = false;
			bool sent = false;
		};
		void Fail(const std::string& key, std::string& replies);
		std::map<std::string, Pending> m_pending;
		std::deque<std::string> m_failed;
		std::string m_fromProvider;
		std::string m_fromDesktop;
	};
	inline constexpr std::string_view kRemoteStopOutcomePrefix = "UAM_REMOTE_STOP_OUTCOME ";
	/// Last stderr line of an attach whose bridge failed on a reachable host where neither a helper nor a process of that
	/// session is left running. It settles a stop like the helper's own "does not exist".
	inline constexpr std::string_view kRemoteHelperNotRunningLine = "The remote helper is not running on this host.";
	/// Last stderr lines that settle nothing: the host shows a process that may belong to the session, a helper that is
	/// running but did not answer, or a process table that could not be read after the helper answered "does not exist".
	inline constexpr std::string_view kRemoteSessionProcessRunningLine = "A process is still running in this chat's folder on the host.";
	inline constexpr std::string_view kRemoteHelperUnreachableLine = "The remote helper is running but did not answer.";
	inline constexpr std::string_view kRemoteProcessUncheckedLine = "The host could not be checked for a process of this chat.";
	/// The host-side check behind those lines; empty for a platform or input it cannot check. It is read-only on the host.
	/// It prints kRemoteHelperNotRunningToken only after it could read this account's processes and found, with
	/// check_helper, no helper of that version (deleted binary included) and, on
	/// Linux, no process of that session: none carrying its marker and no unmarked one working in its workspace. It prints
	/// UAM_HELPER_RUNNING or UAM_SESSION_PROCESS_RUNNING for what it found and exits 3 when it could not tell.
	/// Without check_helper only the session's processes are looked for (Linux only). proc_root is /proc outside tests.
	inline constexpr std::string_view kRemoteHelperNotRunningToken = "UAM_HELPER_NOT_RUNNING";
	std::string RemoteProcessCheckCommand(const std::string& platform, const std::string& version, const std::string& session_id,
	    const std::string& workspace, bool check_helper = true, const std::string& proc_root = "/proc");
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
