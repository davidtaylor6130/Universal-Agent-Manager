#pragma once

#include "common/provider/custom_acp/custom_acp_config.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"

#include <vector>

/// The best-effort Custom ACP agent (#523): structured Chat over standard ACP with a user-configured local agent.
/// Features follow only what the agent reports in `initialize` and `session/new`; UAM adds no provider-specific
/// fallbacks. CLI View, text workers, native history import, usage and compaction are unavailable.
class CustomAcpProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	bool SupportsTextWorkers() const override { return false; }
	bool UsesCentralLaunchConfiguration() const override { return false; }
	bool LaunchArgumentsArePrivate() const override { return true; }
	const char* AcpProtocolKind() const override;
	const char* GetAcpDisplayName() const override { return "Custom ACP agent"; }
	bool IsGenericAcpSession() const override { return true; }
	bool SupportsInteractivePromptTracking() const override { return false; }
	bool RecentOutputIndicatesInputPrompt(std::string_view) const override { return false; }
	std::string InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	bool PrepareStructuredLaunch(uam::AcpSessionState& session, const ProviderProfile& profile, std::string_view version, bool remote,
	    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment, std::string& error) const override;
};

const IProviderRuntime& GetCustomAcpProviderRuntime();

/// <summary>Makes `config` the one launches use and shows its name in the provider lists.</summary>
void ApplyCustomAcpConfiguration(std::vector<ProviderProfile>& profiles, uam::custom_acp::AgentConfig config);
