// Compiled with Claude as the only provider and every optional feature off (see CMakeLists.txt):
// excluded providers must vanish from the id tables, and saved data written by a full build must not
// reactivate them, reset the settings that name them, or be dropped when this build saves.
#include "common/config/central_provider_configuration.h"
#include "common/config/provider_availability.h"
#include "common/config/settings_store.h"
#include "common/provider/provider_ids.h"
#include "common/usage/usage_ledger.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
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
	Check(configuration.mcp_overrides.size() == 2, "excluded providers' switches are kept as saved data");
	Check(uam::central_configuration::McpEnabled(configuration, "claude-cli", "local", "files", true) == false, "included providers' switches still apply");
	Check(!IsRetainedExcludedProviderId("claude-cli") && IsRetainedExcludedProviderId("codex-cli") && !IsRetainedExcludedProviderId(kExcludedCliProviderId) && !IsRetainedExcludedProviderId("Bad Id"),
	    "only well-formed excluded ids are retained");
	CentralProviderConfiguration edited; // The interface sends included providers only.
	edited.mcp_overrides.push_back({"claude-cli", "local", "files", true});
	uam::central_configuration::KeepExcludedProviderOverrides(configuration, edited);
	Check(edited.mcp_overrides.size() == 2 && edited.mcp_overrides.back().provider_id == "codex-cli" && !edited.mcp_overrides.back().enabled, "an interface save keeps excluded providers' switches");

	std::vector<ProviderAvailabilityOverride> availability;
	Check(uam::provider_availability::Parse(nlohmann::json::array({{{"providerId", "codex-cli"}, {"executionHostId", "local"}, {"enabled", false}}}), availability) && availability.size() == 1,
	    "a full build's availability policy stays valid (an invalid one would discard the whole settings file)");

	// Round trip: settings a full build saved, loaded and saved again here, keep every excluded provider's entry.
	const std::filesystem::path root = std::filesystem::temp_directory_path() / ("uam-build-flag-profile-" + std::to_string(std::random_device{}()));
	std::filesystem::create_directories(root);
	const std::filesystem::path file = root / "settings.txt";
	AppSettings full;
	full.provider_availability = {{"codex-cli", "local", false}, {"claude-cli", "local", false}};
	full.central_provider_configuration.mcp_overrides = configuration.mcp_overrides;
	full.provider_chat_defaults["codex-cli"].model_id = "gpt-saved";
	full.memory_worker_bindings["codex-cli"] = {"codex-cli", "worker-saved"};
	full.memory_worker_bindings["claude-cli"] = {"codex-cli", "claude-worker"};
	full.active_provider_id = "codex-cli";
	full.default_new_chat_provider_id = "codex-cli";
	full.permission_reviewer_provider_id = "codex-cli";
	ExecutionHost remote;
	remote.id = "build-box";
	remote.label = "Build box";
	remote.transport = "ssh";
	remote.ssh_alias = "build-box";
	full.execution_hosts.push_back(remote);
	full.host_scoped_settings["build-box"].default_new_chat_provider_id = "codex-cli";
	Check(SettingsStore::Save(file, full), "settings with excluded providers save");
	for (int pass = 0; pass < 2; ++pass)
	{
		AppSettings loaded;
		const SettingsLoadResult result = SettingsStore::Load(file, loaded);
		Check(result.loaded && !result.recovered_from_backup && !result.unrecovered_error, "the saved settings load as valid, not from a backup");
		Check(loaded.provider_availability.size() == 2 && !uam::provider_availability::IsEnabled(loaded, "claude-cli"), "availability keeps every provider's entry");
		Check(loaded.central_provider_configuration.mcp_overrides.size() == 2, "MCP switches keep every provider's entry");
		Check(loaded.provider_chat_defaults.contains("codex-cli") && loaded.provider_chat_defaults["codex-cli"].model_id == "gpt-saved", "chat defaults keep the excluded provider's entry");
		Check(loaded.memory_worker_bindings.contains("codex-cli") && loaded.memory_worker_bindings["codex-cli"].worker_model_id == "worker-saved", "worker bindings keep the excluded provider's entry");
		Check(loaded.memory_worker_bindings["claude-cli"].worker_provider_id == "codex-cli", "an included provider's binding keeps its saved worker");
		Check(loaded.active_provider_id == "claude-cli" && loaded.default_new_chat_provider_id == "claude-cli" && loaded.permission_reviewer_provider_id.empty(),
		    "provider choices naming an excluded provider fall back to included ones at runtime");
		Check(loaded.host_scoped_settings.contains("build-box") && !loaded.host_scoped_settings["build-box"].default_new_chat_provider_id, "a host default naming an excluded provider falls back");
		Check(SettingsStore::Save(file, loaded), "the restricted build saves the loaded settings");
		std::string text;
		{
			std::ifstream in(file);
			text.assign(std::istreambuf_iterator<char>(in), {});
		}
		for (const char* key : {"active_provider_id=codex-cli", "default_new_chat_provider_id=codex-cli", "permission_reviewer_provider_id=codex-cli"})
			Check(text.find(key) != std::string::npos, "a provider choice naming an excluded provider is saved unchanged");
		Check(text.find("defaultProviderId") != std::string::npos, "a host default naming an excluded provider is saved unchanged");
	}
	{
		// Reloading a file that now names an included provider forgets the earlier remembered choice.
		AppSettings reused;
		(void)SettingsStore::Load(file, reused);
		const std::filesystem::path included = root / "included.txt";
		AppSettings plain;
		plain.active_provider_id = "claude-cli";
		Check(SettingsStore::Save(included, plain) && SettingsStore::Load(included, reused).loaded, "a settings file naming only included providers loads");
		Check(reused.excluded_provider_selections.empty(), "a reload forgets choices remembered from an earlier file");
	}
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);

	// Usage accounting written by a full build: excluded providers' rows are skipped, not treated as corruption.
	const nlohmann::json coverage = nlohmann::json::array({{{"providerId", "codex-cli"}}, {{"providerId", "claude-cli"}}});
	Check(uam::usage::IncludedProviderRows(coverage) == uam::provider_ids::kAllCliProviderIds.size(), "coverage counts only included providers");
	Check(uam::usage::SkipsExcludedProviderRecord(nlohmann::json::array({"[\"native\",\"codex-cli\",\"x\"]", 1, nlohmann::json::array({"codex-cli"})})), "an excluded provider's saved record is skipped");
	Check(!uam::usage::SkipsExcludedProviderRecord(nlohmann::json::array({"[\"native\",\"claude-cli\",\"x\"]", 1, nlohmann::json::array({"claude-cli"})})), "an included provider's record is read");
	Check(!uam::usage::SkipsExcludedProvider(""), "an empty provider stays invalid rather than skipped");

	if (failures == 0) std::puts("build flag profile checks passed");
	return failures == 0 ? 0 : 1;
}
