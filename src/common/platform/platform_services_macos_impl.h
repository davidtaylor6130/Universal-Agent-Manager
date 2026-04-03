#pragma once

#if defined(__APPLE__)

#include "platform_services.h"

#include "common/paths/app_paths.h"
#include "common/platform/sdl_includes.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>

namespace
{

	std::string ToLowerCopy(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return value;
	}

	bool ContainsInsensitive(const std::string& haystack, const std::string& needle)
	{
		const std::string lowered_haystack = ToLowerCopy(haystack);
		const std::string lowered_needle = ToLowerCopy(needle);
		return lowered_haystack.find(lowered_needle) != std::string::npos;
	}

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

				if (!working_directory.empty())
				{
					std::error_code ec;
					std::filesystem::create_directories(working_directory, ec);
					(void)chdir(working_directory.c_str());
				}

				setenv("TERM", "xterm-256color", 1);
				std::vector<char*> argv_ptrs;
				argv_ptrs.reserve(argv.size() + 1);

				for (const std::string& arg : argv)
				{
					argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
				}

				argv_ptrs.push_back(nullptr);
				execvp(argv_ptrs[0], argv_ptrs.data());
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
			return false;
		}
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
			std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);

			if (pipe == nullptr)
			{
				if (output_out != nullptr)
				{
					output_out->clear();
				}

				if (raw_status_out != nullptr)
				{
					*raw_status_out = -1;
				}

				if (error_out != nullptr)
				{
					*error_out = "Failed to launch command.";
				}

				return false;
			}

			std::array<char, 4096> buffer{};
			std::string output;

			while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr)
			{
				output += buffer.data();
			}

			const int raw_status = pipe.get_deleter()(pipe.release());

			if (output_out != nullptr)
			{
				*output_out = output;
			}

			if (raw_status_out != nullptr)
			{
				*raw_status_out = raw_status;
			}

			if (error_out != nullptr)
			{
				error_out->clear();
			}

			return true;
		}

		int NormalizeCapturedCommandExitCode(const int raw_status) const override
		{
			if (WIFEXITED(raw_status))
			{
				return WEXITSTATUS(raw_status);
			}

			return raw_status;
		}

		std::string GeminiDowngradeCommand() const override
		{
			return "brew install gemini-cli@0.30.0";
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

		std::string OpenCodeBridgeBinaryName() const override
		{
			return "uam_ollama_engine_bridge";
		}

		bool StartOpenCodeBridgeProcess(const std::vector<std::string>& argv, uam::OpenCodeBridgeState& state, std::string* error_out = nullptr) const override
		{
			if (argv.empty() || TrimAsciiWhitespace(argv.front()).empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "OpenCode bridge command is empty.";
				}

				return false;
			}

			const pid_t child_pid = fork();

			if (child_pid < 0)
			{
				if (error_out != nullptr)
				{
					*error_out = "fork failed while starting OpenCode bridge.";
				}

				return false;
			}

			if (child_pid == 0)
			{
				setsid();
				const int null_fd = open("/dev/null", O_RDWR);

				if (null_fd >= 0)
				{
					dup2(null_fd, STDIN_FILENO);
					dup2(null_fd, STDOUT_FILENO);
					dup2(null_fd, STDERR_FILENO);

					if (null_fd > STDERR_FILENO)
					{
						close(null_fd);
					}
				}

				std::vector<char*> argv_ptrs;
				argv_ptrs.reserve(argv.size() + 1);

				for (const std::string& arg : argv)
				{
					argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
				}

				argv_ptrs.push_back(nullptr);

				if (argv.front().find('/') != std::string::npos)
				{
					execv(argv_ptrs.front(), argv_ptrs.data());
				}
				else
				{
					execvp(argv_ptrs.front(), argv_ptrs.data());
				}

				_exit(127);
			}

			state.process_id = child_pid;
			return true;
		}

		bool IsOpenCodeBridgeProcessRunning(uam::OpenCodeBridgeState& state) const override
		{
			if (state.process_id <= 0)
			{
				return false;
			}

			int status = 0;
			const pid_t wait_result = waitpid(state.process_id, &status, WNOHANG);

			if (wait_result == 0)
			{
				return true;
			}

			if (wait_result == state.process_id || (wait_result < 0 && errno == ECHILD))
			{
				state.process_id = -1;
				return false;
			}

			return true;
		}

		void StopLocalBridgeProcess(uam::OpenCodeBridgeState& state) const override
		{
			if (state.process_id > 0)
			{
				const pid_t pid = state.process_id;
				int status = 0;
				kill(pid, SIGTERM);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);

				while (std::chrono::steady_clock::now() < deadline)
				{
					const pid_t wait_result = waitpid(pid, &status, WNOHANG);

					if (wait_result == pid || (wait_result < 0 && errno == ECHILD))
					{
						break;
					}

					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}

				const pid_t wait_result = waitpid(pid, &status, WNOHANG);

				if (wait_result == 0)
				{
					kill(pid, SIGKILL);
					waitpid(pid, &status, WNOHANG);
				}
			}

			state.process_id = -1;
		}

		uintmax_t NativeGeminiSessionMaxFileBytes() const override
		{
			return 0;
		}

		std::size_t NativeGeminiSessionMaxMessages() const override
		{
			return 0;
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

		std::filesystem::path ResolveOpenCodeConfigPath() const override
		{
			if (const std::optional<std::filesystem::path> home = ResolveUserHomePath(); home.has_value())
			{
				return home.value() / ".config" / "opencode" / "opencode.json";
			}

			std::error_code cwd_ec;
			const std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
			return cwd_ec ? std::filesystem::path("opencode.json") : (cwd / ".config" / "opencode" / "opencode.json");
		}
	};

	class MacUiTraits final : public IPlatformUiTraits
	{
	  public:
		void ApplyProcessDpiAwareness() const override
		{
		}

		void ConfigureOpenGlAttributes() const override
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
		}

		const char* OpenGlGlslVersion() const override
		{
			return "#version 150";
		}

		float AdjustSidebarWidth(const float layout_width, const float current_sidebar_width, const float effective_ui_scale) const override
		{
			(void)layout_width;
			(void)effective_ui_scale;
			return current_sidebar_width;
		}

		bool UseWindowsLayoutAdjustments() const override
		{
			return false;
		}

		bool UsesLogicalPointsForUiScale() const override
		{
			return true;
		}

		float PlatformUiSpacingScale() const override
		{
			return 1.0f;
		}

		std::optional<bool> DetectSystemPrefersLightTheme() const override
		{
			if (const char* env_style = std::getenv("AppleInterfaceStyle"))
			{
				return ContainsInsensitive(env_style, "dark") ? std::optional<bool>(false) : std::optional<bool>(true);
			}

			FILE* pipe = popen("defaults read -g AppleInterfaceStyle 2>/dev/null", "r");

			if (pipe == nullptr)
			{
				return std::nullopt;
			}

			std::array<char, 128> buffer{};
			std::string output;

			while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
			{
				output += buffer.data();
			}

			pclose(pipe);
			const std::string trimmed = TrimAsciiWhitespace(output);

			if (trimmed.empty())
			{
				return std::nullopt;
			}

			return ContainsInsensitive(trimmed, "dark") ? std::optional<bool>(false) : std::optional<bool>(true);
		}
	};

	PlatformServices& CreatePlatformServices()
	{
		static MacTerminalRuntime terminal_runtime;
		static MacProcessService process_service;
		static MacFileDialogService file_dialog_service;
		static MacPathService path_service;
		static MacUiTraits ui_traits;
		static PlatformServices services{
		    terminal_runtime,
		    process_service,
		    file_dialog_service,
		    path_service,
		    ui_traits,
		};
		return services;
	}

} // namespace

#endif // defined(__APPLE__)
