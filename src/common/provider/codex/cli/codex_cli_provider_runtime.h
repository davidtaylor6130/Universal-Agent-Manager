#pragma once

#include "common/provider/provider_runtime.h"

class CodexCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	nlohmann::json ReadLocalModelCatalog() const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	bool RecentOutputIndicatesInputPrompt(std::string_view recent_output) const override;
	std::string ResolveInteractiveResumeId(const uam::AppState& app, const ChatSession& chat) const override;
	std::vector<std::string> SnapshotInteractiveSessionIds() const override;
	std::string DiscoverInteractiveSessionId(const std::vector<std::string>& before, const std::filesystem::path& workspace) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	void ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json& native_item) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	void NormalizeLoadedNativeSessionId(ChatSession& chat) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;

	const char* AcpProtocolKind() const override { return "codex-app-server"; }
	std::string AcpTurnIdentity(const uam::AcpSessionState& session) const override;
	void RestoreAcpTurnIdentity(uam::AcpSessionState& session, const std::string& identity) const override;
	const char* GetAcpDisplayName() const override { return "Codex app-server"; }
	std::string RecoverAcpResponseMethod(const nlohmann::json&) const override { return {}; }
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const override;
	bool OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const override;
	void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const override;
	bool OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const uam::acp_detail::AcpResponseFailureDetails& details) const override;
	bool OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const std::string& method, const std::string& request_id, const nlohmann::json& result) const override;
	nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const override;
	std::string OnAcpValidateResumeId(const ChatSession& chat) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const override;
	bool SupportsAcpSteering() const override { return true; }
	nlohmann::json OnAcpBuildSteer(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, std::string& out_method) const override;
	nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState& session,
	    int request_id, std::string& out_method) const override;
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState&) const override { return ProviderAcpSettingChangeAction::ApplyLocally; }
	ProviderAcpSettingChangeAction AcpModelChangeAction() const override { return ProviderAcpSettingChangeAction::ApplyLocally; }
	nlohmann::json OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
	    const std::string& option_id, bool cancelled) const override;
	bool OnAcpTryAutoApprove(uam::AcpSessionState& session, const ChatSession& chat,
	    std::string* error_out) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetCodexCliProviderRuntime();
