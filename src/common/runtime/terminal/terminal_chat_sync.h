#pragma once

#include "common/state/app_state.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uam
{

bool ChatSyncIdsMatch(std::string_view lhs, std::string_view rhs);
std::string NormalizeChatSyncTargetId(std::string_view chat_id);
auto FindPendingCallForChat(const AppState& app, std::string_view chat_id) -> decltype(app.pending_calls.end());
bool HasPendingCallForChat(const AppState& app, std::string_view chat_id);
bool HasAnyPendingCall(const AppState& app);
const PendingRuntimeCall* FirstPendingCallForChat(const AppState& app, std::string_view chat_id);
bool ChatHasActiveAcpSession(const AppState& app, std::string_view chat_id);
bool CliTerminalHasActiveTurn(const CliTerminalState& terminal);
bool ChatHasBusyCliTerminal(const AppState& app, std::string_view chat_id);
bool ChatHasRunningRuntime(const AppState& app, std::string_view chat_id);
bool ChatHasActiveCliTerminal(const AppState& app, std::string_view chat_id);
bool HasAnyActiveCliTerminal(const AppState& app);
void MarkChatUnseen(AppState& app, std::string_view chat_id);
void MarkSelectedChatSeen(AppState& app);
bool ChatExists(const AppState& app, std::string_view chat_id);
bool NativeChatMatchesPreferredSyncId(const ChatSession& chat, std::string_view preferred_chat_id);
void RemoveMissingChatIds(const AppState& app, std::unordered_set<std::string>& chat_ids);
void FinalizeChatSyncSelection(AppState& app, std::string_view selected_before, std::string_view preferred_chat_id, bool preserve_selection = false);
bool SyncChatsFromLoadedNative(AppState& app, std::vector<ChatSession> native_chats, std::string_view preferred_chat_id, bool preserve_selection = false);
bool SyncChatsFromNative(AppState& app, std::string_view preferred_chat_id, bool preserve_selection = false);

} // namespace uam
