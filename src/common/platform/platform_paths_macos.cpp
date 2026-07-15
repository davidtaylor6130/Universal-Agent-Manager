#include "platform_services_macos_impl_internal.h"

#include <algorithm>

using namespace uam::platform_macos_impl;

namespace uam::platform_macos_impl
{

class MacPathService final : public IPlatformPathService
{
  public:
	bool CanProbeDirectoryWithoutPrompt(const std::filesystem::path& path) const override
	{
		const std::optional<std::filesystem::path> home = ResolveUserHomePath();
		if (!home)
		{
			return true;
		}

		const std::filesystem::path candidate = path.lexically_normal();
		for (const char* folder : {"Desktop", "Documents", "Downloads"})
		{
			const std::filesystem::path protected_root = (*home / folder).lexically_normal();
			const auto mismatch = std::mismatch(protected_root.begin(), protected_root.end(), candidate.begin(), candidate.end());
			if (mismatch.first == protected_root.end())
			{
				return false;
			}
		}

		return true;
	}

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
