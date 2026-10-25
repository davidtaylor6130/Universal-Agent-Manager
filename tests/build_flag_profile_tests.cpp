// Compiled with Claude as the only provider and every optional feature off (see CMakeLists.txt):
// excluded providers must vanish from the id tables, and saved data written by a full build must not
// reactivate them or reset the settings that name them.
#include "common/config/central_provider_configuration.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/usage/usage_ledger.h"

#include <cstdio>
#include <string>

#if UAM_ENABLE_RUNTIME_GEMINI_CLI || UAM_ENABLE_RUNTIME_CODEX_CLI || !UAM_ENABLE_RUNTIME_CLAUDE_CLI || UAM_ENABLE_SSH
#error "build_flag_profile_tests expects the Claude-only, features-off profile."
#endif

namespace
{
	int failures = 0;
	void Check(bool condition, const char* what)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", what);
			++failures;
		}
	}
}

int main()
{
	using namespace uam::provider_ids;
	Check(kAllCliProviderIds.size() == 1 && std::string(kAllCliProviderIds.front()) == "claude-cli", "only the included provider is listed");
	Check(std::string(kDefaultCliProviderId) == "claude-cli", "the first included provider is the default");
	Check(std::string(provider_build_config::FirstEnabledProviderId()) == "claude-cli", "the build default follows the included providers");
	Check(std::string(kCodexCli) == kExcludedCliProviderId && std::string(kGeminiCli) == kExcludedCliProviderId, "excluded providers carry the placeholder id");
	Check(!IsKnownCliProviderId("codex-cli") && !IsKnownCliProviderId(kExcludedCliProviderId), "saved excluded ids and the placeholder are unknown");
	Check(NormalizeCliProviderAlias("codex").empty() && NormalizeCliProviderAlias("claude") == "claude-cli", "aliases exist only for included providers");
	Check(NormalizeVersionManagedCliProviderId("gemini-cli") == "claude-cli", "version management falls back to an included provider");
	Check(!IsCliProviderAliasOf("codex-cli", kCodexCli), "an excluded provider never matches its own saved id");

	CentralProviderConfiguration configuration;
	std::string error;
	const nlohmann::json saved = {{"mcpOverrides", nlohmann::json::array({
	    {{"providerId", "codex-cli"}, {"executionHostId", "local"}, {"serverId", "files"}, {"enabled", false}},
	    {{"providerId", "claude-cli"}, {"executionHostId", "local"}, {"serverId", "files"}, {"enabled", false}},
	})}};
	Check(uam::central_configuration::Parse(saved, configuration, error), "a full build's configuration still loads");
	Check(configuration.mcp_overrides.size() == 1 && configuration.mcp_overrides.front().provider_id == "claude-cli", "excluded providers' switches are skipped, included ones kept");

	// Usage accounting written by a full build: excluded providers' rows are skipped, not treated as corruption.
	const nlohmann::json coverage = nlohmann::json::array({{{"providerId", "codex-cli"}}, {{"providerId", "claude-cli"}}});
	Check(uam::usage::IncludedProviderRows(coverage) == uam::provider_ids::kAllCliProviderIds.size(), "coverage counts only included providers");
	Check(uam::usage::SkipsExcludedProviderRecord(nlohmann::json::array({"[\"native\",\"codex-cli\",\"x\"]", 1, nlohmann::json::array({"codex-cli"})})), "an excluded provider's saved record is skipped");
	Check(!uam::usage::SkipsExcludedProviderRecord(nlohmann::json::array({"[\"native\",\"claude-cli\",\"x\"]", 1, nlohmann::json::array({"claude-cli"})})), "an included provider's record is read");
	Check(!uam::usage::SkipsExcludedProvider(""), "an empty provider stays invalid rather than skipped");

	if (failures == 0) std::puts("build flag profile checks passed");
	return failures == 0 ? 0 : 1;
}
