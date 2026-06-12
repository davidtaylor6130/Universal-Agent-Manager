#include "platform_services_macos_impl.h"
#include <Security/Security.h>

#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/state/app_state.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

namespace
{
	bool IsInterruptedErrno()
	{
		return errno == EINTR;
	}

	bool IsWouldBlockErrno()
	{
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}

	void CloseFdIfOpen(int& fd)
	{
		if (fd >= 0)
		{
			close(fd);
			fd = -1;
		}
	}

	void ClosePipeFds(int (&pipe_fds)[2])
	{
		CloseFdIfOpen(pipe_fds[0]);
		CloseFdIfOpen(pipe_fds[1]);
	}

	// Child provider runtimes (e.g. Bun-based OpenCode) refuse to start when the
	// inherited soft RLIMIT_NOFILE is low, so raise it as close to the hard limit
	// as the kernel allows before exec. Async-signal-safe; intended for use
	// between fork and execv.
	void RaiseFdLimitBestEffort()
	{
		struct rlimit limit
		{
		};
		if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
		{
			return;
		}

		const rlim_t candidates[] = {limit.rlim_max, 1048576, 524288, 262144, 122880, 65536, 61440, 32768};
		for (const rlim_t candidate : candidates)
		{
			if (candidate == RLIM_INFINITY || candidate <= limit.rlim_cur)
			{
				continue;
			}

			struct rlimit raised
			{
			};
			raised.rlim_cur = candidate;
			raised.rlim_max = limit.rlim_max;
			if (setrlimit(RLIMIT_NOFILE, &raised) == 0)
			{
				return;
			}
		}
	}

	void SetFdNonBlockingIfOpen(int fd)
	{
		if (fd < 0)
		{
			return;
		}

		const int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
		{
			(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
	}

	std::string ErrorWithOptionalDetail(const std::string& base_message, const std::string& detail)
	{
		return detail.empty() ? base_message : base_message + ": " + detail;
	}

	std::vector<char*> BuildMutableArgv(const std::vector<std::string>& argv, std::vector<std::vector<char>>& storage)
	{
		storage.clear();
		storage.reserve(argv.size());

		std::vector<char*> argv_ptrs;
		argv_ptrs.reserve(argv.size() + 1);

		for (const std::string& arg : argv)
		{
			storage.emplace_back(arg.begin(), arg.end());
			storage.back().push_back('\0');
			argv_ptrs.push_back(storage.back().data());
		}

		argv_ptrs.push_back(nullptr);
		return argv_ptrs;
	}

	bool ValidateProgramArgv(const std::vector<std::string>& argv, std::string* error_out = nullptr)
	{
		if (!argv.empty() && !argv.front().empty())
		{
			return true;
		}

		if (error_out != nullptr)
		{
			*error_out = "Executable path is empty.";
		}
		return false;
	}

	bool WaitForSuccessfulProgramExit(const pid_t pid, std::string* error_out = nullptr)
	{
		int status = 0;
		while (waitpid(pid, &status, 0) < 0)
		{
			if (IsInterruptedErrno())
			{
				continue;
			}

			if (error_out != nullptr)
			{
				*error_out = "waitpid failed: " + std::string(std::strerror(errno));
			}
			return false;
		}

		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		{
			return true;
		}

		if (error_out != nullptr)
		{
			if (WIFEXITED(status))
			{
				*error_out = "process exited with status " + std::to_string(WEXITSTATUS(status)) + ".";
			}
			else if (WIFSIGNALED(status))
			{
				*error_out = "process terminated by signal " + std::to_string(WTERMSIG(status)) + ".";
			}
			else
			{
				*error_out = "process ended without a normal exit status.";
			}
		}

		return false;
	}

	bool RunProgramAndWait(const std::vector<std::string>& argv, std::string* error_out = nullptr)
	{
		if (!ValidateProgramArgv(argv, error_out))
		{
			return false;
		}

		std::vector<std::vector<char>> argv_storage;
		std::vector<char*> argv_ptrs = BuildMutableArgv(argv, argv_storage);

		const pid_t pid = fork();
		if (pid < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "fork failed: " + std::string(std::strerror(errno));
			}
			return false;
		}

		if (pid == 0)
		{
			execv(argv_ptrs[0], argv_ptrs.data());
			_exit(127);
		}

		return WaitForSuccessfulProgramExit(pid, error_out);
	}

	bool RunProgramAndCapture(const std::vector<std::string>& argv, std::string* output_out = nullptr, std::string* error_out = nullptr)
	{
		if (!ValidateProgramArgv(argv, error_out))
		{
			return false;
		}

		int output_pipe[2] = {-1, -1};
		if (pipe(output_pipe) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "pipe failed: " + std::string(std::strerror(errno));
			}
			return false;
		}

		std::vector<std::vector<char>> argv_storage;
		std::vector<char*> argv_ptrs = BuildMutableArgv(argv, argv_storage);

		const pid_t pid = fork();
		if (pid < 0)
		{
			ClosePipeFds(output_pipe);
			if (error_out != nullptr)
			{
				*error_out = "fork failed: " + std::string(std::strerror(errno));
			}
			return false;
		}

		if (pid == 0)
		{
			CloseFdIfOpen(output_pipe[0]);
			if (dup2(output_pipe[1], STDOUT_FILENO) < 0)
			{
				_exit(126);
			}
			CloseFdIfOpen(output_pipe[1]);
			execv(argv_ptrs[0], argv_ptrs.data());
			_exit(127);
		}

		CloseFdIfOpen(output_pipe[1]);
		std::string output;
		std::array<char, 512> buffer{};
		bool read_ok = true;
		std::string read_error;

		while (true)
		{
			const ssize_t bytes_read = read(output_pipe[0], buffer.data(), buffer.size());
			if (bytes_read > 0)
			{
				output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
				continue;
			}

			if (bytes_read == 0)
			{
				break;
			}

			if (IsInterruptedErrno())
			{
				continue;
			}

			read_ok = false;
			read_error = "read failed: " + std::string(std::strerror(errno));
			break;
		}

		CloseFdIfOpen(output_pipe[0]);

		if (output_out != nullptr)
		{
			*output_out = uam::strings::Trim(output);
		}

		if (!read_ok)
		{
			(void)WaitForSuccessfulProgramExit(pid);
			if (error_out != nullptr)
			{
				*error_out = read_error;
			}
			return false;
		}

		return WaitForSuccessfulProgramExit(pid, error_out);
	}

	bool IsExecutableFile(const std::filesystem::path& candidate)
	{
		if (!uam::paths::IsRegularFileNoThrow(candidate))
		{
			return false;
		}

		return access(candidate.c_str(), X_OK) == 0;
	}

	std::vector<std::string> SplitPathEnv(const std::string& value)
	{
		std::vector<std::string> entries;
		std::string current;
		for (const char ch : value)
		{
			if (ch == ':')
			{
				if (!current.empty())
				{
					entries.push_back(current);
					current.clear();
				}

				continue;
			}

			current.push_back(ch);
		}

		if (!current.empty())
		{
			entries.push_back(current);
		}

		return entries;
	}

	void AppendUniquePathEntry(std::vector<std::string>& entries, const std::string& entry)
	{
		uam::ranges::PushUniqueNonEmptyString(entries, entry);
	}

	std::vector<std::string> CollectTerminalPathSearchDirs()
	{
		std::vector<std::string> candidate_dirs;
		if (const std::optional<std::string> path_env = uam::env::GetNonEmptyString("PATH"))
		{
			candidate_dirs = SplitPathEnv(*path_env);
		}

		const auto fallback_dirs = std::to_array<const char*>({
		    "/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin", "/usr/local/sbin", "/usr/bin", "/bin", "/usr/sbin", "/sbin",
		});
		for (const char* dir : fallback_dirs)
		{
			AppendUniquePathEntry(candidate_dirs, dir);
		}

		if (const std::optional<std::filesystem::path> home_path = uam::env::GetTrimmedPath("HOME"))
		{
			AppendUniquePathEntry(candidate_dirs, (*home_path / ".volta" / "bin").string());
			AppendUniquePathEntry(candidate_dirs, (*home_path / ".asdf" / "shims").string());
			AppendUniquePathEntry(candidate_dirs, (*home_path / ".fnm").string());

			const std::filesystem::path nvm_versions_dir = *home_path / ".nvm" / "versions" / "node";
			std::error_code ec;
			if (uam::paths::IsDirectoryNoThrow(nvm_versions_dir))
			{
				for (std::filesystem::directory_iterator it(nvm_versions_dir, ec), end; !ec && it != end; it.increment(ec))
				{
					const std::filesystem::directory_entry& entry = *it;
					if (!uam::paths::IsDirectoryEntryNoThrow(entry))
					{
						continue;
					}

					const std::filesystem::path bin_dir = entry.path() / "bin";
					if (uam::paths::IsDirectoryNoThrow(bin_dir))
					{
						AppendUniquePathEntry(candidate_dirs, bin_dir.string());
					}
				}
			}
		}

		return candidate_dirs;
	}

	std::string JoinPathEntries(const std::vector<std::string>& entries)
	{
		std::string joined;
		for (const std::string& entry : entries)
		{
			if (entry.empty())
			{
				continue;
			}

			if (!joined.empty())
			{
				joined.push_back(':');
			}
			joined += entry;
		}
		return joined;
	}

	std::string ResolveExecutablePathForTerminal(const std::string& command, const std::vector<std::string>& search_dirs)
	{
		if (command.empty())
		{
			return "";
		}

		if (uam::strings::Contains(command, '/'))
		{
			return IsExecutableFile(command) ? command : "";
		}

		for (const std::string& dir : search_dirs)
		{
			if (dir.empty())
			{
				continue;
			}

			const std::filesystem::path candidate = std::filesystem::path(dir) / command;
			if (IsExecutableFile(candidate))
			{
				return candidate.string();
			}
		}

		return "";
	}

	bool ScriptShebangMentionsNode(const std::filesystem::path& executable_path)
	{
		const std::optional<std::string> first_line = uam::io::ReadFirstTextFileLine(executable_path);
		if (!first_line)
		{
			return false;
		}

		if (!uam::strings::StartsWith(*first_line, "#!"))
		{
			return false;
		}

		return uam::strings::Contains(*first_line, "node");
	}

	bool CommandNeedsNodePreflight(const std::string& command, const std::filesystem::path& executable_path)
	{
		return command == "gemini" && ScriptShebangMentionsNode(executable_path);
	}

	std::string CommandNotFoundOnPathMessage(const std::string& command)
	{
		return command + " not found on PATH in app environment";
	}

	std::string NodeRuntimeNotFoundMessage(const std::string& command)
	{
		return "node not found on PATH in app environment (required by " + command + ")";
	}

	bool ValidateRequiredNodeRuntime(const std::string& command, const std::filesystem::path& executable_path, const std::vector<std::string>& search_dirs, std::string* error_out)
	{
		if (!CommandNeedsNodePreflight(command, executable_path))
		{
			return true;
		}

		if (!ResolveExecutablePathForTerminal("node", search_dirs).empty())
		{
			return true;
		}

		if (error_out != nullptr)
		{
			*error_out = NodeRuntimeNotFoundMessage(command);
		}
		return false;
	}

	bool PrepareWorkingDirectory(const std::filesystem::path& working_directory, const char* prepare_context_name, const char* access_context_name, bool require_execute_access, std::string* error_out)
	{
		if (working_directory.empty())
		{
			return true;
		}

		std::error_code wd_ec;
		if (!uam::paths::CreateDirectoriesNoThrow(working_directory, &wd_ec) || !uam::paths::IsDirectoryNoThrow(working_directory))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to prepare " + std::string(prepare_context_name) + " working directory: " + (wd_ec ? wd_ec.message() : working_directory.string());
			}
			return false;
		}

		if (require_execute_access && access(working_directory.c_str(), X_OK) != 0)
		{
			if (error_out != nullptr)
			{
				*error_out = std::string(access_context_name) + " working directory is not accessible: " + std::strerror(errno);
			}
			return false;
		}

		return true;
	}

	std::string EscapeAppleScriptQuotedString(const std::string& value)
	{
		std::string escaped;
		escaped.reserve(value.size());

		for (const char ch : value)
		{
			if (ch == '\\')
			{
				escaped += "\\\\";
			}
			else if (ch == '"')
			{
				escaped += "\\\"";
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		return escaped;
	}

	bool ReadAvailablePipeData(int fd, std::string* output_out, std::string* error_out = nullptr)
	{
		if (output_out == nullptr)
		{
			return false;
		}

		std::array<char, 4096> buffer{};

		for (;;)
		{
			const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());

			if (bytes_read > 0)
			{
				output_out->append(buffer.data(), static_cast<std::size_t>(bytes_read));
				continue;
			}

			if (bytes_read == 0)
			{
				return true;
			}

			if (IsInterruptedErrno())
			{
				continue;
			}

			if (IsWouldBlockErrno())
			{
				return true;
			}

			if (error_out != nullptr)
			{
				*error_out = std::strerror(errno);
			}

			return false;
		}
	}

	ProcessExecutionResult ExecuteCapturedCommandPosix(const std::string& command, int timeout_ms, std::stop_token stop_token)
	{
		ProcessExecutionResult result;
		int pipe_fds[2] = {-1, -1};

		if (pipe(pipe_fds) != 0)
		{
			result.error = "Failed to create capture pipe.";
			return result;
		}

		const pid_t pid = fork();

		if (pid < 0)
		{
			ClosePipeFds(pipe_fds);
			result.error = "fork failed.";
			return result;
		}

		if (pid == 0)
		{
			dup2(pipe_fds[1], STDOUT_FILENO);
			dup2(pipe_fds[1], STDERR_FILENO);
			ClosePipeFds(pipe_fds);
			execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}

		CloseFdIfOpen(pipe_fds[1]);
		const int read_fd = pipe_fds[0];
		const int original_flags = fcntl(read_fd, F_GETFL, 0);

		if (original_flags >= 0)
		{
			(void)fcntl(read_fd, F_SETFL, original_flags | O_NONBLOCK);
		}

		const auto started_at = std::chrono::steady_clock::now();
		int raw_status = -1;
		bool finished = false;

		while (!finished)
		{
			std::string read_error;
			(void)ReadAvailablePipeData(read_fd, &result.output, &read_error);

			if (stop_token.stop_requested())
			{
				result.canceled = true;
				result.error = "Command canceled.";
				kill(pid, SIGTERM);
				(void)waitpid(pid, &raw_status, 0);
				finished = true;
				break;
			}

			if (timeout_ms >= 0)
			{
				const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count();

				if (elapsed_ms > timeout_ms)
				{
					result.timed_out = true;
					result.error = "Command timed out.";
					kill(pid, SIGTERM);
					(void)waitpid(pid, &raw_status, 0);
					finished = true;
					break;
				}
			}

			const pid_t wait_result = waitpid(pid, &raw_status, WNOHANG);

			if (wait_result == pid)
			{
				finished = true;
				break;
			}

			if (wait_result < 0)
			{
				result.error = "waitpid failed.";
				kill(pid, SIGTERM);
				(void)waitpid(pid, &raw_status, 0);
				finished = true;
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		std::string final_read_error;
		(void)ReadAvailablePipeData(read_fd, &result.output, &final_read_error);
		close(read_fd);

		if (result.canceled || result.timed_out)
		{
			result.exit_code = -1;
			return result;
		}

		if (raw_status == -1)
		{
			if (result.error.empty())
			{
				result.error = "Command did not produce an exit status.";
			}

			return result;
		}

		if (WIFEXITED(raw_status))
		{
			result.exit_code = WEXITSTATUS(raw_status);
		}
		else if (WIFSIGNALED(raw_status))
		{
			result.exit_code = 128 + WTERMSIG(raw_status);
		}
		else
		{
			result.exit_code = raw_status;
		}

		result.ok = (result.exit_code == 0);
		return result;
	}

	void TerminateChildProcess(const pid_t pid)
	{
		if (pid <= 0)
		{
			return;
		}

		int status = 0;
		kill(pid, SIGTERM);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);

		while (std::chrono::steady_clock::now() < deadline)
		{
			const pid_t wait_result = waitpid(pid, &status, WNOHANG);

			if (wait_result == pid || (wait_result < 0 && errno == ECHILD))
			{
				return;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		kill(pid, SIGKILL);
		waitpid(pid, &status, WNOHANG);
	}

	enum class ChildWaitResult
	{
		Running,
		Exited,
		NoChild,
	};

	ChildWaitResult WaitForChildProcess(pid_t child_pid, bool wait_for_exit, double timeout_seconds)
	{
		int status = 0;
		const auto wait_start = std::chrono::steady_clock::now();
		const auto wait_timeout = std::chrono::duration<double>(std::max(0.0, timeout_seconds));

		while (true)
		{
			const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);

			if (wait_result == child_pid)
			{
				return ChildWaitResult::Exited;
			}

			if (wait_result < 0)
			{
				if (IsInterruptedErrno())
				{
					continue;
				}

				return errno == ECHILD ? ChildWaitResult::NoChild : ChildWaitResult::Running;
			}

			if (!wait_for_exit)
			{
				return ChildWaitResult::Running;
			}

			if ((std::chrono::steady_clock::now() - wait_start) >= wait_timeout)
			{
				return ChildWaitResult::Running;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(8));
		}
	}

	bool ChildWaitResultClearsPid(const ChildWaitResult result)
	{
		return result == ChildWaitResult::Exited || result == ChildWaitResult::NoChild;
	}

	void SignalTerminalProcessGroup(pid_t child_pid, int signal_number)
	{
		if (child_pid <= 0)
		{
			return;
		}

		(void)kill(-child_pid, signal_number);
	}

	int ExitCodeFromWaitStatus(int status)
	{
		if (WIFEXITED(status))
		{
			return WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status))
		{
			return 128 + WTERMSIG(status);
		}
		return status;
	}

	std::ptrdiff_t ReadNonBlockingFd(int fd, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr)
	{
		if (fd < 0)
		{
			if (error_out != nullptr)
			{
				*error_out = "stdio pipe handle is closed.";
			}
			return -1;
		}

		while (true)
		{
			const ssize_t read_bytes = read(fd, buffer, buffer_size);

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

			if (error_out != nullptr)
			{
				*error_out = std::strerror(errno);
			}
			return -1;
		}
	}

	bool WriteAllToFd(int fd, const char* bytes, std::size_t len, std::string* error_out = nullptr)
	{
		if (bytes == nullptr || len == 0)
		{
			return true;
		}

		std::size_t offset = 0;
		while (offset < len)
		{
			const ssize_t written = write(fd, bytes + offset, len - offset);
			if (written > 0)
			{
				offset += static_cast<std::size_t>(written);
				continue;
			}
			if (written < 0 && IsInterruptedErrno())
			{
				continue;
			}
			if (error_out != nullptr)
			{
				*error_out = std::strerror(errno);
			}
			return false;
		}
		return true;
	}

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

	class MacDataRootLock final : public uam::platform::DataRootLock
	{
	  public:
		explicit MacDataRootLock(int fd) : m_fd(fd)
		{
		}

		~MacDataRootLock() override
		{
			if (m_fd >= 0)
			{
				(void)flock(m_fd, LOCK_UN);
				(void)close(m_fd);
			}
		}

		MacDataRootLock(const MacDataRootLock&) = delete;
		MacDataRootLock& operator=(const MacDataRootLock&) = delete;

	  private:
		int m_fd = -1;
	};

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

	class MacFileDialogService final : public IPlatformFileDialogService
	{
	  public:
		bool SupportsNativeDialogs() const override
		{
			return true;
		}

		bool BrowsePath(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr) const override
		{
			if (selected_path_out != nullptr)
			{
				selected_path_out->clear();
			}

			if (!IsExecutableFile("/usr/bin/osascript"))
			{
				if (error_out != nullptr)
				{
					*error_out = "Native path picker is unavailable (missing osascript).";
				}

				return false;
			}

			const bool choosing_directory = target == PlatformPathBrowseTarget::Directory;
			const std::string chooser_kind = choosing_directory ? "folder" : "file";
			const std::string prompt = choosing_directory ? "\"Select folder\"" : "\"Select file\"";

			std::string script = "set selectedPath to POSIX path of (choose " + chooser_kind + " with prompt " + prompt;

			if (!initial_path.empty())
			{
				script += " default location POSIX file \"" + EscapeAppleScriptQuotedString(initial_path.string()) + "\"";
			}

			script += ")";
			std::string selected_path;

			if (!RunProgramAndCapture({"/usr/bin/osascript", "-e", script, "-e", "return selectedPath"}, &selected_path) || selected_path.empty())
			{
				return false;
			}

			if (selected_path_out != nullptr)
			{
				*selected_path_out = selected_path;
			}

			return true;
		}

		bool OpenFolderInFileManager(const std::filesystem::path& folder_path, std::string* error_out = nullptr) const override
		{
			if (folder_path.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Folder path is empty.";
				}

				return false;
			}

			std::error_code ec;
			if (!uam::paths::CreateDirectoriesNoThrow(folder_path, &ec))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create folder: " + ec.message();
				}

				return false;
			}

			std::string launch_error;
			if (!RunProgramAndWait({"/usr/bin/open", folder_path.string()}, &launch_error))
			{
				if (error_out != nullptr)
				{
					*error_out = ErrorWithOptionalDetail("Failed to open folder in file manager.", launch_error);
				}

				return false;
			}

			return true;
		}

		bool OpenFolderInEditorPreset(const std::filesystem::path& folder_path, const std::string& editor_preset_id, std::string* error_out = nullptr) const override
		{
			if (folder_path.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Folder path is empty.";
				}
				return false;
			}

			if (!uam::paths::IsDirectoryNoThrow(folder_path))
			{
				if (error_out != nullptr)
				{
					*error_out = "Workspace directory does not exist.";
				}
				return false;
			}

			std::string app_name;
			if (editor_preset_id == "xcode")
			{
				app_name = "Xcode";
			}
			else if (editor_preset_id == "clion")
			{
				app_name = "CLion";
			}
			else if (editor_preset_id == "rider")
			{
				app_name = "Rider";
			}
			else if (editor_preset_id == "visualstudio")
			{
				app_name = "Visual Studio";
			}
			else if (editor_preset_id == "webstorm")
			{
				app_name = "WebStorm";
			}
			else if (editor_preset_id == "pycharm")
			{
				app_name = "PyCharm";
			}
			else if (editor_preset_id == "idea")
			{
				app_name = "IntelliJ IDEA";
			}
			else if (editor_preset_id == "goland")
			{
				app_name = "GoLand";
			}
			else if (editor_preset_id == "rustrover")
			{
				app_name = "RustRover";
			}
			else
			{
				app_name = "Visual Studio Code";
			}

			std::string launch_error;
			if (!RunProgramAndWait({"/usr/bin/open", "-a", app_name, folder_path.string()}, &launch_error))
			{
				if (error_out != nullptr)
				{
					*error_out = ErrorWithOptionalDetail("Failed to open workspace in " + app_name + ".", launch_error);
				}
				return false;
			}
			return true;
		}

		bool RevealPathInFileManager(const std::filesystem::path& file_path, std::string* error_out = nullptr) const override
		{
			if (file_path.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "File path is empty.";
				}

				return false;
			}

			if (!uam::paths::PathExistsNoThrow(file_path))
			{
				return OpenFolderInFileManager(file_path.parent_path(), error_out);
			}

			std::string launch_error;
			if (!RunProgramAndWait({"/usr/bin/open", "-R", file_path.string()}, &launch_error))
			{
				if (error_out != nullptr)
				{
					*error_out = ErrorWithOptionalDetail("Failed to reveal file in file manager.", launch_error);
				}

				return false;
			}

			return true;
		}
	};

	class MacPathService final : public IPlatformPathService
	{
	  public:
		std::filesystem::path DefaultDataRootPath() const override
		{
			return AppPaths::DefaultDataRootPath();
		}

		std::optional<std::filesystem::path> ResolveUserHomePath() const override
		{
			if (const std::optional<std::filesystem::path> home = uam::env::GetTrimmedPath("HOME"))
			{
				return *home;
			}

			return std::nullopt;
		}

		std::filesystem::path ExpandLeadingTildePath(const std::string& raw_path) const override
		{
			const std::string trimmed = uam::strings::Trim(raw_path);

			if (trimmed.empty())
			{
				return {};
			}

			if (trimmed[0] != '~')
			{
				return std::filesystem::path(trimmed);
			}

			if (const std::optional<std::filesystem::path> home = ResolveUserHomePath())
			{
				if (trimmed.size() == 1)
				{
					return *home;
				}

				if (trimmed[1] == '/')
				{
					return *home / trimmed.substr(2);
				}
			}

			return std::filesystem::path(trimmed);
		}
	};

} // namespace

PlatformServices& CreatePlatformServices()
{
	static MacTerminalRuntime terminal_runtime;
	static MacProcessService process_service;
	static MacFileDialogService file_dialog_service;
	static MacPathService path_service;
	static PlatformServices services{
	    terminal_runtime,
	    process_service,
	    file_dialog_service,
	    path_service,
	};
	return services;
}
