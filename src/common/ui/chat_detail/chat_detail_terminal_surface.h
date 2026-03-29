#pragma once

/// <summary>
/// Converts libvterm cell colors into ImGui draw-list colors.
/// </summary>
static ImU32 VTermColorToImU32(const VTermScreen* screen, VTermColor color, const bool background) {
  if ((background && VTERM_COLOR_IS_DEFAULT_BG(&color)) || (!background && VTERM_COLOR_IS_DEFAULT_FG(&color))) {
    const ImVec4 fallback = background ? ui::kInputSurface : ui::kTextPrimary;
    return ImGui::GetColorU32(fallback);
  }
  vterm_screen_convert_color_to_rgb(screen, &color);
  return IM_COL32(color.rgb.red, color.rgb.green, color.rgb.blue, 255);
}

static uam::TerminalTextCell MakeTerminalTextCell(const VTermScreenCell& cell) {
  uam::TerminalTextCell out;
  out.width = cell.width;
  for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i) {
    out.text += CodepointToUtf8(cell.chars[i]);
  }
  return out;
}

static bool TerminalHasSelectionRange(const CliTerminalState& terminal) {
  return terminal.selection_active &&
         (terminal.selection_anchor_row != terminal.selection_current_row ||
          terminal.selection_anchor_col != terminal.selection_current_col);
}

static void ClearTerminalSelection(CliTerminalState& terminal) {
  terminal.selection_active = false;
  terminal.selection_dragging = false;
  terminal.selection_anchor_row = 0;
  terminal.selection_anchor_col = 0;
  terminal.selection_current_row = 0;
  terminal.selection_current_col = 0;
}

/// <summary>
/// Draws the embedded CLI terminal viewport for the selected chat.
/// </summary>
static void DrawCliTerminalSurface(AppState& app, ChatSession& chat, const bool show_footer = false) {
  CliTerminalState& terminal = EnsureCliTerminalForChat(app, chat);
  const ProviderProfile& provider = ProviderForChatOrDefault(app, chat);
  if (!terminal.running && terminal.should_launch) {
    const float mono_h = (g_font_mono != nullptr ? g_font_mono->FontSize : ImGui::GetTextLineHeight());
    const float mono_w = std::max(7.0f, ImGui::CalcTextSize("W").x);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int rows = std::max(8, static_cast<int>(avail.y / mono_h) - 1);
    const int cols = std::max(20, static_cast<int>(avail.x / mono_w) - 1);
    if (!StartCliTerminalForChat(app, terminal, chat, rows, cols)) {
      terminal.should_launch = false;
      ImGui::TextColored(ui::kError, "Failed to start provider terminal.");
      if (!terminal.last_error.empty()) {
        ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
      }
      return;
    }
  }
  if (terminal.running && !CliTerminalRuntimeReady(terminal)) {
    StopCliTerminal(terminal);
    terminal.should_launch = false;
    terminal.last_error = "Provider terminal state became invalid. Restart the terminal.";
  }
  if (!terminal.running) {
    ImGui::TextColored(ui::kWarning, "Provider terminal is stopped.");
    if (DrawButton("Restart Terminal", ImVec2(130.0f, 34.0f), ButtonKind::Primary)) {
      terminal.last_error.clear();
      terminal.should_launch = true;
    }
    if (ProviderUsesOpenCodeLocalBridge(provider)) {
      ImGui::SameLine();
      if (DrawButton("Select Local Model", ImVec2(150.0f, 34.0f), ButtonKind::Ghost)) {
        EnsureSelectedLocalRuntimeModelForProvider(app);
      }
    }
    if (!terminal.last_error.empty()) {
      ImGui::TextColored(ui::kTextMuted, "%s", terminal.last_error.c_str());
    }
    return;
  }

  PushFontIfAvailable(g_font_mono);
  const float cell_h = (g_font_mono != nullptr) ? g_font_mono->FontSize + 1.0f : ImGui::GetTextLineHeight();
  const float cell_w = std::max(7.0f, ImGui::CalcTextSize("W").x);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int rows = std::max(8, static_cast<int>(avail.y / cell_h));
  const int visible_cols = std::max(20, static_cast<int>(avail.x / cell_w));
  const int cols = uam::ClampCliTerminalColumns(visible_cols, app.settings.cli_max_columns);
  ResizeCliTerminal(terminal, rows, cols);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 terminal_size(std::max(avail.x, cell_w * static_cast<float>(visible_cols)), cell_h * terminal.rows);
  ImGui::InvisibleButton("embedded_cli_surface", terminal_size, ImGuiButtonFlags_None);
  const bool hovered = ImGui::IsItemHovered();
  const bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive() || hovered;
  const int max_scrollback_offset = static_cast<int>(terminal.scrollback_lines.size());
  terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset, 0, max_scrollback_offset);

  if (hovered) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      const int lines = std::max(1, static_cast<int>(std::round(std::abs(wheel) * 3.0f)));
      const int delta = (wheel > 0.0f) ? lines : -lines;
      terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset + delta, 0, max_scrollback_offset);
    }
  }

  bool consumed_navigation = false;
  if (focused) {
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
      terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset + std::max(3, terminal.rows / 2), 0, max_scrollback_offset);
      consumed_navigation = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
      terminal.scrollback_view_offset = std::clamp(terminal.scrollback_view_offset - std::max(3, terminal.rows / 2), 0, max_scrollback_offset);
      consumed_navigation = true;
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
      terminal.scrollback_view_offset = max_scrollback_offset;
      consumed_navigation = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
      terminal.scrollback_view_offset = 0;
      consumed_navigation = true;
    }
  }

  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(ImVec2(origin.x + 1.0f, origin.y + 3.0f), ImVec2(origin.x + terminal_size.x + 1.0f, origin.y + terminal_size.y + 3.0f),
                      ImGui::GetColorU32(ui::kShadow), 10.0f);
  draw->AddRectFilled(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kInputSurface), 10.0f);
  draw->AddRect(origin, ImVec2(origin.x + terminal_size.x, origin.y + terminal_size.y), ImGui::GetColorU32(ui::kBorder), 10.0f);

  std::vector<std::vector<uam::TerminalTextCell>> visible_rows;
  visible_rows.reserve(static_cast<std::size_t>(terminal.rows));
  if (terminal.screen != nullptr) {
    const int scrollback_count = static_cast<int>(terminal.scrollback_lines.size());
    const int start_virtual_row = std::max(0, scrollback_count - terminal.scrollback_view_offset);
    uam::TerminalSelectionPoint selection_start{};
    uam::TerminalSelectionPoint selection_end{};
    const bool has_selection =
        TerminalHasSelectionRange(terminal) &&
        uam::NormalizeTerminalSelectionRange({terminal.selection_anchor_row, terminal.selection_anchor_col},
                                             {terminal.selection_current_row, terminal.selection_current_col},
                                             &selection_start,
                                             &selection_end);
    for (int row = 0; row < terminal.rows; ++row) {
      std::vector<uam::TerminalTextCell> row_cells;
      row_cells.reserve(static_cast<std::size_t>(terminal.cols));
      const int virtual_row = start_virtual_row + row;
      for (int col = 0; col < terminal.cols; ++col) {
        VTermScreenCell cell = BlankTerminalCell();
        if (virtual_row < scrollback_count) {
          const std::vector<VTermScreenCell>& line = terminal.scrollback_lines[static_cast<std::size_t>(virtual_row)].cells;
          if (col < static_cast<int>(line.size())) {
            cell = line[static_cast<std::size_t>(col)];
          }
        } else {
          const int screen_row = virtual_row - scrollback_count;
          if (screen_row >= 0 && screen_row < terminal.rows) {
            VTermPos pos{screen_row, col};
            vterm_screen_get_cell(terminal.screen, pos, &cell);
          }
        }

        const ImVec2 cell_min(origin.x + col * cell_w, origin.y + row * cell_h);
        const ImVec2 cell_max(cell_min.x + cell_w * std::max<int>(1, cell.width), cell_min.y + cell_h);

        const ImU32 bg = VTermColorToImU32(terminal.screen, cell.bg, true);
        draw->AddRectFilled(cell_min, cell_max, bg);
        row_cells.push_back(MakeTerminalTextCell(cell));

        if (cell.chars[0] == 0 || cell.width == 0) {
          if (has_selection && uam::TerminalSelectionContainsCell(selection_start, selection_end, row, col)) {
            draw->AddRectFilled(cell_min, cell_max, ImGui::GetColorU32(ui::kAccentSoft));
          }
          continue;
        }

        std::string glyph;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i) {
          glyph += CodepointToUtf8(cell.chars[i]);
        }
        if (has_selection && uam::TerminalSelectionContainsCell(selection_start, selection_end, row, col)) {
          draw->AddRectFilled(cell_min, cell_max, ImGui::GetColorU32(ui::kAccentSoft));
        }
        const ImU32 fg = VTermColorToImU32(terminal.screen, cell.fg, false);
        draw->AddText(ImVec2(cell_min.x, cell_min.y), fg, glyph.c_str());
      }
      visible_rows.push_back(std::move(row_cells));
    }

    if (terminal.state != nullptr && terminal.scrollback_view_offset == 0) {
      VTermPos cursor{0, 0};
      vterm_state_get_cursorpos(terminal.state, &cursor);
      const bool blink_on = !terminal.cursor_blink || (static_cast<int>(ImGui::GetTime() * 2.0f) % 2 == 0);
      if (terminal.cursor_visible && blink_on && cursor.row >= 0 && cursor.row < terminal.rows && cursor.col >= 0 &&
          cursor.col < terminal.cols) {
        const ImVec2 cursor_min(origin.x + cursor.col * cell_w, origin.y + cursor.row * cell_h);
        const ImVec2 cursor_max(cursor_min.x + cell_w, cursor_min.y + cell_h);
        switch (terminal.cursor_shape) {
          case VTERM_PROP_CURSORSHAPE_UNDERLINE:
            draw->AddRectFilled(ImVec2(cursor_min.x, cursor_max.y - std::max(2.0f, cell_h * 0.16f)),
                                cursor_max,
                                ImGui::GetColorU32(ui::kAccent));
            break;
          case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
            draw->AddRectFilled(cursor_min,
                                ImVec2(cursor_min.x + std::max(2.0f, cell_w * 0.16f), cursor_max.y),
                                ImGui::GetColorU32(ui::kAccent));
            break;
          case VTERM_PROP_CURSORSHAPE_BLOCK:
          default:
            draw->AddRectFilled(cursor_min, cursor_max, ImGui::GetColorU32(Rgb(94, 160, 255, 0.30f)));
            draw->AddRect(cursor_min, cursor_max, ImGui::GetColorU32(ui::kAccent), 0.0f, 0, 1.2f);
            break;
        }
      }
    }
  }

  if (terminal.cols > 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    terminal.selection_active = true;
    terminal.selection_dragging = true;
    terminal.selection_anchor_row = std::clamp(static_cast<int>((mouse.y - origin.y) / cell_h), 0, terminal.rows - 1);
    terminal.selection_anchor_col = std::clamp(static_cast<int>((mouse.x - origin.x) / cell_w), 0, terminal.cols - 1);
    terminal.selection_current_row = terminal.selection_anchor_row;
    terminal.selection_current_col = terminal.selection_anchor_col;
  }
  if (terminal.selection_dragging) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      terminal.selection_current_row = std::clamp(static_cast<int>((mouse.y - origin.y) / cell_h), 0, terminal.rows - 1);
      terminal.selection_current_col = std::clamp(static_cast<int>((mouse.x - origin.x) / cell_w), 0, terminal.cols - 1);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      terminal.selection_dragging = false;
      if (TerminalHasSelectionRange(terminal)) {
        const std::string selected = uam::ExtractTerminalSelectionText(
            visible_rows,
            {terminal.selection_anchor_row, terminal.selection_anchor_col},
            {terminal.selection_current_row, terminal.selection_current_col});
        if (!selected.empty()) {
          SDL_SetClipboardText(selected.c_str());
        }
      }
    }
  }

  const ImGuiIO& io = ImGui::GetIO();
  const bool copy_shortcut_pressed =
      focused && TerminalHasSelectionRange(terminal) && ImGui::IsKeyPressed(ImGuiKey_C, false) && (io.KeyCtrl || io.KeySuper);
  if (copy_shortcut_pressed) {
    const std::string selected = uam::ExtractTerminalSelectionText(
        visible_rows,
        {terminal.selection_anchor_row, terminal.selection_anchor_col},
        {terminal.selection_current_row, terminal.selection_current_col});
    if (!selected.empty()) {
      SDL_SetClipboardText(selected.c_str());
    }
  } else if (focused && !consumed_navigation && terminal.scrollback_view_offset == 0) {
    FeedCliTerminalKeyboard(terminal);
  }

  PopFontIfAvailable(g_font_mono);
  if (show_footer) {
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    if (terminal.scrollback_view_offset > 0) {
      ImGui::TextColored(ui::kTextMuted, "Scrollback: %d lines up (End to jump bottom)", terminal.scrollback_view_offset);
    } else {
      ImGui::TextColored(ui::kTextMuted,
                         "Native provider terminal for this chat. Entered columns: %d (max %d).",
                         terminal.cols,
                         app.settings.cli_max_columns);
    }
  }
}
