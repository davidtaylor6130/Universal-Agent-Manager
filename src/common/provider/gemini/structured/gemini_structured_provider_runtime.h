#pragma once

#include "common/provider/provider_runtime.h"

class GeminiStructuredProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	bool IsEnabled() const override;
	const char* DisabledReason() const override;
	std::string BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files) const override;
	std::string BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const override;
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const override;
	MessageRole RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const override;
	bool SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat) const override;
	bool UsesNativeOverlayHistory(const ProviderProfile& profile) const override;
	bool SupportsGeminiJsonHistory(const ProviderProfile& profile) const override;
	bool UsesLocalHistory(const ProviderProfile& profile) const override;
	bool UsesInternalEngine(const ProviderProfile& profile) const override;
	bool UsesCliOutput(const ProviderProfile& profile) const override;
	bool UsesStructuredOutput(const ProviderProfile& profile) const override;
	bool UsesGeminiPathBootstrap(const ProviderProfile& profile) const override;
};

const IProviderRuntime& GetGeminiStructuredProviderRuntime();