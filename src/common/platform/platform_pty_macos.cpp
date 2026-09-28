#include "platform_services_macos_impl_internal.h"

#include <cstdio>

using namespace uam::platform_macos_impl;

namespace
{
	constexpr const char* kMacTerminalChildArgument = "--uam-mac-terminal-child";
}

namespace uam::platform
{
	std::optional<int> RunMacTerminalChildIfRequested(int argc, char* argv[])
	{
		if (argc < 2 || std::string_view(argv[1]) != kMacTerminalChildArgument) return std::nullopt;
		if (argc < 3 || argv[2][0] != '/')
		{
			std::fprintf(stderr, "Terminal startup requires an absolute provider executable.\n");
			return 126;
		}

		// posix_spawn's session creation does not acquire the slave as a controlling tty
		// on macOS. Do this after exec, without running fork-child code inside CEF.
		struct sigaction default_action{};
		default_action.sa_handler = SIG_DFL;
		sigemptyset(&default_action.sa_mask);
		for (int signal_number = 1; signal_number < NSIG; ++signal_number)
		{
			if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;
			if (sigaction(signal_number, &default_action, nullptr) != 0)
			{
				std::perror("Could not reset terminal signal disposition");
				return 126;
			}
		}
		sigset_t empty_mask;
		sigemptyset(&empty_mask);
		if (sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0 ||
		    ioctl(STDIN_FILENO, TIOCSCTTY, 0) != 0 ||
		    tcsetpgrp(STDIN_FILENO, getpgrp()) != 0)
		{
			std::perror("Could not establish provider controlling terminal");
			return 126;
		}
		execv(argv[2], argv + 2);
		std::perror("Could not execute terminal provider");
		return 127;
	}
}


namespace uam::platform_macos_impl
{

class MacTerminalRuntime final : public IPlatformTerminalRuntime
{
  public:
	bool IsAvailable() const override
	{
		return true;
	}

	bool StartCliTerminalProcess(uam::CliTerminalState& terminal, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string* error_out = nullptr, const std::vector<std::pair<std::string, std::string>>& environment_overrides = {}) const override
	{
		if (argv.empty() || uam::strings::IsBlank(argv.front()))
		{
			if (error_out != nullptr)
			{
				*error_out = "Interactive provider command is empty.";
			}

			return false;
		}

		if (!PrepareWorkingDirectory(working_directory, "provider", "Provider", true, error_out))
		{
			return false;
		}

		int master_fd = -1;
		int slave_fd = -1;
		struct winsize ws
		{
		};
		ws.ws_row = static_cast<unsigned short>(terminal.rows);
		ws.ws_col = static_cast<unsigned short>(terminal.cols);

		if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "openpty failed.";
			}

			return false;
		}
		std::array<char, 1024> slave_name{};
		if (ttyname_r(slave_fd, slave_name.data(), slave_name.size()) != 0)
		{
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			if (error_out != nullptr)
			{
				*error_out = "Failed to resolve terminal device.";
			}
			return false;
		}

		const std::vector<std::string> terminal_path_dirs = CollectTerminalPathSearchDirs();
		const std::string terminal_path_env = JoinPathEntries(terminal_path_dirs);
		const std::string resolved_executable = ResolveExecutablePathForTerminal(argv.front(), terminal_path_dirs);
		if (resolved_executable.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = CommandNotFoundOnPathMessage(argv.front());
			}
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			return false;
		}

		if (!ValidateRequiredNodeRuntime(argv.front(), resolved_executable, terminal_path_dirs, error_out))
		{
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			return false;
		}

		const std::string bootstrap_executable = uam::paths::Utf8PathString(
		    PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath());
		if (bootstrap_executable.empty())
		{
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			if (error_out != nullptr) *error_out = "Failed to resolve terminal bootstrap executable.";
			return false;
		}
		const std::string provider_executable = uam::paths::Utf8PathString(
		    uam::paths::AbsolutePathNoThrow(uam::paths::PathFromUtf8(resolved_executable)));
		std::vector<std::string> resolved_argv = {bootstrap_executable, kMacTerminalChildArgument, provider_executable};
		resolved_argv.insert(resolved_argv.end(), argv.begin() + 1, argv.end());
		std::vector<std::vector<char>> argv_storage;
		std::vector<char*> argv_ptrs = BuildMutableArgv(resolved_argv, argv_storage);
		std::vector<std::pair<std::string, std::string>> child_environment{{"TERM", "xterm-256color"}};
		if (!terminal_path_env.empty()) child_environment.emplace_back("PATH", terminal_path_env);
		child_environment.insert(child_environment.end(), environment_overrides.begin(), environment_overrides.end());
		std::vector<std::vector<char>> environment_storage;
		std::vector<char*> environment_ptrs = BuildChildEnvironment(child_environment, environment_storage);

		pid_t pid = -1;
		if (!SpawnSuspendedProcess(
		        pid,
		        bootstrap_executable,
		        argv_ptrs.data(),
		        environment_ptrs.data(),
		        working_directory,
		        {},
		        slave_name.data(),
		        true,
		        error_out))
		{
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			return false;
		}

		pid_t watchdog_pid = -1;
		if (!ArmParentDeathWatchdogAndReleaseChild(pid, watchdog_pid, error_out))
		{
			int ignored_status = 0;
			(void)TerminateCapturedCommandProcess(pid, &ignored_status);
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			return false;
		}
		CloseFdIfOpen(slave_fd);
		terminal.master_fd = master_fd;
		terminal.child_pid = pid;
		terminal.watchdog_pid = watchdog_pid;
		SetFdNonBlockingIfOpen(terminal.master_fd);

		return true;
	}

	void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const override
	{
		terminal.input_writer.reset();
		StopParentDeathWatchdog(terminal.watchdog_pid);
		CloseFdIfOpen(terminal.master_fd);

		if (terminal.child_pid > 0 && ChildWaitResultClearsPid(WaitForChildProcess(terminal.child_pid, false, 0.0)))
		{
			terminal.child_pid = -1;
		}
	}

	bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, std::size_t len) const override
	{
		if (terminal.input_writer == nullptr)
		{
			terminal.input_writer = CreateAsyncFdWriter(terminal.master_fd);
		}
		return terminal.input_writer != nullptr && terminal.input_writer->Enqueue(bytes, len);
	}

	void StopCliTerminalProcess(uam::CliTerminalState& terminal, bool fast_exit) const override
	{
		if (terminal.child_pid <= 0)
		{
			StopParentDeathWatchdog(terminal.watchdog_pid);
			return;
		}
		StopParentDeathWatchdog(terminal.watchdog_pid);

		const pid_t child_pid = terminal.child_pid;
		const auto wait_and_clear = [&](bool wait_for_exit, double timeout_seconds) -> bool
		{
			const ChildWaitResult result = WaitForChildProcess(child_pid, wait_for_exit, timeout_seconds);
			if (ChildWaitResultClearsPid(result))
			{
				terminal.child_pid = -1;
				return true;
			}
			return false;
		};

		if (fast_exit)
		{
			SignalTerminalProcessGroup(child_pid, SIGHUP);
			SignalTerminalProcessGroup(child_pid, SIGTERM);
			SignalTerminalProcessGroup(child_pid, SIGKILL);
			(void)wait_and_clear(true, 1.0);
		}
		else
		{
			SignalTerminalProcessGroup(child_pid, SIGHUP);

			if (!wait_and_clear(true, 0.25))
			{
				SignalTerminalProcessGroup(child_pid, SIGTERM);

				if (!wait_and_clear(true, 0.35))
				{
					SignalTerminalProcessGroup(child_pid, SIGKILL);
					(void)wait_and_clear(true, 1.0);
				}
			}
		}
	}

	void ResizeCliTerminal(uam::CliTerminalState& terminal) const override
	{
		if (terminal.master_fd >= 0)
		{
			struct winsize ws
			{
			};

			ws.ws_row = static_cast<unsigned short>(terminal.rows);
			ws.ws_col = static_cast<unsigned short>(terminal.cols);
			ioctl(terminal.master_fd, TIOCSWINSZ, &ws);
		}
	}

	std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState& terminal, char* buffer, std::size_t buffer_size) const override
	{
		while (true)
		{
			const ssize_t read_bytes = read(terminal.master_fd, buffer, buffer_size);

			if (read_bytes > 0)
			{
				return static_cast<std::ptrdiff_t>(read_bytes);
			}

			if (read_bytes == 0)
			{
				return 0;
			}

			if (IsInterruptedErrno())
			{
				continue;
			}

			if (IsWouldBlockErrno())
			{
				return -2;
			}

			return -1;
		}
	}

	bool HasReadableTerminalOutputHandle(const uam::CliTerminalState& terminal) const override
	{
		return terminal.master_fd >= 0;
	}

	bool PollCliTerminalProcessExited(uam::CliTerminalState& terminal) const override
	{
		if (terminal.child_pid <= 0)
		{
			return true;
		}

		const ChildWaitResult result = WaitForChildProcess(terminal.child_pid, false, 0.0);
		if (ChildWaitResultClearsPid(result))
		{
			terminal.child_pid = -1;
			StopParentDeathWatchdog(terminal.watchdog_pid);
			return true;
		}

		return false;
	}

	bool SupportsAsyncNativeGeminiHistoryRefresh() const override
	{
		return true;
	}
};

IPlatformTerminalRuntime& GetMacTerminalRuntime()
{
	static MacTerminalRuntime instance;
	return instance;
}

} // namespace uam::platform_macos_impl
