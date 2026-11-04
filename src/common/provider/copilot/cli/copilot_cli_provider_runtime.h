#pragma once

#include "common/provider/copilot_native_history.h"
#include "common/provider/copilot/cli/copilot_session_paths.h"

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

	const char* AcpProtocolKind() const override { return "copilot-sdk"; }
	bool SupportsSavedImageRetry(const uam::AcpSessionState& session) const override;
	bool SupportsRemoteOwnedImageRetry(const uam::AcpSessionState& session) const override;
	SavedImageRetryPreflight PrepareSavedImageRetry(uam::AcpSessionState& session, const ChatSession& chat,
	    const std::vector<MessageAttachment>& images, std::string& error) const override;
	nlohmann::json SavedImageRetryMetadata(const uam::AcpSessionState& session, const ChatSession& chat) const override;
	bool EncodePreparedSavedImageRetry(nlohmann::json& request, const nlohmann::json& prepared, std::string& error,
	    const std::vector<MessageAttachment>& images = {}, const nlohmann::json& native_metadata = nullptr) const override;
	std::string AcpTurnIdentity(const uam::AcpSessionState& session) const override;
	std::size_t AcpBackgroundTaskCount(const uam::AcpSessionState& session) const override;
	void RestoreAcpTurnIdentity(uam::AcpSessionState& session, const std::string& identity) const override;
	const char* GetAcpDisplayName() const override { return "GitHub Copilot SDK server"; }
	void PrepareStructuredMessage(nlohmann::json& message) const override;
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const override;
	void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const override;
	nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const override;
	AcpRequestPreflight OnAcpPrepareSetup(uam::AppState&, uam::AcpSessionState&,
	    const ChatSession&, const std::string& cwd, std::string& error) const override;
	nlohmann::json OnAcpBuildSessionSetupRequest(uam::AcpSessionState&, int request_id,
	    const ChatSession&, const std::string& cwd, bool can_load, std::string& out_method) const override;
	AcpRequestPreflight OnAcpPrepareInput(uam::AppState&, uam::AcpSessionState&,
	    const ChatSession&, bool final_write, std::string& error) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const override;
	bool OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const override;
	bool OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const std::string& method, const std::string& request_id, const nlohmann::json& result) const override;
	bool SupportsAcpCompaction() const override { return true; }
	nlohmann::json OnAcpBuildCompact(const uam::AcpSessionState& session, int request_id, std::string& method) const override;
	bool SupportsAcpSteering() const override { return true; }
	nlohmann::json OnAcpBuildSteer(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, std::string& method) const override;
	nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState& session, int request_id, std::string& method) const override;
	using IProviderRuntime::OnAcpBuildPermissionResponse;
	nlohmann::json OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
	    const uam::AcpPendingPermissionState& pending, const std::string& option_id, bool cancelled) const override;
	nlohmann::json OnAcpBuildUserInputResponse(const uam::AcpSessionState& session,
	    const std::map<std::string, std::vector<std::string>>& answers) const override;
	bool PrepareAcpUserInputResponse(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const std::map<std::string, std::vector<std::string>>& answers, nlohmann::json& response, std::string* error) const override;
	void NoteAcpUserInputResponse(uam::AcpSessionState& session, const nlohmann::json& response) const override;
	void NoteAcpPermissionResponse(uam::AcpSessionState& session, const nlohmann::json& response) const override;
	bool AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const override;
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState&) const override { return ProviderAcpSettingChangeAction::RestartSession; }
	ProviderAcpSettingChangeAction AcpModelChangeAction() const override { return ProviderAcpSettingChangeAction::RestartSession; }
	bool OnAcpPollNativeModelOptions(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat) const override;
	bool OnAcpRefreshNativeModelOptions(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat, std::string* error_out) const override;
	bool OnAcpRequestNativeModelOption(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat, std::string_view option_id,
	    const nlohmann::json& value, const nlohmann::json& expected_ownership, std::string* error_out) const override;
	nlohmann::json OnAcpNativeModelOptionsForFrontend(const uam::AcpSessionState& session) const override;
	bool OnAcpConfigOptionsUpdated(uam::AcpSessionState& session, const nlohmann::json& config_options) const override;
	bool OnAcpReconcileModelOptions(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetCopilotCliProviderRuntime();

namespace uam::copilot
{
/// Validates an explicit plan-review decision while retaining the prior ceiling until native confirmation is saved.
bool PrepareUserInputResponse(AppState& app, AcpSessionState& session, ChatSession& chat,
	const std::map<std::string, std::vector<std::string>>& answers, nlohmann::json& response, std::string* error);
void NoteUserInputResponse(AcpSessionState& session, const nlohmann::json& response);
void NotePermissionResponse(AcpSessionState& session, const nlohmann::json& response);
}
