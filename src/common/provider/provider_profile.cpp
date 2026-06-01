#include "common/provider/provider_profile.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr std::size_t kMaxBuiltInProviderProfiles = uam::provider_ids::kAllCliProviderIds.size();

	struct BuiltInProviderProfileDefinition
	{
		std::string_view id;
		std::string_view title;
		std::string_view command_template;
		std::string_view interactive_command;
		std::string_view structured_protocol;
		std::string_view resume_argument;
		std::string_view history_adapter;
		std::string_view prompt_bootstrap;
		std::string_view prompt_bootstrap_path;
		std::initializer_list<std::string_view> user_message_types;
		std::initializer_list<std::string_view> assistant_message_types;
	};

	std::vector<std::string> StringList(const std::initializer_list<std::string_view> values)
	{
		std::vector<std::string> result;
		result.reserve(values.size());
		for (const std::string_view value : values)
		{
			result.emplace_back(value);
		}
		return result;
	}

	ProviderProfile MakeBuiltInCliProfile(const BuiltInProviderProfileDefinition& definition)
	{
		ProviderProfile profile;
		profile.id = definition.id;
		profile.title = definition.title;
		profile.execution_mode = uam::provider_profile_constants::kExecutionModeCli;
		profile.output_mode = uam::provider_profile_constants::kOutputModeCli;
		profile.command_template = definition.command_template;
		profile.interactive_command = definition.interactive_command;
		profile.supports_cli = true;
		profile.supports_structured = true;
		profile.structured_protocol = definition.structured_protocol;
		profile.supports_interactive = true;
		profile.supports_resume = true;
		profile.resume_argument = definition.resume_argument;
		profile.history_adapter = definition.history_adapter;
		profile.prompt_bootstrap = definition.prompt_bootstrap;
		profile.prompt_bootstrap_path = definition.prompt_bootstrap_path;
		profile.user_message_types = StringList(definition.user_message_types);
		profile.assistant_message_types = StringList(definition.assistant_message_types);
		return profile;
	}

	std::string CanonicalProviderProfileId(std::string_view provider_id)
	{
		return uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
	}

	bool ProviderProfileIdMatchesCanonicalId(std::string_view profile_id, std::string_view canonical_id)
	{
		return CanonicalProviderProfileId(profile_id) == canonical_id;
	}

	template <typename Profiles> auto FindProfileById(Profiles& profiles, std::string_view id)
	{
		const std::string canonical_id = CanonicalProviderProfileId(id);
		if (canonical_id.empty())
		{
			return std::ranges::end(profiles);
		}

		return std::ranges::find_if(profiles, [&canonical_id](const ProviderProfile& profile) {
			return ProviderProfileIdMatchesCanonicalId(profile.id, canonical_id);
		});
	}

	template <typename Profiles> auto* ProfileOrNull(Profiles& profiles, decltype(profiles.begin()) found)
	{
		return found == profiles.end() ? nullptr : &*found;
	}
} // namespace

ProviderProfile ProviderProfileStore::DefaultGeminiProfile()
{
	return MakeBuiltInCliProfile({
	    .id = uam::provider_ids::kGeminiCli,
	    .title = "Gemini CLI",
	    .command_template = "gemini -r {resume} {flags} {prompt}",
	    .interactive_command = "gemini",
	    .structured_protocol = uam::provider_profile_constants::kProtocolGeminiAcp,
	    .resume_argument = "-r",
	    .history_adapter = uam::provider_profile_constants::kHistoryAdapterGeminiCliJson,
	    .prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapGeminiAtPath,
	    .prompt_bootstrap_path = uam::provider_profile_constants::kGeminiPromptBootstrapPath,
	    .user_message_types = {"user"},
	    .assistant_message_types = {"assistant", "model", "gemini"},
	});
}

ProviderProfile ProviderProfileStore::DefaultCodexProfile()
{
	return MakeBuiltInCliProfile({
	    .id = uam::provider_ids::kCodexCli,
	    .title = "Codex CLI",
	    .command_template = "codex exec {flags} {prompt}",
	    .interactive_command = "codex --no-alt-screen",
	    .structured_protocol = uam::provider_profile_constants::kProtocolCodexAppServer,
	    .resume_argument = "",
	    .history_adapter = uam::provider_profile_constants::kHistoryAdapterLocalJson,
	    .prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapNone,
	    .prompt_bootstrap_path = "",
	    .user_message_types = {"user"},
	    .assistant_message_types = {"assistant", "codex"},
	});
}

ProviderProfile ProviderProfileStore::DefaultClaudeProfile()
{
	return MakeBuiltInCliProfile({
	    .id = uam::provider_ids::kClaudeCli,
	    .title = "Claude Code",
	    .command_template = "claude -p {prompt}",
	    .interactive_command = "claude",
	    .structured_protocol = uam::provider_profile_constants::kProtocolClaudeCodeStreamJson,
	    .resume_argument = "--resume",
	    .history_adapter = uam::provider_profile_constants::kHistoryAdapterLocalJson,
	    .prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapNone,
	    .prompt_bootstrap_path = "",
	    .user_message_types = {"user", "human"},
	    .assistant_message_types = {"assistant", "claude"},
	});
}

ProviderProfile ProviderProfileStore::DefaultOpenCodeProfile()
{
	return MakeBuiltInCliProfile({
	    .id = uam::provider_ids::kOpenCodeCli,
	    .title = "OpenCode",
	    .command_template = "opencode run --session {resume} {flags} {prompt}",
	    .interactive_command = "opencode",
	    .structured_protocol = uam::provider_profile_constants::kProtocolOpenCodeAcp,
	    .resume_argument = "--session",
	    .history_adapter = uam::provider_profile_constants::kHistoryAdapterLocalJson,
	    .prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapNone,
	    .prompt_bootstrap_path = "",
	    .user_message_types = {"user"},
	    .assistant_message_types = {"assistant", "opencode"},
	});
}

ProviderProfile ProviderProfileStore::DefaultCopilotProfile()
{
	return MakeBuiltInCliProfile({
	    .id = uam::provider_ids::kCopilotCli,
	    .title = "GitHub Copilot CLI",
	    .command_template = "copilot -p {prompt} {flags}",
	    .interactive_command = "copilot",
	    .structured_protocol = uam::provider_profile_constants::kProtocolCopilotAcp,
	    .resume_argument = "--resume",
	    .history_adapter = uam::provider_profile_constants::kHistoryAdapterLocalJson,
	    .prompt_bootstrap = uam::provider_profile_constants::kPromptBootstrapNone,
	    .prompt_bootstrap_path = "",
	    .user_message_types = {"user", "human"},
	    .assistant_message_types = {"assistant", "copilot"},
	});
}

std::vector<ProviderProfile> ProviderProfileStore::BuiltInProfiles()
{
	std::vector<ProviderProfile> profiles;
	profiles.reserve(kMaxBuiltInProviderProfiles);
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
	profiles.reserve(profiles.size() + built_ins.size());

	for (const ProviderProfile& built_in : built_ins)
	{
		if (ProviderProfileStore::FindById(profiles, built_in.id) == nullptr)
		{
			profiles.push_back(built_in);
		}
	}
}

const ProviderProfile* ProviderProfileStore::FindById(const std::vector<ProviderProfile>& profiles, std::string_view id)
{
	return ProfileOrNull(profiles, FindProfileById(profiles, id));
}

ProviderProfile* ProviderProfileStore::FindById(std::vector<ProviderProfile>& profiles, std::string_view id)
{
	return ProfileOrNull(profiles, FindProfileById(profiles, id));
}
