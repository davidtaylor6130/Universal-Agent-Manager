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
	void StartVersionCheck(uam::AppState& app, bool force, bool include_remote = false) const;
	bool StartProviderVersionCheck(uam::AppState& app, std::string_view provider_id, bool force, std::string_view execution_host_id = {}, std::string* error_out = nullptr) const;
	bool StartInstallProviderVersion(uam::AppState& app, std::string_view provider_id, std::string_view version, std::string* error_out = nullptr, std::string_view execution_host_id = {}) const;
	void Poll(uam::AppState& app) const;
	std::vector<CliProviderVersionOption> SupportedVersionsForProvider(std::string_view provider_id) const;
	std::string PreferredVersionForProvider(std::string_view provider_id) const;
	bool IsSupportedVersionForProvider(std::string_view provider_id, std::string_view version) const;
	std::string CompatibilityStatusForProvider(std::string_view provider_id, std::string_view version) const;
	std::string VerifiedVersionForProvider(std::string_view provider_id) const;
	std::string VerifiedAtForProvider(std::string_view provider_id) const;
	std::string VersionProbeCommandForProvider(std::string_view provider_id) const;
	std::string InstallCommandForProviderVersion(std::string_view provider_id, std::string_view version) const;
};

std::string BuildCliProviderVersionProbeCommandForTests(std::string_view provider_id);
std::string BuildCliProviderInstallCommandForTests(std::string_view provider_id, std::string_view version);
std::string BuildCliProviderInstallCommandForMethodForTests(std::string_view provider_id, std::string_view version, std::string_view install_method);
std::string ExtractCliProviderSemverVersionForTests(std::string_view output);
std::string ExtractCliProviderInstallMethodForTests(std::string_view output);
bool CliProviderVersionOutputIndicatesMissingCommandForTests(std::string_view output);
bool ProviderCliInstallBlockedByActiveRuntimeForTests(const uam::AppState& app, std::string_view provider_id);
/// <summary>Defers new provider work until the matching machine's installer is consumed.</summary>
std::string ProviderCliLaunchBlockReason(const uam::AppState& app, std::string_view provider_id, std::string_view execution_host_id = {});

/// <summary>Returns the npm package name for a CLI provider id, or empty if unknown.</summary>
std::string GetNpmPackageNameForProvider(std::string_view provider_id);

/// <summary>Identifies one provider installation; local keys remain canonical provider IDs.</summary>
std::string CliProviderVersionStateKey(std::string_view provider_id, std::string_view execution_host_id = {});

/// <summary>Tests the same remote installation identity check used before mutation.</summary>
bool ValidateRemoteCliInstallProbeForTests(std::string_view previous, std::string_view current, std::string_view platform, std::string* error);
std::string BuildInstallAwareCliProbeForTests(std::string_view provider_id, std::string_view platform);
