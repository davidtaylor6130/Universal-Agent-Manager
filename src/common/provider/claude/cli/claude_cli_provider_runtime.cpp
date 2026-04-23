#include "common/provider/claude/cli/claude_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/string_utils.h"

#include <sstream>

using namespace provider_runtime_internal;

namespace
{
	std::vector<std::string> ClaudeFlagsFromSettings(const AppSettings& settings)
	{
		std::vector<std::string> flags;
		if (settings.provider_yolo_mode)
		{
			flags.push_back("--dangerously-skip-permissions");
		}

		const std::vector<std::string> extra_flags = SplitCommandLineWords(settings.provider_extra_flags);
		flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());
		return flags;
	}

	std::string BuildClaudeShellCommand(const std::vector<std::string>& argv)
	{
		std::ostringstream out;
		bool first = true;
		for (const std::string& arg : argv)
		{
			if (!first)
			{
				out << ' ';
			}
			out << ShellEscape(arg);
			first = false;
		}
		return out.str();
	}

	void AppendClaudeModeArgs(std::vector<std::string>& argv, const ChatSession& chat, const AppSettings& settings)
	{
		const std::string model_id = uam::strings::Trim(chat.model_id);
		if (!model_id.empty())
		{
			argv.push_back("--model");
			argv.push_back(model_id);
		}

		if (!settings.provider_yolo_mode)
		{
			const std::string approval_mode = uam::strings::Trim(chat.approval_mode);
			if (approval_mode == "default" || approval_mode == "plan")
			{
				argv.push_back("--permission-mode");
				argv.push_back(approval_mode);
			}
		}
	}
} // namespace

const char* ClaudeCliProviderRuntime::RuntimeId() const
{
	return "claude-cli";
}

bool ClaudeCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* ClaudeCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string ClaudeCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string ClaudeCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = {"claude", "-p"};
	if (profile.supports_resume && !uam::strings::Trim(resume_session_id).empty())
	{
		argv.push_back("--resume");
		argv.push_back(uam::strings::Trim(resume_session_id));
	}

	const std::vector<std::string> flags = ClaudeFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	argv.push_back(BuildPrompt(profile, prompt, files));
	return BuildClaudeShellCommand(argv);
}

std::vector<std::string> ClaudeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = {"claude"};
	if (profile.supports_resume && !uam::strings::Trim(chat.native_session_id).empty())
	{
		argv.push_back("--resume");
		argv.push_back(uam::strings::Trim(chat.native_session_id));
	}

	AppendClaudeModeArgs(argv, chat, provider_settings);
	const std::vector<std::string> flags = ClaudeFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	return argv;
}

MessageRole ClaudeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ClaudeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool ClaudeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool ClaudeCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
