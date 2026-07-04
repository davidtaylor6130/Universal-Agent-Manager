#pragma once

#include "common/provider/provider_runtime.h"

class ClaudeCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;

	const char* AcpProtocolKind() const override { return "claude-code-stream-json"; }
	const char* GetAcpDisplayName() const override { return "Claude stream-json"; }
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const override;
	void OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const override;
	nlohmann::json OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
	    const std::string& cwd, bool can_load, std::string& out_method) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
	    const std::string& prompt, const ChatSession& chat, std::string& out_method) const override;
	bool OnAcpSetModeLocally(uam::AcpSessionState& session, const std::string& mode_id) const override;
	bool OnAcpSetModelLocally(uam::AcpSessionState& session, const std::string& model_id) const override;
	bool OnAcpCanSendPromptWithoutSessionId() const override { return true; }
};

const IProviderRuntime& GetClaudeCliProviderRuntime();
