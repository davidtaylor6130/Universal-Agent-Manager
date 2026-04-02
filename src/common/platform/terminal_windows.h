#pragma once

#if defined(_WIN32)
using CreatePseudoConsoleFunc = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFunc = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFunc = void(WINAPI*)(HPCON);

static CreatePseudoConsoleFunc GetCreatePseudoConsoleFunc()
{
	static CreatePseudoConsoleFunc func = reinterpret_cast<CreatePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole"));
	return func;
}

static ResizePseudoConsoleFunc GetResizePseudoConsoleFunc()
{
	static ResizePseudoConsoleFunc func = reinterpret_cast<ResizePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ResizePseudoConsole"));
	return func;
}

static ClosePseudoConsoleFunc GetClosePseudoConsoleFunc()
{
	static ClosePseudoConsoleFunc func = reinterpret_cast<ClosePseudoConsoleFunc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole"));
	return func;
}

static void ClosePseudoConsoleSafe(HPCON handle)
{
	const auto close_proc = GetClosePseudoConsoleFunc();

	if (close_proc != nullptr && handle != nullptr)
	{
		close_proc(handle);
	}
}

static void ResizePseudoConsoleSafe(HPCON handle, COORD size)
{
	const auto resize_proc = GetResizePseudoConsoleFunc();

	if (resize_proc != nullptr && handle != nullptr)
	{
		resize_proc(handle, size);
	}
}

static std::wstring WideFromUtf8(const std::string& value)
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

static std::wstring EscapeCmdPercentExpansion(const std::wstring& value)
{
	std::wstring out;
	out.reserve(value.size() + 8);

	for (const wchar_t ch : value)
	{
		if (ch == L'%')
		{
			out += L"%%";
		}
		else
		{
			out.push_back(ch);
		}
	}

	return out;
}

static std::string QuoteWindowsArg(const std::string& arg)
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

static std::string BuildWindowsCommandLine(const std::vector<std::string>& argv)
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

static bool StartCliTerminalWindows(AppState& app, CliTerminalState& terminal, const ChatSession& chat)
{
	const fs::path workspace_root = ResolveWorkspaceRootPath(app, chat);
	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;

	HANDLE pipe_pty_in = INVALID_HANDLE_VALUE;
	HANDLE pipe_pty_out = INVALID_HANDLE_VALUE;
	HANDLE pipe_con_in = INVALID_HANDLE_VALUE;
	HANDLE pipe_con_out = INVALID_HANDLE_VALUE;

	if (!CreatePipe(&pipe_pty_in, &pipe_con_out, &sa, 0) || !CreatePipe(&pipe_con_in, &pipe_pty_out, &sa, 0))
	{
		terminal.last_error = "Failed to create ConPTY pipes.";
		CloseHandle(pipe_pty_in);
		CloseHandle(pipe_pty_out);
		CloseHandle(pipe_con_in);
		CloseHandle(pipe_con_out);
		return false;
	}

	const COORD size{static_cast<SHORT>(terminal.cols), static_cast<SHORT>(terminal.rows)};
	HPCON pseudo_console = nullptr;
	const auto create_pseudo_console = GetCreatePseudoConsoleFunc();

	if (create_pseudo_console == nullptr || create_pseudo_console(size, pipe_con_in, pipe_con_out, 0, &pseudo_console) != S_OK)
	{
		terminal.last_error = "CreatePseudoConsole failed.";
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
		terminal.last_error = "Failed to initialize attribute list.";
		ClosePseudoConsoleSafe(pseudo_console);
		CloseHandle(pipe_pty_in);
		CloseHandle(pipe_pty_out);
		return false;
	}

	if (!UpdateProcThreadAttribute(terminal.attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudo_console, sizeof(HPCON), nullptr, nullptr))
	{
		terminal.last_error = "Failed to attach pseudo console.";
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

	const std::vector<std::string> argv_vec = BuildProviderInteractiveArgv(app, chat);

	if (argv_vec.empty())
	{
		terminal.last_error = "Interactive provider command is empty.";
		DeleteProcThreadAttributeList(terminal.attr_list);
		HeapFree(GetProcessHeap(), 0, terminal.attr_list);
		terminal.attr_list = nullptr;
		ClosePseudoConsoleSafe(pseudo_console);
		CloseHandle(pipe_pty_in);
		CloseHandle(pipe_pty_out);
		return false;
	}

	const std::wstring invocation_line = WideFromUtf8(BuildWindowsCommandLine(argv_vec));

	if (invocation_line.empty())
	{
		terminal.last_error = "Failed to encode interactive provider command line.";
		DeleteProcThreadAttributeList(terminal.attr_list);
		HeapFree(GetProcessHeap(), 0, terminal.attr_list);
		terminal.attr_list = nullptr;
		ClosePseudoConsoleSafe(pseudo_console);
		CloseHandle(pipe_pty_in);
		CloseHandle(pipe_pty_out);
		return false;
	}

	std::vector<wchar_t> cmd_mutable(invocation_line.begin(), invocation_line.end());
	cmd_mutable.push_back(L'\0');
	const std::wstring working_dir_w = workspace_root.wstring();
	LPCWSTR working_dir = working_dir_w.empty() ? nullptr : working_dir_w.c_str();

	DWORD launch_error = ERROR_SUCCESS;
	bool launched = CreateProcessW(nullptr, cmd_mutable.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, working_dir, &si.StartupInfo, &pi) != 0;
	bool attempted_cmd_fallback = false;

	if (!launched)
	{
		launch_error = GetLastError();

		if (launch_error == ERROR_FILE_NOT_FOUND || launch_error == ERROR_PATH_NOT_FOUND || launch_error == ERROR_BAD_EXE_FORMAT)
		{
			attempted_cmd_fallback = true;
			const wchar_t* comspec_env = _wgetenv(L"ComSpec");
			const std::wstring comspec = (comspec_env != nullptr && *comspec_env != L'\0') ? std::wstring(comspec_env) : L"C:\\Windows\\System32\\cmd.exe";
			const std::wstring cmd_shell_args = L"/d /s /c \"\"" + EscapeCmdPercentExpansion(invocation_line) + L"\"\"";
			std::vector<wchar_t> cmd_shell_mutable(cmd_shell_args.begin(), cmd_shell_args.end());
			cmd_shell_mutable.push_back(L'\0');
			launched = CreateProcessW(comspec.c_str(), cmd_shell_mutable.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, working_dir, &si.StartupInfo, &pi) != 0;

			if (!launched)
			{
				launch_error = GetLastError();
			}
		}
	}

	if (!launched)
	{
		const std::string detail = attempted_cmd_fallback ? "CreateProcess for Gemini failed after cmd fallback (Win32 error " : "CreateProcess for Gemini failed (Win32 error ";
		terminal.last_error = detail + std::to_string(launch_error) + ").";
		DeleteProcThreadAttributeList(terminal.attr_list);
		HeapFree(GetProcessHeap(), 0, terminal.attr_list);
		terminal.attr_list = nullptr;
		ClosePseudoConsoleSafe(pseudo_console);
		CloseHandle(pipe_pty_in);
		CloseHandle(pipe_pty_out);
		return false;
	}

	terminal.pipe_input = pipe_pty_out;
	terminal.pipe_output = pipe_pty_in;
	terminal.process_info = pi;
	terminal.pseudo_console = pseudo_console;
	return true;
}

#endif
