#pragma once

#include "common/models/app_models.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>

namespace uam::copilot
{
std::optional<ChatSession> ParseAgentHistorySnapshot(const ExecutionHost& host, const ChatSession& source,
	std::string_view reference, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token = {});
std::optional<ChatSession> LoadAgentHistory(const ChatSession& source, std::string_view reference,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token = {});
}
