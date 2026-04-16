#include "platform_services_windows_impl.h"

#include "common/paths/app_paths.h"
#include "common/state/app_state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shellapi.h>
#include <shobjidl.h>
#include <wincontypes.h>
#include <windows.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

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

	std::string ShellQuoteWindowsForCmd(const std::string& value)
	{
		std::string escaped = "\"";

		for (const char ch : value)
		{
			if (ch == '"')
			{
				escaped += "\"\"";
			}
			else if (ch == '%')
			{
				escaped += "%%";
			}
			else if (ch == '\r' || ch == '\n')
			{
				escaped.push_back(' ');
			}
			else
			{
				escaped.push_back(ch);
			}
		}

		escaped.push_back('"');
		return escaped;
	}

	std::wstring WideFromUtf8(const std::string& value)
	{
		if (value.empty())
		{
			return std::wstring();
		}

		auto convert = [&](const UINT code_page, const DWORD flags) -> std::wstring
		{
			const int wide_len = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);

			if (wide_len <= 0)
			{
				return std::wstring();
			}

			std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');

			if (MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), wide.data(), wide_len) <= 0)
			{
				return std::wstring();
			}

			return wide;
		};

		std::wstring wide = convert(CP_UTF8, MB_ERR_INVALID_CHARS);

		if (!wide.empty())
		{
			return wide;
		}

		return convert(CP_ACP, 0);
	}

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
		{
			return std::string();
		}

		const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);

		if (utf8_len <= 0)
		{
			return std::string();
		}

		std::string utf8(static_cast<std::size_t>(utf8_len), '\0');

		if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), utf8_len, nullptr, nullptr) <= 0)
		{
			return std::string();
		}

		return utf8;
	}

	std::string QuoteWindowsArg(const std::string& arg)
	{
		if (arg.empty())
		{
			return "\"\"";
		}

		const bool needs_quotes = (arg.find_first_of(" \t\"") != std::string::npos);

		if (!needs_quotes)
		{
			return arg;
		}

		std::string result = "\"";
		int backslashes = 0;

		for (char ch : arg)
		{
			if (ch == '\\')
			{
				backslashes++;
			}
			else if (ch == '"')
			{
				result.append(backslashes * 2 + 1, '\\');
				result.push_back('"');
				backslashes = 0;
			}
			else if (ch == '\r' || ch == '\n')
			{
				if (backslashes > 0)
				{
					result.append(backslashes, '\\');
					backslashes = 0;
				}

				result.push_back(' ');
			}
			else
			{
				if (backslashes > 0)
				{
					result.append(backslashes, '\\');
					backslashes = 0;
				}

				result.push_back(ch);
			}
		}

		if (backslashes > 0)
		{
			result.append(backslashes * 2, '\\');
		}

		result.push_back('"');
		return result;
	}

	std::string BuildWindowsCommandLine(const std::vector<std::string>& argv)
	{
		std::ostringstream out;
		bool first = true;

		for (const std::string& arg : argv)
		{
			if (!first)
			{
				out << ' ';
			}

			out << QuoteWindowsArg(arg);
			first = false;
		}

		return out.str();
	}

	using ResizePseudoConsoleFunc = HRESULT(WINAPI*)(HPCON, COORD);
	using ClosePseudoConsoleFunc = void(WINAPI*)(HPCON);

	ResizePseudoConsoleFunc GetResizePseudoConsoleFunc()
	{
		static ResizePseudoConsoleFunc func = reinterpret_cast<ResizePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"));
		return func;
	}

	ClosePseudoConsoleFunc GetClosePseudoConsoleFunc()
	{
		static ClosePseudoConsoleFunc func = reinterpret_cast<ClosePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole"));
		return func;
	}

	void ClosePseudoConsoleSafe(HPCON handle)
	{
		const auto close_proc = GetClosePseudoConsoleFunc();

		if (close_proc != nullptr && handle != nullptr)
		{
			close_proc(handle);
		}
	}

	void ResizePseudoConsoleSafe(HPCON handle, COORD size)
	{
		const auto resize_proc = GetResizePseudoConsoleFunc();

		if (resize_proc != nullptr && handle != nullptr)
		{
			resize_proc(handle, size);
		}
	}

	std::optional<std::filesystem::path> ResolveWindowsHomePath()
	{
		if (const char* user_profile = std::getenv("USERPROFILE"))
		{
			const std::string value = TrimAsciiWhitespace(user_profile);

			if (!value.empty())
			{
				return std::filesystem::path(value);
			}
		}

		if (const char* home_drive = std::getenv("HOMEDRIVE"))
		{
			if (const char* home_path = std::getenv("HOMEPATH"))
			{
				const std::string drive = TrimAsciiWhitespace(home_drive);
				const std::string path = TrimAsciiWhitespace(home_path);

				if (!drive.empty() && !path.empty())
				{
					return std::filesystem::path(drive + path);
				}
			}
		}

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

	bool BrowsePathWithNativeDialogWindows(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr)
	{
		if (selected_path_out == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Selected path output is null.";
			}

			return false;
		}

		const HRESULT co_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const bool should_uninitialize = SUCCEEDED(co_init);

		if (FAILED(co_init) && co_init != RPC_E_CHANGED_MODE)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to initialize native file dialog.";
			}

			return false;
		}

		IFileOpenDialog* dialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));

		if (FAILED(hr) || dialog == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to create native file dialog.";
			}

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		DWORD options = 0;
		hr = dialog->GetOptions(&options);

		if (FAILED(hr))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to configure native file dialog.";
			}

			dialog->Release();

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;

		if (target == PlatformPathBrowseTarget::Directory)
		{
			options |= FOS_PICKFOLDERS;
		}
		else
		{
			options |= FOS_FILEMUSTEXIST;
		}

		dialog->SetOptions(options);
		std::filesystem::path initial_folder = initial_path;

		if (!initial_folder.empty())
		{
			std::error_code ec;
			const bool exists = std::filesystem::exists(initial_folder, ec);
			const bool is_directory = exists && std::filesystem::is_directory(initial_folder, ec);

			if (!exists || ec || !is_directory)
			{
				initial_folder = initial_folder.parent_path();
			}
		}

		if (!initial_folder.empty())
		{
			IShellItem* folder_item = nullptr;
			const std::wstring folder_wide = initial_folder.wstring();

			if (SUCCEEDED(SHCreateItemFromParsingName(folder_wide.c_str(), nullptr, IID_PPV_ARGS(&folder_item))) && folder_item != nullptr)
			{
				dialog->SetDefaultFolder(folder_item);
				dialog->SetFolder(folder_item);
				folder_item->Release();
			}
		}

		hr = dialog->Show(nullptr);

		if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		{
			dialog->Release();

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		if (FAILED(hr))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open native file dialog.";
			}

			dialog->Release();

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		IShellItem* selected_item = nullptr;
		hr = dialog->GetResult(&selected_item);

		if (FAILED(hr) || selected_item == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "No path was selected.";
			}

			dialog->Release();

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		PWSTR selected_wide = nullptr;
		hr = selected_item->GetDisplayName(SIGDN_FILESYSPATH, &selected_wide);

		if (FAILED(hr) || selected_wide == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to resolve selected path.";
			}

			selected_item->Release();
			dialog->Release();

			if (should_uninitialize)
			{
				CoUninitialize();
			}

			return false;
		}

		*selected_path_out = std::filesystem::path(selected_wide).string();
		CoTaskMemFree(selected_wide);
		selected_item->Release();
		dialog->Release();

		if (should_uninitialize)
		{
			CoUninitialize();
		}

		return true;
	}

	class WindowsTerminalRuntime final : public IPlatformTerminalRuntime
	{
	  public:
		bool IsAvailable() const override
		{
			return true;
		}

		bool StartCliTerminalProcess(uam::CliTerminalState& terminal, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string* error_out = nullptr) const override
		{
			if (argv.empty() || TrimAsciiWhitespace(argv.front()).empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Interactive provider command is empty.";
				}

				return false;
			}

			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;

			HANDLE pipe_pty_in = INVALID_HANDLE_VALUE;
			HANDLE pipe_pty_out = INVALID_HANDLE_VALUE;
			HANDLE pipe_con_in = INVALID_HANDLE_VALUE;
			HANDLE pipe_con_out = INVALID_HANDLE_VALUE;

			if (!CreatePipe(&pipe_pty_in, &pipe_con_out, &sa, 0) || !CreatePipe(&pipe_con_in, &pipe_pty_out, &sa, 0))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create ConPTY pipes.";
				}

				if (pipe_pty_in != INVALID_HANDLE_VALUE)
				{
					CloseHandle(pipe_pty_in);
				}

				if (pipe_pty_out != INVALID_HANDLE_VALUE)
				{
					CloseHandle(pipe_pty_out);
				}

				if (pipe_con_in != INVALID_HANDLE_VALUE)
				{
					CloseHandle(pipe_con_in);
				}

				if (pipe_con_out != INVALID_HANDLE_VALUE)
				{
					CloseHandle(pipe_con_out);
				}

				return false;
			}

			// Ensure our end of the pipes are not inherited by the child process.
			// This prevents the child from keeping the pipes alive and causing hangs on ClosePseudoConsole.
			SetHandleInformation(pipe_pty_in, HANDLE_FLAG_INHERIT, 0);
			SetHandleInformation(pipe_pty_out, HANDLE_FLAG_INHERIT, 0);

			const COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
			HPCON pseudo_console = nullptr;
			const auto create_pseudo_console = reinterpret_cast<HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole"));

			if (create_pseudo_console == nullptr || create_pseudo_console(size, pipe_con_in, pipe_con_out, 0, &pseudo_console) != S_OK)
			{
				if (error_out != nullptr)
				{
					*error_out = "CreatePseudoConsole failed.";
				}

				CloseHandle(pipe_pty_in);
				CloseHandle(pipe_pty_out);
				CloseHandle(pipe_con_in);
				CloseHandle(pipe_con_out);
				return false;
			}

			CloseHandle(pipe_con_in);
			CloseHandle(pipe_con_out);

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
				CloseHandle(pipe_pty_in);
				CloseHandle(pipe_pty_out);
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
				CloseHandle(pipe_pty_in);
				CloseHandle(pipe_pty_out);
				return false;
			}

			STARTUPINFOEXW si{};
			si.StartupInfo.cb = sizeof(si);
			si.lpAttributeList = terminal.attr_list;
			PROCESS_INFORMATION pi{};
			
			std::string command_str = BuildWindowsCommandLine(argv);
			command_str = "cmd.exe /C \"" + command_str + "\"";
			const std::wstring command_w = WideFromUtf8(command_str);

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
				CloseHandle(pipe_pty_in);
				CloseHandle(pipe_pty_out);
				return false;
			}

			std::vector<wchar_t> command_line(command_w.begin(), command_w.end());
			command_line.push_back(L'\0');
			const std::wstring working_directory_w = working_directory.empty() ? std::wstring() : working_directory.wstring();
			const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, working_directory.empty() ? nullptr : working_directory_w.c_str(), &si.StartupInfo, &pi);

		if (!created)
		{
			const DWORD last_error = GetLastError();
			std::string cmd_line_str;
			if (command_w.empty())
			{
				cmd_line_str = "(empty)";
			}
			else
			{
				cmd_line_str = WideToUtf8(command_w);
			}
			if (error_out != nullptr)
			{
				*error_out = "Failed to start provider process. Error code: " + std::to_string(last_error) + ". Command: " + cmd_line_str;
			}

			DeleteProcThreadAttributeList(terminal.attr_list);
			HeapFree(GetProcessHeap(), 0, terminal.attr_list);
			terminal.attr_list = nullptr;
			ClosePseudoConsoleSafe(pseudo_console);
			CloseHandle(pipe_pty_in);
			CloseHandle(pipe_pty_out);
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

			HANDLE job = CreateJobObjectW(nullptr, nullptr);
			if (job != nullptr)
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit_info{};
				limit_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit_info, sizeof(limit_info)))
				{
					AssignProcessToJobObject(job, pi.hProcess);
				}
				terminal.job_object = job;
			}

			return true;
		}

		void CloseCliTerminalHandles(uam::CliTerminalState& terminal) const override
		{
			if (terminal.pipe_input != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.pipe_input);
				terminal.pipe_input = INVALID_HANDLE_VALUE;
			}

			if (terminal.pipe_output != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.pipe_output);
				terminal.pipe_output = INVALID_HANDLE_VALUE;
			}

			if (terminal.attr_list != nullptr)
			{
				DeleteProcThreadAttributeList(terminal.attr_list);
				HeapFree(GetProcessHeap(), 0, terminal.attr_list);
				terminal.attr_list = nullptr;
			}

			if (terminal.process_info.hThread != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.process_info.hThread);
				terminal.process_info.hThread = INVALID_HANDLE_VALUE;
			}

			if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.process_info.hProcess);
				terminal.process_info.hProcess = INVALID_HANDLE_VALUE;
			}

			if (terminal.pseudo_console != nullptr)
			{
				ClosePseudoConsoleSafe(terminal.pseudo_console);
				terminal.pseudo_console = nullptr;
			}

			terminal.process_info.dwProcessId = 0;
			terminal.process_info.dwThreadId = 0;
		}

		bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, const std::size_t len) const override
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

		void StopCliTerminalProcess(uam::CliTerminalState& terminal, const bool fast_exit) const override
		{
			if (terminal.process_info.hProcess == INVALID_HANDLE_VALUE)
			{
				return;
			}

			if (fast_exit)
			{
				TerminateProcess(terminal.process_info.hProcess, 1);
				
				// Very short wait for process termination.
				(void)WaitForSingleObject(terminal.process_info.hProcess, 50);

				if (terminal.pseudo_console != nullptr)
				{
					ClosePseudoConsoleSafe(terminal.pseudo_console);
					terminal.pseudo_console = nullptr;
				}

				if (terminal.pipe_input != INVALID_HANDLE_VALUE)
				{
					CloseHandle(terminal.pipe_input);
					terminal.pipe_input = INVALID_HANDLE_VALUE;
				}

				if (terminal.pipe_output != INVALID_HANDLE_VALUE)
				{
					CloseHandle(terminal.pipe_output);
					terminal.pipe_output = INVALID_HANDLE_VALUE;
				}

				if (terminal.process_info.hThread != INVALID_HANDLE_VALUE)
				{
					CloseHandle(terminal.process_info.hThread);
					terminal.process_info.hThread = INVALID_HANDLE_VALUE;
				}

				if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE)
				{
					CloseHandle(terminal.process_info.hProcess);
					terminal.process_info.hProcess = INVALID_HANDLE_VALUE;
				}

				terminal.process_info.dwProcessId = 0;
				terminal.process_info.dwThreadId = 0;
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
				TerminateProcess(terminal.process_info.hProcess, 1);
				wait_result = WaitForSingleObject(terminal.process_info.hProcess, 250);
			}

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

			if (terminal.pipe_input != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.pipe_input);
				terminal.pipe_input = INVALID_HANDLE_VALUE;
			}

			if (terminal.pipe_output != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.pipe_output);
				terminal.pipe_output = INVALID_HANDLE_VALUE;
			}

			if (terminal.process_info.hThread != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.process_info.hThread);
				terminal.process_info.hThread = INVALID_HANDLE_VALUE;
			}

			if (terminal.process_info.hProcess != INVALID_HANDLE_VALUE)
			{
				CloseHandle(terminal.process_info.hProcess);
				terminal.process_info.hProcess = INVALID_HANDLE_VALUE;
			}

			if (terminal.attr_list != nullptr)
			{
				DeleteProcThreadAttributeList(terminal.attr_list);
				HeapFree(GetProcessHeap(), 0, terminal.attr_list);
				terminal.attr_list = nullptr;
			}

			terminal.process_info.dwProcessId = 0;
			terminal.process_info.dwThreadId = 0;
		}

		void ResizeCliTerminal(uam::CliTerminalState& terminal) const override
		{
			if (terminal.pseudo_console != nullptr)
			{
				COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
				ResizePseudoConsoleSafe(terminal.pseudo_console, size);
			}
		}

		std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState& terminal, char* buffer, const std::size_t buffer_size) const override
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
				return -2;
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

			return WaitForSingleObject(terminal.process_info.hProcess, 0) == WAIT_OBJECT_0;
		}

		bool SupportsAsyncNativeGeminiHistoryRefresh() const override
		{
			return true;
		}
	};

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
			return "cd /d " + ShellQuoteWindowsForCmd(working_directory.string()) + " && " + command;
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
			if (raw_status == STILL_ACTIVE)
			{
				return -1;
			}
			return raw_status;
		}

		ProcessExecutionResult ExecuteCommand(const std::string& command, const int timeout_ms = -1, std::stop_token stop_token = {}) const override
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
				CloseHandle(stdout_read);
				CloseHandle(stdout_write);
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
			CloseHandle(stdout_write);

			if (!created)
			{
				const DWORD launch_error = GetLastError();
				CloseHandle(stdout_read);
				result.error = "Failed to launch command (Win32 error " + std::to_string(launch_error) + ").";
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
					TerminateProcess(process_info.hProcess, 1);
					WaitForSingleObject(process_info.hProcess, 250);
					process_finished = true;
				}

				if (!process_finished && timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline)
				{
					result.timed_out = true;
					result.error = "Command timed out.";
					TerminateProcess(process_info.hProcess, 1);
					WaitForSingleObject(process_info.hProcess, 250);
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
			CloseHandle(stdout_read);
			CloseHandle(process_info.hProcess);
			CloseHandle(process_info.hThread);

			if (!result.timed_out && !result.canceled)
			{
				result.exit_code = static_cast<int>(exit_code);
				result.ok = result.error.empty() && result.exit_code == 0;
			}

			return result;
		}

		std::string GeminiDowngradeCommand() const override
		{
			return "npm install -g @google/gemini-cli@latest";
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
			sprintf_s(uuid, sizeof(uuid),
				"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
				guid.Data1, guid.Data2, guid.Data3,
				guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
				guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
			return std::string(uuid);
		}
	};

	class WindowsFileDialogService final : public IPlatformFileDialogService
	{
	  public:
		bool SupportsNativeDialogs() const override
		{
			return true;
		}

		bool BrowsePath(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr) const override
		{
			return BrowsePathWithNativeDialogWindows(target, initial_path, selected_path_out, error_out);
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

			const HINSTANCE result = ShellExecuteW(nullptr, L"open", folder_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

			if (reinterpret_cast<INT_PTR>(result) <= 32)
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

			const std::wstring params = L"/select,\"" + file_path.wstring() + L"\"";
			const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);

			if (reinterpret_cast<INT_PTR>(result) <= 32)
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

	class WindowsPathService final : public IPlatformPathService
	{
	  public:
		std::filesystem::path DefaultDataRootPath() const override
		{
			return AppPaths::DefaultDataRootPath();
		}

		std::optional<std::filesystem::path> ResolveUserHomePath() const override
		{
			return ResolveWindowsHomePath();
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

				if (trimmed[1] == '\\' || trimmed[1] == '/')
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
	static WindowsTerminalRuntime terminal_runtime;
	static WindowsProcessService process_service;
	static WindowsFileDialogService file_dialog_service;
	static WindowsPathService path_service;
	static PlatformServices services{
	    terminal_runtime, process_service, file_dialog_service, path_service,
	};
	return services;
}
