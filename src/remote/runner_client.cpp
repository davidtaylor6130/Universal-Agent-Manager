#include "remote/runner_client.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_protocol.h"

#include <nlohmann/json.hpp>

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
	}

	std::vector<std::string> SshBridgeArgv(const std::string& ssh_alias,
	                                       const std::string& platform,
	                                       const std::string& version)
	{
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias) || version.empty() ||
		    !std::ranges::all_of(version, [](unsigned char character)
		    { return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.'; }))
			return {};
		std::string command;
		if (platform == "linux" || platform == "macos" || platform == "Linux" ||
		    platform == "Darwin")
		{
			const std::string runner = "~/.local/share/uam/runner/" + version + "/uam-runner";
			command = runner + " start --socket ~/.local/share/uam/runner/uam.sock && exec " +
			          runner + " bridge --socket ~/.local/share/uam/runner/uam.sock";
		}
		else if (platform == "windows" || platform == "Windows")
		{
			const std::string runner = "$HOME/.uam/runner/" + version + "/uam-runner.exe";
			command = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			          "-Command \"& '" + runner + "' start; if ($LASTEXITCODE -ne 0) { exit "
			          "$LASTEXITCODE }; & '" + runner + "' bridge\"";
		}
		else return {};
		return {"ssh", "-T", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes", "-o",
		        "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o",
		        "ServerAliveCountMax=2", ssh_alias, std::move(command)};
	}

	RunnerClient::RunnerClient(IPlatformProcessService& process_service,
	                           std::vector<std::string> bridge_argv,
	                           std::string expected_version)
	    : m_processService(process_service), m_bridgeArgv(std::move(bridge_argv)),
	      m_expectedVersion(std::move(expected_version))
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
		if (!Request({{"type", "hello"}, {"protocolVersion", kRunnerProtocolVersion},
		              {"nonce", nonce}},
		             response, &error) || response.value("nonce", "") != nonce ||
		    response.value("protocolVersion", 0) != kRunnerProtocolVersion ||
		    (!m_expectedVersion.empty() &&
		     response.value("runnerVersion", "") != m_expectedVersion) ||
		    !response.value("capabilities", nlohmann::json::object())
		         .value("processExecution", false) ||
		    response.value("capabilities", nlohmann::json::object())
		        .value("computerUse", true))
		{
			Disconnect();
			if (error_out != nullptr)
				*error_out = error.empty() ? "The remote runner handshake is incompatible." : error;
			return false;
		}
		return true;
	}

	bool RunnerClient::Request(nlohmann::json request, nlohmann::json& response,
	                           std::string* error_out)
	{
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
		    !ReadResponse(response, &error))
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

	bool RunnerClient::ReadResponse(nlohmann::json& response, std::string* error_out)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		std::array<char, 16 * 1024> buffer{};
		while (std::chrono::steady_clock::now() < deadline)
		{
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
	    std::string* error_out, bool attach_if_exists)
	{
		if (!Connect(error_out)) return false;
		nlohmann::json environment_json = nlohmann::json::object();
		for (const auto& [name, value] : environment) environment_json[name] = value;
		nlohmann::json response;
		return Request({{"type", "process.start"}, {"sessionId", session_id},
		                {"cwd", working_directory.string()}, {"argv", argv},
		                {"environment", std::move(environment_json)},
		                {"attachIfExists", attach_if_exists}},
		               response, error_out);
	}

	bool RunnerClient::WriteProcess(const std::string& session_id, std::string_view bytes,
	                                std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "process.write"}, {"sessionId", session_id},
		                {"dataBase64", uam::base64::Encode(bytes)}},
		               response, error_out);
	}

	bool RunnerClient::OpenChannel(const std::string& channel_id, std::string* error_out,
	                               bool attach_if_exists)
	{
		if (!Connect(error_out)) return false;
		nlohmann::json response;
		return Request({{"type", "channel.open"}, {"channelId", channel_id},
		                {"attachIfExists", attach_if_exists}}, response, error_out);
	}

	bool RunnerClient::WriteChannel(const std::string& channel_id,
	                                std::string_view direction, std::string_view bytes,
	                                std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "channel.write"}, {"channelId", channel_id},
		                {"direction", direction}, {"dataBase64", uam::base64::Encode(bytes)}},
		               response, error_out);
	}

	bool RunnerClient::PollChannel(const std::string& channel_id,
	                               std::string_view direction, std::string& bytes,
	                               std::string* error_out)
	{
		nlohmann::json response;
		if (!Request({{"type", "channel.poll"}, {"channelId", channel_id},
		              {"direction", direction}}, response, error_out)) return false;
		if (!response.contains("result") || !response["result"].is_object() ||
		    !uam::base64::Decode(response["result"].value("dataBase64", ""), bytes))
		{
			if (error_out != nullptr) *error_out = "The runner channel returned invalid data.";
			return false;
		}
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

	bool RunnerClient::CloseProcessInput(const std::string& session_id, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "process.closeInput"}, {"sessionId", session_id}}, response,
		               error_out);
	}

	bool RunnerClient::PollProcess(const std::string& session_id, ProcessPollResult& result,
	                               std::string* error_out)
	{
		nlohmann::json response;
		if (!Request({{"type", "process.poll"}, {"sessionId", session_id}}, response,
		             error_out))
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
		return true;
	}

	bool RunnerClient::StopProcess(const std::string& session_id, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "process.stop"}, {"sessionId", session_id}}, response,
		               error_out);
	}

	bool RunnerClient::RemoveProcess(const std::string& session_id, std::string* error_out)
	{
		nlohmann::json response;
		return Request({{"type", "process.remove"}, {"sessionId", session_id}}, response,
		               error_out);
	}

	void RunnerClient::Disconnect()
	{
		if (!m_connected) return;
		m_processService.StopStdioProcess(m_bridge, true);
		m_received.clear();
		m_connected = false;
	}
}
