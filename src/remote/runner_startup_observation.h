#pragma once

#include "remote/runner_startup.h"
#include "common/utils/string_utils.h"

namespace uam::remote
{
	/// <summary>Reads this helper's owned registration without changing login settings.</summary>
	inline std::string ObserveLinuxRunnerStartup(IPlatformProcessService& service,
	    const std::filesystem::path& home, const std::filesystem::path& executable,
	    const std::filesystem::path& socket, std::string& error)
	{
		error.clear();
		const std::string expected = LinuxRunnerStartupUnit(executable, socket);
		if (!home.is_absolute() || expected.empty())
		{ error = "The runner startup paths are invalid."; return {}; }
		const std::string name = RunnerStartupIdentity(executable) + ".service";
		const std::filesystem::path path = home / ".config/systemd/user" / name;
		std::error_code ec;
		const std::filesystem::file_status file = std::filesystem::symlink_status(path, ec);
		if (ec && ec != std::errc::no_such_file_or_directory)
		{ error = "The runner startup entry could not be inspected."; return {}; }
		const bool present = std::filesystem::exists(file);
		std::string contents;
		if (present && (!std::filesystem::is_regular_file(file) || !io::TryReadTextFile(path, contents, expected.size()) || contents != expected))
		{ error = "The runner startup entry does not match this helper."; return {}; }
		const ProcessExecutionResult registration = service.ExecuteCommand(
		    shell::JoinEscapedArgs({"systemctl", "--user", "is-enabled", name}), 15000);
		if (registration.timed_out || registration.canceled || registration.output_truncated || !registration.error.empty())
		{ error = "Runner startup could not be confirmed in the systemd user session."; return {}; }
		const std::string state = strings::Trim(registration.output);
		if (state == "enabled" && registration.exit_code == 0 && present)
		{
			const ProcessExecutionResult fragment = service.ExecuteCommand(shell::JoinEscapedArgs(
			    {"systemctl", "--user", "show", "--property=FragmentPath", "--value", name}), 15000);
			if (!fragment.timed_out && !fragment.canceled && !fragment.output_truncated && fragment.error.empty() && fragment.exit_code == 0 && strings::Trim(fragment.output) == paths::Utf8PathString(path)) return "enabled";
			error = "The active runner startup entry does not match this helper.";
			return {};
		}
		if ((state == "disabled" || state == "static" || state == "indirect" || state == "masked") && present && (registration.exit_code == 0 || registration.exit_code == 1)) return "disabled";
		if (state == "not-found" && !present && (registration.exit_code == 0 || registration.exit_code == 1 || registration.exit_code == 4)) return "disabled";
		error = "Runner startup could not be confirmed in the systemd user session.";
		return {};
	}

#if defined(_WIN32)
	inline std::string ObserveWindowsRunnerStartup(const std::filesystem::path& executable, std::string& error)
	{
		error.clear();
		if (!executable.is_absolute()) { error = "The runner startup path is invalid."; return {}; }
		HKEY key = nullptr;
		const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key);
		if (opened == ERROR_FILE_NOT_FOUND) return "disabled";
		if (opened != ERROR_SUCCESS) { error = "The user's login registry could not be inspected."; return {}; }
		const std::wstring name = paths::PathFromUtf8(RunnerStartupIdentity(executable)).native();
		std::array<wchar_t, 8192> value{};
		DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
		DWORD kind = 0;
		const LSTATUS queried = RegQueryValueExW(key, name.c_str(), nullptr, &kind, reinterpret_cast<BYTE*>(value.data()), &bytes);
		RegCloseKey(key);
		if (queried == ERROR_FILE_NOT_FOUND) return "disabled";
		if (queried != ERROR_SUCCESS || kind != REG_SZ || bytes < sizeof(wchar_t) || bytes > value.size() * sizeof(wchar_t) ||
		    bytes % sizeof(wchar_t) != 0 || value[bytes / sizeof(wchar_t) - 1] != L'\0' ||
		    wcslen(value.data()) != bytes / sizeof(wchar_t) - 1 ||
		    std::wstring(value.data()) != L"\"" + executable.native() + L"\" start")
		{ error = "The runner login registration does not match this helper."; return {}; }
		return "enabled";
	}
#endif
}
