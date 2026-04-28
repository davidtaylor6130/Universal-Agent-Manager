#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/string_utils.h"

#include <sstream>

using namespace provider_runtime_internal;

namespace
{
	std::vector<std::string> CopilotFlagsFromSettings(const AppSettings& settings)
	{
		std::vector<std::string> flags;
		const std::vector<std::string> extra_flags = SplitCommandLineWords(settings.provider_extra_flags);
		flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());
		return flags;
	}

	std::string ShellJoin(const std::vector<std::string>& argv)
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

	void AppendCopilotModeArgs(std::vector<std::string>& argv, const ChatSession& chat, const AppSettings& settings)
	{
		const std::string model_id = uam::strings::Trim(chat.model_id);
		if (!model_id.empty())
		{
			argv.push_back("--model");
			argv.push_back(model_id);
		}

		const std::string approval_mode = uam::strings::Trim(chat.approval_mode);
		if (approval_mode == "plan")
		{
			argv.push_back("--plan");
		}
		else if (approval_mode == "yolo" || settings.provider_yolo_mode)
		{
			argv.push_back("--allow-all");
		}
	}
} // namespace

const char* CopilotCliProviderRuntime::RuntimeId() const
{
	return "copilot-cli";
}

bool CopilotCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* CopilotCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string CopilotCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string CopilotCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = {"copilot", "-p"};
	if (profile.supports_resume && !uam::strings::Trim(resume_session_id).empty())
	{
		argv.push_back("--resume");
		argv.push_back(uam::strings::Trim(resume_session_id));
	}

	ChatSession command_chat;
	command_chat.approval_mode = settings.provider_yolo_mode ? "yolo" : "";
	AppendCopilotModeArgs(argv, command_chat, provider_settings);
	const std::vector<std::string> flags = CopilotFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	argv.push_back(BuildPrompt(profile, prompt, files));
	return ShellJoin(argv);
}

std::vector<std::string> CopilotCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = SplitCommandLineWords(profile.interactive_command.empty() ? "copilot" : profile.interactive_command);
	if (argv.empty())
	{
		argv.push_back("copilot");
	}

	if (profile.supports_resume && !uam::strings::Trim(chat.native_session_id).empty())
	{
		argv.push_back("--resume");
		argv.push_back(uam::strings::Trim(chat.native_session_id));
	}

	AppendCopilotModeArgs(argv, chat, provider_settings);
	const std::vector<std::string> flags = CopilotFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	return argv;
}

MessageRole CopilotCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CopilotCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool CopilotCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool CopilotCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool CopilotCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool CopilotCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetCopilotCliProviderRuntime()
{
	static const CopilotCliProviderRuntime runtime;
	return runtime;
}
