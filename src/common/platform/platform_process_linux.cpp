#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace uam::platform_linux_impl
{
	namespace
	{
		void CloseFd(int& descriptor)
		{
			if (descriptor >= 0) (void)close(descriptor);
			descriptor = -1;
		}

		int ExitCode(int status)
		{
			if (WIFEXITED(status)) return WEXITSTATUS(status);
			if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
			return -1;
		}

		std::ptrdiff_t ReadNonBlocking(int descriptor, char* buffer,
		                              std::size_t size, std::string* error_out)
		{
			if (descriptor < 0) return 0;
			for (;;)
			{
				const ssize_t count = read(descriptor, buffer, size);
				if (count >= 0) return static_cast<std::ptrdiff_t>(count);
				if (errno == EINTR) continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
				if (error_out != nullptr) *error_out = std::strerror(errno);
				return -1;
			}
		}

		std::shared_ptr<uam::platform::AsyncByteWriter> CreateWriter(
		    int descriptor, std::string* error_out)
		{
			const int writer = dup(descriptor);
			if (writer < 0)
			{
				if (error_out != nullptr) *error_out = std::strerror(errno);
				return {};
			}
			const int flags = fcntl(writer, F_GETFL, 0);
			if (flags < 0 || fcntl(writer, F_SETFL, flags | O_NONBLOCK) != 0)
			{
				if (error_out != nullptr) *error_out = std::strerror(errno);
				(void)close(writer);
				return {};
			}
			return std::make_shared<uam::platform::AsyncByteWriter>(
			    [writer](const char* bytes, std::size_t size,
			             std::string& error) -> std::ptrdiff_t
			    {
				    const ssize_t written = write(writer, bytes, size);
				    if (written >= 0) return written;
				    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
				    error = std::strerror(errno);
				    return -1;
			    },
			    [writer] { (void)close(writer); });
		}

		class LinuxProcessService final : public IPlatformProcessService
		{
		  public:
			bool SupportsDetachedProcesses() const override { return true; }
			bool PopulateLocalTime(std::time_t value, std::tm* output) const override
			{
				return output != nullptr && localtime_r(&value, output) != nullptr;
			}
			std::string BuildShellCommandWithWorkingDirectory(
			    const std::filesystem::path&, const std::string&) const override { return {}; }
			bool CaptureCommandOutput(const std::string&, std::string*, int*,
			                          std::string* error_out) const override
			{
				if (error_out != nullptr) *error_out = "Captured shell commands are unavailable in the runner.";
				return false;
			}
			int NormalizeCapturedCommandExitCode(int status) const override { return status; }
			ProcessExecutionResult ExecuteCommand(const std::string&, int,
			                                      std::stop_token) const override
			{
				ProcessExecutionResult result;
				result.error = "Shell commands are unavailable in the runner.";
				return result;
			}

			bool StartStdioProcess(
			    uam::platform::StdioProcessPlatformFields& process,
			    const std::filesystem::path& working_directory,
			    const std::vector<std::string>& argv, std::string* error_out,
			    const std::vector<std::pair<std::string, std::string>>& environment) const override
			{
				if (argv.empty() || argv.front().empty() || !working_directory.is_absolute() ||
				    !std::filesystem::is_directory(working_directory))
				{
					if (error_out != nullptr) *error_out = "A valid command and absolute working directory are required.";
					return false;
				}
				for (const auto& [name, value] : environment)
				{
					if (name.empty() || name.find('=') != std::string::npos ||
					    name.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
					{
						if (error_out != nullptr) *error_out = "Invalid process environment override.";
						return false;
					}
				}
				int input[2] = {-1, -1};
				int output[2] = {-1, -1};
				int diagnostic[2] = {-1, -1};
				if (pipe2(input, O_CLOEXEC) != 0 || pipe2(output, O_CLOEXEC) != 0 ||
				    pipe2(diagnostic, O_CLOEXEC) != 0)
				{
					if (error_out != nullptr) *error_out = std::strerror(errno);
					for (int* pair : {input, output, diagnostic})
					{
						CloseFd(pair[0]);
						CloseFd(pair[1]);
					}
					return false;
				}
				const pid_t child = fork();
				if (child < 0)
				{
					if (error_out != nullptr) *error_out = std::strerror(errno);
					for (int* pair : {input, output, diagnostic})
					{
						CloseFd(pair[0]);
						CloseFd(pair[1]);
					}
					return false;
				}
				if (child == 0)
				{
					(void)setpgid(0, 0);
					if (chdir(working_directory.c_str()) != 0 ||
					    dup2(input[0], STDIN_FILENO) < 0 ||
					    dup2(output[1], STDOUT_FILENO) < 0 ||
					    dup2(diagnostic[1], STDERR_FILENO) < 0)
						_exit(126);
					for (int* pair : {input, output, diagnostic})
					{
						CloseFd(pair[0]);
						CloseFd(pair[1]);
					}
					for (const auto& [name, value] : environment)
						if (setenv(name.c_str(), value.c_str(), 1) != 0) _exit(126);
					std::vector<char*> arguments;
					arguments.reserve(argv.size() + 1);
					for (const std::string& argument : argv)
						arguments.push_back(const_cast<char*>(argument.c_str()));
					arguments.push_back(nullptr);
					execvp(arguments.front(), arguments.data());
					_exit(127);
				}
				CloseFd(input[0]);
				CloseFd(output[1]);
				CloseFd(diagnostic[1]);
				process.stdin_write_fd = input[1];
				process.stdout_read_fd = output[0];
				process.stderr_read_fd = diagnostic[0];
				process.child_pid = child;
				for (const int descriptor : {process.stdout_read_fd, process.stderr_read_fd})
				{
					const int flags = fcntl(descriptor, F_GETFL, 0);
					if (flags >= 0) (void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
				}
				return true;
			}

			bool StartStdioProcessWithInput(
			    uam::platform::StdioProcessPlatformFields& process,
			    const std::filesystem::path& working_directory,
			    const std::vector<std::string>& argv, std::string_view input,
			    std::string* error_out,
			    const std::vector<std::pair<std::string, std::string>>& environment) const override
			{
				if (!StartStdioProcess(process, working_directory, argv, error_out, environment)) return false;
				if (!WriteToStdioProcess(process, input.data(), input.size(), error_out))
				{
					StopStdioProcess(process, true);
					return false;
				}
				CloseStdioProcessInput(process);
				return true;
			}

			void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
			{
				process.stdin_writer.reset();
				CloseFd(process.stdin_write_fd);
				CloseFd(process.stdout_read_fd);
				CloseFd(process.stderr_read_fd);
				process.child_pid = -1;
			}
			bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process,
			                         const char* bytes, std::size_t size,
			                         std::string* error_out) const override
			{
				if (bytes == nullptr || size == 0) return true;
				if (process.stdin_write_fd < 0)
				{
					if (error_out != nullptr) *error_out = "stdin pipe is closed.";
					return false;
				}
				if (process.stdin_writer == nullptr)
					process.stdin_writer = CreateWriter(process.stdin_write_fd, error_out);
				return process.stdin_writer != nullptr &&
				       process.stdin_writer->Enqueue(bytes, size, error_out);
			}
			void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields& process) const override
			{
				if (process.stdin_writer != nullptr)
					(void)process.stdin_writer->Flush(uam::platform::kAsyncInputCloseDrainTimeout);
				process.stdin_writer.reset();
				CloseFd(process.stdin_write_fd);
			}
			void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields& process,
			                           bool fast_exit) const override
			{
				if (process.child_pid <= 0) return;
				const pid_t child = process.child_pid;
				const auto signal_process = [child](int signal)
				{
					(void)kill(-child, signal);
					(void)kill(child, signal);
				};
				signal_process(fast_exit ? SIGKILL : SIGTERM);
				int status = 0;
				const auto wait_bounded = [&]
				{
					for (int attempt = 0; attempt < 100; ++attempt)
					{
						const pid_t result = waitpid(child, &status, WNOHANG);
						if (result == child || (result < 0 && errno == ECHILD)) return true;
						std::this_thread::sleep_for(std::chrono::milliseconds(10));
					}
					return false;
				};
				if (!wait_bounded() && !fast_exit)
				{
					signal_process(SIGKILL);
					(void)wait_bounded();
				}
				if (waitpid(child, &status, WNOHANG) == 0)
					std::thread([child]
					{
						int child_status = 0;
						while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
					}).detach();
				process.child_pid = -1;
			}
			void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process,
			                      bool fast_exit) const override
			{
				TerminateStdioProcess(process, fast_exit);
				CloseStdioProcessHandles(process);
			}
			std::ptrdiff_t ReadStdioProcessStdout(
			    uam::platform::StdioProcessPlatformFields& process, char* buffer,
			    std::size_t size, std::string* error_out) const override
			{
				return ReadNonBlocking(process.stdout_read_fd, buffer, size, error_out);
			}
			std::ptrdiff_t ReadStdioProcessStderr(
			    uam::platform::StdioProcessPlatformFields& process, char* buffer,
			    std::size_t size, std::string* error_out) const override
			{
				return ReadNonBlocking(process.stderr_read_fd, buffer, size, error_out);
			}
			bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process,
			                            int* exit_code_out) const override
			{
				if (process.child_pid <= 0) return true;
				int status = 0;
				const pid_t result = waitpid(process.child_pid, &status, WNOHANG);
				if (result == 0) return false;
				if (result == process.child_pid || (result < 0 && errno == ECHILD))
				{
					if (exit_code_out != nullptr)
						*exit_code_out = result == process.child_pid ? ExitCode(status) : -1;
					process.child_pid = -1;
					return true;
				}
				return false;
			}
			std::filesystem::path ResolveCurrentExecutablePath() const override
			{
				std::array<char, 4096> path{};
				const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
				return size > 0 ? std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(size))) : std::filesystem::path{};
			}
			std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(
			    const std::filesystem::path&, std::string*) const override { return {}; }
			uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
			std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
			std::string GenerateUuid() const override
			{
				std::random_device random;
				return std::to_string(static_cast<unsigned long long>(random())) + "-" +
				       std::to_string(static_cast<unsigned long long>(random()));
			}
			bool LaunchShellAt(const std::filesystem::path&, std::string* error_out) const override
			{
				if (error_out != nullptr) *error_out = "Shell launch is unavailable in the runner.";
				return false;
			}
			void SetKeepSystemAwake(bool) const override {}
			bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
		};
	}

	IPlatformProcessService& GetLinuxProcessService()
	{
		static LinuxProcessService service;
		return service;
	}
}
