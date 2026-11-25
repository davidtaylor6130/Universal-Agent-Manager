// Launch helpers for CEF's bootstrap.exe layout (see cef_sandbox_folder_access.h). Kept apart from the folder grant
// so every target that compiles the Windows process launcher, including uam-runner, links only kernel32 for them.
#include "common/platform/cef_sandbox_folder_access.h"

#include "common/paths/path_utils.h"

#include <windows.h>

#include <cwchar>

namespace uam::platform
{
	namespace
	{
		bool SameExecutable(const std::filesystem::path& left, const std::filesystem::path& right)
		{
			return !left.empty() && !right.empty() && _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
		}
	}

	bool IsCefSubprocessCommandLine(std::wstring_view command_line)
	{
		bool quoted = false;
		bool token_start = true;
		for (std::size_t index = 0; index < command_line.size(); ++index)
		{
			const wchar_t character = command_line[index];
			if (character == L'"')
			{
				quoted = !quoted;
				token_start = false;
				continue;
			}
			if (!quoted && (character == L' ' || character == L'\t'))
			{
				token_start = true;
				continue;
			}
			if (token_start && !quoted)
			{
				const std::wstring_view rest = command_line.substr(index);
				if (rest.starts_with(L"--type=")) return true;
				if (rest.starts_with(L"--") && (rest.size() == 2 || rest[2] == L' ' || rest[2] == L'\t')) return false;
			}
			token_start = false;
		}
		return false;
	}

	void AddLaunchWorkingDirectory(std::vector<std::pair<std::string, std::string>>& environment, std::string_view child_executable_utf8,
	    const std::filesystem::path& working_directory, const std::filesystem::path& current_executable)
	{
		if (working_directory.empty() || !SameExecutable(uam::paths::PathFromUtf8(child_executable_utf8), current_executable)) return;
		environment.emplace_back(kLaunchWorkingDirectoryVariable, uam::paths::Utf8PathString(working_directory));
	}

	void AddLaunchWorkingDirectory(std::vector<std::pair<std::string, std::string>>& environment, std::string_view child_executable_utf8,
	    const std::filesystem::path& working_directory)
	{
		if (working_directory.empty()) return;
		std::wstring buffer(32768, L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0 || length >= buffer.size()) return;
		buffer.resize(length);
		AddLaunchWorkingDirectory(environment, child_executable_utf8, working_directory, std::filesystem::path(buffer));
	}

}
