#include "platform_services_windows_impl_internal.h"

using namespace uam::platform_windows_impl;

namespace uam::platform_windows_impl
{

class WindowsProcessService final : public IPlatformProcessService
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

		return localtime_s(tm_out, &timestamp) == 0;
	}

	std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path& working_directory, const std::string& command) const override
	{
		return "cd /d " + uam::shell::EscapeArg(working_directory.string()) + " && " + command;
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
		if (raw_status == STILL_ACTIVE)
		{
			return -1;
		}
		return raw_status;
	}

	ProcessExecutionResult ExecuteCommand(const std::string& command, int timeout_ms = -1, std::stop_token stop_token = {}) const override
	{
		ProcessExecutionResult result;
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE stdout_read = INVALID_HANDLE_VALUE;
		HANDLE stdout_write = INVALID_HANDLE_VALUE;

		if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
		{
			result.error = "Failed to create command output pipe.";
			return result;
		}

		SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
		const std::wstring command_w = WideFromUtf8("cmd.exe /C " + command);

		if (command_w.empty())
		{
			CloseInvalidHandleIfOpen(stdout_read);
			CloseInvalidHandleIfOpen(stdout_write);
			result.error = "Failed to encode command line.";
			return result;
		}

		std::vector<wchar_t> command_line(command_w.begin(), command_w.end());
		command_line.push_back(L'\0');

		STARTUPINFOW startup_info{};
		startup_info.cb = sizeof(startup_info);
		startup_info.dwFlags = STARTF_USESTDHANDLES;
		startup_info.hStdInput = INVALID_HANDLE_VALUE;
		startup_info.hStdOutput = stdout_write;
		startup_info.hStdError = stdout_write;

		PROCESS_INFORMATION process_info{};
		const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info);
		CloseInvalidHandleIfOpen(stdout_write);

		if (!created)
		{
			const DWORD launch_error = GetLastError();
			CloseInvalidHandleIfOpen(stdout_read);
			result.error = "Failed to launch command (Win32 error " + std::to_string(launch_error) + ").";
			return result;
		}

		HANDLE job_object = nullptr;
		std::string job_error;
		if (!CreateKillOnCloseJobForProcess(process_info.hProcess, &job_object, &job_error))
		{
			TerminateProcessAndWaitBriefly(process_info.hProcess, 1);
			CloseInvalidHandleIfOpen(stdout_read);
			CloseInvalidHandleIfOpen(process_info.hProcess);
			CloseInvalidHandleIfOpen(process_info.hThread);
			result.error = uam::strings::NonEmptyOrFallback(job_error, "Failed to protect command process tree.");
			return result;
		}

		std::array<char, 4096> buffer{};
		const auto deadline = (timeout_ms >= 0) ? (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)) : std::chrono::steady_clock::time_point::max();
		bool process_finished = false;
		bool pipe_closed = false;

		while (!process_finished || !pipe_closed)
		{
			DWORD available = 0;

			if (!pipe_closed && PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr))
			{
				while (available > 0)
				{
					DWORD bytes_read = 0;
					const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));

					if (!ReadFile(stdout_read, buffer.data(), to_read, &bytes_read, nullptr))
					{
						pipe_closed = true;
						break;
					}

					if (bytes_read == 0)
					{
						pipe_closed = true;
						break;
					}

					result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
					available -= bytes_read;
				}
			}

			const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 0);

			if (wait_result == WAIT_OBJECT_0)
			{
				process_finished = true;
			}

			if (!process_finished && stop_token.stop_requested())
			{
				result.canceled = true;
				result.error = "Command canceled.";
				TerminateProcessTreeAndWaitBriefly(job_object, process_info.hProcess, 1);
				process_finished = true;
			}

			if (!process_finished && timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
			{
				result.timed_out = true;
				result.error = "Command timed out.";
				TerminateProcessTreeAndWaitBriefly(job_object, process_info.hProcess, 1);
				process_finished = true;
			}

			if (process_finished && !pipe_closed)
			{
				DWORD bytes_read = 0;

				while (ReadFile(stdout_read, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) && bytes_read > 0)
				{
					result.output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
				}

				pipe_closed = true;
			}

			if (process_finished && pipe_closed)
			{
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		DWORD exit_code = 1;
		GetExitCodeProcess(process_info.hProcess, &exit_code);
		CloseInvalidHandleIfOpen(stdout_read);
		CloseHandle(job_object);
		CloseInvalidHandleIfOpen(process_info.hProcess);
		CloseInvalidHandleIfOpen(process_info.hThread);

		if (!result.timed_out && !result.canceled)
		{
			result.exit_code = static_cast<int>(exit_code);
			result.ok = result.error.empty() && result.exit_code == 0;
		}

		return result;
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

		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HANDLE stdin_read = INVALID_HANDLE_VALUE;
		HANDLE stdin_write = INVALID_HANDLE_VALUE;
		HANDLE stdout_read = INVALID_HANDLE_VALUE;
		HANDLE stdout_write = INVALID_HANDLE_VALUE;
		HANDLE stderr_read = INVALID_HANDLE_VALUE;
		HANDLE stderr_write = INVALID_HANDLE_VALUE;

		if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) || !CreatePipe(&stdout_read, &stdout_write, &sa, 0) || !CreatePipe(&stderr_read, &stderr_write, &sa, 0))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create stdio process pipes.";
			}
			CloseStdioPipeHandles(stdin_read, stdin_write, stdout_read, stdout_write, stderr_read, stderr_write);
			return false;
		}

		SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

		const WindowsLaunchCommand launch = BuildWindowsLaunchCommand(argv);
		const std::wstring command_w = WideFromUtf8(launch.command_line);
		if (command_w.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to encode stdio command line.";
			}
			CloseStdioPipeHandles(stdin_read, stdin_write, stdout_read, stdout_write, stderr_read, stderr_write);
			return false;
		}

		std::vector<wchar_t> command_line(command_w.begin(), command_w.end());
		command_line.push_back(L'\0');
		const std::wstring working_directory_w = working_directory.empty() ? std::wstring() : working_directory.wstring();

		STARTUPINFOW startup_info{};
		startup_info.cb = sizeof(startup_info);
		startup_info.dwFlags = STARTF_USESTDHANDLES;
		startup_info.hStdInput = stdin_read;
		startup_info.hStdOutput = stdout_write;
		startup_info.hStdError = stderr_write;

		PROCESS_INFORMATION process_info{};
		const DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
		const wchar_t* working_directory_arg = working_directory.empty() ? nullptr : working_directory_w.c_str();
		const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, creation_flags, nullptr, working_directory_arg, &startup_info, &process_info);

		CloseStdioChildPipeEnds(stdin_read, stdout_write, stderr_write);

		if (!created)
		{
			const DWORD launch_error = GetLastError();
			if (error_out != nullptr)
			{
				*error_out = "Failed to start stdio process. " + FormatWindowsError(launch_error) + ". Command: " + launch.command_line + "." + WindowsLaunchDiagnosticSuffix(launch);
			}
			CloseStdioParentPipeEnds(stdin_write, stdout_read, stderr_read);
			return false;
		}

		HANDLE job = nullptr;
		std::string job_error;
		if (!CreateKillOnCloseJobForProcess(process_info.hProcess, &job, &job_error))
		{
			if (error_out != nullptr)
			{
				*error_out = uam::strings::NonEmptyOrFallback(job_error, "Failed to protect stdio process tree.");
			}
			TerminateProcessAndWaitBriefly(process_info.hProcess, 1);
			CloseInvalidHandleIfOpen(process_info.hThread);
			CloseInvalidHandleIfOpen(process_info.hProcess);
			CloseStdioParentPipeEnds(stdin_write, stdout_read, stderr_read);
			return false;
		}

		process.stdin_write = stdin_write;
		process.stdout_read = stdout_read;
		process.stderr_read = stderr_read;
		process.process_info = process_info;
		process.job_object = job;

		return true;
	}

	void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
	{
		CloseInvalidHandleIfOpen(process.stdin_write);
		CloseInvalidHandleIfOpen(process.stdout_read);
		CloseInvalidHandleIfOpen(process.stderr_read);
		if (process.job_object != nullptr)
		{
			CloseHandle(process.job_object);
			process.job_object = nullptr;
		}
		CloseInvalidHandleIfOpen(process.process_info.hThread);
		CloseInvalidHandleIfOpen(process.process_info.hProcess);
		process.process_info.dwProcessId = 0;
		process.process_info.dwThreadId = 0;
	}

	bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, std::size_t len, std::string* error_out = nullptr) const override
	{
		if (bytes == nullptr || len == 0)
		{
			return true;
		}
		if (process.stdin_write == INVALID_HANDLE_VALUE)
		{
			if (error_out != nullptr)
			{
				*error_out = "stdin pipe handle is closed.";
			}
			return false;
		}

		std::size_t offset = 0;
		while (offset < len)
		{
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(len - offset, static_cast<std::size_t>(MAXDWORD)));
			DWORD written = 0;
			if (!WriteFile(process.stdin_write, bytes + offset, chunk, &written, nullptr) || written == 0)
			{
				if (error_out != nullptr)
				{
					const DWORD err = GetLastError();
					*error_out = written == 0 ? "stdin pipe write returned zero bytes." : FormatWindowsError(err);
				}
				return false;
			}
			offset += written;
		}
		return true;
	}

	void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast_exit) const override
	{
		if (process.process_info.hProcess == INVALID_HANDLE_VALUE)
		{
			CloseStdioProcessHandles(process);
			return;
		}

		TerminateProcessTree(process.job_object, process.process_info.hProcess, 1);
		WaitForSingleObject(process.process_info.hProcess, fast_exit ? 80 : 600);
		CloseStdioProcessHandles(process);
	}

	std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const override
	{
		return ReadPipeNonBlocking(process.stdout_read, buffer, buffer_size, error_out);
	}

	std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr) const override
	{
		return ReadPipeNonBlocking(process.stderr_read, buffer, buffer_size, error_out);
	}

	bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process, int* exit_code_out = nullptr) const override
	{
		if (process.process_info.hProcess == INVALID_HANDLE_VALUE)
		{
			if (exit_code_out != nullptr)
			{
				*exit_code_out = -1;
			}
			return true;
		}

		if (WaitForSingleObject(process.process_info.hProcess, 0) != WAIT_OBJECT_0)
		{
			return false;
		}

		if (exit_code_out != nullptr)
		{
			DWORD exit_code = 0;
			if (GetExitCodeProcess(process.process_info.hProcess, &exit_code))
			{
				*exit_code_out = static_cast<int>(exit_code);
			}
			else
			{
				*exit_code_out = -1;
			}
		}
		return true;
	}

	std::filesystem::path ResolveCurrentExecutablePath() const override
	{
		std::wstring buffer(static_cast<std::size_t>(MAX_PATH), L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

		if (length == 0 || length >= buffer.size())
		{
			return {};
		}

		buffer.resize(static_cast<std::size_t>(length));
		return std::filesystem::path(buffer);
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
		const HANDLE handle = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			if (error_out != nullptr)
			{
				const DWORD error = GetLastError();
				*error_out = (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) ? "Another Universal Agent Manager instance is already using this data root." : "Failed to open data root lock file.";
			}
			return nullptr;
		}

		const std::string pid_text = std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "\n";
		DWORD written = 0;
		SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
		SetEndOfFile(handle);
		WriteFile(handle, pid_text.data(), static_cast<DWORD>(pid_text.size()), &written, nullptr);
		return std::make_unique<WindowsDataRootLock>(handle);
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
		GUID guid;
		if (CoCreateGuid(&guid) != S_OK)
		{
			return "";
		}
		char uuid[37];
		const unsigned int d4_0 = guid.Data4[0];
		const unsigned int d4_1 = guid.Data4[1];
		const unsigned int d4_2 = guid.Data4[2];
		const unsigned int d4_3 = guid.Data4[3];
		const unsigned int d4_4 = guid.Data4[4];
		const unsigned int d4_5 = guid.Data4[5];
		const unsigned int d4_6 = guid.Data4[6];
		const unsigned int d4_7 = guid.Data4[7];
		sprintf_s(uuid, sizeof(uuid), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3, d4_0, d4_1, d4_2, d4_3, d4_4, d4_5, d4_6, d4_7);
		return std::string(uuid);
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

		SHELLEXECUTEINFOA shim{};
		shim.cbSize = sizeof(SHELLEXECUTEINFOA);
		shim.fMask = SEE_MASK_NOCLOSEPROCESS;
		shim.hwnd = nullptr;
		shim.lpVerb = "open";
		shim.lpFile = "cmd.exe";
		shim.lpParameters = nullptr;
		shim.lpDirectory = working_directory.string().c_str();
		shim.nShow = SW_SHOWNORMAL;

		if (!ShellExecuteExA(&shim))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to launch terminal.";
			}
			return false;
		}

		if (shim.hProcess != nullptr)
		{
			WaitForSingleObject(shim.hProcess, 2000);
			CloseHandle(shim.hProcess);
		}

		return true;
	}
};

IPlatformProcessService& GetWindowsProcessService()
{
	static WindowsProcessService instance;
	return instance;
}

} // namespace uam::platform_windows_impl
