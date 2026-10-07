#pragma once

#include "common/provider/provider_runtime.h"

/// <summary>Runs Antigravity's interactive terminal without a structured control adapter.</summary>
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
	std::vector<std::string> BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const override { return {}; }
};

const IProviderRuntime& GetAntigravityCliProviderRuntime();
