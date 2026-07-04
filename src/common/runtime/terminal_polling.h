#pragma once

#include "common/provider/provider_profile.h"
#include "common/state/app_state.h"
#include "cef/cef_push.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uam
{
	inline constexpr double kCliNativeHistoryRefreshIntervalSeconds = 1.25;

	double LatestCliTransportActivityTime(const uam::CliTerminalState& terminal);
	bool ProviderRecentOutputIndicatesInputPrompt(const ProviderProfile& provider, std::string_view recent_output);
	std::string AsyncNativeChatLoadTaskKey(std::string_view provider_id, const std::filesystem::path& chats_dir);
	std::string NativeHistorySnapshotDigest(const std::vector<ChatSession>& chats);
	uam::platform::AsyncNativeChatLoadTask& AsyncNativeChatLoadTaskFor(uam::AppState& app, const std::string& provider_id, const std::filesystem::path& chats_dir);
	bool StartAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir);
	bool TryConsumeAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir, std::vector<ChatSession>& chats_out, std::string& digest_out, std::string& error_out);
	bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app, uam::CliTerminalState& terminal, int previous_message_count, const std::string& selected_chat_id);
	std::unordered_set<std::string> BlockedNativeSessionIdsForTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal);
	bool TryAttachNativeSessionFromHistory(uam::AppState& app, uam::CliTerminalState& terminal, const std::vector<ChatSession>& native_chats);
	bool ShouldAttemptOpenCodeLocalHistoryRebind(const ProviderProfile& terminal_provider, const uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats);
	bool ShouldPollOpenCodeLocalHistoryRebind(const ProviderProfile& terminal_provider, const uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats);
	bool PersistRebindDiscoveredSession(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, ChatSession* previous_chat, std::string_view discovered, std::string_view previous_session_id, std::string_view event_kind);
	bool TryAttachOpenCodeLocalHistorySessionFromChatFile(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, const std::vector<ChatSession>& matching_chats);
	bool LocalHistoryChatMatchesTerminalProvider(const ChatSession& chat, const ProviderProfile& terminal_provider);
	bool TryAttachLocalHistorySessionFromChats(uam::AppState& app, const ProviderProfile& terminal_provider, uam::CliTerminalState& terminal, const std::vector<ChatSession>& local_chats);
	bool UpdateNativeHistorySnapshotDigestIfChanged(uam::CliTerminalState& terminal, std::string_view native_snapshot_digest);
	void StopCliTerminalAfterProviderExit(uam::AppState& app, uam::CliTerminalState& terminal, bool was_shutting_down);
	bool PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, bool preserve_selection);
	bool PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app);

} // namespace uam
