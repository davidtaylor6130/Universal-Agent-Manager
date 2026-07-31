#pragma once

#include "common/provider/provider_runtime.h"

class CopilotCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const override;
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const override;

	const char* AcpProtocolKind() const override { return "copilot-acp"; }
	const char* GetAcpDisplayName() const override { return "GitHub Copilot ACP"; }
	bool IsGenericAcpSession() const override { return true; }
	std::string OnAcpValidateResumeId(const ChatSession& chat) const override;
	std::string OnAcpMapApprovalModeId(const std::string& mode_id) const override;
};

const IProviderRuntime& GetCopilotCliProviderRuntime();
std::string NormalizeCopilotReasoningEffort(std::string_view value);
