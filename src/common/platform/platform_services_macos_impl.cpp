#include "platform_services_macos_impl.h"
#include <Security/Security.h>

#include "common/paths/app_paths.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

namespace
{

	std::string TrimAsciiWhitespace(const std::string& value)
	{
		const std::size_t start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const std::size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	std::string ShellQuotePosix(const std::string& value)
	{
		std::string escaped = "'";

		for (const char ch : value)
		{
			if (ch == '\'')
			{
				escaped += "'\\''";
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('\'');
		return escaped;
	}

	bool RunShellCommand(const std::string& command)
	{
		return std::system(command.c_str()) == 0;
	}

	bool RunShellCommandCapture(const std::string& command, std::string* output_out = nullptr)
	{
		FILE* pipe = popen(command.c_str(), "r");

		if (pipe == nullptr)
		{
			if (output_out != nullptr)
			{
				output_out->clear();
			}

			return false;
		}

		std::string output;
		std::array<char, 512> buffer{};

		while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
		{
			output.append(buffer.data());
		}

		const int status = pclose(pipe);

		if (output_out != nullptr)
		{
			*output_out = TrimAsciiWhitespace(output);
		}

		return status == 0;
	}

	bool IsShellCommandAvailable(const std::string& command)
	{
		return RunShellCommand("command -v " + command + " >/dev/null 2>&1");
	}

	bool IsExecutableFile(const std::filesystem::path& candidate)
	{
		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec) || !std::filesystem::is_regular_file(candidate, ec))
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
		if (entry.empty())
		{
			return;
		}

		if (std::find(entries.begin(), entries.end(), entry) == entries.end())
		{
			entries.push_back(entry);
		}
	}

	std::vector<std::string> CollectTerminalPathSearchDirs()
	{
		std::vector<std::string> candidate_dirs;
		if (const char* path_env = std::getenv("PATH"); path_env != nullptr)
		{
			candidate_dirs = SplitPathEnv(path_env);
		}

		const std::array<const char*, 8> fallback_dirs = {
			"/opt/homebrew/bin",
			"/opt/homebrew/sbin",
			"/usr/local/bin",
			"/usr/local/sbin",
			"/usr/bin",
			"/bin",
			"/usr/sbin",
			"/sbin",
		};
		for (const char* dir : fallback_dirs)
		{
			AppendUniquePathEntry(candidate_dirs, dir);
		}

		if (const char* home = std::getenv("HOME"); home != nullptr)
		{
			const std::filesystem::path home_path(home);
			AppendUniquePathEntry(candidate_dirs, (home_path / ".volta" / "bin").string());
			AppendUniquePathEntry(candidate_dirs, (home_path / ".asdf" / "shims").string());
			AppendUniquePathEntry(candidate_dirs, (home_path / ".fnm").string());

			const std::filesystem::path nvm_versions_dir = home_path / ".nvm" / "versions" / "node";
			std::error_code ec;
			if (std::filesystem::exists(nvm_versions_dir, ec) && std::filesystem::is_directory(nvm_versions_dir, ec))
			{
				for (const auto& entry : std::filesystem::directory_iterator(nvm_versions_dir, ec))
				{
					if (ec || !entry.is_directory())
					{
						continue;
					}

					const std::filesystem::path bin_dir = entry.path() / "bin";
					if (std::filesystem::exists(bin_dir, ec) && std::filesystem::is_directory(bin_dir, ec))
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

		if (command.find('/') != std::string::npos)
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

	std::string ResolveExecutablePathForTerminal(const std::string& command)
	{
		return ResolveExecutablePathForTerminal(command, CollectTerminalPathSearchDirs());
	}

	bool ScriptShebangMentionsNode(const std::filesystem::path& executable_path)
	{
		std::ifstream stream(executable_path);
		if (!stream.is_open())
		{
			return false;
		}

		std::string first_line;
		if (!std::getline(stream, first_line))
		{
			return false;
		}

		if (first_line.rfind("#!", 0) != 0)
		{
			return false;
		}

		return first_line.find("node") != std::string::npos;
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

	bool ReadAvailablePipeData(const int fd, std::string* output_out, std::string* error_out = nullptr)
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

			if (errno == EINTR)
			{
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK)
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

	ProcessExecutionResult ExecuteCapturedCommandPosix(const std::string& command, const int timeout_ms, std::stop_token stop_token)
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
			close(pipe_fds[0]);
			close(pipe_fds[1]);
			result.error = "fork failed.";
			return result;
		}

		if (pid == 0)
		{
			dup2(pipe_fds[1], STDOUT_FILENO);
			dup2(pipe_fds[1], STDERR_FILENO);
			close(pipe_fds[0]);
			close(pipe_fds[1]);
			execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}

		close(pipe_fds[1]);
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

	std::ptrdiff_t ReadNonBlockingFd(const int fd, char* buffer, const std::size_t buffer_size)
	{
		if (fd < 0)
		{
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

			if (errno == EINTR)
			{
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return -2;
			}

			return -1;
		}
	}

	class MacTerminalRuntime final : public IPlatformTerminalRuntime
	{
	  public:
		bool IsAvailable() const override
		{
			return true;
		}

		bool StartCliTerminalProcess(uam::CliTerminalState& terminal,
		                             const std::filesystem::path& working_directory,
		                             const std::vector<std::string>& argv,
		                             std::string* error_out = nullptr) const override
		{
			if (argv.empty() || TrimAsciiWhitespace(argv.front()).empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Interactive provider command is empty.";
				}

				return false;
			}

			if (!working_directory.empty())
			{
				std::error_code wd_ec;
				std::filesystem::create_directories(working_directory, wd_ec);
				if (wd_ec || !std::filesystem::is_directory(working_directory, wd_ec))
				{
					if (error_out != nullptr)
					{
						*error_out = "Failed to prepare provider working directory: " + (wd_ec ? wd_ec.message() : working_directory.string());
					}
					return false;
				}

				if (access(working_directory.c_str(), X_OK) != 0)
				{
					if (error_out != nullptr)
					{
						*error_out = "Provider working directory is not accessible: " + std::string(std::strerror(errno));
					}
					return false;
				}
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
					*error_out = "gemini not found on PATH in app environment";
				}
				close(master_fd);
				close(slave_fd);
				return false;
			}

			if (ScriptShebangMentionsNode(resolved_executable))
			{
				const std::string resolved_node = ResolveExecutablePathForTerminal("node", terminal_path_dirs);
				if (resolved_node.empty())
				{
					if (error_out != nullptr)
					{
						*error_out = "node not found on PATH in app environment (required by gemini CLI)";
					}
					close(master_fd);
					close(slave_fd);
					return false;
				}
			}

			const pid_t pid = fork();

			if (pid < 0)
			{
				if (error_out != nullptr)
				{
					*error_out = "fork failed.";
				}

				close(master_fd);
				close(slave_fd);
				return false;
			}

			if (pid == 0)
			{
				setsid();
				ioctl(slave_fd, TIOCSCTTY, 0);
				dup2(slave_fd, STDIN_FILENO);
				dup2(slave_fd, STDOUT_FILENO);
				dup2(slave_fd, STDERR_FILENO);
				close(master_fd);
				close(slave_fd);

				if (!working_directory.empty() && chdir(working_directory.c_str()) != 0)
				{
					_exit(126);
				}

				setenv("TERM", "xterm-256color", 1);
				if (!terminal_path_env.empty())
				{
					setenv("PATH", terminal_path_env.c_str(), 1);
				}
				std::vector<std::string> resolved_argv = argv;
				resolved_argv[0] = resolved_executable;
				std::vector<char*> argv_ptrs;
				argv_ptrs.reserve(resolved_argv.size() + 1);

				for (const std::string& arg : resolved_argv)
				{
					argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
				}

				argv_ptrs.push_back(nullptr);
				execv(argv_ptrs[0], argv_ptrs.data());
				_exit(127);
			}

			close(slave_fd);
			terminal.master_fd = master_fd;
			terminal.child_pid = pid;
			const int flags = fcntl(terminal.master_fd, F_GETFL, 0);

			if (flags >= 0)
			{
				(void)fcntl(terminal.master_fd, F_SETFL, flags | O_NONBLOCK);
			}

			return true;
		}

		void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const override
		{
			if (terminal.master_fd >= 0)
			{
				close(terminal.master_fd);
				terminal.master_fd = -1;
			}

			terminal.child_pid = -1;
		}

		bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, const std::size_t len) const override
		{
			if (bytes == nullptr || len == 0)
			{
				return true;
			}

			std::size_t offset = 0;

			while (offset < len)
			{
				const ssize_t written = write(terminal.master_fd, bytes + offset, len - offset);

				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}

				if (written < 0 && errno == EINTR)
				{
					continue;
				}

				return false;
			}

			return true;
		}

		void StopCliTerminalProcess(uam::CliTerminalState& terminal, const bool fast_exit) const override
		{
			if (terminal.child_pid <= 0)
			{
				return;
			}

			const pid_t child_pid = terminal.child_pid;
			int status = 0;
			const auto has_exited = [&](const bool wait_for_exit, const double timeout_seconds) -> bool
			{
				const auto wait_start = std::chrono::steady_clock::now();
				const auto wait_timeout = std::chrono::duration<double>(std::max(0.0, timeout_seconds));

				while (true)
				{
					const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);

					if (wait_result == child_pid)
					{
						return true;
					}

					if (wait_result < 0)
					{
						if (errno == EINTR)
						{
							continue;
						}

						return errno == ECHILD;
					}

					if (!wait_for_exit)
					{
						return false;
					}

					if ((std::chrono::steady_clock::now() - wait_start) >= wait_timeout)
					{
						return false;
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(8));
				}
			};

			if (fast_exit)
			{
				kill(child_pid, SIGHUP);
				kill(child_pid, SIGTERM);
				kill(child_pid, SIGKILL);
				(void)has_exited(false, 0.0);
			}
			else
			{
				kill(child_pid, SIGHUP);

				if (!has_exited(true, 0.25))
				{
					kill(child_pid, SIGTERM);

					if (!has_exited(true, 0.35))
					{
						kill(child_pid, SIGKILL);
						(void)has_exited(true, 0.15);
					}
				}
			}

			terminal.child_pid = -1;
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

		std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState& terminal, char* buffer, const std::size_t buffer_size) const override
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

				if (errno == EINTR)
				{
					continue;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK)
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

			int status = 0;
			const pid_t wait_result = waitpid(terminal.child_pid, &status, WNOHANG);

			if (wait_result == 0)
			{
				return false;
			}

			if (wait_result == terminal.child_pid || (wait_result < 0 && errno == ECHILD))
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
		explicit MacDataRootLock(const int fd) : m_fd(fd)
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
			return "cd " + ShellQuotePosix(working_directory.string()) + " && " + command;
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

		int NormalizeCapturedCommandExitCode(const int raw_status) const override
		{
			if (WIFEXITED(raw_status))
			{
				return WEXITSTATUS(raw_status);
			}

			return raw_status;
		}

		ProcessExecutionResult ExecuteCommand(const std::string& command, const int timeout_ms = -1, std::stop_token stop_token = {}) const override
		{
			return ExecuteCapturedCommandPosix(command, timeout_ms, stop_token);
		}

		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields& process,
		                       const std::filesystem::path& working_directory,
		                       const std::vector<std::string>& argv,
		                       std::string* error_out = nullptr) const override
		{
			if (argv.empty() || TrimAsciiWhitespace(argv.front()).empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Stdio process command is empty.";
				}
				return false;
			}

			if (!working_directory.empty())
			{
				std::error_code wd_ec;
				std::filesystem::create_directories(working_directory, wd_ec);
				if (wd_ec || !std::filesystem::is_directory(working_directory, wd_ec))
				{
					if (error_out != nullptr)
					{
						*error_out = "Failed to prepare process working directory: " + (wd_ec ? wd_ec.message() : working_directory.string());
					}
					return false;
				}
			}

			int stdin_pipe[2] = {-1, -1};
			int stdout_pipe[2] = {-1, -1};
			int stderr_pipe[2] = {-1, -1};
			auto close_pipe = [](int (&pipe_fds)[2])
			{
				if (pipe_fds[0] >= 0)
				{
					close(pipe_fds[0]);
					pipe_fds[0] = -1;
				}
				if (pipe_fds[1] >= 0)
				{
					close(pipe_fds[1]);
					pipe_fds[1] = -1;
				}
			};

			if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create stdio process pipes.";
				}
				close_pipe(stdin_pipe);
				close_pipe(stdout_pipe);
				close_pipe(stderr_pipe);
				return false;
			}

			const std::vector<std::string> path_dirs = CollectTerminalPathSearchDirs();
			const std::string path_env = JoinPathEntries(path_dirs);
			const std::string resolved_executable = ResolveExecutablePathForTerminal(argv.front(), path_dirs);
			if (resolved_executable.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "gemini not found on PATH in app environment";
				}
				close_pipe(stdin_pipe);
				close_pipe(stdout_pipe);
				close_pipe(stderr_pipe);
				return false;
			}

			if (ScriptShebangMentionsNode(resolved_executable) && ResolveExecutablePathForTerminal("node", path_dirs).empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "node not found on PATH in app environment (required by gemini CLI)";
				}
				close_pipe(stdin_pipe);
				close_pipe(stdout_pipe);
				close_pipe(stderr_pipe);
				return false;
			}

			const pid_t pid = fork();
			if (pid < 0)
			{
				if (error_out != nullptr)
				{
					*error_out = "fork failed.";
				}
				close_pipe(stdin_pipe);
				close_pipe(stdout_pipe);
				close_pipe(stderr_pipe);
				return false;
			}

			if (pid == 0)
			{
				dup2(stdin_pipe[0], STDIN_FILENO);
				dup2(stdout_pipe[1], STDOUT_FILENO);
				dup2(stderr_pipe[1], STDERR_FILENO);
				close_pipe(stdin_pipe);
				close_pipe(stdout_pipe);
				close_pipe(stderr_pipe);

				if (!working_directory.empty() && chdir(working_directory.c_str()) != 0)
				{
					_exit(126);
				}

				if (!path_env.empty())
				{
					setenv("PATH", path_env.c_str(), 1);
				}

				std::vector<std::string> resolved_argv = argv;
				resolved_argv[0] = resolved_executable;
				std::vector<char*> argv_ptrs;
				argv_ptrs.reserve(resolved_argv.size() + 1);
				for (const std::string& arg : resolved_argv)
				{
					argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
				}
				argv_ptrs.push_back(nullptr);
				execv(argv_ptrs[0], argv_ptrs.data());
				_exit(127);
			}

			close(stdin_pipe[0]);
			close(stdout_pipe[1]);
			close(stderr_pipe[1]);
			process.stdin_write_fd = stdin_pipe[1];
			process.stdout_read_fd = stdout_pipe[0];
			process.stderr_read_fd = stderr_pipe[0];
			process.child_pid = pid;

			for (const int fd : {process.stdout_read_fd, process.stderr_read_fd})
			{
				const int flags = fcntl(fd, F_GETFL, 0);
				if (flags >= 0)
				{
					(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
				}
			}

			return true;
		}

		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
		{
			if (process.stdin_write_fd >= 0)
			{
				close(process.stdin_write_fd);
				process.stdin_write_fd = -1;
			}
			if (process.stdout_read_fd >= 0)
			{
				close(process.stdout_read_fd);
				process.stdout_read_fd = -1;
			}
			if (process.stderr_read_fd >= 0)
			{
				close(process.stderr_read_fd);
				process.stderr_read_fd = -1;
			}
			process.child_pid = -1;
		}

		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, const std::size_t len) const override
		{
			if (bytes == nullptr || len == 0)
			{
				return true;
			}
			if (process.stdin_write_fd < 0)
			{
				return false;
			}

			std::size_t offset = 0;
			while (offset < len)
			{
				const ssize_t written = write(process.stdin_write_fd, bytes + offset, len - offset);
				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR)
				{
					continue;
				}
				return false;
			}
			return true;
		}

		void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, const bool fast_exit) const override
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

		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, const std::size_t buffer_size) const override
		{
			return ReadNonBlockingFd(process.stdout_read_fd, buffer, buffer_size);
		}

		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields& process, char* buffer, const std::size_t buffer_size) const override
		{
			return ReadNonBlockingFd(process.stderr_read_fd, buffer, buffer_size);
		}

		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process) const override
		{
			if (process.child_pid <= 0)
			{
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
				process.child_pid = -1;
				return true;
			}
			return false;
		}

		std::string GeminiDowngradeCommand() const override
		{
			return "npm install -g @google/gemini-cli@latest";
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
			std::filesystem::create_directories(data_root, ec);
			if (ec)
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
			if (!IsShellCommandAvailable("osascript"))
			{
				if (error_out != nullptr)
				{
					*error_out = "Native path picker is unavailable (missing osascript).";
				}

				return false;
			}

			std::string script = "set selectedPath to POSIX path of (choose " + std::string(target == PlatformPathBrowseTarget::Directory ? "folder" : "file") + " with prompt " + std::string(target == PlatformPathBrowseTarget::Directory ? "\"Select folder\"" : "\"Select file\"");

			if (!initial_path.empty())
			{
				script += " default location POSIX file \"" + EscapeAppleScriptQuotedString(initial_path.string()) + "\"";
			}

			script += ")";
			std::string selected_path;
			const std::string command = "osascript -e " + ShellQuotePosix(script) + " -e " + ShellQuotePosix("return selectedPath");

			if (!RunShellCommandCapture(command, &selected_path) || selected_path.empty())
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
			std::filesystem::create_directories(folder_path, ec);

			if (ec)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create folder: " + ec.message();
				}

				return false;
			}

			const std::string command = "open " + ShellQuotePosix(folder_path.string());

			if (!RunShellCommand(command))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to open folder in file manager.";
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

			if (!std::filesystem::exists(file_path))
			{
				return OpenFolderInFileManager(file_path.parent_path(), error_out);
			}

			const std::string command = "open -R " + ShellQuotePosix(file_path.string());

			if (!RunShellCommand(command))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to reveal file in file manager.";
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
			if (const char* home = std::getenv("HOME"))
			{
				const std::string value = TrimAsciiWhitespace(home);

				if (!value.empty())
				{
					return std::filesystem::path(value);
				}
			}

			return std::nullopt;
		}

		std::filesystem::path ExpandLeadingTildePath(const std::string& raw_path) const override
		{
			const std::string trimmed = TrimAsciiWhitespace(raw_path);

			if (trimmed.empty())
			{
				return {};
			}

			if (trimmed[0] != '~')
			{
				return std::filesystem::path(trimmed);
			}

			if (const std::optional<std::filesystem::path> home = ResolveUserHomePath(); home.has_value())
			{
				if (trimmed.size() == 1)
				{
					return home.value();
				}

				if (trimmed[1] == '/')
				{
					return home.value() / trimmed.substr(2);
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
