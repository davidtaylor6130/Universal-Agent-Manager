#pragma once

#include "common/provider/provider_runtime.h"
#include "common/provider/gemini_options.h"

class GeminiCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* AcpProtocolKind() const override { return "gemini-acp"; }
	const char* GetAcpDisplayName() const override { return "Gemini ACP"; }
	bool IsGenericAcpSession() const override
	{
		return true;
	}
	const char* RuntimeId() const override;
	std::vector<std::string> ModelReasoningEfforts(std::string_view model, std::string_view version) const override { return uam::gemini::SupportedReasoningEfforts(model, version); }
	bool ModelOptionsRequireRestart() const override { return true; }
	bool OnAcpHandleResult(uam::AppState&, uam::AcpSessionState&, ChatSession&,
	    const std::string&, const std::string&, const nlohmann::json&) const override;
	bool OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const uam::acp_detail::AcpResponseFailureDetails& details) const override;
	bool SupportsSavedImageRetry(const uam::AcpSessionState& session) const override;
	bool SupportsRemoteOwnedImageRetry(const uam::AcpSessionState& session) const override;
	SavedImageRetryPreflight PrepareSavedImageRetry(uam::AcpSessionState&, const ChatSession&, const std::vector<MessageAttachment>&, std::string&) const override;
	nlohmann::json SavedImageRetryMetadata(const uam::AcpSessionState&, const ChatSession&) const override;
	bool EncodePreparedSavedImageRetry(nlohmann::json&, const nlohmann::json&, std::string&, const std::vector<MessageAttachment>& = {}, const nlohmann::json& = nullptr) const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	bool UsesNativeOverlayHistory(const ProviderProfile& profile) const override;
	std::string GenerateSessionUUID() const override;
	std::string BuildSessionFilename(const ChatSession& chat) const override;
	std::string NativeTypeFromRole(MessageRole role) const override;
	std::filesystem::path GetNativeSessionDirectory(const std::filesystem::path& workspace_path) const override;
	bool RebuildNativeSessionFile(const ProviderProfile& profile, const ChatSession& chat, const std::filesystem::path& workspace_path) const override;
	bool SupportsLocalPassiveNativeMedia(const ProviderProfile&) const override { return true; }
	bool SupportsGeminiJsonHistory(const ProviderProfile& profile) const override;
	bool UsesLocalHistory(const ProviderProfile& profile) const override;
	bool UsesCliOutput(const ProviderProfile& profile) const override;
	bool UsesGeminiPathBootstrap(const ProviderProfile& profile) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
	ProviderDiscoveryResult DiscoverChatSources(const ProviderProfile& profile) const override;
};

const IProviderRuntime& GetGeminiCliProviderRuntime();
