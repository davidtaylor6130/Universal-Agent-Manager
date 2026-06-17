#include "platform_services_macos_impl_internal.h"

using namespace uam::platform_macos_impl;

namespace uam::platform_macos_impl
{

class MacTerminalRuntime final : public IPlatformTerminalRuntime
{
  public:
	bool IsAvailable() const override
	{
		return true;
	}

	bool StartCliTerminalProcess(uam::CliTerminalState& terminal, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string* error_out = nullptr) const override
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

		std::vector<std::string> resolved_argv = argv;
		resolved_argv[0] = resolved_executable;
		std::vector<std::vector<char>> argv_storage;
		std::vector<char*> argv_ptrs = BuildMutableArgv(resolved_argv, argv_storage);

		const pid_t pid = fork();

		if (pid < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "fork failed.";
			}

			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);
			return false;
		}

		if (pid == 0)
		{
			setsid();
			ioctl(slave_fd, TIOCSCTTY, 0);
			dup2(slave_fd, STDIN_FILENO);
			dup2(slave_fd, STDOUT_FILENO);
			dup2(slave_fd, STDERR_FILENO);
			CloseFdIfOpen(master_fd);
			CloseFdIfOpen(slave_fd);

			if (!working_directory.empty() && chdir(working_directory.c_str()) != 0)
			{
				_exit(126);
			}

			setenv("TERM", "xterm-256color", 1);
			if (!terminal_path_env.empty())
			{
				setenv("PATH", terminal_path_env.c_str(), 1);
			}
			RaiseFdLimitBestEffort();
			execv(argv_ptrs[0], argv_ptrs.data());
			_exit(127);
		}

		CloseFdIfOpen(slave_fd);
		terminal.master_fd = master_fd;
		terminal.child_pid = pid;
		SetFdNonBlockingIfOpen(terminal.master_fd);

		return true;
	}

	void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const override
	{
		CloseFdIfOpen(terminal.master_fd);

		if (terminal.child_pid > 0 && ChildWaitResultClearsPid(WaitForChildProcess(terminal.child_pid, false, 0.0)))
		{
			terminal.child_pid = -1;
		}
	}

	bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, std::size_t len) const override
	{
		return WriteAllToFd(terminal.master_fd, bytes, len);
	}

	void StopCliTerminalProcess(uam::CliTerminalState& terminal, bool fast_exit) const override
	{
		if (terminal.child_pid <= 0)
		{
			return;
		}

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
