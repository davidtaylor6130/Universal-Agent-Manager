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
#include <deque>
#include <mutex>
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
		struct DecodedProxySpec
		{
			std::string session_id;
			std::filesystem::path working_directory;
			std::vector<std::string> argv;
			std::vector<std::pair<std::string, std::string>> environment;
		};

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

#if defined(__APPLE__)
		void RunLocalMcpRelay(std::stop_token stop_token, const std::string& ssh_alias,
		                      const std::string& platform, const std::string& version,
		                      const std::string& runner_directory,
		                      DecodedProxySpec spec)
		{
			auto& process_service = uam::platform_macos_impl::GetMacProcessService();
			RunnerClient channel(process_service, SshBridgeArgv(
			                         ssh_alias, platform, version, runner_directory),
			                     UAM_REMOTE_RUNNER_VERSION);
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
				(void)channel.CloseChannel(spec.session_id);
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
				if (!channel.PollChannel(spec.session_id, "remoteToDesktop", input, &error)) break;
				if (!input.empty() && !process_service.WriteToStdioProcess(
				                          process, input.data(), input.size(), &error))
					break;
				int exit_code = -1;
				running = !process_service.PollStdioProcessExited(process, &exit_code);
				if (output <= 0 && input.empty())
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (running) process_service.StopStdioProcess(process, true);
			process_service.CloseStdioProcessHandles(process);
			(void)channel.CloseChannel(spec.session_id);
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
	    const std::vector<std::pair<std::string, std::string>>& environment)
	{
		nlohmann::json environment_json = nlohmann::json::object();
		for (const auto& [name, value] : environment) environment_json[name] = value;
		return uam::base64::Encode(nlohmann::json{
		    {"sessionId", session_id}, {"cwd", working_directory.string()}, {"argv", argv},
		    {"environment", std::move(environment_json)},
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
	                    const std::string& version, const std::string& runner_directory)
	{
#if !defined(__APPLE__)
		(void)ssh_alias;
		std::cerr << "Remote process proxy is not yet available on this platform.\n";
		return 70;
#else
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
		const auto encoded = uam::env::GetNonEmptyString(kRemoteProcessSpecEnvironment);
		(void)unsetenv(kRemoteProcessSpecEnvironment);
		const std::optional<DecodedProxySpec> spec = encoded ? DecodeProxySpec(*encoded) : std::nullopt;
		if (!spec)
		{
			std::cerr << "Remote process specification is missing or invalid.\n";
			return 2;
		}

		RunnerClient client(uam::platform_macos_impl::GetMacProcessService(),
		                    SshBridgeArgv(ssh_alias, platform, version, runner_directory),
		                    UAM_REMOTE_RUNNER_VERSION);
		std::string error;
		const std::string& session_id = spec->session_id;
		if (!client.Connect(&error) ||
		    !client.StartProcess(session_id, spec->working_directory, spec->argv, spec->environment,
		                         &error, true))
		{
			std::cerr << (error.empty() ? "Remote process could not start." : error) << '\n';
			return 70;
		}

		const int previous_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
		if (previous_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK) < 0)
		{
			(void)client.StopProcess(session_id);
			std::cerr << "Remote process proxy could not configure stdin.\n";
			return 70;
		}
		bool input_closed = false;
		std::string pending_input;
		std::optional<std::jthread> mcp_relay;
		std::array<char, 16 * 1024> input{};
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
		for (;;)
		{
			if (!input_closed)
			{
				const ssize_t count = read(STDIN_FILENO, input.data(), input.size());
				if (count > 0)
				{
					pending_input.append(input.data(), static_cast<std::size_t>(count));
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
							                  runner_directory,
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
				}
				if (count == 0)
				{
					if (!pending_input.empty() && !write_remote(std::exchange(pending_input, {})))
					{
						std::cerr << error << '\n';
						return 70;
					}
					input_closed = true;
					(void)client.CloseProcessInput(session_id);
				}
				else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				{
					std::cerr << "Remote process proxy could not read stdin.\n";
					return 70;
				}
			}

			ProcessPollResult polled;
			if (!client.PollProcess(session_id, polled, &error))
			{
				std::cerr << (error.empty() ? "Remote process polling failed." : error) << '\n';
				return 70;
			}
			if (!polled.standard_output.empty())
			{
				std::cout.write(polled.standard_output.data(),
				                static_cast<std::streamsize>(polled.standard_output.size()));
				std::cout.flush();
			}
			if (!polled.standard_error.empty())
			{
				std::cerr.write(polled.standard_error.data(),
				                static_cast<std::streamsize>(polled.standard_error.size()));
				std::cerr.flush();
			}
			if (!polled.running)
			{
				(void)client.RemoveProcess(session_id);
				return polled.exit_code < 0 ? 70 : polled.exit_code;
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
				break;
			if (count == 0)
			{
				(void)channel.CloseChannel(channel_id);
				return 0;
			}
			if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
			std::string output;
			if (!channel.PollChannel(channel_id, "desktopToRemote", output, &error)) break;
			if (!output.empty())
			{
				std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
				std::cout.flush();
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
		std::mutex input_mutex;
		std::deque<std::string> input;
		std::atomic<bool> input_closed{false};
		std::atomic<bool> input_failed{false};
		std::size_t input_bytes = 0;
		std::jthread reader([&]
		{
			std::array<char, 16 * 1024> bytes{};
			for (;;)
			{
				DWORD read = 0;
				if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes.data(),
				              static_cast<DWORD>(bytes.size()), &read, nullptr) || read == 0) break;
				std::scoped_lock lock(input_mutex);
				if (input_bytes + read > 1024 * 1024)
				{
					input_failed.store(true, std::memory_order_release);
					break;
				}
				input.emplace_back(bytes.data(), read);
				input_bytes += read;
			}
			input_closed.store(true, std::memory_order_release);
		});
		for (;;)
		{
			std::string pending;
			{
				std::scoped_lock lock(input_mutex);
				if (!input.empty())
				{
					pending = std::move(input.front());
					input_bytes -= pending.size();
					input.pop_front();
				}
			}
			if (!pending.empty() && !channel.WriteChannel(
			        channel_id, "remoteToDesktop", pending, &error))
			{
				input_failed.store(true, std::memory_order_release);
				break;
			}
			std::string output;
			if (!channel.PollChannel(channel_id, "desktopToRemote", output, &error))
			{
				input_failed.store(true, std::memory_order_release);
				break;
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
						input_failed.store(true, std::memory_order_release);
						break;
					}
					offset += written;
				}
				if (input_failed.load(std::memory_order_acquire)) break;
			}
			if (input_closed.load(std::memory_order_acquire) && pending.empty()) break;
			if (pending.empty() && output.empty()) Sleep(10);
		}
		(void)channel.CloseChannel(channel_id);
		#if defined(__MINGW32__)
			(void)CancelSynchronousIo(reinterpret_cast<HANDLE>(reader.native_handle()));
		#else
			(void)CancelSynchronousIo(reader.native_handle());
		#endif
		reader.join();
		return !input_failed.load(std::memory_order_acquire) && error.empty() ? 0 : 70;
#endif
	}
}
