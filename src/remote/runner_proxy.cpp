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

#if defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <unistd.h>
namespace uam::platform_macos_impl
{
	IPlatformProcessService& GetMacProcessService();
}
#elif defined(_WIN32)
#include <windows.h>
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
			    !result.working_directory.is_absolute())
				return std::nullopt;
			return result;
		}

#if defined(__APPLE__)
		void RunLocalMcpRelay(std::stop_token stop_token, const std::string& ssh_alias,
		                      DecodedProxySpec spec)
		{
			auto& process_service = uam::platform_macos_impl::GetMacProcessService();
			RunnerClient channel(process_service, SshBridgeArgv(ssh_alias),
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
#elif defined(_WIN32)
		std::array<wchar_t, 32768> buffer{};
		const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (size == 0 || size == buffer.size()) return {};
		const std::filesystem::path executable(std::wstring_view(buffer.data(), size));
		return executable.parent_path() / "remote" / "uam-runner.exe";
#else
		return {};
#endif
		return executable.parent_path() / "uam-runner";
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
	    const std::string& ssh_alias, const std::filesystem::path& working_directory,
	    const std::vector<std::string>& argv)
	{
		if (!uam::execution_hosts::IsSafeSshAlias(ssh_alias) || argv.empty() ||
		    !working_directory.is_absolute())
			return {};
		return {"ssh", "-tt", "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes",
		        "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o",
		        "ServerAliveCountMax=3", ssh_alias,
		        "~/.local/share/uam/runner/current/uam-runner", "terminal",
		        BuildProcessProxySpec("terminal", working_directory, argv, {})};
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
#if !defined(__APPLE__)
		(void)encoded_spec;
		std::cerr << "Remote terminal execution is not yet available on this platform.\n";
		return 70;
#else
		std::string decoded;
		if (!uam::base64::Decode(encoded_spec, decoded)) return 2;
		const nlohmann::json spec = nlohmann::json::parse(decoded, nullptr, false);
		if (!spec.is_object() || !spec.contains("cwd") || !spec["cwd"].is_string() ||
		    !spec.contains("argv") || !spec["argv"].is_array() ||
		    !spec.value("environment", nlohmann::json::object()).empty())
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

	int RunProcessProxy(const std::string& ssh_alias)
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
		const auto encoded = uam::env::GetNonEmptyString(kRemoteProcessSpecEnvironment);
		(void)unsetenv(kRemoteProcessSpecEnvironment);
		const std::optional<DecodedProxySpec> spec = encoded ? DecodeProxySpec(*encoded) : std::nullopt;
		if (!spec)
		{
			std::cerr << "Remote process specification is missing or invalid.\n";
			return 2;
		}

		RunnerClient client(uam::platform_macos_impl::GetMacProcessService(),
		                    SshBridgeArgv(ssh_alias), UAM_REMOTE_RUNNER_VERSION);
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
							mcp_relay.emplace(RunLocalMcpRelay, ssh_alias, std::move(*mcp_spec));
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
#if !defined(__APPLE__)
		(void)channel_id;
		(void)socket_path;
		return 70;
#else
		const std::filesystem::path executable =
		    uam::platform_macos_impl::GetMacProcessService().ResolveCurrentExecutablePath();
		RunnerClient channel(uam::platform_macos_impl::GetMacProcessService(),
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
}
