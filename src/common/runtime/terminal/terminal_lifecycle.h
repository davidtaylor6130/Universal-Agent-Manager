#pragma once

#include <algorithm>
#include <string>

#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"
#include "common/runtime/app_time.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/state/app_state.h"

// ---------------------------------------------------------------------------
// VTerm is replaced by xterm.js; stub so existing callers compile cleanly.
// ---------------------------------------------------------------------------
inline void FreeCliTerminalVTerm(uam::CliTerminalState& /*terminal*/)
{
	// No-op: terminal rendering is handled by xterm.js in the frontend.
}

inline void CloseCliTerminalHandles(uam::CliTerminalState& terminal)
{
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
}

inline bool WriteToCliTerminal(uam::CliTerminalState& terminal, const char* bytes, const std::size_t len)
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

inline void MarkCliTerminalTurnBusy(uam::CliTerminalState& terminal)
{
	terminal.turn_state = uam::CliTerminalTurnState::Busy;
}

inline void MarkCliTerminalTurnIdle(uam::CliTerminalState& terminal)
{
	terminal.turn_state = uam::CliTerminalTurnState::Idle;
}

inline bool IsCliTerminalTurnBusy(const uam::CliTerminalState& terminal)
{
	return terminal.turn_state == uam::CliTerminalTurnState::Busy;
}

inline void ClearCliReadyForChat(uam::AppState& app, const std::string& chat_id)
{
	if (chat_id.empty())
	{
		return;
	}

	app.chats_with_unseen_updates.erase(chat_id);
}

inline void RequestCliTerminalQuit(uam::CliTerminalState& terminal)
{
	if (!terminal.running || !uam::platform::CliTerminalHasWritableInput(terminal))
		return;

	static constexpr char kQuitCommand[] = "/quit\r\n";
	(void)WriteToCliTerminal(terminal, kQuitCommand, sizeof(kQuitCommand) - 1);
}

enum class CliTerminalStopMode
{
	Graceful,
	FastExit,
};

inline void StopCliTerminal(uam::CliTerminalState& terminal, const bool clear_identity = false, const CliTerminalStopMode stop_mode = CliTerminalStopMode::Graceful)
{
	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, stop_mode == CliTerminalStopMode::FastExit);

	CloseCliTerminalHandles(terminal);
	FreeCliTerminalVTerm(terminal);
	terminal.running = false;
	terminal.input_ready = false;
	terminal.startup_time_s = 0.0;
	terminal.generation_in_progress = false;
	terminal.turn_state = uam::CliTerminalTurnState::Idle;
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

inline uam::CliTerminalState* FindCliTerminalForChat(uam::AppState& app, const std::string& chat_id)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && (terminal->frontend_chat_id == chat_id || terminal->attached_chat_id == chat_id))
			return terminal.get();
	}
	return nullptr;
}

inline void StopAndEraseCliTerminalForChat(uam::AppState& app, const std::string& chat_id, const bool sync_to_history = true)
{
	auto matches_chat_terminal = [&](std::unique_ptr<uam::CliTerminalState>& terminal)
	{
		if (terminal == nullptr || (terminal->frontend_chat_id != chat_id && terminal->attached_chat_id != chat_id))
			return false;

		if (sync_to_history && !terminal->attached_chat_id.empty())
			SyncChatsFromNative(app, terminal->attached_chat_id, true);

		StopCliTerminal(*terminal, true, CliTerminalStopMode::FastExit);
		return true;
	};

	app.cli_terminals.erase(std::remove_if(app.cli_terminals.begin(), app.cli_terminals.end(), matches_chat_terminal), app.cli_terminals.end());
}

inline void StopAllCliTerminals(uam::AppState& app, const bool clear_identity = true)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr)
		{
			if (!terminal->attached_chat_id.empty())
				SyncChatsFromNative(app, terminal->attached_chat_id, true);
			StopCliTerminal(*terminal, clear_identity);
		}
	}
}

inline void FastStopCliTerminalsForExit(uam::AppState& app)
{
	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
			continue;
		if (!terminal_ptr->attached_chat_id.empty())
			SyncChatsFromNative(app, terminal_ptr->attached_chat_id, true);
		StopCliTerminal(*terminal_ptr, true, CliTerminalStopMode::FastExit);
	}
}
