#pragma once

#include "common/state/app_state.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uam
{

/// <summary>Discovers and saves a local CLI session link before polling or changing views.</summary>
bool DiscoverCliTerminalNativeSession(AppState& app, CliTerminalState& terminal);
/// <summary>Captures the launch-owned native status reply before allowing queued user input.</summary>
bool PollCliNativeIdentityQuery(AppState& app, CliTerminalState& terminal, std::string_view provider_id, std::string_view output, double now_s);
/// <summary>Unbound concurrent consumers cannot own a workspace-only native history candidate.</summary>
bool HasCompetingUnboundCliSession(const AppState& app, const CliTerminalState& terminal);

bool ChatSyncIdsMatch(std::string_view lhs, std::string_view rhs);
std::string NormalizeChatSyncTargetId(std::string_view chat_id);
bool ChatHasActiveAcpSession(const AppState& app, std::string_view chat_id);
bool CliTerminalHasActiveTurn(const CliTerminalState& terminal);
bool ChatHasBusyCliTerminal(const AppState& app, std::string_view chat_id);
bool ChatHasRunningRuntime(const AppState& app, std::string_view chat_id);
bool ChatHasActiveCliTerminal(const AppState& app, std::string_view chat_id);
bool HasAnyActiveCliTerminal(const AppState& app);
void MarkChatUnseen(AppState& app, std::string_view chat_id);
bool ChatExists(const AppState& app, std::string_view chat_id);
bool NativeChatMatchesPreferredSyncId(const ChatSession& chat, std::string_view preferred_chat_id);
void RemoveMissingChatIds(const AppState& app, std::unordered_set<std::string>& chat_ids);
/// <summary>Restores selection and loads its transcript; returns false if history cannot be loaded.</summary>
bool FinalizeChatSyncSelection(AppState& app, std::string_view selected_before, std::string_view preferred_chat_id, bool preserve_selection = false);
bool SyncChatsFromLoadedNative(AppState& app, std::vector<ChatSession> native_chats, std::string_view preferred_chat_id, bool preserve_selection = false);
bool SyncChatsFromNative(AppState& app, std::string_view preferred_chat_id, bool preserve_selection = false);

} // namespace uam
