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

#if defined(_WIN32)
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
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <util.h>
#else
#error "PlatformServices supports only Windows and macOS."
#endif

namespace
{

#if defined(_WIN32)

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
			const std::wstring command_w = WideFromUtf8(BuildWindowsCommandLine(argv));

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
			const BOOL created = CreateProcessW(nullptr,
			                                    command_line.data(),
			                                    nullptr,
			                                    nullptr,
			                                    TRUE,
			                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			                                    nullptr,
			                                    working_directory.empty() ? nullptr : working_directory_w.c_str(),
			                                    &si.StartupInfo,
			                                    &pi);

			if (!created)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to start provider process.";
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
				(void)WaitForSingleObject(terminal.process_info.hProcess, 60);
				return;
			}

			if (terminal.running && terminal.pipe_input != INVALID_HANDLE_VALUE)
			{
				static constexpr char kQuitCommand[] = "/quit\r\n";
				(void)WriteToCliTerminal(terminal, kQuitCommand, sizeof(kQuitCommand) - 1);
			}

			DWORD wait_result = WaitForSingleObject(terminal.process_info.hProcess, 700);

			if (wait_result == WAIT_TIMEOUT)
			{
				TerminateProcess(terminal.process_info.hProcess, 1);
				wait_result = WaitForSingleObject(terminal.process_info.hProcess, 1200);
			}

			if (wait_result == WAIT_TIMEOUT)
			{
				TerminateProcess(terminal.process_info.hProcess, 1);
			}
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
			std::unique_ptr<FILE, int (*)(FILE*)> pipe(_popen(command.c_str(), "r"), _pclose);

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
			return raw_status;
		}

		std::string GeminiDowngradeCommand() const override
		{
			return "npm install -g @google/gemini-cli@0.30.0";
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

		std::string OpenCodeBridgeBinaryName() const override
		{
			return "uam_ollama_engine_bridge.exe";
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

			const std::wstring command_w = WideFromUtf8(BuildWindowsCommandLine(argv));

			if (command_w.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to encode OpenCode bridge command line.";
				}

				return false;
			}

			std::vector<wchar_t> command_line(command_w.begin(), command_w.end());
			command_line.push_back(L'\0');

			STARTUPINFOW startup_info{};
			startup_info.cb = sizeof(startup_info);
			PROCESS_INFORMATION process_info{};
			const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info);

			if (!created)
			{
				const DWORD launch_error = GetLastError();

				if (error_out != nullptr)
				{
					*error_out = "Failed to launch OpenCode bridge process (Win32 error " + std::to_string(launch_error) + ").";
				}

				return false;
			}

			state.process_handle = process_info.hProcess;
			state.process_thread = process_info.hThread;
			state.process_id = process_info.dwProcessId;
			return true;
		}

		bool IsOpenCodeBridgeProcessRunning(uam::OpenCodeBridgeState& state) const override
		{
			if (state.process_handle == INVALID_HANDLE_VALUE || state.process_handle == nullptr)
			{
				return false;
			}

			return WaitForSingleObject(state.process_handle, 0) == WAIT_TIMEOUT;
		}

		void StopLocalBridgeProcess(uam::OpenCodeBridgeState& state) const override
		{
			if (state.process_handle != INVALID_HANDLE_VALUE && state.process_handle != nullptr)
			{
				const DWORD wait_result = WaitForSingleObject(state.process_handle, 0);

				if (wait_result == WAIT_TIMEOUT)
				{
					TerminateProcess(state.process_handle, 1);
					WaitForSingleObject(state.process_handle, 1000);
				}

				CloseHandle(state.process_handle);
				state.process_handle = INVALID_HANDLE_VALUE;
			}

			if (state.process_thread != INVALID_HANDLE_VALUE && state.process_thread != nullptr)
			{
				CloseHandle(state.process_thread);
				state.process_thread = INVALID_HANDLE_VALUE;
			}

			state.process_id = 0;
		}

		uintmax_t NativeGeminiSessionMaxFileBytes() const override
		{
			return 12ULL * 1024ULL * 1024ULL;
		}

		std::size_t NativeGeminiSessionMaxMessages() const override
		{
			return 12000;
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

	class WindowsUiTraits final : public IPlatformUiTraits
	{
	  public:
		void ApplyProcessDpiAwareness() const override
		{
			SetProcessDPIAware();
		}

		void ConfigureOpenGlAttributes() const override
		{
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
			SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		}

		const char* OpenGlGlslVersion() const override
		{
			return "#version 130";
		}

		float AdjustSidebarWidth(const float layout_width, const float current_sidebar_width, const float effective_ui_scale) const override
		{
			(void)current_sidebar_width;
			const float width_bias = 1.0f + ((std::max(1.0f, effective_ui_scale) - 1.0f) * 0.36f);
			const float sidebar_ratio = (layout_width < 1180.0f) ? 0.35f : 0.30f;
			float sidebar_width = std::clamp(layout_width * sidebar_ratio, 280.0f * width_bias, 470.0f * width_bias);
			const float max_sidebar_from_main_floor = std::max(220.0f, layout_width - 560.0f);
			return std::clamp(sidebar_width, 220.0f, max_sidebar_from_main_floor);
		}

		bool UseWindowsLayoutAdjustments() const override
		{
			return true;
		}

		bool UsesLogicalPointsForUiScale() const override
		{
			return false;
		}

		float PlatformUiSpacingScale() const override
		{
			return 1.14f;
		}

		std::optional<bool> DetectSystemPrefersLightTheme() const override
		{
			DWORD value = 1;
			DWORD value_size = sizeof(value);
			const LONG rc = RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &value_size);

			if (rc == ERROR_SUCCESS)
			{
				return value != 0;
			}

			return std::nullopt;
		}
	};

#elif defined(__APPLE__)

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

#endif

} // namespace

PlatformServices& PlatformServicesFactory::Instance()
{
#if defined(_WIN32)
	static WindowsTerminalRuntime terminal_runtime;
	static WindowsProcessService process_service;
	static WindowsFileDialogService file_dialog_service;
	static WindowsPathService path_service;
	static WindowsUiTraits ui_traits;
#elif defined(__APPLE__)
	static MacTerminalRuntime terminal_runtime;
	static MacProcessService process_service;
	static MacFileDialogService file_dialog_service;
	static MacPathService path_service;
	static MacUiTraits ui_traits;
#else
#error "PlatformServicesFactory supports only Windows and macOS."
#endif
	static PlatformServices services{
	    terminal_runtime,
	    process_service,
	    file_dialog_service,
	    path_service,
	    ui_traits,
	};
	return services;
}
