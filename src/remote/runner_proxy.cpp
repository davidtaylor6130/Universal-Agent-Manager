#include "remote/runner_proxy.h"

#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/utils/base64.h"
#include "common/utils/env_utils.h"
#include "remote/runner_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifndef UAM_REMOTE_RUNNER_VERSION
#define UAM_REMOTE_RUNNER_VERSION "development"
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <unistd.h>
#if defined(__APPLE__)
namespace uam::platform_macos_impl
{
	IPlatformProcessService& GetMacProcessService();
}
#else
namespace uam::platform_linux_impl
{
	IPlatformProcessService& GetLinuxProcessService();
}
#endif
#elif defined(_WIN32)
#include "common/platform/platform_services_windows_impl_internal.h"
#include <fcntl.h>
#include <io.h>
#include <windows.h>
namespace uam::platform_windows_impl
{
	IPlatformProcessService& GetWindowsProcessService();
}
#endif

namespace uam::remote
{
	namespace
	{
		bool IsControlField(std::string_view value, std::size_t max_size)
		{
			return !value.empty() && value.size() <= max_size &&
			       std::ranges::all_of(value, [](unsigned char character)
			       {
				       return std::isalnum(character) != 0 || character == '-' ||
				              character == '_' || character == '.';
			       });
		}

		IPlatformProcessService& ProxyProcessService()
		{
#if defined(__APPLE__)
			return uam::platform_macos_impl::GetMacProcessService();
#elif defined(_WIN32)
			return uam::platform_windows_impl::GetWindowsProcessService();
#else
			return uam::platform_linux_impl::GetLinuxProcessService();
#endif
		}

		struct DecodedProxySpec
		{
			std::string session_id;
			std::filesystem::path working_directory;
			std::vector<std::string> argv;
			std::vector<std::pair<std::string, std::string>> environment;
			bool attach_only = false;
			std::string delivery_token;
			std::uintmax_t delivered_stdout_cursor = 0;
			std::uintmax_t delivered_stderr_cursor = 0;
		};

#if defined(_WIN32)
		class WindowsPipeInput
		{
		  public:
			void Drain(std::string& output)
			{
				if (m_closed || m_failed) return;
				HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
				DWORD available = 0;
				if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
				{
					const DWORD error = GetLastError();
					m_closed = error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
					m_failed = !m_closed;
					return;
				}
				if (available == 0) return;
				if (output.size() + available > 1024 * 1024)
				{
					m_failed = true;
					return;
				}
				std::array<char, 16 * 1024> buffer{};
				const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
				DWORD read = 0;
				if (!ReadFile(input, buffer.data(), requested, &read, nullptr))
				{
					const DWORD error = GetLastError();
					m_closed = error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
					m_failed = !m_closed;
					return;
				}
				if (read == 0) m_closed = true;
				else output.append(buffer.data(), read);
			}

			bool Closed() const { return m_closed; }
			bool Failed() const { return m_failed; }

		  private:
			bool m_closed = false;
			bool m_failed = false;
		};
#endif

		std::optional<DecodedProxySpec> DecodeProxySpec(std::string_view encoded)
		{
			std::string decoded;
			if (!uam::base64::Decode(encoded, decoded)) return std::nullopt;
			const nlohmann::json spec = nlohmann::json::parse(decoded, nullptr, false);
			if (!spec.is_object() || !spec.contains("sessionId") || !spec["sessionId"].is_string() ||
			    !spec.contains("cwd") || !spec["cwd"].is_string() || !spec.contains("argv") ||
			    !spec["argv"].is_array() || !spec.contains("environment") ||
			    !spec["environment"].is_object())
				return std::nullopt;
			DecodedProxySpec result;
			try
			{
				result.session_id = spec["sessionId"].get<std::string>();
				result.working_directory = spec["cwd"].get<std::string>();
				result.argv = spec["argv"].get<std::vector<std::string>>();
				for (const auto& [name, value] : spec["environment"].items())
					result.environment.emplace_back(name, value.get<std::string>());
				result.attach_only = spec.value("attachOnly", false);
				result.delivery_token = spec.value("deliveryToken", "");
				result.delivered_stdout_cursor = spec.value(
				    "deliveredStdoutCursor", static_cast<std::uintmax_t>(0));
				result.delivered_stderr_cursor = spec.value(
				    "deliveredStderrCursor", static_cast<std::uintmax_t>(0));
			}
			catch (...)
			{
				return std::nullopt;
			}
			if (result.session_id.empty() || result.argv.empty() ||
			    result.working_directory.empty())
				return std::nullopt;
			return result;
		}

		bool WriteAndAcknowledgeProcessOutput(RunnerClient& client,
		                                      const std::string& session_id,
		                                      const ProcessPollResult& polled,
		                                      std::string& error)
		{
			if (!polled.standard_output.empty())
			{
				std::cout.write(polled.standard_output.data(),
				                static_cast<std::streamsize>(polled.standard_output.size()));
				std::cout.flush();
				if (!std::cout)
				{
					error = "Remote process stdout could not be delivered.";
					return false;
				}
			}
			if (!polled.standard_error.empty())
			{
				std::cerr.write(polled.standard_error.data(),
				                static_cast<std::streamsize>(polled.standard_error.size()));
				std::cerr.flush();
				if (!std::cerr)
				{
					error = "Remote process stderr could not be delivered.";
					return false;
				}
			}
			return client.AcknowledgeProcessOutput(session_id, polled, &error);
		}


#if defined(__APPLE__) || defined(_WIN32)
		void RunLocalMcpRelay(std::stop_token stop_token, const std::string& ssh_alias,
		                      const std::string& platform, const std::string& version,
		                      const std::string& runner_directory,
		                      int protocol_version, DecodedProxySpec spec)
		{
			auto& process_service = ProxyProcessService();
			RunnerClient channel(process_service, SshBridgeArgv(
			                         ssh_alias, platform, version, runner_directory,
			                         protocol_version),
			                     version, protocol_version);
			std::string error;
			if (!channel.OpenChannel(spec.session_id, &error, true))
			{
				std::cerr << error << '\n';
				return;
			}
			uam::platform::StdioProcessPlatformFields process;
			if (!process_service.StartStdioProcess(process, spec.working_directory, spec.argv,
			                                       &error, spec.environment))
			{
					std::cerr << (error.empty() ? "UAM control MCP could not start." : error) << '\n';
				return;
			}
			std::array<char, 16 * 1024> buffer{};
			bool running = true;
			while (!stop_token.stop_requested() && running)
			{
				const std::ptrdiff_t output = process_service.ReadStdioProcessStdout(
				    process, buffer.data(), buffer.size(), &error);
				if (output > 0 && !channel.WriteChannel(
				                      spec.session_id, "desktopToRemote",
				                      std::string_view(buffer.data(), static_cast<std::size_t>(output)),
				                      &error))
					break;
				const std::ptrdiff_t diagnostics = process_service.ReadStdioProcessStderr(
				    process, buffer.data(), buffer.size(), &error);
				if (diagnostics > 0)
				{
					std::cerr.write(buffer.data(), static_cast<std::streamsize>(diagnostics));
					std::cerr.flush();
				}
				std::string input;
				std::uintmax_t input_cursor = 0;
				if (!channel.PollChannel(spec.session_id, "remoteToDesktop", input, &error,
				                         &input_cursor))
				{
					channel.Disconnect();
					if (!channel.OpenChannel(spec.session_id, &error, true)) break;
					continue;
				}
				if (!input.empty() && !process_service.WriteToStdioProcess(
				                          process, input.data(), input.size(), &error))
					break;
				if (!input.empty() && !channel.AcknowledgeChannel(
				                          spec.session_id, "remoteToDesktop", input_cursor, &error))
					break;
				int exit_code = -1;
				running = !process_service.PollStdioProcessExited(process, &exit_code);
				if (output <= 0 && input.empty())
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (running) process_service.StopStdioProcess(process, true);
			process_service.CloseStdioProcessHandles(process);
			// The channel belongs to the provider session, not this disposable relay.
			// A replacement proxy can attach and resume at the acknowledged cursors.
		}
#endif
	}

	std::filesystem::path PackagedRunnerPath()
	{
#if defined(__APPLE__)
		std::uint32_t size = 0;
		(void)_NSGetExecutablePath(nullptr, &size);
		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
		buffer.resize(std::char_traits<char>::length(buffer.c_str()));
		const std::filesystem::path executable = std::filesystem::weakly_canonical(buffer);
		if (executable.parent_path().filename() == "MacOS")
			return executable.parent_path().parent_path() / "Resources" / "remote" / "uam-runner";
		return executable.parent_path() / "uam-runner";
#elif defined(_WIN32)
		std::array<wchar_t, 32768> buffer{};
		const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (size == 0 || size == buffer.size()) return {};
		const std::filesystem::path executable(std::wstring_view(buffer.data(), size));
		return executable.parent_path() / "remote" / "uam-runner.exe";
#else
		return {};
#endif
	}

	std::string BuildProcessProxySpec(
	    const std::string& session_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment,
	    bool attach_only, const std::string& delivery_token,
	    std::uintmax_t delivered_stdout_cursor,
	    std::uintmax_t delivered_stderr_cursor)
	{
		nlohmann::json environment_json = nlohmann::json::object();
		for (const auto& [name, value] : environment) environment_json[name] = value;
		return uam::base64::Encode(nlohmann::json{
		    {"sessionId", session_id}, {"cwd", working_directory.string()}, {"argv", argv},
		    {"environment", std::move(environment_json)}, {"attachOnly", attach_only},
		    {"deliveryToken", delivery_token},
		    {"deliveredStdoutCursor", delivered_stdout_cursor},
		    {"deliveredStderrCursor", delivered_stderr_cursor},
		}.dump());
	}

	std::vector<std::string> BuildRemoteTerminalSshArgv(
	    const std::string& ssh_alias, const std::string& platform,
	    const std::string& version, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv, const std::string& runner_directory)
	{
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias) || argv.empty() ||
		    !uam::execution_hosts::IsAbsoluteRemotePath(platform,
		        working_directory.string()))
			return {};
		if (SshBridgeArgv(ssh_alias, platform, version, runner_directory).empty()) return {};
		std::string command;
		if (platform == "linux" || platform == "macos" || platform == "Linux" ||
		    platform == "Darwin")
		{
			const std::string runner = "~/" + uam::execution_hosts::RunnerDirectory(
			    platform, runner_directory) + "/" + version + "/uam-runner";
			command = runner + " terminal " +
			          BuildProcessProxySpec("terminal", working_directory, argv, {});
		}
		else if (platform == "windows" || platform == "Windows")
		{
			command = "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass "
			          "-Command \"& (Join-Path $HOME '" + uam::execution_hosts::RunnerDirectory(
			              platform, runner_directory) + "/" + version +
			          "/uam-runner.exe') terminal '" +
			          BuildProcessProxySpec("terminal", working_directory, argv, {}) + "'\"";
		}
		else return {};
		return {"ssh", "-tt", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes",
		        "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o",
		        "ServerAliveCountMax=3", ssh_alias, std::move(command)};
	}

	std::string BuildRemoteMcpControlLine(
	    const std::string& channel_id, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv,
	    const std::vector<std::pair<std::string, std::string>>& environment)
	{
		if (channel_id.empty() || argv.empty() || !working_directory.is_absolute()) return {};
		return std::string(kRemoteMcpControlPrefix) +
		       BuildProcessProxySpec(channel_id, working_directory, argv, environment) + "\n";
	}

	std::string BuildRemoteInputDeliveryLine(
	    std::string_view delivery_token, std::string_view delivery_id,
	    std::string_view payload)
	{
		if (!IsControlField(delivery_token, 256) || !IsControlField(delivery_id, 96) ||
		    payload.empty() || payload.size() > 2 * 1024 * 1024)
			return {};
		return std::string(kRemoteInputDeliveryPrefix) + std::string(delivery_token) + " " +
		       std::string(delivery_id) + " " + uam::base64::Encode(payload) + "\n";
	}

	int RunTerminalProcess(const std::string& encoded_spec)
	{
#if defined(_WIN32)
		std::string decoded;
		if (!uam::base64::Decode(encoded_spec, decoded)) return 2;
		const nlohmann::json spec = nlohmann::json::parse(decoded, nullptr, false);
		if (!spec.is_object() || !spec.contains("cwd") || !spec["cwd"].is_string() ||
		    !spec.contains("argv") || !spec["argv"].is_array() ||
		    !spec.contains("environment") || !spec["environment"].is_object() ||
		    !spec["environment"].empty()) return 2;
		std::vector<std::string> arguments;
		try { arguments = spec["argv"].get<std::vector<std::string>>(); }
		catch (...) { return 2; }
		if (arguments.empty()) return 2;
		const std::filesystem::path cwd = spec["cwd"].get<std::string>();
		if (!cwd.is_absolute()) return 2;
		const auto launch = uam::platform_windows_impl::BuildWindowsLaunchCommand(arguments);
		std::wstring command = uam::platform_windows_impl::WideFromUtf8(launch.command_line);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION process{};
		if (command.empty() || !CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
		    CREATE_SUSPENDED, nullptr, cwd.c_str(), &startup, &process)) return 70;
		HANDLE job = nullptr;
		std::string job_error;
		if (!uam::platform_windows_impl::CreateKillOnCloseJobForProcess(
		        process.hProcess, &job, &job_error))
		{
			TerminateProcess(process.hProcess, 70);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			return 70;
		}
		(void)ResumeThread(process.hThread);
		CloseHandle(process.hThread);
		(void)WaitForSingleObject(process.hProcess, INFINITE);
		DWORD exit_code = 70;
		(void)GetExitCodeProcess(process.hProcess, &exit_code);
		CloseHandle(process.hProcess);
		CloseHandle(job);
		return static_cast<int>(exit_code);
#elif !defined(__APPLE__) && !defined(__linux__)
		(void)encoded_spec;
		std::cerr << "Remote terminal execution is not yet available on this platform.\n";
		return 70;
#else
		std::string decoded;
		if (!uam::base64::Decode(encoded_spec, decoded)) return 2;
		const nlohmann::json spec = nlohmann::json::parse(decoded, nullptr, false);
		if (!spec.is_object() || !spec.contains("cwd") || !spec["cwd"].is_string() ||
		    !spec.contains("argv") || !spec["argv"].is_array() ||
		    !spec.contains("environment") || !spec["environment"].is_object() ||
		    !spec["environment"].empty())
			return 2;
		std::vector<std::string> arguments;
		try
		{
			arguments = spec["argv"].get<std::vector<std::string>>();
		}
		catch (...)
		{
			return 2;
		}
		const std::filesystem::path cwd = spec["cwd"].get<std::string>();
		if (arguments.empty() || !cwd.is_absolute() || chdir(cwd.c_str()) != 0) return 2;
		std::vector<char*> native_arguments;
		native_arguments.reserve(arguments.size() + 1);
		for (std::string& argument : arguments) native_arguments.push_back(argument.data());
		native_arguments.push_back(nullptr);
		execvp(native_arguments.front(), native_arguments.data());
		return 70;
#endif
	}

	int RunProcessProxy(const std::string& ssh_alias, const std::string& platform,
	                    const std::string& version, const std::string& runner_directory,
	                    int protocol_version)
	{
#if !defined(__APPLE__) && !defined(_WIN32)
		(void)ssh_alias;
		std::cerr << "Remote process proxy is not yet available on this platform.\n";
		return 70;
#else
		#if defined(_WIN32)
		(void)_setmode(_fileno(stdin), _O_BINARY);
		(void)_setmode(_fileno(stdout), _O_BINARY);
		(void)_setmode(_fileno(stderr), _O_BINARY);
		#endif
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias))
		{
			std::cerr << "Use one exact alias from ~/.ssh/config.\n";
			return 2;
		}
		if (!uam::execution_hosts::IsSafeRunnerDirectory(runner_directory))
		{
			std::cerr << "The remote runner folder is invalid.\n";
			return 2;
		}
		if (protocol_version < 2 || protocol_version > kRunnerProtocolVersion)
		{
			std::cerr << "The remote runner protocol is unsupported.\n";
			return 2;
		}
		const auto encoded = uam::env::GetNonEmptyString(kRemoteProcessSpecEnvironment);
#if defined(_WIN32)
		(void)_putenv_s(kRemoteProcessSpecEnvironment, "");
#else
		(void)unsetenv(kRemoteProcessSpecEnvironment);
#endif
		const std::optional<DecodedProxySpec> spec = encoded ? DecodeProxySpec(*encoded) : std::nullopt;
		if (!spec)
		{
			std::cerr << "Remote process specification is missing or invalid.\n";
			return 2;
		}

		auto& process_service = ProxyProcessService();
		RunnerClient client(process_service,
		                    SshBridgeArgv(ssh_alias, platform, version, runner_directory,
		                                  protocol_version),
		                    version, protocol_version);
		std::string error;
		const std::string& session_id = spec->session_id;
		std::optional<ProcessPollResult> prefetched_poll;
		if (!client.Connect(&error))
		{
			std::cerr << (error.empty() ? "Remote process could not start." : error) << '\n';
			return 70;
		}
		if (protocol_version >= 3)
			client.SetProcessControlToken(session_id, spec->delivery_token);
		if (spec->attach_only)
		{
			ProcessPollResult persisted;
			persisted.stdout_cursor = spec->delivered_stdout_cursor;
			persisted.stderr_cursor = spec->delivered_stderr_cursor;
			const bool resume_persisted_output = !spec->delivery_token.empty() ||
			    persisted.stdout_cursor != 0 || persisted.stderr_cursor != 0;
			ProcessPollResult existing;
			if (!client.PollProcess(session_id, existing, &error, {}, nullptr,
			                        resume_persisted_output ? &persisted : nullptr))
			{
				std::cerr << (error.empty() ? "The remote process is no longer available." : error) << '\n';
				return 70;
			}
			std::cout << R"({"jsonrpc":"2.0","method":"uam/remoteAttached"})" << '\n';
			std::cout.flush();
			prefetched_poll = std::move(existing);
		}
		else if (!client.StartProcess(session_id, spec->working_directory, spec->argv,
		                              spec->environment, &error, false,
		                              spec->delivery_token))
		{
			std::cerr << (error.empty() ? "Remote process could not start." : error) << '\n';
			return 70;
		}
		RunnerClient poll_client(process_service,
		                         SshBridgeArgv(ssh_alias, platform, version,
		                                       runner_directory, protocol_version), version,
		                         protocol_version);
		if (protocol_version >= 3)
			poll_client.SetProcessControlToken(session_id, spec->delivery_token);
		if (!poll_client.Connect(&error))
		{
			std::cerr << (error.empty() ? "Remote process polling could not start." : error) << '\n';
			return 70;
		}
		RunnerClient* poll_channel = &poll_client;
		ProcessPollResult first_poll;
		if (prefetched_poll.has_value())
		{
			// The attach probe already supplied the first durable batch.
		}
		else if (poll_client.PollProcess(session_id, first_poll, &error))
		{
			prefetched_poll = std::move(first_poll);
		}
		else if (error.find("does not exist") != std::string::npos)
		{
			// bridge-direct is an isolated test/development transport. Production
			// bridges share the resident runner service and use the independent
			// polling channel so an explicit stop can interrupt a blocked poll.
			poll_channel = &client;
			error.clear();
		}
		else
		{
			std::cerr << (error.empty() ? "Remote process polling could not start." : error) << '\n';
			return 70;
		}

		#if defined(_WIN32)
		WindowsPipeInput windows_input;
		#else
		const int previous_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		if (previous_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK) < 0)
		{
			std::cerr << "Remote process proxy could not configure stdin.\n";
			return 70;
		}
		#endif
		bool input_closed = false;
		std::string pending_input;
		std::optional<std::jthread> mcp_relay;
		std::array<char, 16 * 1024> input{};
		bool stop_completed = false;
		std::string pending_stdout;
		std::string pending_stderr;
		std::uintmax_t pending_stdout_base = 0;
		bool pending_stdout_base_set = false;
		ProcessPollResult latest_cursor;
		bool has_latest_cursor = false;
		std::optional<ProcessPollResult> awaiting_output_ack;
		std::size_t awaiting_stdout_bytes = 0;
		bool source_exited = false;
		int source_exit_code = -1;
		bool source_exit_announced = false;
		bool legacy_exit_observed = false;
		int legacy_exit_code = -1;
		int legacy_quiet_polls = 0;
		std::chrono::steady_clock::time_point legacy_drain_deadline{};
		const auto expected_output_ack = [&](const ProcessPollResult& delivery)
		{
			return std::string(kRemoteOutputAckPrefix) + spec->delivery_token + " " +
			       std::to_string(delivery.stdout_cursor) + " " +
			       std::to_string(delivery.stderr_cursor) + "\n";
		};
		const auto queue_output_delivery = [&](std::size_t stdout_bytes,
		                                       bool terminate_partial_line)
		{
			ProcessPollResult delivery = latest_cursor;
			delivery.stdout_cursor = pending_stdout_base + stdout_bytes;
			delivery.standard_output.assign(pending_stdout.data(), stdout_bytes);
			if (terminate_partial_line) delivery.standard_output.push_back('\n');
			delivery.standard_error = pending_stderr;
			delivery.acknowledgement_required = true;
			if (!delivery.standard_output.empty())
			{
				std::cout.write(delivery.standard_output.data(),
				                static_cast<std::streamsize>(delivery.standard_output.size()));
			}
			if (!delivery.standard_error.empty())
			{
				std::cerr.write(delivery.standard_error.data(),
				                static_cast<std::streamsize>(delivery.standard_error.size()));
				std::cerr.flush();
			}
			std::cout << kRemoteOutputMarkerPrefix << spec->delivery_token << ' '
			          << delivery.stdout_cursor << ' ' << delivery.stderr_cursor << '\n';
			std::cout.flush();
			if (!std::cout || !std::cerr)
			{
				error = "Remote process output could not be delivered.";
				return false;
			}
			awaiting_stdout_bytes = stdout_bytes;
			awaiting_output_ack = std::move(delivery);
			return true;
		};
		const auto write_remote = [&](std::string_view bytes)
		{
			while (!bytes.empty())
			{
				const std::size_t size = std::min<std::size_t>(bytes.size(), 256 * 1024);
				if (!client.WriteProcess(session_id, bytes.substr(0, size), &error)) return false;
				bytes.remove_prefix(size);
			}
			return true;
		};
		const auto write_remote_delivery = [&](std::string_view bytes,
		                                       std::string_view delivery_id)
		{
			const std::size_t chunk_count =
			    (bytes.size() + 256 * 1024 - 1) / (256 * 1024);
			for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index)
			{
				const std::size_t size = std::min<std::size_t>(bytes.size(), 256 * 1024);
				const std::string chunk_delivery_id = std::string(delivery_id) + "." +
				    std::to_string(chunk_index) + "." + std::to_string(chunk_count);
				if (!client.WriteProcess(session_id, bytes.substr(0, size), &error,
				                         chunk_delivery_id))
					return false;
				bytes.remove_prefix(size);
			}
			return true;
		};
		const auto read_proxy_input = [&]() -> int
		{
		#if defined(_WIN32)
			windows_input.Drain(pending_input);
			if (windows_input.Failed()) return -1;
			return windows_input.Closed() ? 0 : 1;
		#else
			const ssize_t count = read(STDIN_FILENO, input.data(), input.size());
			if (count > 0) pending_input.append(input.data(), static_cast<std::size_t>(count));
			if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
			return count == 0 ? 0 : 1;
		#endif
		};
		const auto stop_while_polling = [&]
		{
			if (input_closed || stop_completed) return stop_completed;
			(void)read_proxy_input();
			const std::size_t stop = pending_input.find(kRemoteStopControlLine);
			if (stop != std::string::npos &&
			    (stop == 0 || pending_input[stop - 1] == '\n'))
			{
				if (!client.StopProcess(session_id, &error) ||
				    !client.RemoveProcess(session_id, &error))
					return true;
				stop_completed = true;
				return true;
			}
			return false;
		};
		for (;;)
		{
			if (!input_closed)
			{
				const int input_state = read_proxy_input();
				if (input_state < 0)
				{
					std::cerr << "Remote process proxy could not read stdin.\n";
					return 70;
				}
				for (std::size_t newline = pending_input.find('\n'); newline != std::string::npos;
				     newline = pending_input.find('\n'))
				{
					const std::string line = pending_input.substr(0, newline + 1);
					pending_input.erase(0, newline + 1);
					if (line == kRemoteStopControlLine)
					{
						if (!client.StopProcess(session_id, &error) ||
						    !client.RemoveProcess(session_id, &error))
						{
							std::cerr << error << '\n';
							return 70;
						}
						return 0;
					}
					if (awaiting_output_ack.has_value() &&
					    line == expected_output_ack(*awaiting_output_ack))
					{
						if (!poll_channel->AcknowledgeProcessOutput(
						        session_id, *awaiting_output_ack, &error))
						{
							std::cerr << error << '\n';
							return 70;
						}
						pending_stdout.erase(0, awaiting_stdout_bytes);
						pending_stdout_base = awaiting_output_ack->stdout_cursor;
						pending_stdout_base_set = true;
						pending_stderr.clear();
						awaiting_output_ack.reset();
						awaiting_stdout_bytes = 0;
						continue;
					}
					if (source_exit_announced &&
					    line == std::string(kRemoteSourceExitAckPrefix) +
					                spec->delivery_token + " " +
					                std::to_string(source_exit_code) + "\n")
					{
						if (!client.RemoveProcess(session_id, &error))
						{
							std::cerr << error << '\n';
							return 70;
						}
						return source_exit_code < 0 ? 70 : source_exit_code;
					}
					if (line.starts_with(kRemoteInputDeliveryPrefix))
					{
						if (protocol_version < 3)
						{
							std::cerr << "This remote helper cannot acknowledge input delivery.\n";
							return 70;
						}
						std::string_view fields(
						    line.data() + kRemoteInputDeliveryPrefix.size(),
						    line.size() - kRemoteInputDeliveryPrefix.size() - 1);
						const std::size_t token_end = fields.find(' ');
						const std::size_t id_end = token_end == std::string_view::npos
						    ? std::string_view::npos : fields.find(' ', token_end + 1);
						if (token_end == std::string_view::npos || id_end == std::string_view::npos ||
						    fields.substr(0, token_end) != spec->delivery_token)
						{
							std::cerr << "Remote input delivery authority is invalid.\n";
							return 70;
						}
						const std::string delivery_id(fields.substr(
						    token_end + 1, id_end - token_end - 1));
						std::string payload;
						if (!IsControlField(delivery_id, 96) ||
						    !uam::base64::Decode(fields.substr(id_end + 1), payload) ||
						    payload.empty() || payload.size() > 2 * 1024 * 1024 ||
						    !write_remote_delivery(payload, delivery_id))
						{
							std::cerr << (error.empty()
							    ? "Remote input delivery is invalid.\n" : error + "\n");
							return 70;
						}
						std::cout << kRemoteInputReceiptPrefix << spec->delivery_token << ' '
						          << delivery_id << '\n';
						std::cout.flush();
						if (!std::cout) return 70;
						continue;
					}
					if (line.starts_with(kRemoteMcpControlPrefix))
					{
						const std::string_view encoded_mcp(line.data() + kRemoteMcpControlPrefix.size(),
						                                   line.size() - kRemoteMcpControlPrefix.size() - 1);
						std::optional<DecodedProxySpec> mcp_spec = DecodeProxySpec(encoded_mcp);
						if (!mcp_spec)
						{
							std::cerr << "Remote UAM control specification is invalid.\n";
							return 70;
						}
						mcp_relay.reset();
						mcp_relay.emplace(RunLocalMcpRelay, ssh_alias, platform, version,
						                  runner_directory, protocol_version,
						                  std::move(*mcp_spec));
						continue;
					}
					if (!write_remote(line))
					{
						std::cerr << error << '\n';
						return 70;
					}
				}
				if (!pending_input.starts_with('\x1e') &&
				    pending_input.size() > kRemoteStopControlLine.size() &&
				    !write_remote(std::exchange(pending_input, {})))
				{
					std::cerr << error << '\n';
					return 70;
				}
				if (input_state == 0)
				{
					// The desktop transport disappearing is a detach, never permission to
					// terminate helper-owned work. Only the explicit stop control line above
					// may close or terminate the remote provider.
					return 0;
				}
			}
			if (awaiting_output_ack.has_value())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (source_exited)
			{
				if (!pending_stdout.empty() || !pending_stderr.empty())
				{
					if (!queue_output_delivery(pending_stdout.size(), !pending_stdout.empty()))
					{
						std::cerr << error << '\n';
						return 70;
					}
					continue;
				}
				if (!spec->delivery_token.empty())
				{
					if (!source_exit_announced)
					{
						std::cout << kRemoteSourceExitPrefix << spec->delivery_token << ' '
						          << source_exit_code << '\n';
						std::cout.flush();
						if (!std::cout) return 70;
						source_exit_announced = true;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					continue;
				}
				(void)client.RemoveProcess(session_id);
				return source_exit_code < 0 ? 70 : source_exit_code;
			}

			ProcessPollResult polled;
			bool poll_interrupted = false;
			const bool polled_ok = prefetched_poll.has_value()
			                           ? (polled = std::move(*prefetched_poll), prefetched_poll.reset(), true)
			                           : poll_channel->PollProcess(
			                                 session_id, polled, &error,
			                                 poll_channel == &poll_client ? stop_while_polling
			                                                              : std::function<bool()>{},
			                                 &poll_interrupted,
			                                 has_latest_cursor ? &latest_cursor : nullptr);
			if (!polled_ok)
			{
				if (poll_interrupted && stop_completed) return 0;
				std::cerr << (error.empty() ? "Remote process polling failed." : error) << '\n';
				return 70;
			}
			const bool durable_delivery = !spec->delivery_token.empty() &&
			    (polled.acknowledgement_required || has_latest_cursor ||
			     !pending_stdout.empty() || !pending_stderr.empty());
				if (!durable_delivery)
				{
					if (UsesDurableRemoteOutputHandshake(
					        protocol_version, spec->delivery_token) && !polled.running)
				{
					source_exited = true;
					source_exit_code = polled.exit_code;
					continue;
				}
				if (!WriteAndAcknowledgeProcessOutput(*poll_channel, session_id, polled, error))
				{
					std::cerr << error << '\n';
					return 70;
				}
				if (!polled.running)
				{
					if (protocol_version < 3)
					{
						if (!legacy_exit_observed)
						{
							legacy_exit_observed = true;
							legacy_exit_code = polled.exit_code;
							legacy_drain_deadline = std::chrono::steady_clock::now() +
							                        std::chrono::milliseconds(250);
						}
						if (polled.standard_output.empty() && polled.standard_error.empty())
							++legacy_quiet_polls;
						else
							legacy_quiet_polls = 0;
						if (legacy_quiet_polls < 3 &&
						    std::chrono::steady_clock::now() < legacy_drain_deadline)
						{
							std::this_thread::sleep_for(std::chrono::milliseconds(10));
							continue;
						}
					}
					(void)client.RemoveProcess(session_id);
					const int exit_code = legacy_exit_observed ? legacy_exit_code : polled.exit_code;
					return exit_code < 0 ? 70 : exit_code;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			if (!pending_stdout_base_set)
			{
				pending_stdout_base = polled.stdout_cursor - polled.standard_output.size();
				pending_stdout_base_set = true;
			}
			pending_stdout += polled.standard_output;
			pending_stderr += polled.standard_error;
			latest_cursor = polled;
			has_latest_cursor = true;
			source_exited = !polled.running;
			source_exit_code = polled.exit_code;
			const std::size_t newline = pending_stdout.rfind('\n');
			if (newline != std::string::npos || !pending_stderr.empty())
			{
				const std::size_t stdout_bytes = newline == std::string::npos ? 0 : newline + 1;
				if (!queue_output_delivery(stdout_bytes, false))
				{
					std::cerr << error << '\n';
					return 70;
				}
				continue;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
#endif
	}

	int RunRemoteMcpShim(const std::string& channel_id,
	                     const std::filesystem::path& socket_path)
	{
#if !defined(__APPLE__) && !defined(__linux__)
		(void)channel_id;
		(void)socket_path;
		return 70;
#else
		auto& process_service =
#if defined(__APPLE__)
		    uam::platform_macos_impl::GetMacProcessService();
#else
		    uam::platform_linux_impl::GetLinuxProcessService();
#endif
		const std::filesystem::path executable = process_service.ResolveCurrentExecutablePath();
		RunnerClient channel(process_service,
		                     {executable.string(), "bridge", "--socket", socket_path.string()},
		                     UAM_REMOTE_RUNNER_VERSION);
		std::string error;
		if (!channel.OpenChannel(channel_id, &error, true))
		{
			std::cerr << error << '\n';
			return 70;
		}
		const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
		{
			(void)channel.CloseChannel(channel_id);
			return 70;
		}
		std::array<char, 16 * 1024> input{};
		for (;;)
		{
			const ssize_t count = read(STDIN_FILENO, input.data(), input.size());
			if (count > 0 && !channel.WriteChannel(
			                     channel_id, "remoteToDesktop",
			                     std::string_view(input.data(), static_cast<std::size_t>(count)),
			                     &error))
			{
				channel.Disconnect();
				if (!channel.OpenChannel(channel_id, &error, true) ||
				    !channel.WriteChannel(channel_id, "remoteToDesktop",
				        std::string_view(input.data(), static_cast<std::size_t>(count)), &error))
					break;
			}
			if (count == 0)
			{
				(void)channel.CloseChannel(channel_id);
				return 0;
			}
			if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
			std::string output;
			std::uintmax_t output_cursor = 0;
			if (!channel.PollChannel(channel_id, "desktopToRemote", output, &error,
			                         &output_cursor))
			{
				channel.Disconnect();
				if (!channel.OpenChannel(channel_id, &error, true)) break;
				continue;
			}
			if (!output.empty())
			{
				std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
				std::cout.flush();
				if (!std::cout)
					break;
				if (!channel.AcknowledgeChannel(
				        channel_id, "desktopToRemote", output_cursor, &error))
					break;
			}
			if (count < 0 && output.empty())
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		(void)channel.CloseChannel(channel_id);
		if (!error.empty()) std::cerr << error << '\n';
		return 70;
#endif
	}

	int RunRemoteMcpShim(const std::string& channel_id)
	{
#if !defined(_WIN32)
		(void)channel_id;
		return 70;
#else
		auto& process_service = uam::platform_windows_impl::GetWindowsProcessService();
		const std::filesystem::path executable = process_service.ResolveCurrentExecutablePath();
		RunnerClient channel(process_service, {executable.string(), "bridge"},
		                     UAM_REMOTE_RUNNER_VERSION);
		std::string error;
		if (!channel.OpenChannel(channel_id, &error, true)) return 70;
		WindowsPipeInput input;
		bool input_failed = false;
		std::string pending;
		for (;;)
		{
			input.Drain(pending);
			if (input.Failed())
			{
				input_failed = true;
				break;
			}
			if (!pending.empty() && !channel.WriteChannel(
			        channel_id, "remoteToDesktop", pending, &error))
			{
				channel.Disconnect();
				if (!channel.OpenChannel(channel_id, &error, true) ||
				    !channel.WriteChannel(channel_id, "remoteToDesktop", pending, &error))
				{
					input_failed = true;
					break;
				}
			}
			pending.clear();
			std::string output;
			std::uintmax_t output_cursor = 0;
			if (!channel.PollChannel(channel_id, "desktopToRemote", output, &error,
			                         &output_cursor))
			{
				channel.Disconnect();
				if (!channel.OpenChannel(channel_id, &error, true))
				{
					input_failed = true;
					break;
				}
				continue;
			}
			if (!output.empty())
			{
				std::size_t offset = 0;
				while (offset < output.size())
				{
					DWORD written = 0;
					if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data() + offset,
					               static_cast<DWORD>(output.size() - offset), &written,
					               nullptr) || written == 0)
					{
						input_failed = true;
						break;
					}
					offset += written;
				}
				if (input_failed) break;
				if (!channel.AcknowledgeChannel(
				        channel_id, "desktopToRemote", output_cursor, &error))
				{
					input_failed = true;
					break;
				}
			}
			if (input.Closed() && pending.empty()) break;
			if (pending.empty() && output.empty()) Sleep(10);
		}
		(void)channel.CloseChannel(channel_id);
		return !input_failed && error.empty() ? 0 : 70;
#endif
	}
}
