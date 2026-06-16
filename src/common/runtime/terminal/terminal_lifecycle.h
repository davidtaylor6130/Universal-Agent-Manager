#pragma once

#include "common/runtime/terminal/terminal_lifecycle_states.h"
#include "common/state/app_state.h"

#include <cstddef>
#include <string_view>

namespace uam
{

inline constexpr double kCliTerminalDefaultIdleShutdownTimeoutSeconds = 60.0;
inline constexpr std::string_view kCliTerminalQuitCommand = "/quit\r\n";

enum class CliTerminalStopMode
{
	Graceful,
	FastExit,
};

void CloseCliTerminalHandles(CliTerminalState& terminal);
bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, std::size_t len);
const char* CliTerminalLifecycleStateLabel(CliTerminalLifecycleState state);
const char* CliTerminalLifecycleStateLabel(const CliTerminalState& terminal);
bool CliTerminalLifecycleIsProcessing(const CliTerminalState& terminal);
bool CliTerminalLifecycleIsIdleLive(const CliTerminalState& terminal);
void MarkCliTerminalTurnBusy(CliTerminalState& terminal);
void MarkCliTerminalTurnIdle(CliTerminalState& terminal);
bool IsCliTerminalTurnBusy(const CliTerminalState& terminal);
void MarkCliTerminalShuttingDown(CliTerminalState& terminal);
void MarkCliTerminalStopped(CliTerminalState& terminal);
void MarkCliTerminalDisabled(CliTerminalState& terminal);
void ClearCliReadyForChat(AppState& app, std::string_view chat_id);
void RequestCliTerminalQuit(CliTerminalState& terminal);
void BeginCliTerminalIdleShutdown(CliTerminalState& terminal);
bool PendingCallMatchesCliTerminalIdentity(const AppState& app, std::string_view identity);
bool CliTerminalHasPendingCall(const AppState& app, const CliTerminalState& terminal);
bool IsCliTerminalEligibleForBackgroundIdleShutdown(const AppState& app, const CliTerminalState& terminal, std::string_view selected_chat_id, double now, double idle_timeout_seconds = kCliTerminalDefaultIdleShutdownTimeoutSeconds);
void StopCliTerminal(CliTerminalState& terminal, bool clear_identity = false, CliTerminalStopMode stop_mode = CliTerminalStopMode::Graceful);
CliTerminalState* FindCliTerminalForChat(AppState& app, std::string_view chat_id);
void SyncCliTerminalToNativeHistory(AppState& app, const CliTerminalState& terminal);
void StopAndEraseCliTerminalForChat(AppState& app, std::string_view chat_id, bool sync_to_history = true);
void ClearStoppedCliTerminalAttachmentForChat(AppState& app, std::string_view chat_id);
void StopAllCliTerminals(AppState& app, bool clear_identity = true);
void FastStopCliTerminalsForExit(AppState& app);

} // namespace uam
