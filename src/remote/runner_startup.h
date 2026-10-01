#pragma once

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/shell_escape.h"
#include "remote/runner_protocol.h"

#include <array>
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <string>
#include <vector>

namespace uam::remote
{
	inline std::string RunnerStartupIdentity(const std::filesystem::path& executable)
	{
		return "uam-runner-" + hashing::Hex64Padded(hashing::Fnv1a64(paths::Utf8PathString(executable.parent_path().parent_path())));
	}

	/// <summary>Quotes one systemd argument without shell or environment expansion.</summary>
	inline std::string SystemdArgument(std::string_view argument)
	{
		std::string result = "\"";
		for (const char ch : argument)
		{
			if (ch == '\0' || ch == '\n' || ch == '\r') return {};
			if (ch == '\\' || ch == '"') result += '\\';
			if (ch == '%' || ch == '$') result += ch;
			result += ch;
		}
		return result + "\"";
	}

	inline std::string LinuxRunnerStartupUnit(const std::filesystem::path& executable,
	    const std::filesystem::path& socket)
	{
		const std::string exe = SystemdArgument(paths::Utf8PathString(executable));
		const std::string endpoint = SystemdArgument(paths::Utf8PathString(socket));
		if (!executable.is_absolute() || !socket.is_absolute() || exe.empty() || endpoint.empty()) return {};
		return "# UAM managed runner startup\n[Unit]\nDescription=Universal Agent Manager runner\n"
		    "[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=" + exe + " start --socket " + endpoint +
		    "\n[Install]\nWantedBy=default.target\n";
	}

	inline bool ConfigureLinuxRunnerStartup(IPlatformProcessService& service,
	    const std::filesystem::path& home, const std::filesystem::path& executable,
	    const std::filesystem::path& socket, bool enabled, std::string& error)
	{
		const std::string unit = LinuxRunnerStartupUnit(executable, socket);
		if (!home.is_absolute() || unit.empty()) { error = "The runner startup paths are invalid."; return false; }
		const std::string name = RunnerStartupIdentity(executable) + ".service";
		const std::filesystem::path path = home / ".config/systemd/user" / name;
		std::error_code ec;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)))
		{ error = "The runner startup entry cannot be a link."; return false; }
		std::string previous;
		if (std::filesystem::exists(path) && (!io::TryReadTextFile(path, previous) || !previous.starts_with("# UAM managed runner startup\n")))
		{ error = "Another application owns this startup entry."; return false; }
		const auto run = [&service](const std::vector<std::string>& args)
		{ return service.ExecuteCommand(shell::JoinEscapedArgs(args), 15000); };
		if (enabled)
		{
			if (!io::WriteTextFile(path, unit)) { error = "The runner login entry could not be written."; return false; }
			const ProcessExecutionResult reload = run({"systemctl", "--user", "daemon-reload"});
			const ProcessExecutionResult activate = reload.exit_code == 0
			    ? run({"systemctl", "--user", "enable", name}) : reload;
			if (activate.exit_code == 0 && run({"systemctl", "--user", "is-enabled", "--quiet", name}).exit_code == 0) return true;
			if (previous.empty()) std::filesystem::remove(path, ec); else (void)io::WriteTextFile(path, previous);
			(void)run({"systemctl", "--user", "daemon-reload"});
			error = "Runner startup requires a working systemd user session. " + activate.output;
			return false;
		}
		if (previous.empty()) return true;
		const ProcessExecutionResult deactivate = run({"systemctl", "--user", "disable", name});
		if (deactivate.exit_code != 0) { error = "Runner startup could not be disabled. " + deactivate.output; return false; }
		if (!std::filesystem::remove(path, ec) || ec) { error = "The runner login entry could not be removed."; return false; }
		(void)run({"systemctl", "--user", "daemon-reload"});
		return true;
	}
}

#if defined(_WIN32)
namespace uam::remote
{
	inline bool IsOwnedWindowsRunnerStartupValue(const std::wstring& value, const std::filesystem::path& executable)
	{
		if (!value.starts_with(L"\"") || !value.ends_with(L"\" start")) return false;
		const std::filesystem::path previous(value.substr(1, value.size() - 8));
		if (!previous.is_absolute() || previous.filename() != L"uam-runner.exe") return false;
		std::error_code error;
		return std::filesystem::equivalent(previous.parent_path().parent_path(), executable.parent_path().parent_path(), error) && !error;
	}

	/// <summary>Changes only this helper root's per-user Run value; existing jobs stay running.</summary>
	inline bool ConfigureWindowsRunnerStartup(const std::filesystem::path& executable, bool enabled, std::string& error)
	{
		if (!executable.is_absolute()) { error = "The runner startup path is invalid."; return false; }
		const std::wstring name = paths::PathFromUtf8(RunnerStartupIdentity(executable)).native();
		const std::wstring value = L"\"" + executable.native() + L"\" start";
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
		    KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		{ error = "The user's login registry could not be opened."; return false; }
		std::array<wchar_t, 8192> previous{};
		DWORD bytes = static_cast<DWORD>(previous.size() * sizeof(wchar_t));
		DWORD kind = 0;
		const LSTATUS queried = RegQueryValueExW(key, name.c_str(), nullptr, &kind, reinterpret_cast<BYTE*>(previous.data()), &bytes);
		if (queried != ERROR_FILE_NOT_FOUND && (queried != ERROR_SUCCESS || kind != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 ||
		    previous[bytes / sizeof(wchar_t) - 1] != L'\0' || wcslen(previous.data()) != bytes / sizeof(wchar_t) - 1 ||
		    !IsOwnedWindowsRunnerStartupValue(std::wstring(previous.data()), executable)))
		{
			RegCloseKey(key);
			error = "Another entry owns this runner login registration."; return false;
		}
		const LSTATUS changed = enabled ? RegSetValueExW(key, name.c_str(), 0, REG_SZ,
		    reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))
		    : RegDeleteValueW(key, name.c_str());
		RegCloseKey(key);
		if (changed != ERROR_SUCCESS && !(!enabled && changed == ERROR_FILE_NOT_FOUND))
		{ error = "The runner login registration could not be changed."; return false; }
		return true;
	}
}
#endif
