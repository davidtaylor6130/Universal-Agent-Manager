#include "platform_services_windows_impl_internal.h"

using namespace uam::platform_windows_impl;

namespace uam::platform_windows_impl
{

class WindowsPathService final : public IPlatformPathService
{
  public:
	bool EnsureHiddenDirectory(const std::filesystem::path& path, std::error_code* error_out = nullptr) const override
	{
		if (!uam::paths::CreateDirectoriesNoThrow(path, error_out))
		{
			return false;
		}

		const std::wstring wide_path = path.wstring();
		const DWORD attributes = GetFileAttributesW(wide_path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(wide_path.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN))
		{
			if (error_out != nullptr)
			{
				*error_out = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			}
			return false;
		}
		return true;
	}

	bool CanProbeDirectoryWithoutPrompt(const std::filesystem::path&) const override
	{
		return true;
	}

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
		const std::string trimmed = uam::strings::Trim(raw_path);

		if (trimmed.empty())
		{
			return {};
		}

		if (trimmed[0] != '~')
		{
			return uam::paths::PathFromUtf8(trimmed);
		}

		if (const std::optional<std::filesystem::path> home = ResolveUserHomePath())
		{
			if (trimmed.size() == 1)
			{
				return *home;
			}

			if (trimmed[1] == '\\' || trimmed[1] == '/')
			{
				return *home / uam::paths::PathFromUtf8(trimmed.substr(2));
			}
		}

		return uam::paths::PathFromUtf8(trimmed);
	}
};

IPlatformPathService& GetWindowsPathService()
{
	static WindowsPathService instance;
	return instance;
}

} // namespace uam::platform_windows_impl
