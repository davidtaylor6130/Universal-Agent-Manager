#include "core/gemini_cli_compat.h"

#include "common/utils/parse_utils.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace uam
{
namespace
{

constexpr std::string_view kPreferredGeminiCliVersion = "0.38.1";
constexpr std::string_view kMinimumSupportedGeminiCliVersion = "0.36.0";
constexpr std::array<std::string_view, 2> kSupportedGeminiCliVersions{{
	kPreferredGeminiCliVersion,
	kMinimumSupportedGeminiCliVersion,
}};

struct Semver
{
	int major = -1;
	int minor = -1;
	int patch = -1;
};

constexpr Semver kMinimumSupportedGeminiCliSemver{0, 36, 0};

std::optional<int> ParseSemverComponent(std::string_view value)
{
	const std::optional<int> parsed = uam::parse::IntStrict(value);
	if (!parsed || *parsed < 0)
	{
		return std::nullopt;
	}

	return parsed;
}

std::optional<Semver> ParseSemver(std::string_view value)
{
	const std::size_t major_end = value.find('.');
	if (major_end == std::string_view::npos)
	{
		return std::nullopt;
	}

	const std::size_t minor_end = value.find('.', major_end + 1);
	if (minor_end == std::string_view::npos || value.find('.', minor_end + 1) != std::string_view::npos)
	{
		return std::nullopt;
	}

	const std::optional<int> major = ParseSemverComponent(value.substr(0, major_end));
	const std::optional<int> minor = ParseSemverComponent(value.substr(major_end + 1, minor_end - major_end - 1));
	const std::optional<int> patch = ParseSemverComponent(value.substr(minor_end + 1));
	if (!major || !minor || !patch)
	{
		return std::nullopt;
	}

	return Semver{*major, *minor, *patch};
}

} // namespace

std::span<const std::string_view> SupportedGeminiCliVersions()
{
	return std::span<const std::string_view>(kSupportedGeminiCliVersions.data(), kSupportedGeminiCliVersions.size());
}

std::string_view PreferredGeminiCliVersion()
{
	return kPreferredGeminiCliVersion;
}

bool IsSupportedGeminiCliVersion(std::string_view version)
{
	if (std::ranges::find(kSupportedGeminiCliVersions, version) != kSupportedGeminiCliVersions.end())
	{
		return true;
	}

	const std::optional<Semver> parsed = ParseSemver(version);
	if (!parsed)
	{
		return false;
	}

	if (parsed->major > 0)
	{
		return true;
	}

	return parsed->major == kMinimumSupportedGeminiCliSemver.major &&
	       parsed->minor >= kMinimumSupportedGeminiCliSemver.minor;
}

std::string SupportedGeminiCliVersionsLabel()
{
	constexpr std::string_view kPreferredVersionPrefix = " or newer (preferred ";
	std::string label;
	label.reserve(kMinimumSupportedGeminiCliVersion.size() + kPreferredVersionPrefix.size() + kPreferredGeminiCliVersion.size() + 1);
	label.append(kMinimumSupportedGeminiCliVersion);
	label.append(kPreferredVersionPrefix);
	label.append(kPreferredGeminiCliVersion);
	label.push_back(')');
	return label;
}

} // namespace uam
