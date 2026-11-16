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
/// Worker-only passive media admission for a local child (no host) or the
/// parent's exact SSH host; no arbitrary caller candidate can produce this
/// sealed genuine-reader result.
std::optional<AdmittedPassiveNativeHistory> LoadPassiveHistoryForMedia(
	const std::filesystem::path& root, const ChatSession& source, const ProviderProfile& profile,
	std::string_view reference, std::string_view expected_primary, std::string_view expected_summary,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token = {});

std::optional<ChatSession> ParseHistorySnapshot(const ExecutionHost& host, const ChatSession& source,
	std::string_view reference, const nlohmann::json& snapshot, std::string* error, std::stop_token stop_token = {});
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
struct NativeChildConversation
{
	std::string reference;
	std::string preview;
	std::string started_at;
	std::string updated_at;
	std::size_t messages = 0;
};
struct NativeChildListing
{
	std::vector<NativeChildConversation> children;
	std::size_t unreadable = 0;
	bool truncated = false;
};
/// Validates one helper listing against its exact provider, parent, workspace and host.
std::optional<NativeChildListing> ParseGeminiChildListing(const ExecutionHost& host, const ChatSession& source, const nlohmann::json& listing, std::string* error);
/// Lists journals stored under this Gemini parent, without any title or time correlation.
std::optional<NativeChildListing> ListGeminiChildren(const ChatSession& source, const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token = {});
#endif

std::optional<ChatSession> LoadHistory(const ChatSession& source, std::string_view reference,
	const std::optional<ExecutionHost>& host, std::string* error, std::stop_token stop_token = {});
}
