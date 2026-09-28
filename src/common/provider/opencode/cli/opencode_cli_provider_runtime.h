#pragma once

#include "common/provider/provider_runtime.h"
#include <stop_token>

class OpenCodeCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	std::string LocalCliCompatibilityError(const uam::AppState& app) const override;
	bool RequiresNativeSessionCreation() const override { return true; }
	bool SupportsInteractivePromptTracking() const override { return false; }
	std::string CreateNativeSession(const ProviderProfile& profile, const std::filesystem::path& workspace, std::stop_token stop_token, std::string* error_out, const ExecutionHost* remote_host) const override;
	std::string InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const override;
	bool PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& profile, const std::string& resume_id, const ExecutionHost& host, std::string* error_out) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	std::vector<std::string> BuildNativeDiscoveryArgv(const ProviderProfile& profile) const override;
	std::vector<std::string> BuildNativeExportArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	void ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json& native_item) const override;
	void ApplyAcpToolMetadata(uam::AcpToolCallState& tool, const nlohmann::json& update) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	std::vector<std::pair<std::string, std::string>> BuildStructuredLaunchEnvironment(const ProviderProfile& profile, const ChatSession& chat) const override;
	bool OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const uam::acp_detail::AcpResponseFailureDetails& details) const override;
	const char* AcpProtocolKind() const override { return "opencode-acp"; }
	const char* GetAcpDisplayName() const override { return "OpenCode ACP"; }
	bool IsGenericAcpSession() const override { return true; }
	bool AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const override;
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState& session) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetOpenCodeCliProviderRuntime();
