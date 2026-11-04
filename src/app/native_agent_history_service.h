#pragma once

#include "common/models/app_models.h"
#include "common/provider/native_agent_history.h"
#include "common/provider/provider_profile.h"
#include "common/chat/native_media_chat_save.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <stop_token>

namespace uam::native_agents
{
/// Worker-only local passive media admission; no remote snapshot or arbitrary
/// caller candidate can produce this sealed genuine-reader result.
std::optional<AdmittedLocalPassiveNativeHistory> LoadLocalPassiveHistoryForMedia(
	const std::filesystem::path& root, const ChatSession& source, const ProviderProfile& profile,
	std::string_view reference, std::string_view expected_primary, std::string_view expected_summary,
	std::string* error, std::stop_token stop_token = {});

std::optional<ChatSession> ParseHistorySnapshot(const ExecutionHost& host, const ChatSession& source,
	std::string_view reference, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token = {});
std::optional<ChatSession> LoadHistory(const ChatSession& source, std::string_view reference,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token = {});
}
