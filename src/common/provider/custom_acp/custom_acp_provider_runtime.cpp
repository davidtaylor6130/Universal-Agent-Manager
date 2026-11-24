#include "common/provider/custom_acp/custom_acp_provider_runtime.h"

#include "common/provider/custom_acp_config.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/paths/path_utils.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/platform/platform_services.h"

const char* CustomAcpProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCustomAcp;
}

const char* CustomAcpProviderRuntime::AcpProtocolKind() const
{
	return uam::provider_profile_constants::kProtocolCustomAcp;
}

std::string CustomAcpProviderRuntime::InteractiveConfigurationError(const ProviderProfile&, const AppSettings&) const
{
	return "CLI View is not available for the custom ACP agent. Use Chat.";
}

std::vector<std::string> CustomAcpProviderRuntime::BuildInteractiveArgv(const ProviderProfile&, const ChatSession&, const AppSettings&) const
{
	return {};
}

MessageRole CustomAcpProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CustomAcpProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path&, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return {}; // A generic agent has no native history UAM can read; its chats are UAM's own saved chats.
}

bool CustomAcpProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

std::vector<std::string> CustomAcpProviderRuntime::BuildWorkerArgv(const ProviderProfile&, const AppSettings&, std::string_view, std::string_view) const
{
	return {};
}

std::vector<std::string> CustomAcpProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const
{
	// Diagnostics and launch details show the configured argument array; PrepareStructuredLaunch replaces it with the
	// resolved executable after the launch checks pass.
	return uam::custom_acp::LaunchArgv(uam::custom_acp::ActiveConfig());
}

bool CustomAcpProviderRuntime::PrepareStructuredLaunch(uam::AcpSessionState&, const ProviderProfile&, std::string_view, bool remote,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment, std::string& error) const
{
	// Only the Settings configuration decides what starts: anything shared launch preparation appended is replaced.
	std::vector<std::string> launch_argv;
	error = uam::custom_acp::PrepareLaunch(uam::custom_acp::ActiveConfig(), CustomAcpExecutableSearch(), remote, launch_argv, environment);
	if (!error.empty()) return false;
	argv = std::move(launch_argv);
	return true;
}

uam::custom_acp::ExecutableSearch CustomAcpExecutableSearch()
{
	// The folders the platform's own launcher searches (on macOS also Homebrew and Node version managers; on Windows
	// the PATH refreshed from the registry), so the program allowed in Settings is the one that starts.
	uam::custom_acp::ExecutableSearch search = uam::custom_acp::DefaultExecutableSearch();
	const std::vector<std::string> platform_directories = PlatformServicesFactory::Instance().process_service.ExecutableSearchDirectories();
	if (!platform_directories.empty())
	{
		search.directories.clear();
		for (const std::string& directory : platform_directories) search.directories.push_back(uam::paths::PathFromUtf8(directory));
	}
	return search;
}

const IProviderRuntime& GetCustomAcpProviderRuntime()
{
	static const CustomAcpProviderRuntime runtime;
	return runtime;
}

void ApplyCustomAcpConfiguration(std::vector<ProviderProfile>& profiles, uam::custom_acp::AgentConfig config)
{
	if (ProviderProfile* profile = ProviderProfileStore::FindById(profiles, uam::provider_ids::kCustomAcp); profile != nullptr)
		profile->title = uam::custom_acp::DisplayName(config);
	uam::custom_acp::SetActiveConfig(std::move(config));
}
