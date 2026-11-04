#pragma once

#include "common/provider/provider_runtime.h"

std::vector<ChatSession> LoadCopilotSessionStateChats(
    const std::filesystem::path& session_state_root,
    const std::filesystem::path& workspace_filter = {},
    const ProviderRuntimeHistoryLoadOptions& options = {},
    std::string* error_out = nullptr, std::stop_token stop_token = {});
std::optional<ChatSession> LoadCopilotSessionStateChat(
    const std::filesystem::path& session_directory, const std::filesystem::path& workspace_filter = {},
    const ProviderRuntimeHistoryLoadOptions& options = {}, std::string* error_out = nullptr,
    std::stop_token stop_token = {}, std::string_view execution_host_id = "local");

struct CopilotAgentHistoryReference
{
	std::string parent_session_id;
	std::string agent_id;
	std::string execution_host_id;
};
std::string BuildCopilotAgentHistoryId(std::string_view parent_session_id, std::string_view agent_id, std::string_view execution_host_id);
std::optional<CopilotAgentHistoryReference> ParseCopilotAgentHistoryId(std::string_view reference);
std::optional<ChatSession> LoadCopilotAgentSessionStateChat(
	const std::filesystem::path& parent_session_directory, std::string_view agent_id,
	const std::filesystem::path& workspace_filter, std::string_view execution_host_id,
	std::string* error_out = nullptr, std::stop_token stop_token = {});

