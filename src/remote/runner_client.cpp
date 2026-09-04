#include "remote/runner_client.h"

#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
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

	bool RunnerClient::Connect(std::string* error_out)
	{
		if (m_connected) return true;
		if (m_bridgeArgv.empty())
		{
			if (error_out != nullptr) *error_out = "The remote runner bridge command is empty.";
			return false;
		}
		std::string error;
		if (!m_processService.StartStdioProcess(m_bridge, std::filesystem::current_path(),
		                                        m_bridgeArgv, &error))
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
		             response, &error) || response.value("nonce", "") != nonce ||
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
		m_processOutputAcknowledgement = m_expectedProtocolVersion >= 3 &&
		    response["capabilities"].value("processOutputAcknowledgement", false);
		return true;
	}

	bool RunnerClient::Request(nlohmann::json request, nlohmann::json& response,
	                           std::string* error_out,
	                           const std::function<bool()>& interrupt,
	                           bool* interrupted_out)
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
		return true;
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

	bool RunnerClient::StartProcess(
	    const std::string& session_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment,
	    std::string* error_out, bool attach_if_exists, std::string control_token)
	{
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
		                          {"cwd", working_directory.string()}, {"argv", argv},
		                          {"environment", std::move(environment_json)},
		                          {"attachIfExists", attach_if_exists}};
		if (m_expectedProtocolVersion >= 3) request["controlToken"] = control_token;
		if (m_expectedProtocolVersion >= 3 && transient_lease)
			request["transientLeaseMs"] = 60000;
		nlohmann::json response;
		bool started = Request(request, response, error_out);
		if (!started && !m_connected && Connect(error_out))
		{
			request["attachIfExists"] = true;
			started = Request(std::move(request), response, error_out);
		}
		if (started)
			m_processInputSequences[session_id] = response.value(
			    "result", nlohmann::json::object()).value("inputSequence", std::uint64_t{0});
		return started;
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
	                               bool attach_if_exists)
	{
		if (!Connect(error_out)) return false;
		nlohmann::json response;
		if (!Request({{"type", "channel.open"}, {"channelId", channel_id},
		              {"attachIfExists", attach_if_exists}}, response, error_out)) return false;
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

	bool RunnerClient::WriteChannel(const std::string& channel_id,
	                                std::string_view direction, std::string_view bytes,
	                                std::string* error_out)
	{
		const std::string sequence_key = ChannelCursorKey(channel_id, direction);
		nlohmann::json request = {{"type", "channel.write"}, {"channelId", channel_id},
		                          {"direction", direction},
		                          {"dataBase64", uam::base64::Encode(bytes)}};
		const std::uint64_t sequence = m_channelWriteSequences[sequence_key] + 1;
		if (m_expectedProtocolVersion >= 3) request["writeSequence"] = sequence;
		nlohmann::json response;
		bool written = Request(request, response, error_out);
		if (!written && m_expectedProtocolVersion >= 3 && !m_connected && Connect(error_out))
			written = Request(std::move(request), response, error_out);
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
		if (!response.contains("result") || !response["result"].is_object() ||
		    !uam::base64::Decode(response["result"].value("dataBase64", ""), bytes))
		{
			if (error_out != nullptr) *error_out = "The runner channel returned invalid data.";
			return false;
		}
		const std::uintmax_t cursor = response["result"].value(
		    "cursor", m_channelCursors[cursor_key] + bytes.size());
		if (cursor_out != nullptr) *cursor_out = cursor;
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

	bool RunnerClient::CloseChannel(const std::string& channel_id, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "channel.close"}, {"channelId", channel_id}}, response,
		               error_out);
	}

	bool RunnerClient::UploadFile(const std::string& upload_id,
	                              const std::filesystem::path& remote_path,
	                              std::string_view bytes, std::string* error_out)
	{
		if (!Connect(error_out)) return false;
		const auto abort = [&]
		{
			nlohmann::json ignored;
			if (!m_connected) (void)Connect(nullptr);
			if (m_connected)
				(void)Request({{"type", "file.abort"}, {"uploadId", upload_id}}, ignored,
				              nullptr);
		};
		std::uint64_t digest = uam::hashing::kFnv1a64OffsetBasis;
		uam::hashing::UpdateFnv1a64(
		    digest, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
		nlohmann::json response;
		if (!Request({{"type", "file.begin"}, {"uploadId", upload_id},
		              {"path", remote_path.string()}, {"size", bytes.size()},
		              {"digest", uam::hashing::Hex64Padded(digest)}},
		             response, error_out))
		{
			abort();
			return false;
		}
		for (std::size_t offset = 0; offset < bytes.size(); offset += 256 * 1024)
		{
			const std::string_view chunk = bytes.substr(offset, 256 * 1024);
			if (!Request({{"type", "file.write"}, {"uploadId", upload_id},
			              {"dataBase64", uam::base64::Encode(chunk)}}, response, error_out))
			{
				abort();
				return false;
			}
		}
		if (!Request({{"type", "file.commit"}, {"uploadId", upload_id}}, response,
		             error_out))
		{
			abort();
			return false;
		}
		return true;
	}

	bool RunnerClient::RemoveFile(const std::string& request_id,
	                              const std::filesystem::path& remote_path,
	                              std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "file.remove"}, {"uploadId", request_id},
		                {"path", remote_path.string()}}, response, error_out);
	}

	bool RunnerClient::CopyFile(const std::string& request_id,
	                            const std::filesystem::path& source_path,
	                            const std::filesystem::path& target_path,
	                            const bool overwrite, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "file.copy"}, {"uploadId", request_id},
		                {"sourcePath", source_path.string()},
		                {"targetPath", target_path.string()}, {"overwrite", overwrite}},
		               response, error_out);
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
	                               const ProcessPollResult* resume_after)
	{
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
			return false;
		const nlohmann::json& value = response["result"];
		result.running = value.value("running", false);
		result.exit_code = value.contains("exitCode") && value["exitCode"].is_number_integer()
		    ? value["exitCode"].get<int>()
		    : -1;
		if (!uam::base64::Decode(value.value("stdoutBase64", ""), result.standard_output) ||
		    !uam::base64::Decode(value.value("stderrBase64", ""), result.standard_error))
		{
			if (error_out != nullptr) *error_out = "The remote runner returned invalid process output.";
			return false;
		}
		result.stdout_cursor = value.value("stdoutCursor", static_cast<std::uintmax_t>(0));
		result.stderr_cursor = value.value("stderrCursor", static_cast<std::uintmax_t>(0));
		result.input_sequence = value.value("inputSequence", static_cast<std::uint64_t>(0));
		m_processInputSequences[session_id] = std::max(
		    m_processInputSequences[session_id], result.input_sequence);
		result.acknowledgement_required = m_processOutputAcknowledgement &&
		    (!result.standard_output.empty() || !result.standard_error.empty());
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

	bool RunnerClient::StopProcess(const std::string& session_id, std::string* error_out)
	{
		if (!Connect(error_out)) return false;
		nlohmann::json request = {{"type", "process.stop"}, {"sessionId", session_id}};
		if (!AddProcessControlToken(request, session_id, error_out)) return false;
		nlohmann::json response;
		bool stopped = Request(request, response, error_out);
		if (!stopped && !m_connected && Connect(error_out))
			stopped = Request(std::move(request), response, error_out);
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
	}
}
