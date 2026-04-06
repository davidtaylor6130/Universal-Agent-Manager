#ifndef UAM_COMMON_UI_CHAT_DETAIL_CHAT_DETAIL_TERMINAL_SURFACE_H
#define UAM_COMMON_UI_CHAT_DETAIL_CHAT_DETAIL_TERMINAL_SURFACE_H

#include "common/runtime/terminal_common.h"
#include "common/runtime/terminal_polling.h"
#include "common/ui/theme/theme_runtime_state.h"

/// <summary>
/// Converts libvterm cell colors into ImGui draw-list colors.
/// </summary>
inline ImU32 VTermColorToImU32(const VTermScreen* screen, VTermColor color, const bool background)
{
	if ((background && VTERM_COLOR_IS_DEFAULT_BG(&color)) || (!background && VTERM_COLOR_IS_DEFAULT_FG(&color)))
	{
		const ImVec4 fallback = background ? ui::kInputSurface : ui::kTextPrimary;
		return ImGui::GetColorU32(fallback);
	}

	vterm_screen_convert_color_to_rgb(screen, &color);
	return IM_COL32(color.rgb.red, color.rgb.green, color.rgb.blue, 255);
}

/// <summary>
/// Draws the embedded CLI terminal viewport for the selected chat.
/// </summary>
inline void DrawCliTerminalSurface(AppState& app, ChatSession& chat, const bool show_footer = false)
{
	CliTerminalState& terminal = EnsureCliTerminalForChat(app, chat);

	if (!terminal.running && terminal.should_launch)
	{
		const float mono_h = (g_font_mono != nullptr ? g_font_mono->FontSize : ImGui::GetTextLineHeight());
		const float mono_w = std::max(7.0f, ImGui::CalcTextSize("W").x);
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const int rows = std::max(8, static_cast<int>(avail.y / mono_h) - 1);
		const int cols = std::max(20, static_cast<int>(avail.x / mono_w) - 1);

		if (!StartCliTerminalForChat(app, terminal, chat, rows, cols))
		{
			ImGui::TextColored(ui::kError, "Failed to start provider terminal.");

			if (!terminal.last_error.empty())
			{
				ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
			}

			return;
		}
	}

	if (!terminal.running)
	{
		ImGui::TextColored(ui::kWarning, "Provider terminal is stopped.");

		if (DrawButton("Restart Terminal", ImVec2(130.0f, 34.0f), ButtonKind::Primary))
		{
			terminal.should_launch = true;
		}

		if (!terminal.last_error.empty())
		{
			ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
		}

		return;
	}

	PushFontIfAvailable(g_font_mono);
	const float cell_h = (g_font_mono != nullptr) ? g_font_mono->FontSize + 1.0f : ImGui::GetTextLineHeight();
	const float cell_w = std::max(7.0f, ImGui::CalcTextSize("W").x);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const int rows = std::max(8, static_cast<int>(avail.y / cell_h));
	const int cols = std::max(20, static_cast<int>(avail.x / cell_w));
	ResizeCliTerminal(terminal, rows, cols);

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 terminal_size(cell_w * terminal.cols, cell_h * terminal.rows);
	ImGui::InvisibleButton("embedded_cli_surface", terminal_size, ImGuiButtonFlags_None);
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive() || hovered;
	const int max_scrollback_offset = static_cast<int>(terminal.scrollback_lines.size());
	terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset, 0, max_scrollback_offset);

	if (hovered)
	{
		const float wheel = ImGui::GetIO().MouseWheel;

		if (wheel != 0.0f)
		{
			const int lines = std::max(1, static_cast<int>(std::round(std::abs(wheel) * 3.0f)));
			const int delta = (wheel > 0.0f) ? lines : -lines;
			terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset + delta, 0, max_scrollback_offset);
		}

		const ImVec2 mouse_pos = ImGui::GetMousePos();
		const int mouse_col = static_cast<int>((mouse_pos.x - origin.x) / cell_w);
		const int mouse_row = static_cast<int>((mouse_pos.y - origin.y) / cell_h);
		const int scrollback_count = static_cast<int>(terminal.scrollback_lines.size());
		const int absolute_mouse_row = scrollback_count - terminal.scrollback_view_offset + mouse_row;

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			if (mouse_col >= 0 && mouse_col < terminal.cols && mouse_row >= 0 && mouse_row < terminal.rows && absolute_mouse_row >= 0)
			{
				terminal.sel_start_row = absolute_mouse_row;
				terminal.sel_start_col = std::clamp(mouse_col, 0, terminal.cols - 1);
				terminal.sel_end_row = absolute_mouse_row;
				terminal.sel_end_col = std::clamp(mouse_col, 0, terminal.cols - 1);
				terminal.has_selection = false;
			}
		}

		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			if (mouse_col >= 0 && mouse_col < terminal.cols && mouse_row >= 0 && mouse_row < terminal.rows && absolute_mouse_row >= 0)
			{
				terminal.sel_end_row = absolute_mouse_row;
				terminal.sel_end_col = std::clamp(mouse_col, 0, terminal.cols - 1);
				terminal.has_selection = true;
			}
		}
	}

	if (focused)
	{
		bool consumed_navigation = false;

		if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false))
		{
			terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset + std::max(3, terminal.rows / 2), 0, max_scrollback_offset);
			consumed_navigation = true;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
		{
			terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset - std::max(3, terminal.rows / 2), 0, max_scrollback_offset);
			consumed_navigation = true;
		}

		if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Home, false))
		{
			terminal.scrollback_view_offset = max_scrollback_offset;
			consumed_navigation = true;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_End, false))
		{
			terminal.scrollback_view_offset = 0;
			consumed_navigation = true;
		}

		if (!consumed_navigation && terminal.scrollback_view_offset == 0)
		{
			FeedCliTerminalKeyboard(terminal);
		}
	}

	ImDrawList* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(ImVec2(origin.x + 1.0f, origin.y + 3.0f), ImVec2(origin.x + terminal_size.x + 1.0f, origin.y + terminal_size.y + 3.0f), ImGui::GetColorU32(ui::kShadow), 10.0f);
	draw->AddRectFilled(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kInputSurface), 10.0f);
	draw->AddRect(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kBorder), 10.0f);

	if (terminal.screen != nullptr)
	{
		const int scrollback_count = static_cast<int>(terminal.scrollback_lines.size());
		const int start_virtual_row = std::max(0, scrollback_count - terminal.scrollback_view_offset);

		for (int row = 0; row < terminal.rows; ++row)
		{
			const int virtual_row = start_virtual_row + row;
			const int absolute_row = scrollback_count - terminal.scrollback_view_offset + row;

			for (int col = 0; col < terminal.cols; ++col)
			{
				VTermScreenCell cell = BlankTerminalCell();

				if (virtual_row < scrollback_count)
				{
					const std::vector<VTermScreenCell>& line = terminal.scrollback_lines[static_cast<std::size_t>(virtual_row)].cells;

					if (col < static_cast<int>(line.size()))
					{
						cell = line[static_cast<std::size_t>(col)];
					}
				}
				else
				{
					const int screen_row = virtual_row - scrollback_count;

					if (screen_row >= 0 && screen_row < terminal.rows)
					{
						VTermPos pos{screen_row, col};
						vterm_screen_get_cell(terminal.screen, pos, &cell);
					}
				}

				const ImVec2 cell_min(origin.x + col * cell_w, origin.y + row * cell_h);
				const ImVec2 cell_max(cell_min.x + cell_w * std::max<int>(1, cell.width), cell_min.y + cell_h);

				ImU32 bg = VTermColorToImU32(terminal.screen, cell.bg, true);

				if (terminal.has_selection)
				{
					const int min_sel_row = std::min(terminal.sel_start_row, terminal.sel_end_row);
					const int max_sel_row = std::max(terminal.sel_start_row, terminal.sel_end_row);
					const int min_sel_col = std::min(terminal.sel_start_col, terminal.sel_end_col);
					const int max_sel_col = std::max(terminal.sel_start_col, terminal.sel_end_col);

					if (absolute_row >= min_sel_row && absolute_row <= max_sel_row)
					{
						const bool in_range = (absolute_row > min_sel_row && absolute_row < max_sel_row) || (absolute_row == min_sel_row && absolute_row == max_sel_row && col >= min_sel_col && col <= max_sel_col) || (absolute_row == min_sel_row && absolute_row != max_sel_row && col >= min_sel_col) || (absolute_row == max_sel_row && absolute_row != min_sel_row && col <= max_sel_col);

						if (in_range)
						{
							bg = ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 0.4f));
						}
					}
				}

				draw->AddRectFilled(cell_min, cell_max, bg);

				if (cell.chars[0] == 0 || cell.width == 0)
				{
					continue;
				}

				std::string glyph;

				for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i)
				{
					glyph += CodepointToUtf8(cell.chars[i]);
				}

				const ImU32 fg = VTermColorToImU32(terminal.screen, cell.fg, false);
				draw->AddText(ImVec2(cell_min.x, cell_min.y), fg, glyph.c_str());
			}
		}

		if (terminal.state != nullptr && terminal.scrollback_view_offset == 0)
		{
			VTermPos cursor{0, 0};
			vterm_state_get_cursorpos(terminal.state, &cursor);

			if (cursor.row >= 0 && cursor.row < terminal.rows && cursor.col >= 0 && cursor.col < terminal.cols)
			{
				const double current_time = ImGui::GetTime();
				if (current_time - terminal.cursor_blink_last_time > 0.53)
				{
					terminal.cursor_blink_last_time = current_time;
					terminal.cursor_visible = !terminal.cursor_visible;
				}

				if (terminal.cursor_visible)
				{
					const ImVec2 cursor_min(origin.x + cursor.col * cell_w, origin.y + cursor.row * cell_h);
					const ImVec2 cursor_max(cursor_min.x + cell_w, cursor_min.y + cell_h);
					const float bar_width = 2.0f;
					const ImVec2 bar_max(cursor_min.x + bar_width, cursor_max.y);
					draw->AddRectFilled(cursor_min, bar_max, ImGui::GetColorU32(ui::kAccent));
				}
			}
		}
	}

	PopFontIfAvailable(g_font_mono);

	if (show_footer)
	{
		ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));

		if (terminal.scrollback_view_offset > 0)
		{
			ImGui::TextColored(ui::kTextMuted, "Scrollback: %d lines up (End to jump bottom)", terminal.scrollback_view_offset);
		}
		else
		{
			ImGui::TextColored(ui::kTextMuted, "Native provider terminal for this chat.");
		}
	}
}

#endif // UAM_COMMON_UI_CHAT_DETAIL_CHAT_DETAIL_TERMINAL_SURFACE_H
