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
	profile.id = "gemini-structured";
	profile.title = "Gemini (Structured)";
	profile.execution_mode = "cli";
	profile.output_mode = "structured";
	profile.command_template = "gemini -r {resume} {flags} -p {prompt}";
	profile.interactive_command = "gemini";
	profile.supports_interactive = false;
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
	std::vector<ProviderProfile> profiles;
#if UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED
	profiles.push_back(DefaultGeminiProfile());
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	ProviderProfile gemini_cli;
	gemini_cli.id = "gemini-cli";
	gemini_cli.title = "Gemini CLI";
	gemini_cli.execution_mode = "cli";
	gemini_cli.output_mode = "cli";
	gemini_cli.command_template = "gemini -r {resume} {flags} {prompt}";
	gemini_cli.interactive_command = "gemini";
	gemini_cli.supports_interactive = true;
	gemini_cli.supports_resume = true;
	gemini_cli.resume_argument = "-r";
	gemini_cli.history_adapter = "gemini-cli-json";
	gemini_cli.prompt_bootstrap = "gemini-at-path";
	gemini_cli.prompt_bootstrap_path = "@.gemini/gemini.md";
	gemini_cli.user_message_types = {"user"};
	gemini_cli.assistant_message_types = {"assistant", "model", "gemini"};
	profiles.push_back(std::move(gemini_cli));
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
	ProviderProfile codex;
	codex.id = "codex-cli";
	codex.title = "OpenAI Codex CLI";
	codex.execution_mode = "cli";
	codex.output_mode = "cli";
	codex.command_template = "codex exec {flags} {prompt}";
	codex.interactive_command = "codex";
	codex.supports_interactive = true;
	codex.supports_resume = false;
	codex.resume_argument.clear();
	codex.history_adapter = "local-only";
	codex.prompt_bootstrap = "prepend";
	codex.user_message_types = {"user"};
	codex.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(codex));
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	ProviderProfile claude;
	claude.id = "claude-cli";
	claude.title = "Claude CLI";
	claude.execution_mode = "cli";
	claude.output_mode = "cli";
	claude.command_template = "claude {flags} {prompt}";
	claude.interactive_command = "claude";
	claude.supports_interactive = true;
	claude.supports_resume = false;
	claude.resume_argument.clear();
	claude.history_adapter = "local-only";
	claude.prompt_bootstrap = "prepend";
	claude.user_message_types = {"user", "human"};
	claude.assistant_message_types = {"assistant", "model"};
	profiles.push_back(std::move(claude));
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	ProviderProfile opencode;
	opencode.id = "opencode-cli";
	opencode.title = "OpenCode CLI";
	opencode.execution_mode = "cli";
	opencode.output_mode = "cli";
	opencode.command_template = "opencode {flags} {prompt}";
	opencode.interactive_command = "opencode";
	opencode.supports_interactive = true;
	opencode.supports_resume = true;
	opencode.resume_argument = "--session";
	opencode.history_adapter = "local-only";
	opencode.prompt_bootstrap = "prepend";
	opencode.user_message_types = {"user"};
	opencode.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(opencode));
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_LOCAL
	ProviderProfile opencode_local;
	opencode_local.id = "opencode-local";
	opencode_local.title = "OpenCode (Fully Local)";
	opencode_local.execution_mode = "cli";
	opencode_local.output_mode = "cli";
	opencode_local.command_template = "opencode {flags} {prompt}";
	opencode_local.interactive_command = "opencode";
	opencode_local.supports_interactive = true;
	opencode_local.supports_resume = true;
	opencode_local.resume_argument = "--session";
	opencode_local.history_adapter = "local-only";
	opencode_local.prompt_bootstrap = "prepend";
	opencode_local.user_message_types = {"user"};
	opencode_local.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(opencode_local));
#endif

#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	ProviderProfile ollama_engine;
	ollama_engine.id = "ollama-engine";
	ollama_engine.title = "Ollama Engine (Local)";
	ollama_engine.execution_mode = "internal-engine";
	ollama_engine.output_mode = "structured";
	ollama_engine.command_template.clear();
	ollama_engine.interactive_command.clear();
	ollama_engine.supports_interactive = false;
	ollama_engine.supports_resume = false;
	ollama_engine.resume_argument.clear();
	ollama_engine.history_adapter = "local-only";
	ollama_engine.prompt_bootstrap = "prepend";
	ollama_engine.user_message_types = {"user"};
	ollama_engine.assistant_message_types = {"assistant"};
	profiles.push_back(std::move(ollama_engine));
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
