#ifndef UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H
#define UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H

#include <string>
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
	void StartProviderVersionCheck(uam::AppState& app, const std::string& provider_id, bool force) const;
	void StartPinToSupported(uam::AppState& app) const;
	bool StartInstallProviderVersion(uam::AppState& app, const std::string& provider_id, const std::string& version, std::string* error_out = nullptr) const;
	void Poll(uam::AppState& app) const;
	std::vector<CliProviderVersionOption> SupportedVersionsForProvider(const std::string& provider_id) const;
	std::string PreferredVersionForProvider(const std::string& provider_id) const;
	bool IsSupportedVersionForProvider(const std::string& provider_id, const std::string& version) const;
	std::string VersionProbeCommandForProvider(const std::string& provider_id) const;
	std::string InstallCommandForProviderVersion(const std::string& provider_id, const std::string& version) const;
};

std::string BuildCliProviderVersionProbeCommandForTests(const std::string& provider_id);
std::string BuildCliProviderInstallCommandForTests(const std::string& provider_id, const std::string& version);

#endif // UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H
