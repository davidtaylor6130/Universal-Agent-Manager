#pragma once
#include "common/platform/platform_services.h"

/// <summary>
/// Converts active ImGui modifier keys into libvterm modifiers.
/// </summary>
static VTermModifier ActiveVTermModifiers()
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
static void FeedCliTerminalKeyboard(CliTerminalState& terminal)
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

	auto send_key = [&](const ImGuiKey imgui_key, const VTermKey vterm_key)
	{
		if (ImGui::IsKeyPressed(imgui_key, false))
		{
			vterm_keyboard_key(terminal.vt, vterm_key, mod);
		}
	};

	send_key(ImGuiKey_Enter, VTERM_KEY_ENTER);
	send_key(ImGuiKey_KeypadEnter, VTERM_KEY_ENTER);
	send_key(ImGuiKey_Tab, VTERM_KEY_TAB);
	send_key(ImGuiKey_Backspace, VTERM_KEY_BACKSPACE);
	send_key(ImGuiKey_Escape, VTERM_KEY_ESCAPE);
	send_key(ImGuiKey_UpArrow, VTERM_KEY_UP);
	send_key(ImGuiKey_DownArrow, VTERM_KEY_DOWN);
	send_key(ImGuiKey_LeftArrow, VTERM_KEY_LEFT);
	send_key(ImGuiKey_RightArrow, VTERM_KEY_RIGHT);
	send_key(ImGuiKey_Home, VTERM_KEY_HOME);
	send_key(ImGuiKey_End, VTERM_KEY_END);
	send_key(ImGuiKey_PageUp, VTERM_KEY_PAGEUP);
	send_key(ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN);
	send_key(ImGuiKey_Delete, VTERM_KEY_DEL);
	send_key(ImGuiKey_Insert, VTERM_KEY_INS);
	send_key(ImGuiKey_F1, static_cast<VTermKey>(VTERM_KEY_FUNCTION(1)));
	send_key(ImGuiKey_F2, static_cast<VTermKey>(VTERM_KEY_FUNCTION(2)));
	send_key(ImGuiKey_F3, static_cast<VTermKey>(VTERM_KEY_FUNCTION(3)));
	send_key(ImGuiKey_F4, static_cast<VTermKey>(VTERM_KEY_FUNCTION(4)));
	send_key(ImGuiKey_F5, static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)));
	send_key(ImGuiKey_F6, static_cast<VTermKey>(VTERM_KEY_FUNCTION(6)));
	send_key(ImGuiKey_F7, static_cast<VTermKey>(VTERM_KEY_FUNCTION(7)));
	send_key(ImGuiKey_F8, static_cast<VTermKey>(VTERM_KEY_FUNCTION(8)));
	send_key(ImGuiKey_F9, static_cast<VTermKey>(VTERM_KEY_FUNCTION(9)));
	send_key(ImGuiKey_F10, static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)));
	send_key(ImGuiKey_F11, static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)));
	send_key(ImGuiKey_F12, static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)));

	auto send_char = [&](const ImGuiKey imgui_key, const uint32_t ch)
	{
		if (ImGui::IsKeyPressed(imgui_key, false))
		{
			vterm_keyboard_unichar(terminal.vt, ch, mod);
		}
	};

	if ((mod & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) != 0)
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

	for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
	{
		const ImWchar ch = io.InputQueueCharacters[i];

		if (ch >= 32 && ch != 127)
		{
			vterm_keyboard_unichar(terminal.vt, static_cast<uint32_t>(ch), VTERM_MOD_NONE);
		}
	}
}
