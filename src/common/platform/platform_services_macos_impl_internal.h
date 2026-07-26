#pragma once
// Internal helpers and MacDataRootLock shared across platform_pty_macos.cpp,
// platform_process_macos.cpp, platform_dialogs_macos.cpp, platform_paths_macos.cpp.
// Include only from those four TUs and platform_services_macos_impl.cpp.

#include "platform_services_macos_impl.h"
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
#include <string>
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

namespace uam::platform_macos_impl
{

inline bool IsInterruptedErrno()
{
	return errno == EINTR;
}

inline bool IsWouldBlockErrno()
{
	return errno == EAGAIN || errno == EWOULDBLOCK;
}

inline void CloseFdIfOpen(int& fd)
{
	if (fd >= 0)
	{
		close(fd);
		fd = -1;
	}
}

inline void ClosePipeFds(int (&pipe_fds)[2])
{
	CloseFdIfOpen(pipe_fds[0]);
	CloseFdIfOpen(pipe_fds[1]);
}

// Child provider runtimes (e.g. Bun-based OpenCode) refuse to start when the
// inherited soft RLIMIT_NOFILE is low, so raise it as close to the hard limit
// as the kernel allows before exec. Async-signal-safe; intended for use
// between fork and execv.
inline void RaiseFdLimitBestEffort()
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

inline void SetFdNonBlockingIfOpen(int fd)
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

inline std::string ErrorWithOptionalDetail(const std::string& base_message, const std::string& detail)
{
	return detail.empty() ? base_message : base_message + ": " + detail;
}

inline std::vector<char*> BuildMutableArgv(const std::vector<std::string>& argv, std::vector<std::vector<char>>& storage)
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

inline bool ValidateProgramArgv(const std::vector<std::string>& argv, std::string* error_out = nullptr)
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

inline bool WaitForSuccessfulProgramExit(const pid_t pid, std::string* error_out = nullptr)
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

inline bool RunProgramAndWait(const std::vector<std::string>& argv, std::string* error_out = nullptr)
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

inline bool RunProgramAndCapture(const std::vector<std::string>& argv, std::string* output_out = nullptr, std::string* error_out = nullptr)
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

inline bool IsExecutableFile(const std::filesystem::path& candidate)
{
	if (!uam::paths::IsRegularFileNoThrow(candidate))
	{
		return false;
	}

	return access(candidate.c_str(), X_OK) == 0;
}

inline std::vector<std::string> SplitPathEnv(const std::string& value)
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

inline void AppendUniquePathEntry(std::vector<std::string>& entries, const std::string& entry)
{
	uam::ranges::PushUniqueNonEmptyString(entries, entry);
}

inline std::vector<std::string> CollectTerminalPathSearchDirs()
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

inline std::string JoinPathEntries(const std::vector<std::string>& entries)
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

inline std::string ResolveExecutablePathForTerminal(const std::string& command, const std::vector<std::string>& search_dirs)
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

inline bool ScriptShebangMentionsNode(const std::filesystem::path& executable_path)
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

inline bool CommandNeedsNodePreflight(const std::string& command, const std::filesystem::path& executable_path)
{
	return command == "gemini" && ScriptShebangMentionsNode(executable_path);
}

inline std::string CommandNotFoundOnPathMessage(const std::string& command)
{
	return command + " not found on PATH in app environment";
}

inline std::string NodeRuntimeNotFoundMessage(const std::string& command)
{
	return "node not found on PATH in app environment (required by " + command + ")";
}

inline bool ValidateRequiredNodeRuntime(const std::string& command, const std::filesystem::path& executable_path, const std::vector<std::string>& search_dirs, std::string* error_out)
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

inline bool PrepareWorkingDirectory(const std::filesystem::path& working_directory, const char* prepare_context_name, const char* access_context_name, bool require_execute_access, std::string* error_out)
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

inline std::string EscapeAppleScriptQuotedString(const std::string& value)
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

inline bool ReadAvailablePipeData(int fd, std::string* output_out, std::string* error_out = nullptr)
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

inline void SignalTerminalProcessGroup(pid_t child_pid, int signal_number);

inline ProcessExecutionResult ExecuteCapturedCommandPosix(const std::string& command, int timeout_ms, std::stop_token stop_token)
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
		(void)setpgid(0, 0);
		dup2(pipe_fds[1], STDOUT_FILENO);
		dup2(pipe_fds[1], STDERR_FILENO);
		ClosePipeFds(pipe_fds);
		execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
		_exit(127);
	}

	(void)setpgid(pid, pid);
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
			SignalTerminalProcessGroup(pid, SIGTERM);
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
				SignalTerminalProcessGroup(pid, SIGTERM);
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
			SignalTerminalProcessGroup(pid, SIGTERM);
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

inline void TerminateChildProcess(const pid_t pid)
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

inline ChildWaitResult WaitForChildProcess(pid_t child_pid, bool wait_for_exit, double timeout_seconds)
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

inline bool ChildWaitResultClearsPid(const ChildWaitResult result)
{
	return result == ChildWaitResult::Exited || result == ChildWaitResult::NoChild;
}

inline void SignalTerminalProcessGroup(pid_t child_pid, int signal_number)
{
	if (child_pid <= 0)
	{
		return;
	}

	(void)kill(-child_pid, signal_number);
}

inline int ExitCodeFromWaitStatus(int status)
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

inline std::ptrdiff_t ReadNonBlockingFd(int fd, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr)
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

inline bool WriteAllToFd(int fd, const char* bytes, std::size_t len, std::string* error_out = nullptr)
{
	if (bytes == nullptr || len == 0)
	{
		return true;
	}

	(void)fcntl(fd, F_SETNOSIGPIPE, 1);
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

} // namespace uam::platform_macos_impl
