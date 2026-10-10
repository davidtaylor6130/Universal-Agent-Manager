#pragma once

#include "common/provider/provider_runtime.h"

/// <summary>Runs Antigravity's native stream-json chat and interactive terminal; permissions remain provider-owned.</summary>
class AntigravityCliProviderRuntime final : public IProviderRuntime
{
  public:
	const char* RuntimeId() const override;
	bool SupportsTextWorkers() const override { return false; }
	std::vector<std::string> BuildInteractiveArgv(const ProviderProfile&, const ChatSession&, const AppSettings&) const override;
	bool SupportsInteractivePromptTracking() const override { return false; }
	MessageRole RoleFromNativeType(const ProviderProfile&, std::string_view) const override;
	std::vector<ChatSession> LoadHistory(const ProviderProfile&, const std::filesystem::path&, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const override;
	bool SaveHistory(const ProviderProfile&, const std::filesystem::path&, const ChatSession&) const override;
	std::vector<std::string> BuildWorkerArgv(const ProviderProfile&, const AppSettings&, std::string_view, std::string_view) const override { return {}; }
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const override;
	const char* AcpProtocolKind() const override { return "antigravity-stream-json"; }
	nlohmann::json OnAcpBuildInitialize(uam::AcpSessionState&, int) const override;
	nlohmann::json OnAcpBuildSetupRequest(int, const ChatSession&, const std::string&, bool, std::string&) const override;
	nlohmann::json OnAcpBuildPrompt(uam::AcpSessionState&, int, const std::string&, const ChatSession&, std::string&) const override;
	nlohmann::json OnAcpBuildCancel(const uam::AcpSessionState&, int, std::string&) const override;
	bool OnAcpHandleMessage(uam::AppState&, uam::AcpSessionState&, ChatSession&, const nlohmann::json&, const CefRefPtr<CefBrowser>&) const override;
	bool OnAcpCanSendPromptWithoutSessionId() const override { return true; }
	ProviderAcpSettingChangeAction AcpModeChangeAction(const uam::AcpSessionState&) const override { return ProviderAcpSettingChangeAction::RestartSession; }
	ProviderAcpSettingChangeAction AcpModelChangeAction() const override { return ProviderAcpSettingChangeAction::RestartSession; }
};

const IProviderRuntime& GetAntigravityCliProviderRuntime();
