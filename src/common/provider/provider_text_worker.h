#pragma once
#include "common/provider/provider_profile.h"
#include "common/provider/provider_ids.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include <algorithm>
#include <iterator>
#include <vector>
namespace uam::provider_workers
{
	inline bool IsProvider(const ProviderProfile& profile, std::string_view provider_id)
	{
		return uam::provider_ids::IsCliProviderAliasOf(profile.id, provider_id);
	}

	inline bool ApplyWorkerIsolationPolicy(const ProviderProfile& profile, const std::filesystem::path& workspace, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (!uam::io::WriteTextFile(workspace / "opencode.json", R"({"permission":{"*":"deny","external_directory":"deny"},"share":"disabled","autoupdate":false})"))
			{
				return false;
			}
			if (argv.size() < 2) return false;
			argv.insert(argv.begin() + 2, "--pure");
		}
		else if (IsProvider(profile, uam::provider_ids::kGeminiCli))
		{
			const std::filesystem::path policy_file = workspace / "deny-all-tools.toml";
			if (!uam::io::WriteTextFile(policy_file, R"([[rule]]
toolName = "*"
decision = "deny"
priority = 999999

[[rule]]
mcpName = "*"
decision = "deny"
priority = 999999
)"))
			{
				return false;
			}
			if (argv.empty()) return false;
			argv.insert(argv.begin() + 1, {
			                                  "--approval-mode",
			                                  "plan",
			                                  "--admin-policy",
			                                  uam::paths::Utf8PathString(policy_file),
			                              });
		}
		return true;
	}

	inline bool RemoveWorkerPromptArgument(const ProviderProfile& profile, std::vector<std::string>& argv)
	{
		if (IsProvider(profile, uam::provider_ids::kGeminiCli) || IsProvider(profile, uam::provider_ids::kCopilotCli))
		{
			const auto prompt_flag = std::ranges::find(argv, "-p");
			if (prompt_flag == argv.end() || std::next(prompt_flag) == argv.end()) return false;
			argv.erase(prompt_flag, std::next(prompt_flag, 2));
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kClaudeCli))
		{
			if (argv.size() < 2 || argv[argv.size() - 2] != "--") return false;
			argv.erase(argv.end() - 2, argv.end());
			return true;
		}
		if (IsProvider(profile, uam::provider_ids::kCodexCli) || IsProvider(profile, uam::provider_ids::kOpenCodeCli))
		{
			if (argv.size() < 2) return false;
			argv.pop_back();
			return true;
		}
		return false;
	}

}
