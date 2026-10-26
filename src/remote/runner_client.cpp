#include <fstream>
#include "common/utils/io_utils.h"
#include "common/provider/provider_setup.h"
#include "common/config/build_features.h"
#include "remote/memory_protocol.h"
#include "remote/runner_client.h"
#include "remote/native_history_snapshot.h"

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_protocol.h"
#include "common/usage/usage_store.h"
#include "common/usage/antigravity_accounting.h"
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <ranges>
#include <string>
#include <thread>

namespace uam::remote
{
	namespace
	{
		std::string Nonce()
		{
			std::random_device random;
			return std::to_string(static_cast<unsigned long long>(random())) + "-" +
			       std::to_string(static_cast<unsigned long long>(random()));
		}

		std::string ResponseError(const nlohmann::json& response)
		{
			if (!response.is_object()) return "The remote runner returned an invalid response.";
			if (response.contains("error") && response["error"].is_object())
				return response["error"].value("message", "The remote runner rejected the request.");
			return "The remote runner rejected the request.";
		}

		std::string FrameBytes(const nlohmann::json& message)
		{
			const std::string body = message.dump();
			const std::uint32_t size = static_cast<std::uint32_t>(body.size());
			std::string frame(4, '\0');
			frame[0] = static_cast<char>((size >> 24U) & 0xffU);
			frame[1] = static_cast<char>((size >> 16U) & 0xffU);
			frame[2] = static_cast<char>((size >> 8U) & 0xffU);
			frame[3] = static_cast<char>(size & 0xffU);
			frame += body;
			return frame;
		}

		std::string ChannelCursorKey(std::string_view channel_id, std::string_view direction)
		{
			return std::string(channel_id) + "\n" + std::string(direction);
		}

		/** Validate fields consumed by RunnerClient before any caller updates its cursors. */
		bool ValidResultFields(const nlohmann::json& result)
		{
			if (!result.is_object()) return false;
			for (const char* key : {"dataBase64", "stdoutBase64", "stderrBase64"})
			{
				const auto field = result.find(key);
				if (field != result.end() && !field->is_string()) return false;
			}
			for (const char* key : {"cursor", "stdoutCursor", "stderrCursor", "inputSequence",
			                        "remoteToDesktopCursor", "desktopToRemoteCursor",
			                        "remoteToDesktopWriteSequence", "desktopToRemoteWriteSequence"})
			{
				const auto field = result.find(key);
				if (field != result.end() && !field->is_number_unsigned()) return false;
			}
			const auto running = result.find("running");
			if (running != result.end() && !running->is_boolean()) return false;
			const auto exit_code = result.find("exitCode");
			return exit_code == result.end() || exit_code->is_null() ||
			    (exit_code->is_number_integer() && *exit_code >= std::numeric_limits<int>::min() &&
			     *exit_code <= std::numeric_limits<int>::max());
		}

		// Native OpenCode responses are opaque provider values, not runner transport records.
		// Keep generic object/field validation unchanged for every other request type.
		bool ValidOpenCodeNativeResult(const nlohmann::json& request, const nlohmann::json& result)
		{
			const auto operation = request.find("operation");
			if (operation == request.end() || !operation->is_object()) return false;
			const auto field = operation->find("operation");
			if (field == operation->end() || !field->is_string() || result.dump().size() > 262144) return false;
			const std::string& kind = field->get_ref<const std::string&>();
			if (kind == "permissions" || kind == "messages") return result.is_array();
			if (kind == "reply") return result.is_boolean();
			if (kind == "health" || kind == "configProviders" || kind == "session" || kind == "status" || kind == "enableTask" || kind == "disableTask") return result.is_object();
			return false;
		}
	}

	std::vector<std::string> SshBridgeArgv(const std::string& ssh_alias,
	                                       const std::string& platform,
	                                       const std::string& version,
	                                       const std::string& runner_directory,
	                                       int protocol_version)
	{
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias) || version.empty() ||
		    protocol_version < 2 || protocol_version > kRunnerProtocolVersion ||
		    !uam::execution_hosts::IsSafeRunnerDirectory(runner_directory) ||
		    !std::ranges::all_of(version, [](unsigned char character)
		    { return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.'; }))
			return {};
		std::string command;
		if (platform == "linux" || platform == "macos" || platform == "Linux" ||
		    platform == "Darwin")
		{
			const std::string root = uam::execution_hosts::RunnerDirectory(platform, runner_directory);
			const std::string runner = "~/" + root + "/" + version + "/uam-runner";
			const std::string socket = "~/" + root + "/" +
			                           RunnerEndpointName(version, protocol_version) + ".sock";
			command = runner + " start --socket " + socket + " && exec " +
			          runner + " bridge --socket " + socket;
		}
		else if (platform == "windows" || platform == "Windows")
		{
			const std::string runner = uam::execution_hosts::RunnerDirectory(
			    platform, runner_directory) + "/" + version + "/uam-runner.exe";
			command = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			          "-Command \"& (Join-Path $HOME '" + runner +
			          "') start; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; & "
			          "(Join-Path $HOME '" + runner + "') bridge\"";
		}
		else return {};
		return {"ssh", "-T", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes", "-o",
		        "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o",
		        "ServerAliveCountMax=2", ssh_alias, std::move(command)};
	}

	bool CanAttemptRunnerConnection(const ExecutionHost& host)
	{
		return UAM_ENABLE_SSH && uam::execution_hosts::CanConnectRunner(host) &&
		       !SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version,
		                      host.runner_directory, host.runner_protocol_version).empty();
	}

	bool ValidateRemoteWorkspaceDirectory(IPlatformProcessService& service, const ExecutionHost& host,
	    const std::filesystem::path& directory, std::string* error_out)
	{
		RunnerClient client(service, host);
		DirectoryListing listing;
		return client.ListDirectories(directory, listing, error_out);
	}

	RunnerClient::RunnerClient(IPlatformProcessService& process_service, const ExecutionHost& host)
	    : RunnerClient(process_service, SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version,
	          host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version)
	{
		m_connectionAllowed = CanAttemptRunnerConnection(host);
	}

	RunnerClient::RunnerClient(IPlatformProcessService& process_service,
	                           std::vector<std::string> bridge_argv,
	                           std::string expected_version,
	                           int expected_protocol_version)
	    : m_processService(process_service), m_bridgeArgv(std::move(bridge_argv)),
	      m_expectedVersion(std::move(expected_version)),
	      m_expectedProtocolVersion(expected_protocol_version)
	{
	}

	RunnerClient::~RunnerClient()
	{
		Disconnect();
	}

	bool RunnerClient::Connect(std::string* error_out, std::stop_token stop_token, const std::function<bool()>& interrupt)
	try
	{
		if (stop_token.stop_requested()) return false;
		if (!UAM_ENABLE_SSH) { if (error_out) *error_out = "Remote execution is not included in this build."; return false; }
		if (!m_connectionAllowed) { if (error_out) *error_out = "Retry helper setup to reconcile the unfinished or unavailable installation first."; return false; }
		if (m_connected) return true;
		if (m_bridgeArgv.empty())
		{
			if (error_out != nullptr) *error_out = "The remote runner bridge command is empty.";
			return false;
		}
		std::string error;
		std::error_code current_path_error;
		const std::optional<std::filesystem::path> current_path =
		    uam::paths::CurrentPathNoThrow(&current_path_error);
		if (!current_path)
		{
			if (error_out != nullptr)
				*error_out = "The remote runner bridge working directory is unavailable.";
			return false;
		}
		if (!m_processService.StartStdioProcess(m_bridge, *current_path, m_bridgeArgv, &error))
		{
			if (error_out != nullptr)
				*error_out = error.empty() ? "The remote runner bridge could not start." : error;
			return false;
		}
		m_connected = true;
		const std::string nonce = Nonce();
		nlohmann::json response;
		if (!Request({{"type", "hello"}, {"protocolVersion", m_expectedProtocolVersion},
		              {"nonce", nonce}},
		             response, &error, [&] { return stop_token.stop_requested() || (interrupt && interrupt()); }) || response.value("nonce", "") != nonce ||
		    response.value("protocolVersion", 0) != m_expectedProtocolVersion ||
		    (!m_expectedVersion.empty() &&
		     response.value("runnerVersion", "") != m_expectedVersion) ||
		    !response.value("capabilities", nlohmann::json::object())
		         .value("processExecution", false) ||
		    !response.value("capabilities", nlohmann::json::object())
		         .value("fileCopy", false) ||
		    response.value("capabilities", nlohmann::json::object())
		        .value("computerUse", true))
		{
			Disconnect();
			if (error_out != nullptr)
				*error_out = error.empty() ? "The remote runner handshake is incompatible." : error;
			return false;
		}
		m_directoryBrowsing = response["capabilities"].value("directoryBrowsing", false);
		m_safePathInspection = m_expectedProtocolVersion >= 3 && response["capabilities"].value("safePathInspection", false);
		m_contextRead = response["capabilities"].value("contextRead", false);
		m_providerHistoryRead = response["capabilities"].value("providerHistoryRead", false);
		m_workspaceFileRead = response["capabilities"].value("workspaceFileRead", false);
		m_workspaceAttachmentRead = response["capabilities"].value("workspaceAttachmentRead", false);
		m_openCodeNativeControl = response["capabilities"].value("openCodeNativeControl", false);
		m_openCodeTerminalObservation = response["capabilities"].value("openCodeTerminalObservation", false);
		m_usageAccounting = response["capabilities"].value("usageAccounting", false);
		m_usageAccountingStream = response["capabilities"].value("usageAccountingStream", false);
		m_projectMemory = response["capabilities"].value("projectMemory", false);
		m_isolatedTextWorkers = response["capabilities"].value("isolatedTextWorkers", false);
		m_runnerStartup = response["capabilities"].value("runnerStartup", false);
		m_runnerStartupObservation = response["capabilities"].value("runnerStartupObservation", false);
		m_leasedChannelTake = response["capabilities"].value("leasedChannelTake", false);
		m_providerNativeContext = response["capabilities"].value("providerNativeContext", false);
		m_centralProviderConfiguration = response["capabilities"].value("centralProviderConfiguration", false);
		m_geminiThinkingConfiguration = response["capabilities"].value("geminiThinkingConfiguration", false);
		m_claudeTerminalFastConfiguration = response["capabilities"].value("claudeTerminalFastConfiguration", false);
		m_processOutputAcknowledgement = m_expectedProtocolVersion >= 3 &&
		    response["capabilities"].value("processOutputAcknowledgement", false);
		return true;
	}

	catch (const nlohmann::json::exception&)
	{
		Disconnect();
		if (error_out != nullptr) *error_out = "The remote runner returned malformed handshake fields.";
		return false;
	}

	bool RunnerClient::Request(nlohmann::json request, nlohmann::json& response,
	                           std::string* error_out,
	                           const std::function<bool()>& interrupt,
	                           bool* interrupted_out)
	try
	{
		if (interrupted_out != nullptr) *interrupted_out = false;
		if (!m_connected)
		{
			if (error_out != nullptr) *error_out = "The remote runner is disconnected.";
			return false;
		}
		request["id"] = std::to_string(m_nextRequestId++);
		const std::string request_id = request["id"].get<std::string>();
		const std::string frame = FrameBytes(request);
		if (frame.size() - 4 > kMaxRunnerFrameBytes)
		{
			if (error_out != nullptr) *error_out = "The remote runner request is too large.";
			return false;
		}
		if (interrupt && interrupt())
		{
			if (interrupted_out != nullptr) *interrupted_out = true;
			if (error_out != nullptr) *error_out = "The remote runner request was canceled.";
			return false;
		}
		std::string error;
		if (!m_processService.WriteToStdioProcess(m_bridge, frame.data(), frame.size(), &error) ||
		    !ReadResponse(response, &error, interrupt, interrupted_out))
		{
			if (error_out != nullptr)
				*error_out = error.empty() ? "The remote runner request failed." : error;
			Disconnect();
			return false;
		}
		if (response.value("id", "") != request_id)
		{
			if (error_out != nullptr) *error_out = "The remote runner response id does not match the request.";
			Disconnect();
			return false;
		}
		if (!response.value("ok", false))
		{
			if (error_out != nullptr) *error_out = ResponseError(response);
			return false;
		}
		if (request.value("type", "") != "hello" &&
		    (!response.contains("result") || !(request.value("type", "") == "opencode.native"
		        ? ValidOpenCodeNativeResult(request, response["result"])
		        : ValidResultFields(response["result"]))))
		{
			Disconnect();
			if (error_out != nullptr) *error_out = "The remote runner returned malformed result fields.";
			return false;
		}
		return true;
	}

	catch (const nlohmann::json::exception&)
	{
		Disconnect();
		if (error_out != nullptr) *error_out = "The remote runner returned malformed response fields.";
		return false;
	}

	bool RunnerClient::ReadResponse(nlohmann::json& response, std::string* error_out,
	                                const std::function<bool()>& interrupt,
	                                bool* interrupted_out)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		std::array<char, 16 * 1024> buffer{};
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (interrupt && interrupt())
			{
				if (interrupted_out != nullptr) *interrupted_out = true;
				if (error_out != nullptr) *error_out = "The remote runner request was interrupted.";
				return false;
			}
			if (m_received.size() >= 4)
			{
				const auto* header = reinterpret_cast<const unsigned char*>(m_received.data());
				const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
				                           (static_cast<std::uint32_t>(header[1]) << 16U) |
				                           (static_cast<std::uint32_t>(header[2]) << 8U) |
				                           static_cast<std::uint32_t>(header[3]);
				if (size == 0 || size > kMaxRunnerFrameBytes)
				{
					if (error_out != nullptr) *error_out = "The remote runner returned an invalid frame size.";
					return false;
				}
				if (m_received.size() >= 4 + size)
				{
					response = nlohmann::json::parse(m_received.data() + 4,
					                                 m_received.data() + 4 + size, nullptr, false);
					m_received.erase(0, 4 + size);
					if (!response.is_object())
					{
						if (error_out != nullptr) *error_out = "The remote runner returned invalid JSON.";
						return false;
					}
					return true;
				}
			}

			std::string read_error;
			const std::ptrdiff_t read = m_processService.ReadStdioProcessStdout(
			    m_bridge, buffer.data(), buffer.size(), &read_error);
			if (read > 0)
			{
				m_received.append(buffer.data(), static_cast<std::size_t>(read));
				continue;
			}
			if (read == 0 || read == -1)
			{
				if (error_out != nullptr)
					*error_out = read_error.empty() ? "The remote runner bridge closed." : read_error;
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (error_out != nullptr) *error_out = "The remote runner did not respond within 15 seconds.";
		return false;
	}

	bool RunnerClient::InstallProviderConfiguration(const std::filesystem::path& source, const std::filesystem::path& target, std::string* error_out, std::stop_token stop_token)
	{
		if (!Connect(error_out, stop_token)) return false;
		std::string error;
		if (!m_centralProviderConfiguration)
		{
			if (error_out) *error_out = "Update the SSH helper before using central provider configuration.";
			return false;
		}
		std::string bytes;
		if (!uam::provider_setup::ReadFile(source / "owner.json", bytes, error)) { if (error_out) *error_out = error; return false; }
		const nlohmann::json manifest = nlohmann::json::parse(bytes, nullptr, false);
		if (!uam::provider_setup::ValidateManifest(manifest, error)) { if (error_out) *error_out = error; return false; }
		if ((manifest.contains("geminiThinking") && !m_geminiThinkingConfiguration) ||
		    (manifest.contains("claudeTerminalFastMode") && !m_claudeTerminalFastConfiguration))
		{
			if (error_out) *error_out = "Update the SSH helper before using the required Gemini thinking or Claude terminal speed override.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "configuration.prepare"}, {"directory", uam::paths::Utf8PathString(target)}, {"manifest", manifest}}, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		const nlohmann::json missing = response.value("result", nlohmann::json::object()).value("missing", nlohmann::json());
		if (!missing.is_array()) { if (error_out) *error_out = "SSH helper returned an invalid resource list."; return false; }
		if (missing.empty()) return true;
		for (const nlohmann::json& entry : missing)
		{
			if (!entry.is_string() || !manifest["files"].contains(entry.get<std::string>())) { if (error_out) *error_out = "SSH helper requested an unknown resource."; return false; }
			const std::string name = entry.get<std::string>();
			if (!uam::provider_setup::ReadFile(source / uam::paths::PathFromUtf8(name), bytes, error)) { if (error_out) *error_out = error; return false; }
			if (!UploadFile("configuration-" + Nonce(), target / uam::paths::PathFromUtf8(name), bytes, error_out, stop_token)) return false;
		}
		if (!Request({{"type", "configuration.prepare"}, {"directory", uam::paths::Utf8PathString(target)}, {"manifest", manifest}}, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		const nlohmann::json verified = response.value("result", nlohmann::json::object()).value("missing", nlohmann::json());
		if (!verified.is_array() || !verified.empty())
		{
			if (error_out) *error_out = "SSH helper could not verify the installed resources.";
			return false;
		}
		return true;
	}

	bool RunnerClient::StartProcess(
	    const std::string& session_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment,
	    std::string* error_out, bool attach_if_exists, std::string control_token,
	    bool retry_lost_reply, const std::string& configuration_provider, const std::filesystem::path& configuration_directory, const nlohmann::json& session_mcp_servers, bool* configuration_rejected_out)
	{
		if (configuration_rejected_out != nullptr) *configuration_rejected_out = false;
		if (!Connect(error_out)) return false;
		const bool transient_lease = control_token.empty();
		if (transient_lease)
		{
			const auto existing = m_processControlTokens.find(session_id);
			control_token = existing == m_processControlTokens.end() ? Nonce() : existing->second;
		}
		nlohmann::json environment_json = nlohmann::json::object();
		for (const auto& [name, value] : environment) environment_json[name] = value;
		m_processControlTokens[session_id] = control_token;
		nlohmann::json request = {{"type", "process.start"}, {"sessionId", session_id},
		                          {"cwd", uam::paths::Utf8PathString(working_directory)}, {"argv", argv},
		                          {"environment", std::move(environment_json)},
		                          {"attachIfExists", attach_if_exists}};
		if (!configuration_directory.empty())
		{
			request["sessionMcpServers"] = session_mcp_servers;
			request["configurationProvider"] = configuration_provider;
			request["configurationDirectory"] = uam::paths::Utf8PathString(configuration_directory);
		}
		if (m_expectedProtocolVersion >= 3) request["controlToken"] = control_token;
		if (m_expectedProtocolVersion >= 3 && transient_lease)
			request["transientLeaseMs"] = 60000;
		nlohmann::json response;
		bool started = Request(request, response, error_out);
		if (!started && retry_lost_reply && !m_connected && Connect(error_out))
		{
			request["attachIfExists"] = true;
			started = Request(std::move(request), response, error_out);
		}
		// Configuration rejection alone cannot rule out a provider surviving a lost start reply.
		if (!started && m_connected && configuration_rejected_out != nullptr &&
		    !configuration_directory.empty() && response.contains("error") && response["error"].is_object())
		{
			const nlohmann::json::const_iterator code = response["error"].find("code");
			if (code != response["error"].end() && code->is_string() &&
			    (*code == "invalid_request" || *code == "configuration_failed"))
			{
				nlohmann::json existence;
				std::string probe_error;
				const bool exists = Request({{"type", "process.poll"}, {"sessionId", session_id},
				    {"controlToken", control_token}, {"acknowledgedOutput", true}}, existence, &probe_error);
				if (!exists && m_connected && existence.contains("error") && existence["error"].is_object())
				{
					const nlohmann::json::const_iterator missing = existence["error"].find("code");
					*configuration_rejected_out = missing != existence["error"].end() && missing->is_string() && *missing == "session_not_found";
				}
			}
		}
		if (started)
			m_processInputSequences[session_id] = response.value(
			    "result", nlohmann::json::object()).value("inputSequence", std::uint64_t{0});
		return started;
	}

	ProcessExecutionResult RunnerClient::ExecuteCommand(const std::string& session_id,
	    const std::filesystem::path& working_directory, const std::vector<std::string>& argv,
	    int timeout_ms, std::stop_token stop_token, std::string_view standard_input,
	    std::string retained_control_token, bool* start_request_attempted_out)
	{
		if (start_request_attempted_out) *start_request_attempted_out = false;
		ProcessExecutionResult result;
		const bool retained = !retained_control_token.empty();
		const std::chrono::steady_clock::time_point deadline = timeout_ms < 0
		    ? std::chrono::steady_clock::time_point::max()
		    : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		const auto interrupted = [&]()
		{
			result.canceled = stop_token.stop_requested();
			result.timed_out = !result.canceled && std::chrono::steady_clock::now() >= deadline;
			return result.canceled || result.timed_out;
		};
		if (interrupted()) return result;
		if (m_expectedProtocolVersion < 3)
		{
			result.error = "Remote command execution requires runner protocol 3 or newer.";
			return result;
		}
		if (!Connect(&result.error)) return result;
		if (!m_processOutputAcknowledgement)
		{
			result.error = "Remote command execution requires acknowledged process output.";
			return result;
		}
		if (interrupted()) return result;
		ProcessPollResult poll;
		bool have_poll = false;
		if (start_request_attempted_out) *start_request_attempted_out = true;
		bool started = StartProcess(session_id, working_directory, argv, {}, &result.error,
		                            false, std::move(retained_control_token), false);
		// An uncertain start is recovered only by observing its original ID and token.
		if (!started && !m_connected && !interrupted() && Connect(&result.error))
		{
			have_poll = PollProcess(session_id, poll, &result.error, interrupted);
			started = have_poll;
		}
		bool exited = false;
		if (started)
		{
			result.error.clear();
			bool input_written = true;
			for (std::size_t offset = 0; offset < standard_input.size() && input_written; offset += 32768)
			{
				const std::string delivery = session_id + ":stdin:" + std::to_string(offset);
				const std::string_view chunk = standard_input.substr(offset, 32768);
				input_written = !interrupted() && WriteProcess(session_id, chunk, &result.error, delivery);
				if (!input_written && !m_connected && !interrupted() && Connect(&result.error, stop_token))
					input_written = WriteProcess(session_id, chunk, &result.error, delivery);
			}
			if (input_written && !interrupted() && CloseProcessInput(session_id, &result.error))
			{
				while (!interrupted())
				{
					if (!have_poll && !PollProcess(session_id, poll, &result.error, interrupted))
					{
						if (m_connected || interrupted() || !Connect(&result.error) ||
						    !PollProcess(session_id, poll, &result.error, interrupted)) break;
					}
					have_poll = false;
					result.error.clear();
					if (!uam::platform::AppendCapturedCommandOutput(result,
					        poll.standard_output.data(), poll.standard_output.size()) ||
					    !uam::platform::AppendCapturedCommandOutput(result,
					        poll.standard_error.data(), poll.standard_error.size())) break;
					if (!AcknowledgeProcessOutput(session_id, poll, &result.error)) break;
					if (!poll.running)
					{
						result.exit_code = poll.exit_code;
						exited = true;
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}
		}
		if (result.canceled) result.error = "Remote command canceled.";
		else if (result.timed_out) result.error = "Remote command timed out.";
		else if (result.output_truncated)
			result.error = std::string(uam::platform::kCapturedCommandOutputLimitError);
		// Retained mutation records outlive interruption and are removed only after the caller saves settlement.
		if (!retained)
		{
			std::string cleanup_error;
			if (!exited && !StopProcess(session_id, &cleanup_error))
				result.error += " Remote process stop failed: " + cleanup_error;
			if (!RemoveProcess(session_id, &cleanup_error))
				result.error += " Remote process cleanup failed: " + cleanup_error;
		}
		result.ok = exited && result.exit_code == 0 && result.error.empty() &&
		    !result.canceled && !result.timed_out && !result.output_truncated;
		return result;
	}

	void RunnerClient::SetProcessControlToken(const std::string& session_id,
	                                          std::string control_token)
	{
		m_processControlTokens[session_id] = std::move(control_token);
	}

	bool RunnerClient::AddProcessControlToken(nlohmann::json& request,
	                                          const std::string& session_id,
	                                          std::string* error_out)
	{
		if (m_expectedProtocolVersion < 3) return true;
		const auto found = m_processControlTokens.find(session_id);
		if (found == m_processControlTokens.end() || found->second.empty())
		{
			if (error_out != nullptr) *error_out = "The remote process control token is unavailable.";
			return false;
		}
		request["controlToken"] = found->second;
		return true;
	}

	bool RunnerClient::WriteProcess(const std::string& session_id, std::string_view bytes,
	                                std::string* error_out, std::string_view delivery_id)
	{
		nlohmann::json request = {{"type", "process.write"}, {"sessionId", session_id},
		                          {"dataBase64", uam::base64::Encode(bytes)}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		const std::uint64_t sequence = m_processInputSequences[session_id] + 1;
		if (m_expectedProtocolVersion >= 3) request["inputSequence"] = sequence;
		if (m_expectedProtocolVersion >= 3 && !delivery_id.empty())
			request["deliveryId"] = delivery_id;
		nlohmann::json response;
		bool written = Request(request, response, error_out);
		if (!written && m_expectedProtocolVersion >= 3 && !m_connected && Connect(error_out))
			written = Request(std::move(request), response, error_out);
		if (written)
		{
			const std::uint64_t accepted_sequence = response.value(
			    "result", nlohmann::json::object()).value("inputSequence", sequence);
			m_processInputSequences[session_id] = std::max(
			    m_processInputSequences[session_id], accepted_sequence);
		}
		return written;
	}

	bool RunnerClient::OpenChannel(const std::string& channel_id, std::string* error_out,
	                               bool attach_if_exists, std::int64_t lease_ms, std::stop_token stop_token)
	{
		if (!Connect(error_out, stop_token)) return false;
		if (lease_ms != 0 && !m_leasedChannelTake)
		{
			if (error_out != nullptr) *error_out = "The remote runner does not support leased channel handoff.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "channel.open"}, {"channelId", channel_id},
		              {"attachIfExists", attach_if_exists}, {"leaseMs", lease_ms}}, response, error_out, [stop_token] { return stop_token.stop_requested(); })) return false;
		if (m_expectedProtocolVersion >= 3)
		{
			const nlohmann::json& result = response["result"];
			m_channelCursors[ChannelCursorKey(channel_id, "remoteToDesktop")] =
			    result.value("remoteToDesktopCursor", static_cast<std::uintmax_t>(0));
			m_channelCursors[ChannelCursorKey(channel_id, "desktopToRemote")] =
			    result.value("desktopToRemoteCursor", static_cast<std::uintmax_t>(0));
			m_channelWriteSequences[ChannelCursorKey(channel_id, "remoteToDesktop")] =
			    result.value("remoteToDesktopWriteSequence", std::uint64_t{0});
			m_channelWriteSequences[ChannelCursorKey(channel_id, "desktopToRemote")] =
			    result.value("desktopToRemoteWriteSequence", std::uint64_t{0});
		}
		return true;
	}

	bool RunnerClient::TakeChannel(const std::string& channel_id, std::string_view direction,
	                               std::string& bytes, std::string* error_out)
	{
		bytes.clear();
		if (!Connect(error_out)) return false;
		if (!m_leasedChannelTake)
		{
			if (error_out != nullptr) *error_out = "The remote runner does not support leased channel handoff.";
			return false;
		}
		nlohmann::json response;
		// Consumption is destructive: an uncertain response must never be retried.
		if (!Request({{"type", "channel.take"}, {"channelId", channel_id},
		              {"direction", direction}}, response, error_out)) return false;
		const nlohmann::json& result = response["result"];
		if (!result.contains("dataBase64") || !result["dataBase64"].is_string() ||
		    result["dataBase64"].get_ref<const std::string&>().size() > 87384 ||
		    !uam::base64::Decode(result["dataBase64"].get_ref<const std::string&>(), bytes) || bytes.size() > 65536)
		{
			bytes.clear();
			if (error_out != nullptr) *error_out = "The remote runner returned invalid channel handoff bytes.";
			return false;
		}
		return true;
	}

	bool RunnerClient::WriteChannel(const std::string& channel_id,
	                                std::string_view direction, std::string_view bytes,
	                                std::string* error_out, std::stop_token stop_token)
	{
		const std::string sequence_key = ChannelCursorKey(channel_id, direction);
		nlohmann::json request = {{"type", "channel.write"}, {"channelId", channel_id},
		                          {"direction", direction},
		                          {"dataBase64", uam::base64::Encode(bytes)}};
		const std::uint64_t sequence = m_channelWriteSequences[sequence_key] + 1;
		if (m_expectedProtocolVersion >= 3) request["writeSequence"] = sequence;
		nlohmann::json response;
		bool written = Request(request, response, error_out, [stop_token] { return stop_token.stop_requested(); });
		if (!written && m_expectedProtocolVersion >= 3 && !m_connected && Connect(error_out, stop_token))
			written = Request(std::move(request), response, error_out, [stop_token] { return stop_token.stop_requested(); });
		if (written) m_channelWriteSequences[sequence_key] = sequence;
		return written;
	}

	bool RunnerClient::PollChannel(const std::string& channel_id,
	                               std::string_view direction, std::string& bytes,
	                               std::string* error_out, std::uintmax_t* cursor_out)
	{
		const std::string cursor_key = ChannelCursorKey(channel_id, direction);
		nlohmann::json request = {{"type", "channel.poll"}, {"channelId", channel_id},
		                          {"direction", direction}};
		if (m_expectedProtocolVersion >= 3)
		{
			request["acknowledgedOutput"] = true;
			request["cursor"] = m_channelCursors[cursor_key];
		}
		nlohmann::json response;
		if (!Request(std::move(request), response, error_out)) return false;
		std::string parsed;
		if (!response.contains("result") || !response["result"].is_object() ||
		    !uam::base64::Decode(response["result"].value("dataBase64", ""), parsed))
		{
			if (error_out != nullptr) *error_out = "The runner channel returned invalid data.";
			return false;
		}
		const std::uintmax_t cursor = response["result"].value(
		    "cursor", m_channelCursors[cursor_key] + parsed.size());
		if (cursor_out != nullptr) *cursor_out = cursor;
		bytes = std::move(parsed);
		return true;
	}

	bool RunnerClient::AcknowledgeChannel(const std::string& channel_id,
	                                      std::string_view direction,
	                                      std::uintmax_t cursor,
	                                      std::string* error_out)
	{
		const std::string cursor_key = ChannelCursorKey(channel_id, direction);
		if (m_expectedProtocolVersion < 3)
		{
			m_channelCursors[cursor_key] = cursor;
			return true;
		}
		nlohmann::json request = {{"type", "channel.ack"}, {"channelId", channel_id},
		                          {"direction", direction}, {"cursor", cursor}};
		nlohmann::json response;
		bool acknowledged = Request(request, response, error_out);
		if (!acknowledged && !m_connected && Connect(error_out))
			acknowledged = Request(std::move(request), response, error_out);
		if (!acknowledged) return false;
		m_channelCursors[cursor_key] = cursor;
		return true;
	}

	bool RunnerClient::CloseChannel(const std::string& channel_id, std::string* error_out, std::stop_token stop_token)
	{
		nlohmann::json response;
		return Request({{"type", "channel.close"}, {"channelId", channel_id}}, response,
		               error_out, [stop_token] { return stop_token.stop_requested(); });
	}

	bool RunnerClient::UploadFile(const std::string& upload_id,
	                              const std::filesystem::path& remote_path,
	                              std::string_view bytes, std::string* error_out, std::stop_token stop_token)
	{
		if (!Connect(error_out, stop_token)) return false;
		const auto abort = [&]
		{
			if (stop_token.stop_requested()) return;
			nlohmann::json ignored;
			if (!m_connected) (void)Connect(nullptr, stop_token);
			if (m_connected)
				(void)Request({{"type", "file.abort"}, {"uploadId", upload_id}}, ignored,
				              nullptr, [stop_token] { return stop_token.stop_requested(); });
		};
		std::uint64_t digest = uam::hashing::kFnv1a64OffsetBasis;
		uam::hashing::UpdateFnv1a64(
		    digest, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
		nlohmann::json response;
		if (!Request({{"type", "file.begin"}, {"uploadId", upload_id},
		              {"path", uam::paths::Utf8PathString(remote_path)}, {"size", bytes.size()},
		              {"digest", uam::hashing::Hex64Padded(digest)}},
		             response, error_out, [stop_token] { return stop_token.stop_requested(); }))
		{
			abort();
			return false;
		}
		for (std::size_t offset = 0; offset < bytes.size(); offset += 256 * 1024)
		{
			const std::string_view chunk = bytes.substr(offset, 256 * 1024);
			if (!Request({{"type", "file.write"}, {"uploadId", upload_id},
			              {"dataBase64", uam::base64::Encode(chunk)}}, response, error_out, [stop_token] { return stop_token.stop_requested(); }))
			{
				abort();
				return false;
			}
		}
		if (!Request({{"type", "file.commit"}, {"uploadId", upload_id}}, response,
		             error_out, [stop_token] { return stop_token.stop_requested(); }))
		{
			abort();
			return false;
		}
		return true;
	}

	bool RunnerClient::PrepareProviderContext(const std::filesystem::path& directory, std::string* error_out, std::stop_token stop_token)
	{
		nlohmann::json response;
		return Request({{"type", "context.prepare"}, {"directory", uam::paths::Utf8PathString(directory)}}, response, error_out, [stop_token] { return stop_token.stop_requested(); });
	}

	bool RunnerClient::RemoveProviderContext(const std::filesystem::path& directory, std::string* error_out, std::stop_token stop_token)
	{
		nlohmann::json response;
		return Request({{"type", "context.remove"}, {"directory", uam::paths::Utf8PathString(directory)}}, response, error_out, [stop_token] { return stop_token.stop_requested(); });
	}

	bool RunnerClient::RemoveFile(const std::string& request_id,
	                              const std::filesystem::path& remote_path,
	                              std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "file.remove"}, {"uploadId", request_id},
		                {"path", uam::paths::Utf8PathString(remote_path)}}, response, error_out);
	}

	bool RunnerClient::CopyFile(const std::string& request_id,
	                            const std::filesystem::path& source_path,
	                            const std::filesystem::path& target_path,
	                            const bool overwrite, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "file.copy"}, {"uploadId", request_id},
		                {"sourcePath", uam::paths::Utf8PathString(source_path)},
		                {"targetPath", uam::paths::Utf8PathString(target_path)}, {"overwrite", overwrite}},
		               response, error_out);
	}

	bool RunnerClient::ConfigureStartup(bool enabled, std::string* error_out, std::stop_token stop_token)
	{
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_runnerStartup || !m_runnerStartupObservation)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to confirm login startup.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "startup.configure"}, {"enabled", enabled}}, response, error_out,
		    [stop_token]() { return stop_token.stop_requested(); })) return false;
		const nlohmann::json result = response.value("result", nlohmann::json::object());
		if (!result.is_object() || !result.contains("status") || !result["status"].is_string() ||
		    !result.contains("enabled") || !result["enabled"].is_boolean() ||
		    result["enabled"].get<bool>() != enabled || result["status"].get<std::string>() != (enabled ? "enabled" : "disabled"))
		{
			if (error_out != nullptr) *error_out = "The runner startup registration could not be confirmed. Update the host runner and try again.";
			return false;
		}
		return true;
	}

	bool RunnerClient::ObserveStartup(std::string& status, std::string* error_out, std::stop_token stop_token)
	{
		status.clear();
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_runnerStartupObservation)
		{
			if (error_out != nullptr) *error_out = "Update this host's runner to observe login startup.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "startup.observe"}}, response, error_out,
		    [stop_token]() { return stop_token.stop_requested(); })) return false;
		const nlohmann::json result = response.value("result", nlohmann::json::object());
		if (!result.is_object() || !result.contains("status") || !result["status"].is_string() ||
		    !result.contains("enabled") || !result["enabled"].is_boolean())
		{
			if (error_out != nullptr) *error_out = "The runner returned an invalid startup observation.";
			return false;
		}
		const std::string observed = result["status"].get<std::string>();
		if ((observed != "enabled" && observed != "disabled") || result["enabled"].get<bool>() != (observed == "enabled"))
		{
			if (error_out != nullptr) *error_out = "The runner returned an invalid startup observation.";
			return false;
		}
		status = observed;
		return true;
	}

	bool RunnerClient::PrepareTextWorker(std::string_view id, std::string_view provider, std::filesystem::path& directory, std::string* error_out, std::stop_token stop_token)
	{
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_isolatedTextWorkers) { if (error_out != nullptr) *error_out = "Update this host's runner to run isolated memory workers."; return false; }
		nlohmann::json response;
		if (!Request({{"type", "worker.prepare"}, {"workerId", id}, {"providerId", provider}}, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		if (!response["result"].contains("directory") || !response["result"]["directory"].is_string()) { if (error_out != nullptr) *error_out = "The runner returned an invalid text worker directory."; return false; }
		directory = paths::PathFromUtf8(response["result"]["directory"].get<std::string>());
		return uam::execution_hosts::IsAbsoluteRemotePath("linux", paths::Utf8PathString(directory)) || uam::execution_hosts::IsAbsoluteRemotePath("windows", paths::Utf8PathString(directory));
	}
	bool RunnerClient::RemoveTextWorker(std::string_view id, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "worker.remove"}, {"workerId", id}}, response, error_out);
	}

	bool RunnerClient::ListMemoryEntries(const std::filesystem::path& workspace, std::vector<MemoryLibraryStore::Entry>& entries, std::string* error_out)
	{
		entries.clear();
		if (!m_connected && !Connect(error_out)) return false;
		if (!m_projectMemory) { if (error_out != nullptr) *error_out = "Update this host's runner to manage project memory."; return false; }
		nlohmann::json response;
		if (!Request({{"type", "memory.list"}, {"workspace", paths::Utf8PathString(workspace)}}, response, error_out)) return false;
		const nlohmann::json& result = response["result"];
		if (!result.is_object() || !result.contains("entries") || !result["entries"].is_array() || result["entries"].size() > 200) { if (error_out != nullptr) *error_out = "The runner returned invalid project memory."; return false; }
		for (const nlohmann::json& value : result["entries"])
		{
			MemoryLibraryStore::Entry entry;
			if (!DecodeMemoryEntry(value, entry)) { if (error_out != nullptr) *error_out = "The runner returned an invalid memory entry."; entries.clear(); return false; }
			entries.push_back(std::move(entry));
		}
		return true;
	}

	bool RunnerClient::CreateMemoryEntry(const std::filesystem::path& workspace, const MemoryLibraryStore::Draft& draft, MemoryLibraryStore::Entry& created, std::string* error_out, std::stop_token stop_token)
	{
		if (stop_token.stop_requested()) { if (error_out != nullptr) *error_out = "Memory scan canceled."; return false; }
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_projectMemory) { if (error_out != nullptr) *error_out = "Update this host's runner to manage project memory."; return false; }
		nlohmann::json response;
		const nlohmann::json value = {{"category", draft.category}, {"title", draft.title}, {"memory", draft.memory}, {"evidence", draft.evidence}, {"confidence", draft.confidence}, {"sourceChatId", draft.source_chat_id}, {"extractionKey", draft.extraction_key}};
		if (!Request({{"type", "memory.create"}, {"workspace", paths::Utf8PathString(workspace)}, {"draft", value}}, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		if (!response["result"].contains("entry") || !DecodeMemoryEntry(response["result"]["entry"], created)) { if (error_out != nullptr) *error_out = "The runner returned an invalid created memory entry."; return false; }
		return true;
	}

	bool RunnerClient::DeleteMemoryEntry(const std::filesystem::path& workspace, std::string_view id, std::string* error_out)
	{
		if (!m_connected && !Connect(error_out)) return false;
		if (!m_projectMemory) { if (error_out != nullptr) *error_out = "Update this host's runner to manage project memory."; return false; }
		nlohmann::json response;
		return Request({{"type", "memory.delete"}, {"workspace", paths::Utf8PathString(workspace)}, {"entryId", id}}, response, error_out);
	}

	bool RunnerClient::ReadProjectMemory(const std::filesystem::path& workspace, int budget, std::string& text,
	    std::string* error_out, std::stop_token stop_token)
	{
		text.clear();
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_contextRead)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to read remote project memory.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "context.memory"}, {"workspace", uam::paths::Utf8PathString(workspace)}, {"budget", budget}, {"detailReferences", true}},
		    response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		if (!response.contains("result") || !response["result"].is_object() || !response["result"].contains("text") || !response["result"]["text"].is_string() || response["result"]["text"].get_ref<const std::string&>().size() > 65536)
		{
			if (error_out != nullptr) *error_out = "The remote runner returned invalid memory text.";
			return false;
		}
		text = response["result"]["text"].get<std::string>();
		return true;
	}

	bool RunnerClient::ReadWorkspaceFile(const std::string& workspace, const std::string& path, nlohmann::json& file,
	    std::string* error_out, std::stop_token stop_token)
	{
		file = nullptr;
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_workspaceFileRead)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to inspect workspace files.";
			return false;
		}
		std::string text;
		std::string relative_path;
		std::string fingerprint;
		std::size_t total = 0;
		do
		{
			nlohmann::json request = {{"type", "workspace.file.read"}, {"workspace", workspace}, {"path", path}, {"offset", text.size()}};
			if (!fingerprint.empty()) request["fingerprint"] = fingerprint;
			nlohmann::json response;
			if (!Request(request, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
			const nlohmann::json result = response.value("result", nlohmann::json{});
			std::string chunk;
			if (!result.is_object() || !result.contains("totalBytes") || !result["totalBytes"].is_number_integer() ||
			    result["totalBytes"].get<std::int64_t>() < 0 || result["totalBytes"].get<std::int64_t>() > 1024 * 1024 ||
			    !result.contains("offset") || !result["offset"].is_number_integer() || result["offset"] != text.size() ||
			    !result.contains("path") || !result["path"].is_string() || result["path"].get_ref<const std::string&>().empty() ||
			    result["path"].get_ref<const std::string&>().size() > 16384 ||
			    !result.contains("fingerprint") || !result["fingerprint"].is_string() || result["fingerprint"].get_ref<const std::string&>().size() != 16 ||
			    !result.contains("dataBase64") || !result["dataBase64"].is_string() ||
			    result["dataBase64"].get_ref<const std::string&>().size() > 699052 ||
			    !uam::base64::Decode(result["dataBase64"].get_ref<const std::string&>(), chunk))
			{
				if (error_out != nullptr) *error_out = "The runner returned an invalid file preview.";
				return false;
			}
			const std::size_t next_total = result["totalBytes"].get<std::size_t>();
			if (text.size() > next_total || chunk.size() != std::min<std::size_t>(512 * 1024, next_total - text.size()) ||
			    (!fingerprint.empty() && (result["fingerprint"] != fingerprint || result["path"] != relative_path || next_total != total)))
			{
				if (error_out != nullptr) *error_out = "The file changed while loading. Try again.";
				return false;
			}
			total = next_total;
			relative_path = result["path"].get<std::string>();
			fingerprint = result["fingerprint"].get<std::string>();
			text += chunk;
		} while (text.size() < total);
		if (uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(text)) != fingerprint || text.find('\0') != std::string::npos)
		{
			if (error_out != nullptr) *error_out = "The runner returned an invalid file preview.";
			return false;
		}
		file = {{"text", std::move(text)}, {"path", std::move(relative_path)}};
		return true;
	}

	bool RunnerClient::ReadWorkspaceAttachment(const std::string& workspace, const std::string& path, std::string& bytes,
	    std::size_t max_bytes,
	    std::string* error_out, std::stop_token stop_token)
	{
		bytes.clear();
		max_bytes = std::min<std::size_t>(max_bytes, 25 * 1024 * 1024);
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_workspaceAttachmentRead)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to deliver image attachments.";
			return false;
		}
		std::string text;
		std::string relative_path;
		std::string fingerprint;
		std::size_t total = 0;
		do
		{
			nlohmann::json request = {{"type", "workspace.attachment.read"}, {"workspace", workspace}, {"path", path}, {"offset", text.size()}};
			if (!fingerprint.empty()) request["fingerprint"] = fingerprint;
			nlohmann::json response;
			if (!Request(request, response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
			const nlohmann::json result = response.value("result", nlohmann::json{});
			std::string chunk;
			if (!result.is_object() || !result.contains("totalBytes") || !result["totalBytes"].is_number_integer() ||
			    result["totalBytes"].get<std::int64_t>() < 0 || result["totalBytes"].get<std::int64_t>() > static_cast<std::int64_t>(max_bytes) ||
			    !result.contains("offset") || !result["offset"].is_number_integer() || result["offset"] != text.size() ||
			    !result.contains("path") || !result["path"].is_string() || result["path"].get_ref<const std::string&>().empty() ||
			    result["path"].get_ref<const std::string&>().size() > 16384 ||
			    !result.contains("fingerprint") || !result["fingerprint"].is_string() || result["fingerprint"].get_ref<const std::string&>().size() != 16 ||
			    !result.contains("dataBase64") || !result["dataBase64"].is_string() ||
			    result["dataBase64"].get_ref<const std::string&>().size() > 699052 ||
			    !uam::base64::Decode(result["dataBase64"].get_ref<const std::string&>(), chunk))
			{
				if (error_out != nullptr) *error_out = "The runner returned an invalid image attachment.";
				return false;
			}
			const std::size_t next_total = result["totalBytes"].get<std::size_t>();
			if (text.size() > next_total || chunk.size() != std::min<std::size_t>(512 * 1024, next_total - text.size()) ||
			    (!fingerprint.empty() && (result["fingerprint"] != fingerprint || result["path"] != relative_path || next_total != total)))
			{
				if (error_out != nullptr) *error_out = "The image changed while loading. Try again.";
				return false;
			}
			total = next_total;
			relative_path = result["path"].get<std::string>();
			fingerprint = result["fingerprint"].get<std::string>();
			text += chunk;
		} while (text.size() < total);
		if (uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(text)) != fingerprint)
		{
			if (error_out != nullptr) *error_out = "The runner returned an invalid image attachment.";
			return false;
		}
		bytes = std::move(text);
		return true;
	}

	bool RunnerClient::QueryOpenCodeNative(const std::string& session_id, const nlohmann::json& operation, nlohmann::json& result,
	    std::string* error_out, std::stop_token stop_token)
	{
		result = nullptr;
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_openCodeNativeControl)
		{ if (error_out != nullptr) *error_out = "Update this host's UAM runner for native OpenCode child permissions."; return false; }
		nlohmann::json request{{"type", "opencode.native"}, {"sessionId", session_id}, {"operation", operation}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		if (!Request(std::move(request), response, error_out, [stop_token] { return stop_token.stop_requested(); })) return false;
		if (!response.contains("result") || response["result"].dump().size() > 262144)
		{ if (error_out != nullptr) *error_out = "The native OpenCode response exceeded its bound."; return false; }
		result = std::move(response["result"]);
		return true;
	}

	bool RunnerClient::ObserveOpenCodeTerminal(int port, const std::string& token, const std::string& workspace,
	    const nlohmann::json& operation, nlohmann::json& result, std::string* error_out, std::stop_token stop_token)
	{
		result = nullptr;
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_openCodeTerminalObservation)
		{ if (error_out) *error_out = "Update this host's UAM runner for native OpenCode terminal activity."; return false; }
		nlohmann::json response;
		if (!Request({{"type", "opencode.terminal.observe"}, {"port", port}, {"token", token}, {"workspace", workspace}, {"operation", operation}},
		    response, error_out, [stop_token] { return stop_token.stop_requested(); })) return false;
		if (!response.contains("result") || response["result"].dump().size() > 262144)
		{ if (error_out) *error_out = "The native OpenCode observation exceeded its bound."; return false; }
		result = std::move(response["result"]);
		return true;
	}

	bool RunnerClient::ReadUsageAccountingFile(const std::filesystem::path& path, nlohmann::json& progress, std::string* error_out, std::stop_token stop_token)
	{
		progress = nullptr;
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		std::string error;
		std::filesystem::path temporary = path; temporary += ".download-" + Nonce();
		struct Cleanup
		{
			std::filesystem::path path;
			~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
		} cleanup{temporary};
		if (uam::paths::IsLinkOrReparsePointNoThrow(path.parent_path()) || uam::paths::IsLinkOrReparsePointNoThrow(path) || uam::paths::IsLinkOrReparsePointNoThrow(temporary) || !uam::paths::CreateDirectoriesNoThrow(path.parent_path()))
		{ if (error_out) *error_out = "Could not prepare usage cache."; return false; }
		uam::io::AtomicWriteResult staged_result;
		const bool staged = uam::io::WriteAndSyncAtomicTemp(temporary, uam::io::AtomicTempProducer([&](const std::function<bool(std::string_view)>& append)
		{
		if (!m_usageAccountingStream)
		{
			nlohmann::json document;
			if (!ReadUsageAccounting(document, error_out, stop_token)) return false;
			if (document.value("pending", false)) { progress = std::move(document); return true; }
			if (!append(nlohmann::json({{"version", 3}, {"range", document["version"] == 2 ? "all-recorded" : "rolling-90-days"}, {"asOfMs", document["asOfMs"]}, {"metadata", document["metadata"]}}).dump() + "\n")) return false;
			for (const nlohmann::json& fact : document["facts"])
			{
				if (stop_token.stop_requested()) { if (error_out) *error_out = "Usage accounting loading was cancelled."; return false; }
				if (!append(fact.dump() + "\n")) return false;
			}
		}
		else
		{
			std::uint64_t offset = 0, total = 0, hash = uam::hashing::kFnv1a64OffsetBasis;
			std::string fingerprint;
			do
			{
				const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
				const std::function<bool()> interrupted = [stop_token, deadline]() { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; };
				nlohmann::json request{{"type", "usage.accounting"}, {"format", "jsonl-v3"}, {"offset", offset}};
				if (!fingerprint.empty()) request["fingerprint"] = fingerprint;
				nlohmann::json response;
				if (!Request(request, response, error_out, interrupted)) return false;
				const nlohmann::json result = response.value("result", nlohmann::json{});
				if (result.is_object() && result.contains("pending"))
				{
					if (offset || result["pending"] != true || !result.contains("collectionError") || !result["collectionError"].is_string() || result["collectionError"].get_ref<const std::string&>().size() > 1024)
					{ if (error_out) *error_out = "The runner returned invalid usage progress."; return false; }
					progress = result; return true;
				}
				std::string chunk;
				if (!result.is_object() || result.value("format", nlohmann::json{}) != "jsonl-v3" || !result.contains("totalBytes") || !result["totalBytes"].is_number_integer() || result["totalBytes"] <= 0 || result["totalBytes"] > uam::usage::kMaxExactTokenCount ||
				    !result.contains("offset") || !result["offset"].is_number_integer() || result["offset"] != offset || !result.contains("fingerprint") || !result["fingerprint"].is_string() || result["fingerprint"].get_ref<const std::string&>().size() != 16 ||
				    !result.contains("dataBase64") || !result["dataBase64"].is_string() || result["dataBase64"].get_ref<const std::string&>().size() > 699052 || !uam::base64::Decode(result["dataBase64"].get_ref<const std::string&>(), chunk))
				{ if (error_out) *error_out = "The runner returned invalid streamed accounting."; return false; }
				const std::uint64_t next_total = result["totalBytes"].get<std::uint64_t>();
				if (offset > next_total || chunk.size() != std::min<std::uint64_t>(512 * 1024, next_total - offset) || (!fingerprint.empty() && (result["fingerprint"] != fingerprint || total != next_total)) || chunk.find('\0') != std::string::npos)
				{ if (error_out) *error_out = "Usage accounting changed while loading. Try again."; return false; }
				total = next_total; fingerprint = result["fingerprint"].get<std::string>();
				if (!append(chunk)) { if (error_out) *error_out = "Could not save usage cache."; return false; }
				uam::hashing::UpdateFnv1a64(hash, chunk); offset += chunk.size();
			} while (offset < total);
			if (uam::hashing::Hex64Padded(hash) != fingerprint) { if (error_out) *error_out = "The runner returned invalid accounting fingerprint."; return false; }
		}
		return true;
		}), {}, staged_result);
		if (!staged) { if (error_out && error_out->empty()) *error_out = "Could not save usage cache."; return false; }
		if (progress.is_object()) return true;
		const std::function<bool()> cancelled = [stop_token]() { return stop_token.stop_requested(); };
		const std::shared_ptr<const uam::usage::ReceiptSnapshot> snapshot = uam::usage::ReadReceiptSnapshot(temporary, error, cancelled);
		if (!snapshot || !snapshot->Visit([](const uam::usage::AccountingRecord&) { return true; }, error, cancelled))
		{ if (error_out) *error_out = error; return false; }
		if (cancelled()) { if (error_out) *error_out = "Usage accounting loading was cancelled."; return false; }
		const uam::io::AtomicWriteResult published = uam::io::CommitSyncedAtomicTemp(path, temporary);
		if (!published.success) { if (error_out) *error_out = "Could not publish usage cache."; return false; }
		return true;
	}

	bool RunnerClient::ReadUsageAccounting(nlohmann::json& snapshot,
	    std::string* error_out, std::stop_token stop_token)
	{
		snapshot = nullptr;
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_usageAccounting)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to collect usage accounting.";
			return false;
		}
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		const std::function<bool()> interrupted = [stop_token, deadline]() { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; };
		std::string text;
		std::string fingerprint;
		std::size_t total = 0;
		do
		{
			nlohmann::json request = {{"type", "usage.accounting"}, {"offset", text.size()}};
			if (!fingerprint.empty()) request["fingerprint"] = fingerprint;
			nlohmann::json response;
			if (!Request(request, response, error_out, interrupted)) return false;
			const nlohmann::json result = response.value("result", nlohmann::json{});
			if (result.is_object() && result.contains("pending"))
			{
				if (!text.empty() || !result["pending"].is_boolean() || result["pending"] != true ||
				    !result.contains("collectionError") || !result["collectionError"].is_string() || result["collectionError"].get_ref<const std::string&>().size() > 1024)
				{ if (error_out) *error_out = "The runner returned invalid usage progress."; return false; }
				snapshot = result; return true;
			}
			std::string chunk;
			if (!result.is_object() || !result.contains("totalBytes") || !result["totalBytes"].is_number_integer() ||
			    result["totalBytes"].get<std::int64_t>() < 0 || result["totalBytes"].get<std::int64_t>() > 64 * 1024 * 1024 ||
			    !result.contains("offset") || !result["offset"].is_number_integer() || result["offset"] != text.size() ||
			    !result.contains("fingerprint") || !result["fingerprint"].is_string() || result["fingerprint"].get_ref<const std::string&>().size() != 16 ||
			    !result.contains("dataBase64") || !result["dataBase64"].is_string() ||
			    result["dataBase64"].get_ref<const std::string&>().size() > 699052 ||
			    !uam::base64::Decode(result["dataBase64"].get_ref<const std::string&>(), chunk))
			{
				if (error_out != nullptr) *error_out = "The runner returned an invalid usage accounting.";
				return false;
			}
			const std::size_t next_total = result["totalBytes"].get<std::size_t>();
			if (text.size() > next_total || chunk.size() != std::min<std::size_t>(512 * 1024, next_total - text.size()) ||
			    (!fingerprint.empty() && (result["fingerprint"] != fingerprint || next_total != total)))
			{
				if (error_out != nullptr) *error_out = "Usage accounting changed while loading. Try again.";
				return false;
			}
			total = next_total;
			fingerprint = result["fingerprint"].get<std::string>();
			text += chunk;
		} while (text.size() < total);
		if (uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(text)) != fingerprint || text.find('\0') != std::string::npos)
		{
			if (error_out != nullptr) *error_out = "The runner returned an invalid usage accounting.";
			return false;
		}
		snapshot = nlohmann::json::parse(text, nullptr, false);
		if (!snapshot.is_object() || !snapshot.contains("version") || (snapshot["version"] != 1 && snapshot["version"] != 2) || !snapshot.contains("asOfMs") || !snapshot["asOfMs"].is_number_integer() || snapshot["asOfMs"] <= 0 || snapshot["asOfMs"] >= uam::usage::kMaxExactTokenCount ||
		    !snapshot.contains("metadata") || !snapshot["metadata"].is_object() || !snapshot.contains("facts") || !snapshot["facts"].is_array() || snapshot["facts"].size() > 500000)
		{ if (error_out) *error_out = "The runner returned incompatible usage accounting."; snapshot = {}; return false; }
		const bool lifetime = snapshot["version"] == 2;
		if ((lifetime && (!snapshot.contains("range") || snapshot["range"] != "all-recorded")) || (!lifetime && snapshot.contains("range")))
		{ if (error_out) *error_out = "The runner returned incompatible usage accounting scope."; snapshot = {}; return false; }
		const std::int64_t as_of = snapshot["asOfMs"].get<std::int64_t>();
		const std::int64_t start = lifetime ? 0 : std::max<std::int64_t>(0, as_of / 86400000 * 86400000 - 89LL * 86400000);
		std::unordered_set<std::string> identities;
		for (const nlohmann::json& row : snapshot["facts"])
		{
			if (interrupted()) { if (error_out) *error_out = "Usage accounting loading was cancelled or timed out."; snapshot = {}; return false; }
			const std::optional<uam::usage::AccountingRecord> fact = uam::usage::DecodeAccountingRecord(row);
			if (!fact || ((fact->record.timestamp_ms < start || fact->record.timestamp_ms >= as_of) && !uam::usage::DecodeAntigravityAccountingRecord(fact->record)) || !identities.insert(fact->identity).second)
			{ if (error_out) *error_out = "The runner returned invalid accounting identities."; snapshot = {}; return false; }
		}
		return true;
	}

	bool RunnerClient::ReadNativeHistory(const std::string& provider, const std::string& session, const std::string& workspace,
	    nlohmann::json& output, std::string* error_out, std::stop_token stop_token, const std::string& agent_id)
	{
		output = nullptr;
		if (!Connect(error_out, stop_token)) return false;
		if (!m_providerHistoryRead)
		{
			if (error_out) *error_out = "Update this host's UAM runner to refresh native provider history.";
			return false;
		}
		std::string token, fingerprint;
		native_history::Snapshot snapshot;
		snapshot.provider = provider; snapshot.session_id = session; snapshot.agent_id = agent_id;
		std::size_t total = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		const auto interrupted = [&]() { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; };
		const auto finish = [&](bool ok)
		{
			if (!token.empty() && m_connected && !stop_token.stop_requested())
			{
				nlohmann::json released;
				std::string cleanup_error;
				const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
				(void)Request({{"type", "provider.history.release"}, {"providerId", provider}, {"sessionId", session}, {"agentId", agent_id},
				    {"workspace", workspace}, {"snapshotId", token}}, released, &cleanup_error,
				    [cleanup_deadline, stop_token]() { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= cleanup_deadline; });
			}
			if (!ok) output = nullptr;
			return ok;
		};
		for (;;)
		{
			if (interrupted()) { if (error_out) *error_out = "Native history refresh was cancelled or timed out."; return finish(false); }
			nlohmann::json response;
			if (!Request({{"type", "provider.history.read"}, {"providerId", provider}, {"sessionId", session}, {"agentId", agent_id}, {"workspace", workspace},
			    {"snapshotId", token}, {"fingerprint", fingerprint}, {"offset", snapshot.text.size()}}, response, error_out, interrupted)) return finish(false);
			const auto result = response.find("result");
			if (result == response.end() || !result->is_object()) { if (error_out) *error_out = "The runner returned an invalid native history snapshot."; return finish(false); }
			const auto& value = *result;
			const std::string received_token = native_history::String(value, "snapshotId");
			const std::string received_fingerprint = native_history::String(value, "fingerprint");
			const auto count = value.find("totalBytes"), offset = value.find("offset");
			if (token.empty() && native_history::SafeId(received_token)) token = received_token;
			if (count == value.end() || offset == value.end() || !count->is_number_integer() || !offset->is_number_integer() || *offset < 0 ||
			    *count <= 0 || *count > native_history::kMaxSnapshotBytes || *offset != snapshot.text.size() ||
			    !native_history::SafeId(received_token) || received_token != token || received_fingerprint.size() != 16 ||
			    native_history::String(value, "providerId") != provider || native_history::String(value, "sessionId") != session || native_history::String(value, "agentId") != agent_id)
			{ if (error_out) *error_out = "The runner returned mismatched native history ownership or size."; return finish(false); }
			if (snapshot.text.empty())
			{
				total = count->get<std::size_t>(); fingerprint = received_fingerprint;
				snapshot.workspace = native_history::String(value, "workspace");
				snapshot.workspace_yaml = native_history::String(value, "workspaceYaml");
				if (snapshot.workspace.empty() || snapshot.workspace_yaml.size() > 16 * 1024 || total + snapshot.workspace_yaml.size() > native_history::kMaxSnapshotBytes)
				{ if (error_out) *error_out = "Invalid native history workspace metadata."; return finish(false); }
			}
			else if (count->get<std::size_t>() != total || fingerprint != received_fingerprint || snapshot.workspace != native_history::String(value, "workspace"))
			{ if (error_out) *error_out = "The native history snapshot changed while loading."; return finish(false); }
			std::string chunk;
			if (!base64::Decode(native_history::String(value, "data"), chunk) || chunk.empty() || chunk.size() > native_history::kChunkBytes || chunk.size() > total - snapshot.text.size())
			{ if (error_out) *error_out = "The runner returned an invalid native history chunk."; return finish(false); }
			snapshot.text += chunk;
			if (snapshot.text.size() == total) break;
		}
		if (native_history::Fingerprint(snapshot) != fingerprint)
		{ if (error_out) *error_out = "The native history snapshot fingerprint did not match."; return finish(false); }
		output = {{"providerId", provider}, {"sessionId", session}, {"agentId", agent_id}, {"workspace", std::move(snapshot.workspace)},
		    {"text", std::move(snapshot.text)}, {"workspaceYaml", std::move(snapshot.workspace_yaml)}};
		return finish(true);
	}

	bool RunnerClient::ReadTextFile(const std::filesystem::path& remote_path, std::string& text,
	    std::string* error_out, std::stop_token stop_token)
	{
		text.clear();
		if (!m_connected && !Connect(error_out, stop_token)) return false;
		if (!m_contextRead)
		{
			if (error_out != nullptr) *error_out = "Update this host's UAM runner to read remote context files.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "context.read"}, {"path", uam::paths::Utf8PathString(remote_path)}},
		    response, error_out, [stop_token]() { return stop_token.stop_requested(); })) return false;
		if (!response.contains("result") || !response["result"].is_object() || !response["result"].contains("text") || !response["result"]["text"].is_string() || response["result"]["text"].get_ref<const std::string&>().size() > 256 * 1024)
		{
			if (error_out != nullptr) *error_out = "The remote runner returned invalid context text.";
			return false;
		}
		text = response["result"]["text"].get<std::string>();
		return true;
	}

	bool RunnerClient::ListDirectories(const std::filesystem::path& remote_path,
	                                   DirectoryListing& result,
	                                   std::string* error_out)
	{
		if (!Connect(error_out)) return false;
		if (!m_directoryBrowsing)
		{
			if (error_out != nullptr)
				*error_out = "This remote helper does not support directory browsing. Reinstall the helper from this UAM build.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "directory.list"}, {"path", uam::paths::Utf8PathString(remote_path)}},
		             response, error_out)) return false;
		if (!response.contains("result") || !response["result"].is_object() ||
		    !response["result"].contains("directories") ||
		    !response["result"]["directories"].is_array() ||
		    response["result"]["directories"].size() > 200)
		{
			if (error_out != nullptr) *error_out = "The remote runner returned an invalid directory listing.";
			return false;
		}

		DirectoryListing parsed;
		const nlohmann::json& value = response["result"];
		if (!value.contains("directory") || !value["directory"].is_string() ||
		    (value.contains("parentDirectory") && !value["parentDirectory"].is_string()) ||
		    (value.contains("truncated") && !value["truncated"].is_boolean()))
		{
			if (error_out != nullptr) *error_out = "The remote runner returned invalid directory metadata.";
			return false;
		}
		parsed.directory = value["directory"].get<std::string>();
		parsed.parent_directory = value.value("parentDirectory", "");
		parsed.truncated = value.value("truncated", false);
		for (const nlohmann::json& entry : value["directories"])
		{
			if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() ||
			    !entry.contains("path") || !entry["path"].is_string())
			{
				if (error_out != nullptr) *error_out = "The remote runner returned an invalid directory entry.";
				return false;
			}
			const std::string name = entry.value("name", "");
			const std::string path = entry.value("path", "");
			if (name.empty() || path.empty())
			{
				if (error_out != nullptr) *error_out = "The remote runner returned an empty directory entry.";
				return false;
			}
			parsed.directories.emplace_back(name, path);
		}
		if (parsed.directory.empty())
		{
			if (error_out != nullptr) *error_out = "The remote runner returned an invalid directory path.";
			return false;
		}
		result = std::move(parsed);
		return true;
	}

	bool RunnerClient::InspectPath(const std::string& absolute_path, bool allow_missing_leaf,
	    PathInspection& result, std::string* error_out, std::stop_token stop_token)
	try
	{
		result = {};
		if (!Connect(error_out, stop_token)) return false;
		if (!m_safePathInspection)
		{
			if (error_out != nullptr) *error_out = "Update this SSH helper before creating remote worktrees; safe path inspection is unavailable.";
			return false;
		}
		nlohmann::json response;
		if (!Request({{"type", "path.inspect"}, {"path", absolute_path}, {"allowMissingLeaf", allow_missing_leaf}},
		    response, error_out, [stop_token] { return stop_token.stop_requested(); })) return false;
		const nlohmann::json& proof = response.at("result");
		if (!proof.is_object() || !proof.contains("path") || !proof["path"].is_string() ||
		    !proof.contains("exists") || !proof["exists"].is_boolean() ||
		    !proof.contains("directory") || !proof["directory"].is_boolean() ||
		    !proof.contains("regularFile") || !proof["regularFile"].is_boolean())
			throw nlohmann::json::type_error::create(302, "Invalid path inspection fields.", &proof);
		PathInspection parsed{proof["path"].get<std::string>(), proof["exists"].get<bool>(),
		    proof["directory"].get<bool>(), proof["regularFile"].get<bool>()};
		if (parsed.path.empty() || parsed.path.size() > 4096 ||
		    std::ranges::any_of(parsed.path, [](unsigned char c) { return c < 32 || c == 127; }) ||
		    (!execution_hosts::IsAbsoluteRemotePath("windows", parsed.path) && !execution_hosts::IsAbsoluteRemotePath("linux", parsed.path)) ||
		    (parsed.exists ? parsed.directory == parsed.regular_file : parsed.directory || parsed.regular_file || !allow_missing_leaf))
			throw nlohmann::json::type_error::create(302, "Invalid path inspection proof.", &proof);
		result = std::move(parsed);
		return true;
	}
	catch (const nlohmann::json::exception&)
	{
		if (error_out != nullptr) *error_out = "The SSH helper returned invalid safe path inspection data.";
		return false;
	}

	bool RunnerClient::CloseProcessInput(const std::string& session_id, std::string* error_out)
	{
		nlohmann::json request = {{"type", "process.closeInput"}, {"sessionId", session_id}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		bool closed = Request(request, response, error_out);
		if (!closed && m_expectedProtocolVersion >= 3 && !m_connected && Connect(error_out))
			closed = Request(std::move(request), response, error_out);
		return closed;
	}

	bool RunnerClient::PollProcess(const std::string& session_id, ProcessPollResult& result,
	                               std::string* error_out,
	                               const std::function<bool()>& interrupt,
	                               bool* interrupted_out,
	                               const ProcessPollResult* resume_after, bool* missing_out)
	{
		if (missing_out != nullptr) *missing_out = false;
		nlohmann::json request = {{"type", "process.poll"}, {"sessionId", session_id},
		                          {"acknowledgedOutput", m_processOutputAcknowledgement}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		if (m_processOutputAcknowledgement && resume_after != nullptr)
		{
			request["stdoutCursor"] = resume_after->stdout_cursor;
			request["stderrCursor"] = resume_after->stderr_cursor;
		}
		nlohmann::json response;
		if (!Request(std::move(request), response,
		             error_out, interrupt, interrupted_out))
		{
			if (missing_out != nullptr && m_expectedProtocolVersion >= 3 && m_connected &&
			    response.contains("error") && response["error"].is_object())
			{
				const nlohmann::json::const_iterator code = response["error"].find("code");
				*missing_out = code != response["error"].end() && code->is_string() && *code == "session_not_found";
			}
			return false;
		}
		ProcessPollResult parsed;
		const nlohmann::json& value = response["result"];
		parsed.running = value.value("running", false);
		parsed.exit_code = value.contains("exitCode") && value["exitCode"].is_number_integer()
		    ? value["exitCode"].get<int>()
		    : -1;
		if (!uam::base64::Decode(value.value("stdoutBase64", ""), parsed.standard_output) ||
		    !uam::base64::Decode(value.value("stderrBase64", ""), parsed.standard_error))
		{
			if (error_out != nullptr) *error_out = "The remote runner returned invalid process output.";
			return false;
		}
		parsed.stdout_cursor = value.value("stdoutCursor", static_cast<std::uintmax_t>(0));
		parsed.stderr_cursor = value.value("stderrCursor", static_cast<std::uintmax_t>(0));
		if (m_processOutputAcknowledgement &&
		    (parsed.stdout_cursor < parsed.standard_output.size() ||
		     parsed.stderr_cursor < parsed.standard_error.size()))
		{
			if (error_out != nullptr)
				*error_out = "The remote runner returned process cursors before the delivered output.";
			return false;
		}
		parsed.input_sequence = value.value("inputSequence", static_cast<std::uint64_t>(0));
		m_processInputSequences[session_id] = std::max(
		    m_processInputSequences[session_id], parsed.input_sequence);
		parsed.acknowledgement_required = m_processOutputAcknowledgement &&
		    (!parsed.standard_output.empty() || !parsed.standard_error.empty());
		result = std::move(parsed);
		return true;
	}

	bool RunnerClient::AcknowledgeProcessOutput(const std::string& session_id,
	                                            const ProcessPollResult& result,
	                                            std::string* error_out)
	{
		if (!result.acknowledgement_required) return true;
		nlohmann::json request = {{"type", "process.ack"}, {"sessionId", session_id},
		                          {"stdoutCursor", result.stdout_cursor},
		                          {"stderrCursor", result.stderr_cursor}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		bool acknowledged = Request(request, response, error_out);
		if (!acknowledged && !m_connected && Connect(error_out))
			acknowledged = Request(std::move(request), response, error_out);
		return acknowledged;
	}

	bool RunnerClient::StopProcess(const std::string& session_id, std::string* error_out, bool graceful, std::string* outcome_out)
	{
		if (!Connect(error_out)) return false;
		if (outcome_out != nullptr) *outcome_out = "unknown";
		nlohmann::json request = {{"type", "process.stop"}, {"sessionId", session_id}, {"graceful", graceful}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		bool stopped = Request(request, response, error_out);
		if (!stopped && !m_connected && Connect(error_out))
			stopped = Request(std::move(request), response, error_out);
		if (stopped && outcome_out != nullptr)
		{
			const nlohmann::json result = response.value("result", nlohmann::json::object());
			const std::string outcome = result.is_object() && result.contains("stopOutcome") && result["stopOutcome"].is_string() ? result["stopOutcome"].get<std::string>() : "unknown";
			if (outcome == "graceful" || outcome == "forced" || outcome == "failed") *outcome_out = outcome;
		}
		return stopped;
	}

	bool RunnerClient::RemoveProcess(const std::string& session_id, std::string* error_out)
	{
		if (!Connect(error_out)) return false;
		nlohmann::json request = {{"type", "process.remove"}, {"sessionId", session_id}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		bool removed = Request(request, response, error_out);
		if (!removed && !m_connected && Connect(error_out))
		{
			removed = Request(std::move(request), response, error_out);
			if (!removed && error_out != nullptr &&
			    error_out->find("does not exist") != std::string::npos)
				removed = true;
		}
		if (removed)
		{
			m_processControlTokens.erase(session_id);
			m_processInputSequences.erase(session_id);
		}
		return removed;
	}

	void RunnerClient::Disconnect()
	{
		if (!m_connected) return;
		m_processService.StopStdioProcess(m_bridge, true);
		m_received.clear();
		m_connected = false;
		m_directoryBrowsing = false;
		m_safePathInspection = false;
		m_contextRead = false;
		m_providerHistoryRead = false;
		m_workspaceFileRead = false;
		m_workspaceAttachmentRead = false;
		m_openCodeNativeControl = false;
		m_openCodeTerminalObservation = false;
		m_usageAccounting = false; m_usageAccountingStream = false;
		m_projectMemory = false;
		m_isolatedTextWorkers = false;
		m_runnerStartup = false;
		m_runnerStartupObservation = false;
		m_leasedChannelTake = false;
		m_providerNativeContext = false;
		m_centralProviderConfiguration = false;
		m_geminiThinkingConfiguration = false;
		m_claudeTerminalFastConfiguration = false;
	}
}
