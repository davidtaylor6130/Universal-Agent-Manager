#include "common/provider/provider_profile.h"

#include "common/provider/runtime/provider_build_config.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{
	bool IsSameId(const std::string& lhs, const std::string& rhs)
	{
		auto lower = [](std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		};

		return lower(lhs) == lower(rhs);
	}
} // namespace

ProviderProfile ProviderProfileStore::DefaultGeminiProfile()
{
	ProviderProfile profile;
	profile.id = "gemini-cli";
	profile.title = "Gemini CLI";
	profile.execution_mode = "cli";
	profile.output_mode = "cli";
	profile.command_template = "gemini -r {resume} {flags} {prompt}";
	profile.interactive_command = "gemini";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.resume_argument = "-r";
	profile.history_adapter = "gemini-cli-json";
	profile.prompt_bootstrap = "gemini-at-path";
	profile.prompt_bootstrap_path = "@.gemini/gemini.md";
	profile.user_message_types = {"user"};
	profile.assistant_message_types = {"assistant", "model", "gemini"};
	return profile;
}

std::vector<ProviderProfile> ProviderProfileStore::BuiltInProfiles()
{
	return {DefaultGeminiProfile()};
}

void ProviderProfileStore::EnsureDefaultProfile(std::vector<ProviderProfile>& profiles)
{
	const std::vector<ProviderProfile> built_ins = BuiltInProfiles();

	for (const ProviderProfile& built_in : built_ins)
	{
		bool found = false;

		for (const ProviderProfile& profile : profiles)
		{
			if (IsSameId(profile.id, built_in.id))
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			profiles.push_back(built_in);
		}
	}
}

const ProviderProfile* ProviderProfileStore::FindById(const std::vector<ProviderProfile>& profiles, const std::string& id)
{
	for (const ProviderProfile& profile : profiles)
	{
		if (IsSameId(profile.id, id))
		{
			return &profile;
		}
	}

	return nullptr;
}

ProviderProfile* ProviderProfileStore::FindById(std::vector<ProviderProfile>& profiles, const std::string& id)
{
	for (ProviderProfile& profile : profiles)
	{
		if (IsSameId(profile.id, id))
		{
			return &profile;
		}
	}

	return nullptr;
}
