#include "platform_services_macos_impl_internal.h"

using namespace uam::platform_macos_impl;

namespace uam::platform_macos_impl
{

class MacPathService final : public IPlatformPathService
{
  public:
	std::filesystem::path DefaultDataRootPath() const override
	{
		return AppPaths::DefaultDataRootPath();
	}

	std::optional<std::filesystem::path> ResolveUserHomePath() const override
	{
		if (const std::optional<std::filesystem::path> home = uam::env::GetTrimmedPath("HOME"))
		{
			return *home;
		}

		return std::nullopt;
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

			if (trimmed[1] == '/')
			{
				return *home / trimmed.substr(2);
			}
		}

		return std::filesystem::path(trimmed);
	}
};

IPlatformPathService& GetMacPathService()
{
	static MacPathService instance;
	return instance;
}

} // namespace uam::platform_macos_impl
