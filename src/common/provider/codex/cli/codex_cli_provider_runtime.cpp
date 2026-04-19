#include "common/provider/codex/cli/codex_cli_provider_runtime.h"

#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/runtime/provider_runtime_internal.h"

using namespace provider_runtime_internal;

namespace
{
	std::vector<std::string> CodexFlagsFromSettings(const AppSettings& settings)
	{
		std::vector<std::string> flags;
		if (settings.provider_yolo_mode)
		{
			flags.push_back("--full-auto");
		}

		const std::vector<std::string> extra_flags = SplitCommandLineWords(settings.provider_extra_flags);
		flags.insert(flags.end(), extra_flags.begin(), extra_flags.end());
		return flags;
	}
} // namespace

const char* CodexCliProviderRuntime::RuntimeId() const
{
	return "codex-cli";
}

bool CodexCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* CodexCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string CodexCliProviderRuntime::BuildPrompt(const ProviderProfile&, const std::string& user_prompt, const std::vector<std::string>& files) const
{
	return provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string CodexCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	(void)resume_session_id;
	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	provider_settings.provider_yolo_mode = false;
	if (settings.provider_yolo_mode)
	{
		provider_settings.provider_extra_flags = provider_settings.provider_extra_flags.empty()
			? "--full-auto"
			: "--full-auto " + provider_settings.provider_extra_flags;
	}
	return BuildCommandFromTemplate(provider_settings, prompt, files, "", "codex exec {flags} {prompt}");
}

std::vector<std::string> CodexCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	AppSettings provider_settings = MergeProviderSettings(profile, settings);
	std::vector<std::string> argv;
		const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
		if (profile.supports_resume && !resume_id.empty())
		{
			argv = {"codex", "resume", "--no-alt-screen", resume_id};
		}
	else
	{
		argv = SplitCommandLineWords(profile.interactive_command.empty() ? "codex --no-alt-screen" : profile.interactive_command);
	}

	if (!chat.model_id.empty())
	{
		argv.push_back("-m");
		argv.push_back(chat.model_id);
	}

	const std::vector<std::string> flags = CodexFlagsFromSettings(provider_settings);
	argv.insert(argv.end(), flags.begin(), flags.end());
	return argv;
}

MessageRole CodexCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const
{
	return provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CodexCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	(void)profile;
	return LoadLocalChats(data_root);
}

bool CodexCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return SaveLocalChat(data_root, chat);
}

bool CodexCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool CodexCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool CodexCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetCodexCliProviderRuntime()
{
	static const CodexCliProviderRuntime runtime;
	return runtime;
}
