#pragma once

#include "common/models/app_models.h"
#include "common/provider/native_agent_history.h"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace uam::claude
{
std::optional<ChatSession> LoadNativeAgentHistory(const native_agents::HistoryReference& reference, const std::string& workspace, std::string* error = nullptr, std::stop_token stop_token = {});
std::optional<ChatSession> ParseNativeAgentHistory(const native_agents::HistoryReference& reference, std::string_view text, const std::string& workspace, std::string* error = nullptr, std::stop_token stop_token = {});
/// Shared complete native record projection, used by bulk parent imports and exact passive child reads.
std::optional<ChatSession> ParseNativeTranscript(std::string_view text, const std::string& session_id, const std::string& workspace, std::string* error = nullptr, std::stop_token stop_token = {}, const std::string& agent_id = {});
}

namespace uam::gemini
{
std::optional<ChatSession> LoadNativeAgentHistory(const native_agents::HistoryReference& reference, const std::string& workspace, std::string* error = nullptr, std::stop_token stop_token = {});
std::optional<ChatSession> ParseNativeAgentHistory(const native_agents::HistoryReference& reference, std::string_view text, const std::string& workspace, std::string* error = nullptr, std::stop_token stop_token = {});
}
