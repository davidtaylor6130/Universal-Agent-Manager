#pragma once

#include "common/provider/provider_runtime.h"

class ClaudeCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	const ProviderCliPolicy* CliVersionPolicy() const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;

	const char* AcpProtocolKind() const override { return "claude-code-stream-json"; }
	const char* GetAcpDisplayName() const override { return "Claude stream-json"; }
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const override;
	bool OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const override;
	void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const override;
	nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const override;
	nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState& session, int request_id, std::string& out_method) const override;
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState&) const override { return ProviderAcpSettingChangeAction::RestartSession; }
	ProviderAcpSettingChangeAction AcpModelChangeAction() const override { return ProviderAcpSettingChangeAction::RestartSession; }
	bool OnAcpCanSendPromptWithoutSessionId() const override { return true; }
};

const IProviderRuntime& GetClaudeCliProviderRuntime();
