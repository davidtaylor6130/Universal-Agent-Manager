#pragma once

#include <algorithm>
#include <string>

#include <imgui.h>

#include "app/chat_domain_service.h"
#include "common/platform/platform_services.h"
#include "common/platform/sdl_includes.h"
#include "common/state/app_state.h"

inline void FreeCliTerminalVTerm(uam::CliTerminalState& terminal)
{
	if (terminal.vt != nullptr)
	{
		vterm_free(terminal.vt);
		terminal.vt = nullptr;
		terminal.screen = nullptr;
		terminal.state = nullptr;
	}
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
		terminal.last_activity_time_s = ImGui::GetTime();
	}

	return wrote;
}

inline void RequestCliTerminalQuit(uam::CliTerminalState& terminal)
{
	if (!terminal.running || !uam::platform::CliTerminalHasWritableInput(terminal))
	{
		return;
	}

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
	terminal.scrollback_lines.clear();
	terminal.scrollback_view_offset = 0;
	terminal.needs_full_refresh = true;
	terminal.input_ready = false;
	terminal.startup_time_s = 0.0;
	terminal.pending_structured_prompts.clear();
	terminal.generation_in_progress = false;
	terminal.last_output_time_s = 0.0;

	if (clear_identity)
	{
		terminal.attached_chat_id.clear();
		terminal.attached_session_id.clear();
		terminal.session_ids_before.clear();
		terminal.linked_files_snapshot.clear();
		terminal.should_launch = false;
	}
}

inline uam::CliTerminalState* FindCliTerminalForChat(uam::AppState& app, const std::string& chat_id)
{
	for (auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->attached_chat_id == chat_id)
		{
			return terminal.get();
		}
	}

	return nullptr;
}

inline bool ForwardEscapeToSelectedCliTerminal(uam::AppState& app, const SDL_Event& event)
{
	if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP)
	{
		return false;
	}

	if (event.key.keysym.sym != SDLK_ESCAPE)
	{
		return false;
	}

	if (event.type == SDL_KEYUP)
	{
		return true;
	}

	if (event.key.repeat != 0)
	{
		return true;
	}

	ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected == nullptr)
	{
		return true;
	}

	uam::CliTerminalState* terminal = FindCliTerminalForChat(app, selected->id);

	if (terminal == nullptr || !terminal->running || terminal->vt == nullptr)
	{
		return true;
	}

	VTermModifier mod = VTERM_MOD_NONE;
	const SDL_Keymod key_mod = static_cast<SDL_Keymod>(event.key.keysym.mod);

	if ((key_mod & KMOD_CTRL) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
	}

	if ((key_mod & KMOD_SHIFT) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
	}

	if ((key_mod & KMOD_ALT) != 0)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
	}

	vterm_keyboard_key(terminal->vt, VTERM_KEY_ESCAPE, mod);
	terminal->needs_full_refresh = true;
	return true;
}

inline void StopAndEraseCliTerminalForChat(uam::AppState& app, const std::string& chat_id)
{
	auto matches_chat_terminal = [&](std::unique_ptr<uam::CliTerminalState>& terminal)
	{
		if (terminal == nullptr || terminal->attached_chat_id != chat_id)
		{
			return false;
		}

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
			StopCliTerminal(*terminal, clear_identity);
		}
	}
}

inline void FastStopCliTerminalsForExit(uam::AppState& app)
{
	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		StopCliTerminal(*terminal_ptr, true, CliTerminalStopMode::FastExit);
	}
}
