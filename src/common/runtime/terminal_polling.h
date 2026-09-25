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
	std::string AsyncNativeChatLoadTaskKey(std::string_view provider_id, const std::filesystem::path& chats_dir);
	std::string NativeHistorySnapshotDigest(const std::vector<ChatSession>& chats);
	uam::platform::AsyncNativeChatLoadTask& AsyncNativeChatLoadTaskFor(uam::AppState& app, const std::string& provider_id, const std::filesystem::path& chats_dir);
	bool StartAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir);
	bool TryConsumeAsyncNativeChatLoadForTerminal(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir, std::vector<ChatSession>& chats_out, std::string& digest_out, std::string& error_out);
	bool TryMarkCliTurnCompleteFromSyncedHistory(uam::AppState& app, uam::CliTerminalState& terminal, int previous_message_count, const std::string& selected_chat_id);
	std::unordered_set<std::string> BlockedNativeSessionIdsForTerminal(const uam::AppState& app, const uam::CliTerminalState& terminal);
	bool TryAttachNativeSessionFromHistory(uam::AppState& app, uam::CliTerminalState& terminal, const std::vector<ChatSession>& native_chats);
void StopCliTerminalAfterProviderExit(uam::AppState& app, uam::CliTerminalState& terminal, bool was_shutting_down);
bool HandleCliTerminalInactivityTimeout(uam::AppState& app, uam::CliTerminalState& terminal, double now_seconds);
	bool PollCliTerminal(CefRefPtr<CefBrowser> browser, uam::AppState& app, uam::CliTerminalState& terminal, bool preserve_selection);
	bool PollAllCliTerminals(CefRefPtr<CefBrowser> browser, uam::AppState& app, bool terminal_ui_visible = true);

} // namespace uam
