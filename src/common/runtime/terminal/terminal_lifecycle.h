#pragma once

#include "common/runtime/terminal/terminal_lifecycle_states.h"
#include "common/state/app_state.h"

#include <cstddef>
#include <string_view>

namespace uam
{

inline constexpr double kCliTerminalPromptSettleSeconds = 0.2;
inline constexpr double kCliTerminalSteerRestartTimeoutSeconds = 3.0;
inline constexpr double kCliTerminalSteerFailureTimeoutSeconds = 10.0;
inline constexpr std::string_view kCliTerminalQuitCommand = "/quit\r\n";

enum class CliTerminalSteerRecoveryAction
{
	None,
	Restart,
	ReportTimeout,
};

enum class CliTerminalStopMode
{
	Graceful,
	FastExit,
};

void CloseCliTerminalHandles(CliTerminalState& terminal);
void FailCliTerminalTransport(CliTerminalState& terminal, std::string_view message);
bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, std::size_t len);
std::string BuildCliTerminalPromptInput(std::string_view prompt);
bool RequestCliTerminalSteer(CliTerminalState& terminal, std::string_view prompt, bool retry, std::string* error_out = nullptr);
bool TryDeliverPendingCliTerminalSteer(CliTerminalState& terminal);
CliTerminalSteerRecoveryAction CliTerminalSteerRecovery(const CliTerminalState& terminal, double now_seconds);
const char* CliTerminalLifecycleStateLabel(CliTerminalLifecycleState state);
const char* CliTerminalLifecycleStateLabel(const CliTerminalState& terminal);
bool CliTerminalLifecycleIsProcessing(const CliTerminalState& terminal);
bool CliTerminalLifecycleIsIdleLive(const CliTerminalState& terminal);
bool CliTerminalPromptConfirmsTurnIdle(CliTerminalState& terminal, bool prompt_detected, bool received_output, double now_seconds);
void MarkCliTerminalTurnBusy(CliTerminalState& terminal, bool settle_first_prompt = true);
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
bool IsCliTerminalEligibleForBackgroundIdleShutdown(const AppState& app, const CliTerminalState& terminal, std::string_view selected_chat_id, double now);
void StopCliTerminal(CliTerminalState& terminal, bool clear_identity = false, CliTerminalStopMode stop_mode = CliTerminalStopMode::Graceful);
CliTerminalState* FindCliTerminalForChat(AppState& app, std::string_view chat_id);
bool PrepareCliTerminalForAcpLaunch(AppState& app, std::string_view chat_id, std::string* error_out = nullptr);
void SyncCliTerminalToNativeHistory(AppState& app, const CliTerminalState& terminal);
void StopAndEraseCliTerminalForChat(AppState& app, std::string_view chat_id, bool sync_to_history = true);
void ClearStoppedCliTerminalAttachmentForChat(AppState& app, std::string_view chat_id);
void StopAllCliTerminals(AppState& app, bool clear_identity = true);
void FastStopCliTerminalsForExit(AppState& app);

} // namespace uam
