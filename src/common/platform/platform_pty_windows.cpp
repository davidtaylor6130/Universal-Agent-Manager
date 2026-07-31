#include "platform_services_windows_impl_internal.h"

using namespace uam::platform_windows_impl;

namespace uam::platform_windows_impl
{

class WindowsTerminalRuntime final : public IPlatformTerminalRuntime
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

		HANDLE pipe_pty_in = INVALID_HANDLE_VALUE;
		HANDLE pipe_pty_out = INVALID_HANDLE_VALUE;
		HANDLE pipe_con_in = INVALID_HANDLE_VALUE;
		HANDLE pipe_con_out = INVALID_HANDLE_VALUE;

		if (!CreatePipe(&pipe_pty_in, &pipe_con_out, nullptr, 0) || !CreatePipe(&pipe_con_in, &pipe_pty_out, nullptr, 0))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create ConPTY pipes.";
			}

			CloseConPtyPipeHandles(pipe_pty_in, pipe_pty_out, pipe_con_in, pipe_con_out);

			return false;
		}

		const COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
		HPCON pseudo_console = nullptr;
		const auto create_pseudo_console = reinterpret_cast<HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole"));

		if (create_pseudo_console == nullptr || create_pseudo_console(size, pipe_con_in, pipe_con_out, 0, &pseudo_console) != S_OK)
		{
			if (error_out != nullptr)
			{
				*error_out = "CreatePseudoConsole failed.";
			}

			CloseConPtyPipeHandles(pipe_pty_in, pipe_pty_out, pipe_con_in, pipe_con_out);
			return false;
		}

		SIZE_T attr_size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
		terminal.attr_list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attr_size));

		if (terminal.attr_list == nullptr || !InitializeProcThreadAttributeList(terminal.attr_list, 1, 0, &attr_size))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to initialize attribute list.";
			}

			ClosePseudoConsoleSafe(pseudo_console);
			CloseConPtyPipeHandles(pipe_pty_in, pipe_pty_out, pipe_con_in, pipe_con_out);
			return false;
		}

		if (!UpdateProcThreadAttribute(terminal.attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudo_console, sizeof(HPCON), nullptr, nullptr))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to attach pseudo console.";
			}

			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
			ClosePseudoConsoleSafe(pseudo_console);
			CloseConPtyPipeHandles(pipe_pty_in, pipe_pty_out, pipe_con_in, pipe_con_out);
			return false;
		}

		STARTUPINFOEXW si{};
		si.StartupInfo.cb = sizeof(si);
		// Keep redirected parent stdio from bypassing ConPTY.
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = nullptr;
		si.StartupInfo.hStdOutput = nullptr;
		si.StartupInfo.hStdError = nullptr;
		si.lpAttributeList = terminal.attr_list;
		PROCESS_INFORMATION pi{};

		const WindowsLaunchCommand launch = BuildWindowsLaunchCommand(argv);
		const std::wstring command_w = WideFromUtf8(launch.command_line);

		if (command_w.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to encode interactive command line.";
			}

			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
			ClosePseudoConsoleSafe(pseudo_console);
			CloseConPtyPipeHandles(pipe_pty_in, pipe_pty_out, pipe_con_in, pipe_con_out);
			return false;
		}

		std::vector<wchar_t> command_line(command_w.begin(), command_w.end());
		command_line.push_back(L'\0');
		const std::wstring working_directory_w = working_directory.empty() ? std::wstring() : working_directory.wstring();
		const DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
		const wchar_t* working_directory_arg = working_directory.empty() ? nullptr : working_directory_w.c_str();
		const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, creation_flags, nullptr, working_directory_arg, &si.StartupInfo, &pi);
		CloseInvalidHandleIfOpen(pipe_con_in);
		CloseInvalidHandleIfOpen(pipe_con_out);

		if (!created)
		{
			const DWORD last_error = GetLastError();
			if (error_out != nullptr)
			{
				*error_out = "Failed to start provider process. " + FormatWindowsError(last_error) + ". Command: " + launch.command_line + "." + WindowsLaunchDiagnosticSuffix(launch);
			}

			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
			ClosePseudoConsoleSafe(pseudo_console);
			CloseConPtyAppPipeHandles(pipe_pty_in, pipe_pty_out);
			return false;
		}

		HANDLE job = nullptr;
		std::string job_error;
		if (!CreateKillOnCloseJobForProcess(pi.hProcess, &job, &job_error))
		{
			if (error_out != nullptr)
			{
				*error_out = uam::strings::NonEmptyOrFallback(job_error, "Failed to protect provider process tree.");
			}

			TerminateProcessAndWaitBriefly(pi.hProcess, 1);
			CloseInvalidHandleIfOpen(pi.hThread);
			CloseInvalidHandleIfOpen(pi.hProcess);
			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
			ClosePseudoConsoleSafe(pseudo_console);
			CloseConPtyAppPipeHandles(pipe_pty_in, pipe_pty_out);
			return false;
		}

		DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;

		if (!SetNamedPipeHandleState(pipe_pty_in, &mode, nullptr, nullptr))
		{
			mode = PIPE_READMODE_BYTE;
			(void)SetNamedPipeHandleState(pipe_pty_in, &mode, nullptr, nullptr);
		}

		terminal.pipe_input = pipe_pty_out;
		terminal.pipe_output = pipe_pty_in;
		terminal.process_info = pi;
		terminal.pseudo_console = pseudo_console;
		terminal.job_object = job;

		return true;
	}

	void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const override
	{
		CloseInvalidHandleIfOpen(terminal.pipe_input);
		CloseInvalidHandleIfOpen(terminal.pipe_output);

		if (terminal.attr_list != nullptr)
		{
			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
		}

		CloseInvalidHandleIfOpen(terminal.process_info.hThread);
		CloseInvalidHandleIfOpen(terminal.process_info.hProcess);

		if (terminal.pseudo_console != nullptr)
		{
			ClosePseudoConsoleSafe(terminal.pseudo_console);
			terminal.pseudo_console = nullptr;
		}

		if (terminal.job_object != nullptr)
		{
			CloseHandle(terminal.job_object);
			terminal.job_object = nullptr;
		}

		terminal.process_info.dwProcessId = 0;
		terminal.process_info.dwThreadId = 0;
	}

	bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, std::size_t len) const override
	{
		if (bytes == nullptr || len == 0)
		{
			return true;
		}

		if (terminal.pipe_input == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		std::size_t offset = 0;

		while (offset < len)
		{
			const std::size_t remaining = len - offset;
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)));
			DWORD written = 0;

			if (!WriteFile(terminal.pipe_input, bytes + offset, chunk, &written, nullptr) || written == 0)
			{
				return false;
			}

			offset += written;
		}

		return true;
	}

	void StopCliTerminalProcess(uam::CliTerminalState& terminal, bool fast_exit) const override
	{
		if (terminal.process_info.hProcess == INVALID_HANDLE_VALUE)
		{
			CloseCliTerminalHandles(terminal);
			return;
		}

		if (fast_exit)
		{
			TerminateProcessTree(terminal.job_object, terminal.process_info.hProcess, 1);
			(void)WaitForSingleObject(terminal.process_info.hProcess, 50);
			CloseCliTerminalHandles(terminal);
			return;
		}

		if (terminal.running && terminal.pipe_input != INVALID_HANDLE_VALUE)
		{
			static constexpr char kQuitCommand[] = "/quit\r\n";
			(void)WriteToCliTerminal(terminal, kQuitCommand, sizeof(kQuitCommand) - 1);
		}

		DWORD wait_result = WaitForSingleObject(terminal.process_info.hProcess, 250);

		if (wait_result == WAIT_TIMEOUT)
		{
			TerminateProcessTree(terminal.job_object, terminal.process_info.hProcess, 1);
			wait_result = WaitForSingleObject(terminal.process_info.hProcess, 250);
		}

		CloseCliTerminalHandles(terminal);
	}

	void ResizeCliTerminal(uam::CliTerminalState& terminal) const override
	{
		if (terminal.pseudo_console != nullptr)
		{
			COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
			ResizePseudoConsoleSafe(terminal.pseudo_console, size);
		}
	}

	std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState& terminal, char* buffer, std::size_t buffer_size) const override
	{
		if (terminal.pipe_output == INVALID_HANDLE_VALUE)
		{
			return -1;
		}

		DWORD available = 0;

		if (!PeekNamedPipe(terminal.pipe_output, nullptr, 0, nullptr, &available, nullptr))
		{
			const DWORD err = GetLastError();

			if (err == ERROR_BROKEN_PIPE)
			{
				CloseInvalidHandleIfOpen(terminal.pipe_output);
				return 0;
			}

			return -1;
		}

		if (available == 0)
		{
			return -2;
		}

		const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(buffer_size, available));
		DWORD bytes_read = 0;

		if (!ReadFile(terminal.pipe_output, buffer, to_read, &bytes_read, nullptr))
		{
			const DWORD err = GetLastError();

			if (err == ERROR_BROKEN_PIPE)
			{
				return 0;
			}

			return -1;
		}

		if (bytes_read == 0)
		{
			CloseInvalidHandleIfOpen(terminal.pipe_output);
			return 0;
		}

		return static_cast<std::ptrdiff_t>(bytes_read);
	}

	bool HasReadableTerminalOutputHandle(const uam::CliTerminalState& terminal) const override
	{
		return terminal.pipe_output != INVALID_HANDLE_VALUE;
	}

	bool PollCliTerminalProcessExited(uam::CliTerminalState& terminal) const override
	{
		if (terminal.process_info.hProcess == INVALID_HANDLE_VALUE)
		{
			return true;
		}

		if (WaitForSingleObject(terminal.process_info.hProcess, 0) != WAIT_OBJECT_0)
		{
			return false;
		}

		if (terminal.pipe_output == INVALID_HANDLE_VALUE)
		{
			return true;
		}

		if (terminal.pseudo_console != nullptr)
		{
			CloseInvalidHandleIfOpen(terminal.pipe_input);
			if (terminal.job_object != nullptr)
			{
				CloseHandle(terminal.job_object);
				terminal.job_object = nullptr;
			}

			const HPCON pseudo_console = terminal.pseudo_console;
			terminal.pseudo_console = nullptr;
			try
			{
				// ClosePseudoConsole can emit a final frame, so keep draining output concurrently.
				std::thread([pseudo_console]() { ClosePseudoConsoleSafe(pseudo_console); }).detach();
			}
			catch (...)
			{
				CloseInvalidHandleIfOpen(terminal.pipe_output);
				ClosePseudoConsoleSafe(pseudo_console);
				return true;
			}
			return false;
		}

		DWORD available = 0;
		if (!PeekNamedPipe(terminal.pipe_output, nullptr, 0, nullptr, &available, nullptr) && GetLastError() == ERROR_BROKEN_PIPE)
		{
			CloseInvalidHandleIfOpen(terminal.pipe_output);
			return true;
		}

		return false;
	}

	bool SupportsAsyncNativeGeminiHistoryRefresh() const override
	{
		return true;
	}
};

IPlatformTerminalRuntime& GetWindowsTerminalRuntime()
{
	static WindowsTerminalRuntime instance;
	return instance;
}

} // namespace uam::platform_windows_impl
