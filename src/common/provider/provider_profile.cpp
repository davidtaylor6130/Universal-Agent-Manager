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
	profile.supports_cli = true;
	profile.supports_structured = true;
	profile.structured_protocol = "gemini-acp";
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

ProviderProfile ProviderProfileStore::DefaultCodexProfile()
{
	ProviderProfile profile;
	profile.id = "codex-cli";
	profile.title = "Codex CLI";
	profile.execution_mode = "cli";
	profile.output_mode = "cli";
	profile.command_template = "codex exec {flags} {prompt}";
	profile.interactive_command = "codex --no-alt-screen";
	profile.supports_cli = true;
	profile.supports_structured = true;
	profile.structured_protocol = "codex-app-server";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.resume_argument.clear();
	profile.history_adapter = "local-json";
	profile.prompt_bootstrap = "none";
	profile.prompt_bootstrap_path.clear();
	profile.user_message_types = {"user"};
	profile.assistant_message_types = {"assistant", "codex"};
	return profile;
}

ProviderProfile ProviderProfileStore::DefaultClaudeProfile()
{
	ProviderProfile profile;
	profile.id = "claude-cli";
	profile.title = "Claude Code";
	profile.execution_mode = "cli";
	profile.output_mode = "cli";
	profile.command_template = "claude -p {prompt}";
	profile.interactive_command = "claude";
	profile.supports_cli = true;
	profile.supports_structured = true;
	profile.structured_protocol = "claude-code-stream-json";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.resume_argument = "--resume";
	profile.history_adapter = "local-json";
	profile.prompt_bootstrap = "none";
	profile.prompt_bootstrap_path.clear();
	profile.user_message_types = {"user", "human"};
	profile.assistant_message_types = {"assistant", "claude"};
	return profile;
}

ProviderProfile ProviderProfileStore::DefaultOpenCodeProfile()
{
	ProviderProfile profile;
	profile.id = "opencode-cli";
	profile.title = "OpenCode";
	profile.execution_mode = "cli";
	profile.output_mode = "cli";
	profile.command_template = "opencode run {flags} {prompt}";
	profile.interactive_command = "opencode";
	profile.supports_cli = true;
	profile.supports_structured = true;
	profile.structured_protocol = "opencode-acp";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.resume_argument = "--session";
	profile.history_adapter = "local-json";
	profile.prompt_bootstrap = "none";
	profile.prompt_bootstrap_path.clear();
	profile.user_message_types = {"user"};
	profile.assistant_message_types = {"assistant", "opencode"};
	return profile;
}

ProviderProfile ProviderProfileStore::DefaultCopilotProfile()
{
	ProviderProfile profile;
	profile.id = "copilot-cli";
	profile.title = "GitHub Copilot CLI";
	profile.execution_mode = "cli";
	profile.output_mode = "cli";
	profile.command_template = "copilot -p {prompt} {flags}";
	profile.interactive_command = "copilot";
	profile.supports_cli = true;
	profile.supports_structured = true;
	profile.structured_protocol = "copilot-acp";
	profile.supports_interactive = true;
	profile.supports_resume = true;
	profile.resume_argument = "--resume";
	profile.history_adapter = "local-json";
	profile.prompt_bootstrap = "none";
	profile.prompt_bootstrap_path.clear();
	profile.user_message_types = {"user", "human"};
	profile.assistant_message_types = {"assistant", "copilot"};
	return profile;
}

std::vector<ProviderProfile> ProviderProfileStore::BuiltInProfiles()
{
	std::vector<ProviderProfile> profiles;
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	profiles.push_back(DefaultGeminiProfile());
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	profiles.push_back(DefaultCodexProfile());
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	profiles.push_back(DefaultClaudeProfile());
#endif
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	profiles.push_back(DefaultOpenCodeProfile());
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	profiles.push_back(DefaultCopilotProfile());
#endif
	return profiles;
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
