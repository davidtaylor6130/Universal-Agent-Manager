#pragma once
// Internal helpers, WindowsDataRootLock, shared across platform_pty_windows.cpp, platform_process_windows.cpp,
// platform_dialogs_windows.cpp, and platform_paths_windows.cpp.
// Include only from those four TUs and platform_services_windows_impl.cpp.

#include "platform_services_windows_impl.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/state/app_state.h"
#include "common/utils/env_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
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

namespace uam::platform_windows_impl
{

constexpr auto kDirectWindowsExecutableExtensions = std::to_array<std::string_view>({
    ".exe",
    ".com",
});

constexpr auto kWindowsCommandScriptExtensions = std::to_array<std::string_view>({
    ".cmd",
    ".bat",
});

constexpr auto kLaunchableWindowsCommandExtensions = std::to_array<std::string_view>({
    ".exe",
    ".com",
    ".cmd",
    ".bat",
});

inline std::wstring WideFromUtf8(const std::string& value)
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

inline std::string WideToUtf8(const std::wstring& value)
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

inline std::string FormatWindowsError(const DWORD error)
{
	LPWSTR buffer = nullptr;
	const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	const DWORD len = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
	std::string message;
	if (len > 0 && buffer != nullptr)
	{
		message = WideToUtf8(std::wstring(buffer, buffer + len));
		while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == '.'))
		{
			message.pop_back();
		}
	}
	if (buffer != nullptr)
	{
		LocalFree(buffer);
	}
	if (message.empty())
	{
		message = "Windows error " + std::to_string(static_cast<unsigned long>(error));
	}
	return message + " (" + std::to_string(static_cast<unsigned long>(error)) + ")";
}

inline std::string QuoteWindowsArg(const std::string& arg)
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

inline std::string BuildWindowsCommandLine(const std::vector<std::string>& argv)
{
	std::vector<std::string> quoted_args;
	quoted_args.reserve(argv.size());
	for (const std::string& arg : argv)
	{
		quoted_args.push_back(QuoteWindowsArg(arg));
	}

	return uam::strings::Join(quoted_args, " ");
}

inline bool LooksLikeWindowsPath(const std::string& value)
{
	return uam::strings::Contains(value, '\\') || uam::strings::Contains(value, '/') || uam::strings::Contains(value, ':');
}

inline std::string WindowsPathExtension(const std::string& value)
{
	return uam::strings::ToLowerAscii(std::filesystem::path(value).extension().string());
}

inline bool IsDirectWindowsExecutableExtension(const std::string& extension)
{
	return uam::ranges::Contains(kDirectWindowsExecutableExtensions, std::string_view(extension));
}

inline bool IsWindowsCommandScriptExtension(const std::string& extension)
{
	return uam::ranges::Contains(kWindowsCommandScriptExtensions, std::string_view(extension));
}

inline bool IsLaunchableWindowsCommandExtension(const std::string& extension)
{
	return IsDirectWindowsExecutableExtension(extension) || IsWindowsCommandScriptExtension(extension);
}

inline bool IsExistingRegularFileUtf8(const std::string& path)
{
	const std::wstring wide = WideFromUtf8(path);
	if (wide.empty())
	{
		return false;
	}

	const DWORD attrs = GetFileAttributesW(wide.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::optional<std::string> LaunchableCommandPathIfExists(const std::string& candidate)
{
	if (!IsLaunchableWindowsCommandExtension(WindowsPathExtension(candidate)))
	{
		return std::nullopt;
	}

	if (!IsExistingRegularFileUtf8(candidate))
	{
		return std::nullopt;
	}

	return candidate;
}

inline std::string GetWindowsEnvironmentVariableUtf8(const wchar_t* name)
{
	const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
	if (required == 0)
	{
		return "";
	}

	std::wstring value(static_cast<std::size_t>(required), L'\0');
	const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
	if (written == 0 || written >= required)
	{
		return "";
	}

	value.resize(static_cast<std::size_t>(written));
	return WideToUtf8(value);
}

inline std::string TrimWindowsPathListEntry(const std::string& raw)
{
	std::string value = uam::strings::Trim(raw);
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
	{
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

inline std::vector<std::string> SplitWindowsPathList(const std::string& value)
{
	std::vector<std::string> entries;
	std::size_t start = 0;

	while (start <= value.size())
	{
		const std::size_t end = value.find(';', start);
		const std::string entry = TrimWindowsPathListEntry(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (!entry.empty())
		{
			entries.push_back(entry);
		}

		if (end == std::string::npos)
		{
			break;
		}
		start = end + 1;
	}

	return entries;
}

inline std::string JoinWindowsPathUtf8(const std::string& directory, const std::string& filename)
{
	return (std::filesystem::path(directory) / filename).string();
}

inline std::optional<std::string> ResolveWindowsCommandPath(const std::string& command)
{
	const std::string trimmed = uam::strings::Trim(command);
	if (trimmed.empty())
	{
		return std::nullopt;
	}

	const std::string extension = WindowsPathExtension(trimmed);
	if (LooksLikeWindowsPath(trimmed))
	{
		if (!extension.empty())
		{
			return LaunchableCommandPathIfExists(trimmed);
		}

		for (const std::string_view candidate_extension : kLaunchableWindowsCommandExtensions)
		{
			if (auto resolved = LaunchableCommandPathIfExists(trimmed + std::string(candidate_extension)))
			{
				return resolved;
			}
		}

		return std::nullopt;
	}

	const std::vector<std::string> path_entries = SplitWindowsPathList(GetWindowsEnvironmentVariableUtf8(L"PATH"));
	if (!extension.empty())
	{
		for (const std::string& path_entry : path_entries)
		{
			if (auto resolved = LaunchableCommandPathIfExists(JoinWindowsPathUtf8(path_entry, trimmed)))
			{
				return resolved;
			}
		}

		return std::nullopt;
	}

	for (const std::string& path_entry : path_entries)
	{
		for (const std::string_view candidate_extension : kLaunchableWindowsCommandExtensions)
		{
			if (auto resolved = LaunchableCommandPathIfExists(JoinWindowsPathUtf8(path_entry, trimmed + std::string(candidate_extension))))
			{
				return resolved;
			}
		}
	}

	return std::nullopt;
}

inline std::string ResolveComSpecPath()
{
	std::string comspec = GetWindowsEnvironmentVariableUtf8(L"COMSPEC");
	if (LaunchableCommandPathIfExists(comspec))
	{
		return comspec;
	}

	std::wstring system_directory(static_cast<std::size_t>(MAX_PATH), L'\0');
	const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
	if (length > 0 && length < system_directory.size())
	{
		system_directory.resize(static_cast<std::size_t>(length));
		const std::string system_cmd = WideToUtf8(system_directory) + "\\cmd.exe";
		if (LaunchableCommandPathIfExists(system_cmd))
		{
			return system_cmd;
		}
	}

	return "cmd.exe";
}

inline std::string BuildCmdScriptInvocation(const std::string& script_path, const std::vector<std::string>& argv)
{
	std::ostringstream out;
	out << '"' << uam::shell::EscapeArg(script_path);

	for (std::size_t i = 1; i < argv.size(); ++i)
	{
		out << ' ' << uam::shell::EscapeArg(argv[i]);
	}

	out << '"';
	return out.str();
}

struct WindowsLaunchCommand
{
	std::string command_line;
	std::string original_command_line;
	std::string resolved_command;
	bool uses_cmd_wrapper = false;
};

inline WindowsLaunchCommand BuildWindowsLaunchCommand(const std::vector<std::string>& argv)
{
	WindowsLaunchCommand launch;
	launch.original_command_line = BuildWindowsCommandLine(argv);

	if (argv.empty())
	{
		return launch;
	}

	const std::optional<std::string> resolved_command = ResolveWindowsCommandPath(argv.front());
	if (!resolved_command)
	{
		launch.command_line = launch.original_command_line;
		return launch;
	}

	launch.resolved_command = *resolved_command;
	const std::string extension = WindowsPathExtension(launch.resolved_command);
	if (IsWindowsCommandScriptExtension(extension))
	{
		launch.uses_cmd_wrapper = true;
		launch.command_line = QuoteWindowsArg(ResolveComSpecPath()) + " /D /S /C " + BuildCmdScriptInvocation(launch.resolved_command, argv);
		return launch;
	}

	std::vector<std::string> resolved_argv = argv;
	resolved_argv.front() = launch.resolved_command;
	launch.command_line = BuildWindowsCommandLine(resolved_argv);
	return launch;
}

inline std::string WindowsLaunchDiagnosticSuffix(const WindowsLaunchCommand& launch)
{
	std::ostringstream out;
	if (!launch.original_command_line.empty())
	{
		out << " Original command: " << launch.original_command_line << ".";
	}
	if (!launch.resolved_command.empty())
	{
		out << " Resolved command: " << launch.resolved_command << ".";
	}
	if (launch.uses_cmd_wrapper)
	{
		out << " Wrapper: cmd.exe /D /S /C.";
	}
	return out.str();
}

inline void TerminateProcessTree(HANDLE job_object, HANDLE process_handle, const UINT exit_code)
{
	if (job_object != nullptr)
	{
		TerminateJobObject(job_object, exit_code);
		return;
	}

	if (process_handle != INVALID_HANDLE_VALUE)
	{
		TerminateProcess(process_handle, exit_code);
	}
}

inline void TerminateProcessAndWaitBriefly(HANDLE process_handle, const UINT exit_code)
{
	if (process_handle == nullptr || process_handle == INVALID_HANDLE_VALUE)
	{
		return;
	}

	TerminateProcess(process_handle, exit_code);
	WaitForSingleObject(process_handle, 250);
}

inline void TerminateProcessTreeAndWaitBriefly(HANDLE job_object, HANDLE process_handle, const UINT exit_code)
{
	TerminateProcessTree(job_object, process_handle, exit_code);
	if (process_handle != nullptr && process_handle != INVALID_HANDLE_VALUE)
	{
		WaitForSingleObject(process_handle, 250);
	}
}

inline void CloseInvalidHandleIfOpen(HANDLE& handle)
{
	if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(handle);
		handle = INVALID_HANDLE_VALUE;
	}
}

inline void CloseConPtyPipeHandles(HANDLE& pipe_pty_in, HANDLE& pipe_pty_out, HANDLE& pipe_con_in, HANDLE& pipe_con_out)
{
	CloseInvalidHandleIfOpen(pipe_pty_in);
	CloseInvalidHandleIfOpen(pipe_pty_out);
	CloseInvalidHandleIfOpen(pipe_con_in);
	CloseInvalidHandleIfOpen(pipe_con_out);
}

inline void CloseConPtyAppPipeHandles(HANDLE& pipe_pty_in, HANDLE& pipe_pty_out)
{
	CloseInvalidHandleIfOpen(pipe_pty_in);
	CloseInvalidHandleIfOpen(pipe_pty_out);
}

inline void CloseStdioPipeHandles(HANDLE& stdin_read, HANDLE& stdin_write, HANDLE& stdout_read, HANDLE& stdout_write, HANDLE& stderr_read, HANDLE& stderr_write)
{
	CloseInvalidHandleIfOpen(stdin_read);
	CloseInvalidHandleIfOpen(stdin_write);
	CloseInvalidHandleIfOpen(stdout_read);
	CloseInvalidHandleIfOpen(stdout_write);
	CloseInvalidHandleIfOpen(stderr_read);
	CloseInvalidHandleIfOpen(stderr_write);
}

inline void CloseStdioChildPipeEnds(HANDLE& stdin_read, HANDLE& stdout_write, HANDLE& stderr_write)
{
	CloseInvalidHandleIfOpen(stdin_read);
	CloseInvalidHandleIfOpen(stdout_write);
	CloseInvalidHandleIfOpen(stderr_write);
}

inline void CloseStdioParentPipeEnds(HANDLE& stdin_write, HANDLE& stdout_read, HANDLE& stderr_read)
{
	CloseInvalidHandleIfOpen(stdin_write);
	CloseInvalidHandleIfOpen(stdout_read);
	CloseInvalidHandleIfOpen(stderr_read);
}

inline bool CreateKillOnCloseJobForProcess(HANDLE process_handle, HANDLE* job_out, std::string* error_out)
{
	if (job_out != nullptr)
	{
		*job_out = nullptr;
	}

	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	if (job == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create process job object: " + FormatWindowsError(GetLastError()) + ".";
		}
		return false;
	}

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit_info{};
	limit_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit_info, sizeof(limit_info)))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to configure process job object: " + FormatWindowsError(GetLastError()) + ".";
		}
		CloseHandle(job);
		return false;
	}

	if (!AssignProcessToJobObject(job, process_handle))
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to assign process to job object: " + FormatWindowsError(GetLastError()) + ".";
		}
		CloseHandle(job);
		return false;
	}

	if (job_out != nullptr)
	{
		*job_out = job;
	}
	return true;
}

using ResizePseudoConsoleFunc = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFunc = void(WINAPI*)(HPCON);

inline ResizePseudoConsoleFunc GetResizePseudoConsoleFunc()
{
	static ResizePseudoConsoleFunc func = reinterpret_cast<ResizePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"));
	return func;
}

inline ClosePseudoConsoleFunc GetClosePseudoConsoleFunc()
{
	static ClosePseudoConsoleFunc func = reinterpret_cast<ClosePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole"));
	return func;
}

inline void ClosePseudoConsoleSafe(HPCON handle)
{
	const auto close_proc = GetClosePseudoConsoleFunc();

	if (close_proc != nullptr && handle != nullptr)
	{
		close_proc(handle);
	}
}

inline void ResizePseudoConsoleSafe(HPCON handle, COORD size)
{
	const auto resize_proc = GetResizePseudoConsoleFunc();

	if (resize_proc != nullptr && handle != nullptr)
	{
		resize_proc(handle, size);
	}
}

inline std::ptrdiff_t ReadPipeNonBlocking(HANDLE pipe, char* buffer, std::size_t buffer_size, std::string* error_out = nullptr)
{
	if (pipe == INVALID_HANDLE_VALUE)
	{
		if (error_out != nullptr)
		{
			*error_out = "stdio pipe handle is closed.";
		}
		return -1;
	}

	DWORD available = 0;
	if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
	{
		const DWORD err = GetLastError();
		if (err != ERROR_BROKEN_PIPE && error_out != nullptr)
		{
			*error_out = FormatWindowsError(err);
		}
		return err == ERROR_BROKEN_PIPE ? 0 : -1;
	}

	if (available == 0)
	{
		return -2;
	}

	const DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(buffer_size, available));
	DWORD bytes_read = 0;
	if (!ReadFile(pipe, buffer, to_read, &bytes_read, nullptr))
	{
		const DWORD err = GetLastError();
		if (err != ERROR_BROKEN_PIPE && error_out != nullptr)
		{
			*error_out = FormatWindowsError(err);
		}
		return err == ERROR_BROKEN_PIPE ? 0 : -1;
	}

	if (bytes_read == 0)
	{
		return -2;
	}

	return static_cast<std::ptrdiff_t>(bytes_read);
}

inline std::optional<std::filesystem::path> ResolveWindowsHomePath()
{
	if (const std::optional<std::filesystem::path> user_profile = uam::env::GetTrimmedPath("USERPROFILE"))
	{
		return *user_profile;
	}

	if (const std::optional<std::filesystem::path> home_drive_path = uam::env::GetWindowsHomeDrivePath())
	{
		return *home_drive_path;
	}

	if (const std::optional<std::filesystem::path> home = uam::env::GetTrimmedPath("HOME"))
	{
		return *home;
	}

	return std::nullopt;
}

inline bool BrowsePathWithNativeDialogWindows(const PlatformPathBrowseTarget target, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out = nullptr)
{
	if (selected_path_out == nullptr)
	{
		if (error_out != nullptr)
		{
			*error_out = "Selected path output is null.";
		}

		return false;
	}
	selected_path_out->clear();

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
		if (!uam::paths::IsDirectoryNoThrow(initial_folder))
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

	*selected_path_out = uam::paths::Utf8PathString(std::filesystem::path(selected_wide));
	CoTaskMemFree(selected_wide);
	selected_item->Release();
	dialog->Release();

	if (should_uninitialize)
	{
		CoUninitialize();
	}

	return true;
}

class WindowsDataRootLock final : public uam::platform::DataRootLock
{
  public:
	explicit WindowsDataRootLock(HANDLE handle) : m_handle(handle)
	{
	}

	~WindowsDataRootLock() override
	{
		if (m_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_handle);
		}
	}

	WindowsDataRootLock(const WindowsDataRootLock&) = delete;
	WindowsDataRootLock& operator=(const WindowsDataRootLock&) = delete;

  private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

} // namespace uam::platform_windows_impl
