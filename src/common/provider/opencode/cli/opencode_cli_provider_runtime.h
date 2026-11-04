#pragma once

#include "common/provider/provider_runtime.h"
#include <stop_token>

class OpenCodeCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	std::string LocalCliCompatibilityError(const uam::AppState& app) const override;
	std::string StructuredCliVersionError(const ProviderProfile&, std::string_view, bool = false, const ChatSession* = nullptr) const override;
	bool RequiresStructuredCliVersion(const ProviderProfile&) const override;
	bool PrepareStructuredLaunch(uam::AcpSessionState&, const ProviderProfile&, std::string_view, bool,
	    std::vector<std::string>&, std::vector<std::pair<std::string, std::string>>&, std::string&) const override;
	std::vector<std::string> BuildLocalNativeDiscoveryArgv(const ProviderProfile&) const override;
	std::vector<std::string> BuildLocalNativeExportArgv(const ProviderProfile&, const ChatSession&) const override;
	nlohmann::json OnAcpBuildSessionSetupRequest(uam::AcpSessionState&, int, const ChatSession&, const std::string&, bool, std::string&) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState&, int, const std::string&, const ChatSession&, std::string&) const override;
	AcpRequestPreflight OnAcpPrepareSetup(uam::AppState&, uam::AcpSessionState&, const ChatSession&, const std::string&, std::string&) const override;
	bool PrepareAcpUserInputResponse(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::map<std::string, std::vector<std::string>>&, nlohmann::json&, std::string*) const override;
	bool RequiresNativeSessionCreation() const override { return true; }
	bool SupportsInteractivePromptTracking() const override { return false; }
	std::string CreateNativeSession(const ProviderProfile& profile, const std::filesystem::path& workspace, std::stop_token stop_token, std::string* error_out, const ExecutionHost* remote_host) const override;
	std::string InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const override;
	bool PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& profile, const std::string& resume_id, const ExecutionHost& host, std::string* error_out) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	bool PrepareInteractiveLaunch(uam::AppState&, uam::CliTerminalState&, const ChatSession&, const ExecutionHost&,
	    std::vector<std::string>&, std::vector<std::pair<std::string, std::string>>&, std::string*) const override;
	ProviderTerminalActivity PollInteractiveActivity(uam::CliTerminalState&, std::string_view, const std::filesystem::path&, bool) const override;
	bool InteractiveObservationMatchesHost(const uam::CliTerminalState&, const ExecutionHost&) const override;
	void CheckpointInteractiveSubmission(uam::CliTerminalState&) const override;
	bool RecentOutputIndicatesInputPrompt(std::string_view) const override { return false; }
	std::vector<std::string> BuildNativeDiscoveryArgv(const ProviderProfile& profile) const override;
	std::vector<std::string> BuildNativeExportArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	void ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json& native_item) const override;
	void ApplyAcpToolMetadata(uam::AcpToolCallState& tool, const nlohmann::json& update) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	bool PrepareLocalWorkerLaunch(const uam::AppState&, const ProviderProfile&, std::vector<std::string>&, std::string&, bool* = nullptr) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	std::vector<std::pair<std::string, std::string>> BuildStructuredLaunchEnvironment(const ProviderProfile& profile, const ChatSession& chat) const override;
	bool OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const uam::acp_detail::AcpResponseFailureDetails& details) const override;
	bool OnAcpHoldUnconfirmedInput(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::string&, std::optional<int> = std::nullopt) const override;
	bool OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const std::string& method, const std::string& request_id, const nlohmann::json& result) const override;
	void OnAcpObserveCancelledPromptResult(uam::AppState&, uam::AcpSessionState&,
	    ChatSession&, const nlohmann::json&) const override;
	std::size_t AcpBackgroundTaskCount(const uam::AcpSessionState& session) const override;
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const override;
	bool OnAcpHandleMessage(uam::AppState&, uam::AcpSessionState& session, ChatSession&,
	    const nlohmann::json& message, const CefRefPtr<CefBrowser>&) const override;
	const char* AcpProtocolKind() const override { return "opencode-acp"; }
	const char* GetAcpDisplayName() const override { return "OpenCode ACP"; }
	bool IsGenericAcpSession() const override { return true; }
	bool SupportsSavedImageRetry(const uam::AcpSessionState&) const override;
	SavedImageRetryPreflight PrepareSavedImageRetry(uam::AcpSessionState&, const ChatSession&, const std::vector<MessageAttachment>&, std::string&) const override;
	nlohmann::json SavedImageRetryMetadata(const uam::AcpSessionState&, const ChatSession&) const override;
	bool EncodePreparedSavedImageRetry(nlohmann::json&, const nlohmann::json&, std::string&, const std::vector<MessageAttachment>& = {}, const nlohmann::json& = nullptr) const override;
	bool AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const override;
	bool AcpPermissionIsNonActionQuestion(const uam::AcpPendingPermissionState& pending) const override;
	std::optional<bool> TryWriteAcpOutOfBandMessage(uam::AcpSessionState& session, const nlohmann::json& message, std::string* error) const override;
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState& session) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetOpenCodeCliProviderRuntime();
