#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

namespace
{
	constexpr const char* kOpenCodeDangerouslySkipPermissionsFlag = "--dangerously-skip-permissions";

	std::vector<std::string> OpenCodeFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kOpenCodeDangerouslySkipPermissionsFlag);
	}

} // namespace

const char* OpenCodeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kOpenCodeCli;
}


std::vector<std::string> OpenCodeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

	uam::provider_runtime_internal::AppendArgs(argv, OpenCodeFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole OpenCodeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OpenCodeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	std::vector<ChatSession> chats = uam::provider_runtime_internal::LoadLocalChats(data_root);
	return chats;
}

bool OpenCodeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}


bool OpenCodeCliProviderRuntime::ProviderRecognizesSubagentTool(std::string_view tool_name) const
{
	if (tool_name.empty())
		return false;

	std::string lower_name(tool_name.size(), '\0');
	for (std::size_t i = 0; i < tool_name.size(); ++i)
	{
		lower_name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(tool_name[i])));
	}

	for (const char* word : {"task", "subtask", "delegate"})
	{
		const std::size_t len = strlen(word);
		std::size_t pos = 0;
		while ((pos = lower_name.find(word, pos)) != std::string_view::npos)
		{
			const bool before_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(tool_name[pos - 1])) && tool_name[pos - 1] != '_';
			const std::size_t end_pos = pos + len;
			const bool after_ok = (end_pos >= tool_name.size()) || !std::isalnum(static_cast<unsigned char>(tool_name[end_pos])) && tool_name[end_pos] != '_';
			if (before_ok && after_ok)
				return true;
			pos += 1;
		}
	}
	return false;
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"opencode", "run"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);

	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const
{
	return {"opencode", "acp"};
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}
