#pragma once

#include "common/models/app_models.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace uam::computer_use
{
	inline constexpr const char* kMcpServerName = "uam-computer";
	inline constexpr const char* kMcpServerFlag = "--uam-computer-use-mcp";
	inline constexpr const char* kBackendAuto = "auto";
	inline constexpr const char* kBackendProvider = "provider";
	inline constexpr const char* kBackendUam = "uam";

	std::string BackendPreference(std::string_view value);
	bool ProviderBackendAvailable(std::string_view provider_id);
	bool AvailableForChat(const ChatSession& chat);
	std::string EffectiveBackend(const ChatSession& chat);
	bool UsesUamBackend(const ChatSession& chat);
	bool IsPortableMcpChatId(std::string_view value);

	std::vector<std::string> McpServerArguments(const ChatSession& chat);
	nlohmann::json AcpMcpServers(const ChatSession& chat);
	std::string ClaudeMcpConfig(const ChatSession& chat);
	void AppendCodexMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat);
	void AppendClaudeMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat);
	void AppendGeminiMcpLaunchArguments(std::vector<std::string>& argv, const ChatSession& chat);
} // namespace uam::computer_use
