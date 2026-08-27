#pragma once
// Internal helpers and MacDataRootLock shared across platform_pty_macos.cpp,
// platform_process_macos.cpp, platform_dialogs_macos.cpp, platform_paths_macos.cpp.
// Include only from those four TUs and platform_services_macos_impl.cpp.

#include "platform_services_macos_impl.h"
#include "common/platform/platform_application_macos.h"
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
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <AvailabilityMacros.h>
#include <crt_externs.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <poll.h>
#include <spawn.h>
#include <sys/event.h>
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
// inherited soft RLIMIT_NOFILE is low, so raise the manager's inherited limit
// before posix_spawn.
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

// Build the complete environment before fork. setenv() is not async-signal-safe and can
// deadlock a child of the multithreaded CEF process before it reaches exec.
inline std::vector<char*> BuildChildEnvironment(
    const std::vector<std::pair<std::string, std::string>>& overrides,
    std::vector<std::vector<char>>& storage)
{
	std::map<std::string, std::string> values;
	if (char*** environment = _NSGetEnviron(); environment != nullptr && *environment != nullptr)
	{
		for (char** entry = *environment; *entry != nullptr; ++entry)
		{
			const std::string value(*entry);
			const std::size_t separator = value.find('=');
			if (separator > 0 && separator != std::string::npos)
			{
				values[value.substr(0, separator)] = value.substr(separator + 1);
			}
		}
	}
	for (const auto& [name, value] : overrides) values[name] = value;

	std::vector<std::string> entries;
	entries.reserve(values.size());
	for (const auto& [name, value] : values) entries.push_back(name + "=" + value);
	return BuildMutableArgv(entries, storage);
}

inline int AddSpawnWorkingDirectory(posix_spawn_file_actions_t& actions, const std::filesystem::path& working_directory)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
	if (__builtin_available(macOS 26.0, *)) return posix_spawn_file_actions_addchdir(&actions, working_directory.c_str());
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	const int result = posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
#pragma clang diagnostic pop
	return result;
}

inline bool SpawnSuspendedProcess(
    pid_t& pid,
    const std::string& executable,
    char* const argv[],
    char* const environment[],
    const std::filesystem::path& working_directory,
    const std::vector<std::pair<int, int>>& fd_mappings,
    const std::string& controlling_terminal,
    bool new_session,
    std::string* error_out)
{
	posix_spawn_file_actions_t actions{};
	posix_spawnattr_t attributes{};
	int spawn_error = posix_spawn_file_actions_init(&actions);
	const bool actions_initialized = spawn_error == 0;
	if (spawn_error == 0 && !controlling_terminal.empty())
	{
		spawn_error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, controlling_terminal.c_str(), O_RDWR, 0);
		if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDOUT_FILENO);
		if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDERR_FILENO);
	}
	for (const auto& [source_fd, target_fd] : fd_mappings)
	{
		if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(&actions, source_fd, target_fd);
	}
	if (spawn_error == 0 && !working_directory.empty())
	{
		spawn_error = AddSpawnWorkingDirectory(actions, working_directory);
	}
	if (spawn_error == 0) spawn_error = posix_spawnattr_init(&attributes);
	const bool attributes_initialized = spawn_error == 0;
	if (spawn_error == 0)
	{
		const short flags = static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_START_SUSPENDED |
		                                              (new_session ? POSIX_SPAWN_SETSID : POSIX_SPAWN_SETPGROUP));
		spawn_error = posix_spawnattr_setflags(&attributes, flags);
		if (spawn_error == 0 && !new_session) spawn_error = posix_spawnattr_setpgroup(&attributes, 0);
	}
	if (spawn_error == 0)
	{
		RaiseFdLimitBestEffort();
		spawn_error = posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv, environment);
	}
	if (attributes_initialized) (void)posix_spawnattr_destroy(&attributes);
	if (actions_initialized) (void)posix_spawn_file_actions_destroy(&actions);
	if (spawn_error == 0) return true;
	if (error_out != nullptr) *error_out = "posix_spawn failed: " + std::string(std::strerror(spawn_error)) + ".";
	return false;
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

enum class CapturedPipeReadResult
{
	Drained,
	BudgetExhausted,
	Failed,
	OutputLimit,
};

inline CapturedPipeReadResult ReadAvailablePipeData(int fd, ProcessExecutionResult& result, std::string* error_out = nullptr)
{
	std::array<char, 4096> buffer{};
	std::size_t bytes_drained = 0;

	for (;;)
	{
		const ssize_t bytes_read = read(fd, buffer.data(), buffer.size());

		if (bytes_read > 0)
		{
			const std::size_t read_size = static_cast<std::size_t>(bytes_read);
			if (!uam::platform::AppendCapturedCommandOutput(result, buffer.data(), read_size))
			{
				return CapturedPipeReadResult::OutputLimit;
			}
			bytes_drained += read_size;
			if (bytes_drained >= uam::platform::kCapturedCommandReadBudgetBytes)
			{
				return CapturedPipeReadResult::BudgetExhausted;
			}
			continue;
		}

		if (bytes_read == 0)
		{
			return CapturedPipeReadResult::Drained;
		}

		if (IsInterruptedErrno())
		{
			continue;
		}

		if (IsWouldBlockErrno())
		{
			return CapturedPipeReadResult::Drained;
		}

		if (error_out != nullptr)
		{
			*error_out = std::strerror(errno);
		}

		return CapturedPipeReadResult::Failed;
	}
}

inline void SignalTerminalProcessGroup(pid_t child_pid, int signal_number);

inline void StopParentDeathWatchdog(pid_t& watchdog_pid)
{
	if (watchdog_pid <= 0)
	{
		return;
	}

	(void)kill(watchdog_pid, SIGTERM);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	int status = 0;
	while (std::chrono::steady_clock::now() < deadline)
	{
		const pid_t wait_result = waitpid(watchdog_pid, &status, WNOHANG);
		if (wait_result == watchdog_pid || (wait_result < 0 && errno == ECHILD))
		{
			watchdog_pid = -1;
			return;
		}
		if (wait_result < 0 && errno != EINTR)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	(void)kill(watchdog_pid, SIGKILL);
	const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (std::chrono::steady_clock::now() < kill_deadline)
	{
		const pid_t wait_result = waitpid(watchdog_pid, &status, WNOHANG);
		if (wait_result == watchdog_pid || (wait_result < 0 && errno == ECHILD))
		{
			break;
		}
		if (wait_result < 0 && errno != EINTR)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	watchdog_pid = -1;
}

inline bool ArmParentDeathWatchdogAndReleaseChild(pid_t child_pid, pid_t& watchdog_pid, std::string* error_out = nullptr)
{
	int ready_pipe[2] = {-1, -1};
	if (pipe(ready_pipe) != 0)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create parent-death watchdog readiness pipe.";
		}
		return false;
	}

	const pid_t parent_pid = getpid();
	struct proc_bsdinfo original_child_info
	{
	};
	if (proc_pidinfo(child_pid, PROC_PIDTBSDINFO, 0, &original_child_info, sizeof(original_child_info)) != sizeof(original_child_info))
	{
		ClosePipeFds(ready_pipe);
		if (error_out != nullptr)
		{
			*error_out = "Failed to identify contained child process.";
		}
		return false;
	}

	uint32_t executable_size = 0;
	(void)_NSGetExecutablePath(nullptr, &executable_size);
	std::string executable(executable_size, '\0');
	if (executable_size == 0 || _NSGetExecutablePath(executable.data(), &executable_size) != 0)
	{
		ClosePipeFds(ready_pipe);
		if (error_out != nullptr)
		{
			*error_out = "Failed to resolve parent-death watchdog executable.";
		}
		return false;
	}
	executable.resize(std::strlen(executable.c_str()));
	const std::vector<std::string> watchdog_argv = {
	    executable,
	    uam::platform::kMacParentDeathWatchdogArgument,
	    std::to_string(static_cast<long long>(parent_pid)),
	    std::to_string(static_cast<long long>(child_pid)),
	    std::to_string(static_cast<long long>(original_child_info.pbi_start_tvsec)),
	    std::to_string(static_cast<long long>(original_child_info.pbi_start_tvusec)),
	};
	std::vector<std::vector<char>> watchdog_argv_storage;
	std::vector<char*> watchdog_argv_ptrs = BuildMutableArgv(watchdog_argv, watchdog_argv_storage);
	posix_spawn_file_actions_t actions{};
	posix_spawnattr_t attributes{};
	int spawn_error = posix_spawn_file_actions_init(&actions);
	const bool actions_initialized = spawn_error == 0;
	if (spawn_error == 0) spawn_error = posix_spawn_file_actions_adddup2(&actions, ready_pipe[1], 3);
	if (spawn_error == 0) spawn_error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (spawn_error == 0) spawn_error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	if (spawn_error == 0) spawn_error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	if (spawn_error == 0) spawn_error = posix_spawnattr_init(&attributes);
	const bool attributes_initialized = spawn_error == 0;
	if (spawn_error == 0) spawn_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
	if (spawn_error == 0)
	{
		spawn_error = posix_spawn(&watchdog_pid, executable.c_str(), &actions, &attributes, watchdog_argv_ptrs.data(), *_NSGetEnviron());
	}
	if (attributes_initialized) (void)posix_spawnattr_destroy(&attributes);
	if (actions_initialized) (void)posix_spawn_file_actions_destroy(&actions);
	CloseFdIfOpen(ready_pipe[1]);
	if (spawn_error != 0)
	{
		CloseFdIfOpen(ready_pipe[0]);
		if (error_out != nullptr) *error_out = "Failed to start parent-death watchdog: " + std::string(std::strerror(spawn_error)) + ".";
		return false;
	}

	struct pollfd ready_poll = {ready_pipe[0], POLLIN | POLLHUP, 0};
	const int poll_result = poll(&ready_poll, 1, 5000);
	char ready = 0;
	ssize_t ready_count = -1;
	if (poll_result > 0)
	{
		do
		{
			ready_count = read(ready_pipe[0], &ready, 1);
		} while (ready_count < 0 && errno == EINTR);
	}
	CloseFdIfOpen(ready_pipe[0]);
	if (ready_count != 1 || ready != '1')
	{
		StopParentDeathWatchdog(watchdog_pid);
		if (error_out != nullptr)
		{
			*error_out = "Failed to arm parent-death watchdog.";
		}
		return false;
	}

	if (kill(child_pid, SIGCONT) != 0)
	{
		StopParentDeathWatchdog(watchdog_pid);
		if (error_out != nullptr)
		{
			*error_out = "Failed to resume contained child process.";
		}
		return false;
	}
	return true;
}

inline bool TerminateCapturedCommandProcess(pid_t pid, int* raw_status_out)
{
	if (pid <= 0)
	{
		return true;
	}

	SignalTerminalProcessGroup(pid, SIGTERM);
	const auto graceful_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
	bool reaped = false;
	int raw_status = -1;

	while (std::chrono::steady_clock::now() < graceful_deadline)
	{
		const pid_t wait_result = waitpid(pid, &raw_status, WNOHANG);
		if (wait_result == pid || (wait_result < 0 && errno == ECHILD))
		{
			reaped = true;
			break;
		}
		if (wait_result < 0 && errno != EINTR)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	SignalTerminalProcessGroup(pid, SIGKILL);
	const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!reaped && std::chrono::steady_clock::now() < kill_deadline)
	{
		const pid_t wait_result = waitpid(pid, &raw_status, WNOHANG);
		if (wait_result == pid || (wait_result < 0 && errno == ECHILD))
		{
			reaped = true;
			break;
		}
		if (wait_result < 0 && errno != EINTR)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (raw_status_out != nullptr)
	{
		*raw_status_out = raw_status;
	}
	return reaped;
}

inline ProcessExecutionResult ExecuteCapturedCommandPosix(const std::string& command, int timeout_ms, std::stop_token stop_token)
{
	ProcessExecutionResult result;
	int pipe_fds[2] = {-1, -1};

	if (pipe(pipe_fds) != 0)
	{
		ClosePipeFds(pipe_fds);
		result.error = "Failed to create capture pipe.";
		return result;
	}

	const std::vector<std::string> shell_argv = {"/bin/sh", "-lc", command};
	std::vector<std::vector<char>> shell_argv_storage;
	std::vector<char*> shell_argv_ptrs = BuildMutableArgv(shell_argv, shell_argv_storage);
	const std::string path_env = JoinPathEntries(CollectTerminalPathSearchDirs());
	std::vector<std::vector<char>> environment_storage;
	std::vector<char*> environment_ptrs = BuildChildEnvironment({{"PATH", path_env}}, environment_storage);
	pid_t pid = -1;
	if (!SpawnSuspendedProcess(
	        pid,
	        shell_argv.front(),
	        shell_argv_ptrs.data(),
	        environment_ptrs.data(),
	        {},
	        {{pipe_fds[1], STDOUT_FILENO}, {pipe_fds[1], STDERR_FILENO}},
	        {},
	        false,
	        &result.error))
	{
		ClosePipeFds(pipe_fds);
		return result;
	}

	pid_t watchdog_pid = -1;
	if (!ArmParentDeathWatchdogAndReleaseChild(pid, watchdog_pid, &result.error))
	{
		int ignored_status = 0;
		(void)TerminateCapturedCommandProcess(pid, &ignored_status);
		ClosePipeFds(pipe_fds);
		return result;
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
		if (stop_token.stop_requested())
		{
			result.canceled = true;
			result.error = "Command canceled.";
			(void)TerminateCapturedCommandProcess(pid, &raw_status);
			finished = true;
			break;
		}

		if (timeout_ms >= 0)
		{
			const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count();

			if (elapsed_ms >= timeout_ms)
			{
				result.timed_out = true;
				result.error = "Command timed out.";
				(void)TerminateCapturedCommandProcess(pid, &raw_status);
				finished = true;
				break;
			}
		}

		std::string read_error;
		const CapturedPipeReadResult read_result = ReadAvailablePipeData(read_fd, result, &read_error);
		if (read_result == CapturedPipeReadResult::OutputLimit)
		{
			result.error = std::string(uam::platform::kCapturedCommandOutputLimitError);
			(void)TerminateCapturedCommandProcess(pid, &raw_status);
			finished = true;
			break;
		}
		if (read_result == CapturedPipeReadResult::Failed)
		{
			result.error = "Failed to read command output: " + read_error;
			(void)TerminateCapturedCommandProcess(pid, &raw_status);
			finished = true;
			break;
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
			(void)TerminateCapturedCommandProcess(pid, &raw_status);
			finished = true;
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	StopParentDeathWatchdog(watchdog_pid);

	CapturedPipeReadResult final_read_result = CapturedPipeReadResult::BudgetExhausted;
	while (!result.output_truncated && final_read_result == CapturedPipeReadResult::BudgetExhausted)
	{
		std::string final_read_error;
		final_read_result = ReadAvailablePipeData(read_fd, result, &final_read_error);
		if (final_read_result == CapturedPipeReadResult::OutputLimit && result.error.empty())
		{
			result.error = std::string(uam::platform::kCapturedCommandOutputLimitError);
		}
		else if (final_read_result == CapturedPipeReadResult::Failed && result.error.empty())
		{
			result.error = "Failed to read command output: " + final_read_error;
		}
	}
	close(read_fd);

	if (result.canceled || result.timed_out || result.output_truncated || !result.error.empty())
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

inline std::shared_ptr<uam::platform::AsyncByteWriter> CreateAsyncFdWriter(int fd, std::string* error_out = nullptr)
{
	const int writer_fd = dup(fd);
	if (writer_fd < 0)
	{
		if (error_out != nullptr)
		{
			*error_out = std::strerror(errno);
		}
		return {};
	}
	const int flags = fcntl(writer_fd, F_GETFL, 0);
	if (flags < 0 || fcntl(writer_fd, F_SETFL, flags | O_NONBLOCK) != 0)
	{
		if (error_out != nullptr)
		{
			*error_out = std::strerror(errno);
		}
		close(writer_fd);
		return {};
	}
	(void)fcntl(writer_fd, F_SETNOSIGPIPE, 1);

	return std::make_shared<uam::platform::AsyncByteWriter>(
	    [writer_fd](const char* bytes, std::size_t len, std::string& error) -> std::ptrdiff_t
	    {
		    const ssize_t written = write(writer_fd, bytes, len);
		    if (written >= 0)
		    {
			    return static_cast<std::ptrdiff_t>(written);
		    }
		    if (IsInterruptedErrno() || IsWouldBlockErrno())
		    {
			    return 0;
		    }
		    error = std::strerror(errno);
		    return -1;
	    },
	    [writer_fd] { close(writer_fd); });
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
