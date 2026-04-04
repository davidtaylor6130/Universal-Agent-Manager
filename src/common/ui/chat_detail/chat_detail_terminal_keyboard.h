#pragma once
#include "common/platform/platform_services.h"
#include "common/runtime/terminal/terminal_polling_helpers.h"

#include <algorithm>
#include <string>
#include <vector>

/// <summary>
/// Converts active ImGui modifier keys into libvterm modifiers.
/// </summary>
inline VTermModifier ActiveVTermModifiers()
{
	VTermModifier mod = VTERM_MOD_NONE;
	ImGuiIO& io = ImGui::GetIO();

	if (io.KeyCtrl)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
	}

	if (io.KeyShift)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
	}

	if (io.KeyAlt)
	{
		mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
	}

	return mod;
}

/// <summary>
/// Feeds keyboard input from Dear ImGui into the embedded terminal session.
/// </summary>
inline void FeedCliTerminalKeyboard(CliTerminalState& terminal)
{
	if (terminal.vt == nullptr)
	{
		return;
	}

	if (!uam::platform::CliTerminalHasWritableInput(terminal))
	{
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	const VTermModifier mod = ActiveVTermModifiers();

	auto send_key_repeatable = [&](const ImGuiKey imgui_key, const VTermKey vterm_key)
	{
		if (ImGui::IsKeyPressed(imgui_key, true))
		{
			vterm_keyboard_key(terminal.vt, vterm_key, mod);
		}
	};

	auto send_key_once = [&](const ImGuiKey imgui_key, const VTermKey vterm_key)
	{
		if (ImGui::IsKeyPressed(imgui_key, false))
		{
			vterm_keyboard_key(terminal.vt, vterm_key, mod);
		}
	};

	send_key_once(ImGuiKey_Enter, VTERM_KEY_ENTER);
	send_key_once(ImGuiKey_KeypadEnter, VTERM_KEY_ENTER);
	send_key_once(ImGuiKey_Escape, VTERM_KEY_ESCAPE);
	send_key_once(ImGuiKey_F1, static_cast<VTermKey>(VTERM_KEY_FUNCTION(1)));
	send_key_once(ImGuiKey_F2, static_cast<VTermKey>(VTERM_KEY_FUNCTION(2)));
	send_key_once(ImGuiKey_F3, static_cast<VTermKey>(VTERM_KEY_FUNCTION(3)));
	send_key_once(ImGuiKey_F4, static_cast<VTermKey>(VTERM_KEY_FUNCTION(4)));
	send_key_once(ImGuiKey_F5, static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)));
	send_key_once(ImGuiKey_F6, static_cast<VTermKey>(VTERM_KEY_FUNCTION(6)));
	send_key_once(ImGuiKey_F7, static_cast<VTermKey>(VTERM_KEY_FUNCTION(7)));
	send_key_once(ImGuiKey_F8, static_cast<VTermKey>(VTERM_KEY_FUNCTION(8)));
	send_key_once(ImGuiKey_F9, static_cast<VTermKey>(VTERM_KEY_FUNCTION(9)));
	send_key_once(ImGuiKey_F10, static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)));
	send_key_once(ImGuiKey_F11, static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)));
	send_key_once(ImGuiKey_F12, static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)));

	send_key_repeatable(ImGuiKey_Tab, VTERM_KEY_TAB);
	send_key_repeatable(ImGuiKey_Backspace, VTERM_KEY_BACKSPACE);
	send_key_repeatable(ImGuiKey_UpArrow, VTERM_KEY_UP);
	send_key_repeatable(ImGuiKey_DownArrow, VTERM_KEY_DOWN);
	send_key_repeatable(ImGuiKey_LeftArrow, VTERM_KEY_LEFT);
	send_key_repeatable(ImGuiKey_RightArrow, VTERM_KEY_RIGHT);
	send_key_repeatable(ImGuiKey_Home, VTERM_KEY_HOME);
	send_key_repeatable(ImGuiKey_End, VTERM_KEY_END);
	send_key_repeatable(ImGuiKey_PageUp, VTERM_KEY_PAGEUP);
	send_key_repeatable(ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN);
	send_key_repeatable(ImGuiKey_Delete, VTERM_KEY_DEL);
	send_key_repeatable(ImGuiKey_Insert, VTERM_KEY_INS);

	auto send_char = [&](const ImGuiKey imgui_key, const uint32_t ch)
	{
		if (ImGui::IsKeyPressed(imgui_key, false))
		{
			vterm_keyboard_unichar(terminal.vt, ch, mod);
		}
	};

	if ((mod & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) != 0)
	{
		if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false) && terminal.has_selection)
		{
			std::string selected_text;
			const int min_row = std::min(terminal.sel_start_row, terminal.sel_end_row);
			const int max_row = std::max(terminal.sel_start_row, terminal.sel_end_row);

			for (int row = min_row; row <= max_row; ++row)
			{
				const int start_col = (row == min_row) ? std::min(terminal.sel_start_col, terminal.sel_end_col) : 0;
				const int end_col = (row == max_row) ? std::max(terminal.sel_start_col, terminal.sel_end_col) : terminal.cols - 1;

				for (int col = start_col; col <= end_col; ++col)
				{
					VTermScreenCell cell = BlankTerminalCell();
					const int scrollback_count = static_cast<int>(terminal.scrollback_lines.size());
					const int virtual_row = row - scrollback_count;

					if (virtual_row < 0 && row < scrollback_count)
					{
						const std::vector<VTermScreenCell>& line = terminal.scrollback_lines[static_cast<std::size_t>(row)].cells;
						if (col < static_cast<int>(line.size()))
						{
							cell = line[static_cast<std::size_t>(col)];
						}
					}
					else if (virtual_row >= 0 && terminal.screen != nullptr)
					{
						VTermPos pos{virtual_row, col};
						if (virtual_row >= 0 && virtual_row < terminal.rows && col >= 0 && col < terminal.cols)
						{
							vterm_screen_get_cell(terminal.screen, pos, &cell);
						}
					}

					for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i)
					{
						selected_text += CodepointToUtf8(cell.chars[i]);
					}
				}

				if (row < max_row)
				{
					selected_text += "\n";
				}
			}

			if (!selected_text.empty())
			{
				ImGui::SetClipboardText(selected_text.c_str());
			}

			terminal.has_selection = false;
			terminal.sel_start_row = -1;
			terminal.sel_start_col = -1;
			terminal.sel_end_row = -1;
			terminal.sel_end_col = -1;
		}
		else
		{
			send_char(ImGuiKey_A, 'a');
			send_char(ImGuiKey_B, 'b');
			send_char(ImGuiKey_C, 'c');
			send_char(ImGuiKey_D, 'd');
			send_char(ImGuiKey_E, 'e');
			send_char(ImGuiKey_F, 'f');
			send_char(ImGuiKey_G, 'g');
			send_char(ImGuiKey_H, 'h');
			send_char(ImGuiKey_I, 'i');
			send_char(ImGuiKey_J, 'j');
			send_char(ImGuiKey_K, 'k');
			send_char(ImGuiKey_L, 'l');
			send_char(ImGuiKey_M, 'm');
			send_char(ImGuiKey_N, 'n');
			send_char(ImGuiKey_O, 'o');
			send_char(ImGuiKey_P, 'p');
			send_char(ImGuiKey_Q, 'q');
			send_char(ImGuiKey_R, 'r');
			send_char(ImGuiKey_S, 's');
			send_char(ImGuiKey_T, 't');
			send_char(ImGuiKey_U, 'u');
			send_char(ImGuiKey_V, 'v');
			send_char(ImGuiKey_W, 'w');
			send_char(ImGuiKey_X, 'x');
			send_char(ImGuiKey_Y, 'y');
			send_char(ImGuiKey_Z, 'z');
			send_char(ImGuiKey_0, '0');
			send_char(ImGuiKey_1, '1');
			send_char(ImGuiKey_2, '2');
			send_char(ImGuiKey_3, '3');
			send_char(ImGuiKey_4, '4');
			send_char(ImGuiKey_5, '5');
			send_char(ImGuiKey_6, '6');
			send_char(ImGuiKey_7, '7');
			send_char(ImGuiKey_8, '8');
			send_char(ImGuiKey_9, '9');
			send_char(ImGuiKey_Space, ' ');
			send_char(ImGuiKey_Minus, '-');
			send_char(ImGuiKey_Equal, '=');
			send_char(ImGuiKey_LeftBracket, '[');
			send_char(ImGuiKey_RightBracket, ']');
			send_char(ImGuiKey_Backslash, '\\');
			send_char(ImGuiKey_Semicolon, ';');
			send_char(ImGuiKey_Apostrophe, '\'');
			send_char(ImGuiKey_Comma, ',');
			send_char(ImGuiKey_Period, '.');
			send_char(ImGuiKey_Slash, '/');
			send_char(ImGuiKey_GraveAccent, '`');
		}
	}

	for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
	{
		const ImWchar ch = io.InputQueueCharacters[i];

		if (ch >= 32 && ch != 127)
		{
			vterm_keyboard_unichar(terminal.vt, static_cast<uint32_t>(ch), VTERM_MOD_NONE);
		}
	}
}
