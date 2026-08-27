#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"

namespace
{
	std::vector<std::string> OpenCodeFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings);
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

std::vector<std::pair<std::string, std::string>> OpenCodeCliProviderRuntime::BuildStructuredLaunchEnvironment(const ProviderProfile&, const ChatSession&) const
{
	// OpenCode ACP does not reliably forward child-session permission requests, so
	// disable Task until the provider can mediate those requests without hanging.
	return {{"OPENCODE_PERMISSION", R"({"*":"ask","task":"deny"})"}};
}

std::string OpenCodeCliProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::acp_detail::ValidGenericAcpResumeId(chat);
}

std::string OpenCodeCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	return mode_id == "default" ? "build" : mode_id;
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}
