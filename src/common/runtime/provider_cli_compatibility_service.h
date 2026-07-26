#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct AppState;
}

struct CliProviderVersionOption
{
	std::string version;
	bool preferred = false;
};

class ProviderCliCompatibilityService
{
 public:
	void StartVersionCheck(uam::AppState& app, bool force) const;
	void StartProviderVersionCheck(uam::AppState& app, std::string_view provider_id, bool force) const;
	void StartPinToSupported(uam::AppState& app) const;
	bool StartInstallProviderVersion(uam::AppState& app, std::string_view provider_id, std::string_view version, std::string* error_out = nullptr) const;
	void Poll(uam::AppState& app) const;
	std::vector<CliProviderVersionOption> SupportedVersionsForProvider(std::string_view provider_id) const;
	std::string PreferredVersionForProvider(std::string_view provider_id) const;
	bool IsSupportedVersionForProvider(std::string_view provider_id, std::string_view version) const;
	std::string VersionProbeCommandForProvider(std::string_view provider_id) const;
	std::string InstallCommandForProviderVersion(std::string_view provider_id, std::string_view version) const;
};

std::string BuildCliProviderVersionProbeCommandForTests(std::string_view provider_id);
std::string BuildCliProviderInstallCommandForTests(std::string_view provider_id, std::string_view version);
std::string BuildCliProviderInstallCommandForMethodForTests(std::string_view provider_id, std::string_view version, std::string_view install_method);
std::string ExtractCliProviderSemverVersionForTests(std::string_view output);
std::string ExtractCliProviderInstallMethodForTests(std::string_view output);
bool CliProviderVersionOutputIndicatesMissingCommandForTests(std::string_view output);

/// <summary>Returns the npm package name for a CLI provider id, or empty if unknown.</summary>
std::string GetNpmPackageNameForProvider(std::string_view provider_id);
