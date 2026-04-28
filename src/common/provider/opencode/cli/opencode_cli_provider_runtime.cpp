#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/string_utils.h"

#include <sstream>

using namespace provider_runtime_internal;

namespace
{
	std::vector<std::string> OpenCodeFlagsFromSettings(const AppSettings& settings)
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

	std::string BuildOpenCodeShellCommand(const std::vector<std::string>& argv)
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
} // namespace

const char* OpenCodeCliProviderRuntime::RuntimeId() const
{
	return "opencode-cli";
}

bool OpenCodeCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* OpenCodeCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string OpenCodeCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string OpenCodeCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	provider_settings.provider_yolo_mode = false;

	std::vector<std::string> argv = {"opencode", "run"};
	if (settings.provider_yolo_mode)
	{
		argv.push_back("--dangerously-skip-permissions");
	}

	const std::string resume_id = uam::strings::Trim(resume_session_id);
	if (profile.supports_resume && !resume_id.empty())
	{
		argv.push_back("--session");
		argv.push_back(resume_id);
	}

	const std::vector<std::string> flags = OpenCodeFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	for (const std::string& file : files)
	{
		if (!uam::strings::Trim(file).empty())
		{
			argv.push_back("--file");
			argv.push_back(file);
		}
	}
	argv.push_back(BuildPrompt(profile, prompt, {}));
	return BuildOpenCodeShellCommand(argv);
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = SplitCommandLineWords(profile.interactive_command.empty() ? "opencode" : profile.interactive_command);
	if (argv.empty())
	{
		argv.push_back("opencode");
	}

	const std::string resume_id = uam::strings::Trim(chat.native_session_id);
	if (profile.supports_resume && !resume_id.empty())
	{
		argv.push_back("--session");
		argv.push_back(resume_id);
	}

	const std::string model_id = uam::strings::Trim(chat.model_id);
	if (!model_id.empty())
	{
		argv.push_back("--model");
		argv.push_back(model_id);
	}

	const std::vector<std::string> flags = OpenCodeFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	return argv;
}

MessageRole OpenCodeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OpenCodeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return LoadLocalChats(data_root);
}

bool OpenCodeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool OpenCodeCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}
