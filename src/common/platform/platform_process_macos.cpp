#include "platform_services_macos_impl_internal.h"

#include <Security/Security.h>

using namespace uam::platform_macos_impl;

namespace uam::platform_macos_impl
{

class MacProcessService final : public IPlatformProcessService
{
  public:
	bool SupportsDetachedProcesses() const override
	{
		return true;
	}

	bool PopulateLocalTime(const std::time_t timestamp, std::tm* tm_out) const override
	{
		if (tm_out == nullptr)
		{
			return false;
		}

		return localtime_r(&timestamp, tm_out) != nullptr;
	}

	std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path& working_directory, const std::string& command) const override
	{
		return "cd " + uam::shell::EscapeArg(working_directory.string()) + " && " + command;
	}

	bool CaptureCommandOutput(const std::string& command, std::string* output_out, int* raw_status_out, std::string* error_out = nullptr) const override
	{
		const ProcessExecutionResult result = ExecuteCommand(command);

		if (output_out != nullptr)
		{
			*output_out = result.output;
		}

		if (raw_status_out != nullptr)
		{
			*raw_status_out = result.exit_code;
		}

		if (error_out != nullptr)
		{
			*error_out = result.error;
		}

		return !result.timed_out && !result.canceled && result.error.empty();
	}

	int NormalizeCapturedCommandExitCode(int raw_status) const override
	{
		if (WIFEXITED(raw_status))
		{
			return WEXITSTATUS(raw_status);
		}

		return raw_status;
	}

	ProcessExecutionResult ExecuteCommand(const std::string& command, int timeout_ms = -1, std::stop_token stop_token = {}) const override
	{
		return ExecuteCapturedCommandPosix(command, timeout_ms, stop_token);
	}

	bool StartStdioProcess(uam::platform::StdioProcessPlatformFields& process, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string* error_out = nullptr) const override
	{
		if (argv.empty() || uam::strings::IsBlank(argv.front()))
		{
			if (error_out != nullptr)
			{
				*error_out = "Stdio process command is empty.";
			}
			return false;
		}

		if (!PrepareWorkingDirectory(working_directory, "process", "Process", false, error_out))
		{
			return false;
		}

		int stdin_pipe[2] = {-1, -1};
		int stdout_pipe[2] = {-1, -1};
		int stderr_pipe[2] = {-1, -1};

		if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create stdio process pipes.";
			}
			ClosePipeFds(stdin_pipe);
			ClosePipeFds(stdout_pipe);
			ClosePipeFds(stderr_pipe);
			return false;
		}

		const std::vector<std::string> path_dirs = CollectTerminalPathSearchDirs();
		const std::string path_env = JoinPathEntries(path_dirs);
		const std::string resolved_executable = ResolveExecutablePathForTerminal(argv.front(), path_dirs);
		if (resolved_executable.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = CommandNotFoundOnPathMessage(argv.front());
			}
			ClosePipeFds(stdin_pipe);
			ClosePipeFds(stdout_pipe);
			ClosePipeFds(stderr_pipe);
			return false;
		}

		if (!ValidateRequiredNodeRuntime(argv.front(), resolved_executable, path_dirs, error_out))
		{
			ClosePipeFds(stdin_pipe);
			ClosePipeFds(stdout_pipe);
			ClosePipeFds(stderr_pipe);
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
			ClosePipeFds(stdin_pipe);
			ClosePipeFds(stdout_pipe);
			ClosePipeFds(stderr_pipe);
			return false;
		}

		if (pid == 0)
		{
			dup2(stdin_pipe[0], STDIN_FILENO);
			dup2(stdout_pipe[1], STDOUT_FILENO);
			dup2(stderr_pipe[1], STDERR_FILENO);
			ClosePipeFds(stdin_pipe);
			ClosePipeFds(stdout_pipe);
			ClosePipeFds(stderr_pipe);

			if (!working_directory.empty() && chdir(working_directory.c_str()) != 0)
			{
				_exit(126);
			}

			if (!path_env.empty())
			{
				setenv("PATH", path_env.c_str(), 1);
			}

			RaiseFdLimitBestEffort();
			execv(argv_ptrs[0], argv_ptrs.data());
			_exit(127);
		}

		CloseFdIfOpen(stdin_pipe[0]);
		CloseFdIfOpen(stdout_pipe[1]);
		CloseFdIfOpen(stderr_pipe[1]);
		process.stdin_write_fd = stdin_pipe[1];
		process.stdout_read_fd = stdout_pipe[0];
		process.stderr_read_fd = stderr_pipe[0];
		process.child_pid = pid;

		for (const int fd : {process.stdout_read_fd, process.stderr_read_fd})
		{
			SetFdNonBlockingIfOpen(fd);
		}

		return true;
	}

	void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
	{
		CloseFdIfOpen(process.stdin_write_fd);
		CloseFdIfOpen(process.stdout_read_fd);
		CloseFdIfOpen(process.stderr_read_fd);
		process.child_pid = -1;
	}

	bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, std::size_t len, std::string* error_out = nullptr) const override
	{
		if (bytes == nullptr || len == 0)
		{
			return true;
		}
		if (process.stdin_write_fd < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "stdin pipe handle is closed.";
			}
			return false;
		}

		return WriteAllToFd(process.stdin_write_fd, bytes, len, error_out);
	}

	void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast_exit) const override
	{
		if (process.child_pid <= 0)
		{
			CloseStdioProcessHandles(process);
			return;
		}

		const pid_t child_pid = process.child_pid;
		if (fast_exit)
		{
			kill(child_pid, SIGKILL);
		}
		else
		{
			kill(child_pid, SIGTERM);
		}

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(fast_exit ? 80 : 600);
		int status = 0;
		while (std::chrono::steady_clock::now() < deadline)
		{
			const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
			if (wait_result == child_pid || (wait_result < 0 && errno == ECHILD))
			{
				CloseStdioProcessHandles(process);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		kill(child_pid, SIGKILL);
		(void)waitpid(child_pid, &status, WNOHANG);
		CloseStdioProcessHandles(process);
	}

	std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const override
	{
		return ReadNonBlockingFd(process.stdout_read_fd, buffer, buffer_size, error_out);
	}

	std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const override
	{
		return ReadNonBlockingFd(process.stderr_read_fd, buffer, buffer_size, error_out);
	}

	bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process, int* exit_code_out = nullptr) const override
	{
		if (process.child_pid <= 0)
		{
			if (exit_code_out != nullptr)
			{
				*exit_code_out = -1;
			}
			return true;
		}

		int status = 0;
		const pid_t wait_result = waitpid(process.child_pid, &status, WNOHANG);
		if (wait_result == 0)
		{
			return false;
		}
		if (wait_result == process.child_pid || (wait_result < 0 && errno == ECHILD))
		{
			if (exit_code_out != nullptr)
			{
				*exit_code_out = wait_result == process.child_pid ? ExitCodeFromWaitStatus(status) : -1;
			}
			process.child_pid = -1;
			return true;
		}
		return false;
	}

	std::filesystem::path ResolveCurrentExecutablePath() const override
	{
		uint32_t buffer_size = 0;
		(void)_NSGetExecutablePath(nullptr, &buffer_size);

		if (buffer_size == 0)
		{
			return {};
		}

		std::string buffer(static_cast<std::size_t>(buffer_size), '\0');

		if (_NSGetExecutablePath(buffer.data(), &buffer_size) != 0)
		{
			return {};
		}

		return std::filesystem::path(buffer.c_str());
	}

	std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path& data_root, std::string* error_out = nullptr) const override
	{
		std::error_code ec;
		if (!uam::paths::CreateDirectoriesNoThrow(data_root, &ec))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create data root lock directory: " + ec.message();
			}
			return nullptr;
		}

		const std::filesystem::path lock_path = data_root / ".uam-data-root.lock";
		const int fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
		if (fd < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open data root lock file: " + std::string(std::strerror(errno));
			}
			return nullptr;
		}

		if (flock(fd, LOCK_EX | LOCK_NB) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Another Universal Agent Manager instance is already using this data root.";
			}
			(void)close(fd);
			return nullptr;
		}

		const std::string pid_text = std::to_string(static_cast<long long>(getpid())) + "\n";
		(void)ftruncate(fd, 0);
		(void)write(fd, pid_text.data(), pid_text.size());
		return std::make_unique<MacDataRootLock>(fd);
	}

	uintmax_t NativeGeminiSessionMaxFileBytes() const override
	{
		return 12ULL * 1024ULL * 1024ULL;
	}

	std::size_t NativeGeminiSessionMaxMessages() const override
	{
		return 12000;
	}

	std::string GenerateUuid() const override
	{
#if defined(__APPLE__)
		uint8_t randomBytes[16];
		if (SecRandomCopyBytes(kSecRandomDefault, 16, randomBytes) != errSecSuccess)
		{
			return "";
		}
		randomBytes[6] = (randomBytes[6] & 0x0f) | 0x40;
		randomBytes[8] = (randomBytes[8] & 0x3f) | 0x80;
		const char* hexDigits = "0123456789abcdef";
		char uuid[37];
		for (int i = 0; i < 16; ++i)
		{
			int byte = randomBytes[i];
			uuid[i * 2] = hexDigits[(byte >> 4) & 0x0f];
			uuid[i * 2 + 1] = hexDigits[byte & 0x0f];
		}
		uuid[8] = uuid[13] = uuid[18] = uuid[23] = '-';
		uuid[36] = '\0';
		return std::string(uuid);
#else
		return "";
#endif
	}

	bool LaunchShellAt(const std::filesystem::path& working_directory, std::string* error_out = nullptr) const override
	{
		if (working_directory.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Working directory is empty.";
			}
			return false;
		}

		std::string escaped_path;
		for (char c : working_directory.generic_string())
		{
			if (c == ' ' || c == '\'' || c == '"' || c == '\\' || c == '$')
			{
				escaped_path += '\\';
			}
			escaped_path += c;
		}

		std::string command = "open -a Terminal --args --cd-path '" + escaped_path + "' &";
		int result = std::system(command.c_str());
		if (result != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to launch terminal.";
			}
			return false;
		}

		return true;
	}
};

IPlatformProcessService& GetMacProcessService()
{
	static MacProcessService instance;
	return instance;
}

} // namespace uam::platform_macos_impl
