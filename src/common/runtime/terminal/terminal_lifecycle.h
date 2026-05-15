#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle_states.h"
#include "common/state/app_state.h"

namespace uam
{

inline constexpr double kCliTerminalDefaultIdleShutdownTimeoutSeconds = 60.0;
inline constexpr std::string_view kCliTerminalQuitCommand = "/quit\r\n";

inline void CloseCliTerminalHandles(CliTerminalState& terminal)
{
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
}

inline bool WriteToCliTerminal(CliTerminalState& terminal, const char* bytes, std::size_t len)
{
	const bool wrote = PlatformServicesFactory::Instance().terminal_runtime.WriteToCliTerminal(terminal, bytes, len);
	if (wrote && bytes != nullptr && len > 0)
	{
		const double now = GetAppTimeSeconds();
		terminal.last_activity_time_s = now;
		terminal.last_user_input_time_s = now;
	}
	return wrote;
}

inline const char* CliTerminalLifecycleStateLabel(CliTerminalLifecycleState state)
{
	switch (state)
	{
	case CliTerminalLifecycleState::Disabled:     return "disabled";
	case CliTerminalLifecycleState::Stopped:      return "stopped";
	case CliTerminalLifecycleState::Idle:         return "idle";
	case CliTerminalLifecycleState::Busy:         return "busy";
	case CliTerminalLifecycleState::ShuttingDown: return "shuttingDown";
	}

	return "stopped";
}

inline const char* CliTerminalLifecycleStateLabel(const CliTerminalState& terminal)
{
	return CliTerminalLifecycleStateLabel(terminal.lifecycle_state);
}

inline bool CliTerminalLifecycleIsProcessing(const CliTerminalState& terminal)
{
	return terminal.running && CliTerminalLifecycleStateIsProcessing(terminal.lifecycle_state);
}

inline bool CliTerminalLifecycleIsIdleLive(const CliTerminalState& terminal)
{
	return terminal.running && terminal.lifecycle_state == CliTerminalLifecycleState::Idle;
}

inline void MarkCliTerminalTurnBusy(CliTerminalState& terminal)
{
	const double now = GetAppTimeSeconds();
	terminal.lifecycle_state = CliTerminalLifecycleState::Busy;
	terminal.turn_state = CliTerminalTurnState::Busy;
	terminal.generation_in_progress = true;
	terminal.last_busy_time_s = now;
	terminal.shutdown_requested_time_s = 0.0;
}

inline void MarkCliTerminalTurnIdle(CliTerminalState& terminal)
{
	const double now = GetAppTimeSeconds();
	terminal.lifecycle_state = terminal.running ? CliTerminalLifecycleState::Idle : CliTerminalLifecycleState::Stopped;
	terminal.turn_state = CliTerminalTurnState::Idle;
	terminal.generation_in_progress = false;
	terminal.last_idle_confirmed_time_s = now;
	terminal.shutdown_requested_time_s = 0.0;
}

inline bool IsCliTerminalTurnBusy(const CliTerminalState& terminal)
{
	return terminal.lifecycle_state == CliTerminalLifecycleState::Busy ||
	       terminal.turn_state == CliTerminalTurnState::Busy;
}

inline void MarkCliTerminalShuttingDown(CliTerminalState& terminal)
{
	const double now = GetAppTimeSeconds();
	terminal.lifecycle_state = CliTerminalLifecycleState::ShuttingDown;
	terminal.turn_state = CliTerminalTurnState::Busy;
	terminal.generation_in_progress = false;
	terminal.shutdown_requested_time_s = now;
}

inline void MarkCliTerminalStopped(CliTerminalState& terminal)
{
	terminal.lifecycle_state = CliTerminalLifecycleState::Stopped;
	terminal.turn_state = CliTerminalTurnState::Idle;
	terminal.generation_in_progress = false;
	terminal.last_idle_confirmed_time_s = 0.0;
	terminal.last_busy_time_s = 0.0;
	terminal.shutdown_requested_time_s = 0.0;
}

inline void MarkCliTerminalDisabled(CliTerminalState& terminal)
{
	MarkCliTerminalStopped(terminal);
	terminal.lifecycle_state = CliTerminalLifecycleState::Disabled;
}

inline void ClearCliReadyForChat(AppState& app, std::string_view chat_id)
{
	const std::string_view target_id = TrimCliTerminalIdentityView(chat_id);
	if (target_id.empty())
	{
		return;
	}

	app.chats_with_unseen_updates.erase(std::string(target_id));
}

inline void RequestCliTerminalQuit(CliTerminalState& terminal)
{
	if (!terminal.running || !platform::CliTerminalHasWritableInput(terminal))
	{
		return;
	}

	(void)WriteToCliTerminal(terminal, kCliTerminalQuitCommand.data(), kCliTerminalQuitCommand.size());
}

inline void BeginCliTerminalIdleShutdown(CliTerminalState& terminal)
{
	RequestCliTerminalQuit(terminal);
	MarkCliTerminalShuttingDown(terminal);
}

inline bool PendingCallMatchesCliTerminalIdentity(const AppState& app, std::string_view identity)
{
	const std::string_view target_id = TrimCliTerminalIdentityView(identity);
	return !target_id.empty() && HasPendingCallForChat(app, target_id);
}

inline bool CliTerminalHasPendingCall(const AppState& app, const CliTerminalState& terminal)
{
	const std::string primary_chat_id = CliTerminalPrimaryChatId(terminal);
	const std::string attached_chat_id = CliTerminalAttachedChatId(terminal);
	const std::string attached_session_id = CliTerminalAttachedSessionId(terminal);

	if (PendingCallMatchesCliTerminalIdentity(app, primary_chat_id))
	{
		return true;
	}

	if (attached_chat_id != primary_chat_id && PendingCallMatchesCliTerminalIdentity(app, attached_chat_id))
	{
		return true;
	}

	return PendingCallMatchesCliTerminalIdentity(app, attached_session_id);
}

inline bool IsCliTerminalEligibleForBackgroundIdleShutdown(const AppState& app,
                                                           const CliTerminalState& terminal,
                                                           std::string_view selected_chat_id,
                                                           double now,
                                                           double idle_timeout_seconds = kCliTerminalDefaultIdleShutdownTimeoutSeconds)
{
	if (!terminal.running || terminal.ui_attached || terminal.lifecycle_state != CliTerminalLifecycleState::Idle)
	{
		return false;
	}

	if (CliTerminalPrimaryChatId(terminal).empty())
	{
		return false;
	}

	if (!selected_chat_id.empty() && CliTerminalMatchesChatId(terminal, selected_chat_id))
	{
		return false;
	}

	if (CliTerminalHasPendingCall(app, terminal))
	{
		return false;
	}

	return terminal.last_idle_confirmed_time_s > 0.0 && (now - terminal.last_idle_confirmed_time_s) >= idle_timeout_seconds;
}

enum class CliTerminalStopMode
{
	Graceful,
	FastExit,
};

inline void StopCliTerminal(CliTerminalState& terminal, bool clear_identity = false, CliTerminalStopMode stop_mode = CliTerminalStopMode::Graceful)
{
	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, stop_mode == CliTerminalStopMode::FastExit);

	CloseCliTerminalHandles(terminal);
	terminal.running = false;
	terminal.input_ready = false;
	terminal.startup_time_s = 0.0;
	MarkCliTerminalStopped(terminal);
	terminal.last_output_time_s = 0.0;
	terminal.recent_output_bytes.clear();
	terminal.last_native_history_snapshot_digest.clear();

	if (clear_identity)
	{
		terminal.attached_chat_id.clear();
		terminal.attached_session_id.clear();
		terminal.frontend_chat_id.clear();
		terminal.terminal_id.clear();
		terminal.session_ids_before.clear();
		terminal.linked_files_snapshot.clear();
		terminal.should_launch = false;
		terminal.ui_attached = false;
	}
}

inline CliTerminalState* FindCliTerminalForChat(AppState& app, std::string_view chat_id)
{
	const auto found = std::ranges::find_if(app.cli_terminals, [&chat_id](const std::unique_ptr<CliTerminalState>& terminal) {
		return terminal != nullptr && CliTerminalMatchesChatId(*terminal, chat_id);
	});
	return found == app.cli_terminals.end() ? nullptr : found->get();
}

inline void SyncCliTerminalToNativeHistory(AppState& app, const CliTerminalState& terminal)
{
	const std::string sync_target_id = CliTerminalSyncTargetId(terminal);
	if (!sync_target_id.empty())
	{
		SyncChatsFromNative(app, sync_target_id, true);
	}
}

inline void StopAndEraseCliTerminalForChat(AppState& app, std::string_view chat_id, bool sync_to_history = true)
{
	auto matches_chat_terminal = [&](std::unique_ptr<CliTerminalState>& terminal)
	{
		if (terminal == nullptr || !CliTerminalMatchesChatId(*terminal, chat_id))
		{
			return false;
		}

		if (sync_to_history)
		{
			SyncCliTerminalToNativeHistory(app, *terminal);
		}

		StopCliTerminal(*terminal, true, CliTerminalStopMode::FastExit);
		return true;
	};

	std::erase_if(app.cli_terminals, matches_chat_terminal);
}

inline void StopAllCliTerminals(AppState& app, bool clear_identity = true)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr)
		{
			SyncCliTerminalToNativeHistory(app, *terminal);
			StopCliTerminal(*terminal, clear_identity);
		}
	}
}

inline void FastStopCliTerminalsForExit(AppState& app)
{
	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		SyncCliTerminalToNativeHistory(app, *terminal_ptr);
		StopCliTerminal(*terminal_ptr, true, CliTerminalStopMode::FastExit);
	}
}

} // namespace uam
