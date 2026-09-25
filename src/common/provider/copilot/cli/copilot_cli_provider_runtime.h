#pragma once

#include "common/provider/provider_runtime.h"

class CopilotCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	bool OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const uam::acp_detail::AcpResponseFailureDetails& details) const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	std::string LocalCliCompatibilityError(const uam::AppState& app) const override;
	bool RecentOutputIndicatesInputPrompt(std::string_view recent_output) const override;
	bool PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& profile, const std::string& resume_id, const ExecutionHost& execution_host, std::string* error_out) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;

	const char* AcpProtocolKind() const override { return "copilot-acp"; }
	const char* GetAcpDisplayName() const override { return "GitHub Copilot ACP"; }
	bool IsGenericAcpSession() const override { return true; }
	ProviderAcpSettingChangeAction AcpModelChangeAction() const override { return ProviderAcpSettingChangeAction::SendRequestAndAwaitConfigOptions; }
	bool OnAcpConfigOptionsUpdated(uam::AcpSessionState& session, const nlohmann::json& config_options) const override;
	bool OnAcpReconcileModelOptions(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetCopilotCliProviderRuntime();
std::filesystem::path CopilotSessionStatePath();
std::vector<ChatSession> LoadCopilotSessionStateChats(
    const std::filesystem::path& session_state_root,
    const std::filesystem::path& workspace_filter = {},
    const ProviderRuntimeHistoryLoadOptions& options = {},
    std::string* error_out = nullptr);
