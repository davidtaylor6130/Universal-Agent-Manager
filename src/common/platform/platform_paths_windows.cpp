#include "platform_services_windows_impl_internal.h"

using namespace uam::platform_windows_impl;

namespace uam::platform_windows_impl
{

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
		const std::string trimmed = uam::strings::Trim(raw_path);

		if (trimmed.empty())
		{
			return {};
		}

		if (trimmed[0] != '~')
		{
			return std::filesystem::path(trimmed);
		}

		if (const std::optional<std::filesystem::path> home = ResolveUserHomePath())
		{
			if (trimmed.size() == 1)
			{
				return *home;
			}

			if (trimmed[1] == '\\' || trimmed[1] == '/')
			{
				return *home / trimmed.substr(2);
			}
		}

		return std::filesystem::path(trimmed);
	}
};

IPlatformPathService& GetWindowsPathService()
{
	static WindowsPathService instance;
	return instance;
}

} // namespace uam::platform_windows_impl
