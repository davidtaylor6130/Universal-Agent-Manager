#pragma once

/// <summary>
/// UI actions emitted by a sidebar chat row.
/// </summary>
struct SidebarItemAction {
  bool select = false;
  bool request_delete = false;
  bool request_open_options = false;
};

/// <summary>
/// Draws one chat row in the sidebar tree and returns requested UI actions.
/// </summary>
static SidebarItemAction DrawSidebarItem(AppState& app,
                                         const ChatSession& chat,
                                         const bool selected,
                                         const std::string& item_id,
                                         const int tree_depth = 0,
                                         const bool has_children = false,
                                         const bool children_collapsed = false,
                                         bool* toggle_children = nullptr) {
  const bool light = IsLightPaletteActive();
  SidebarItemAction action;

  ImGui::PushID(item_id.c_str());
  float row_h = 30.0f;
  float row_rounding = ui::kRadiusSmall;
  float accent_w = 3.0f;
  float title_x_offset = 11.0f;
  float title_y_offset = 7.0f;
  float indicator_running_active_offset = 40.0f;
  float indicator_running_idle_offset = 18.0f;
  float indicator_unseen_active_offset = 42.0f;
  float indicator_unseen_idle_offset = 24.0f;
  float options_x_offset = 42.0f;
  float delete_x_offset = 22.0f;
  float delete_y_offset = 6.0f;
  float row_bottom_gap = 4.0f;
  int title_limit = 46;
  const float depth_indent = static_cast<float>(tree_depth) * ScaleUiLength(14.0f);
#if defined(_WIN32)
  // Windows-only DPI/layout mitigation: ensure row geometry scales with text so
  // large user scale values do not cause sidebar overlap. If this starts to
  // happen on macOS later, we can make this universal.
  row_h = std::max(ScaleUiLength(30.0f), ImGui::GetTextLineHeight() + ScaleUiLength(12.0f));
  row_rounding = ScaleUiLength(ui::kRadiusSmall);
  accent_w = std::max(2.0f, ScaleUiLength(3.0f));
  title_x_offset = ScaleUiLength(11.0f);
  title_y_offset = (row_h - ImGui::GetTextLineHeight()) * 0.5f;
  options_x_offset = ScaleUiLength(42.0f);
  delete_x_offset = ScaleUiLength(22.0f);
  delete_y_offset = std::max(ScaleUiLength(3.0f), (row_h - ScaleUiLength(16.0f)) * 0.5f);
  indicator_running_idle_offset = ScaleUiLength(18.0f);
  indicator_unseen_idle_offset = ScaleUiLength(24.0f);
  indicator_running_active_offset = options_x_offset + ScaleUiLength(16.0f);
  indicator_unseen_active_offset = options_x_offset + ScaleUiLength(18.0f);
  row_bottom_gap = ScaleUiLength(4.0f);
#endif
  title_x_offset += depth_indent + (has_children ? ScaleUiLength(18.0f) : 0.0f);
  const ImVec2 row_size(ImGui::GetContentRegionAvail().x, row_h);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("chat_row", row_size);
  ImGui::SetItemAllowOverlap();
  const bool hovered = ImGui::IsItemHovered();
  action.select = ImGui::IsItemClicked();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    action.request_open_options = true;
    action.select = false;
  }
  const ImVec2 max(min.x + row_size.x, min.y + row_size.y);
#if defined(_WIN32)
  {
    const bool showing_actions = (selected || hovered);
    const float avg_char_w = std::max(4.0f, ImGui::CalcTextSize("ABCDEFGHIJKLMNOPQRSTUVWXYZ").x / 26.0f);
    const float reserved_right_w = showing_actions ? (options_x_offset + ScaleUiLength(20.0f)) : ScaleUiLength(24.0f);
    const float available_title_w = std::max(48.0f, row_size.x - title_x_offset - reserved_right_w);
    title_limit = std::max(10, static_cast<int>(std::floor(available_title_w / avg_char_w)));
  }
#endif

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec4 row_bg = selected ? (light ? Rgb(66, 126, 228, 0.13f) : Rgb(94, 160, 255, 0.15f))
                                 : (hovered ? (light ? Rgb(9, 31, 63, 0.06f) : Rgb(255, 255, 255, 0.06f)) : ui::kTransparent);
  const ImVec4 row_border = selected ? ui::kBorderStrong : ui::kBorder;
  if (selected || hovered) {
    draw->AddRectFilled(min, max, ImGui::GetColorU32(row_bg), row_rounding);
    draw->AddRect(min, max, ImGui::GetColorU32(row_border), row_rounding);
  }
  if (selected) {
    draw->AddRectFilled(min, ImVec2(min.x + accent_w, max.y), ImGui::GetColorU32(ui::kAccent), row_rounding, ImDrawFlags_RoundCornersLeft);
  }

  if (has_children) {
    ImGui::SetCursorScreenPos(ImVec2(min.x + depth_indent + ScaleUiLength(2.0f), min.y + delete_y_offset));
    const char* glyph = children_collapsed ? ">" : "v";
    if (DrawMiniIconButton("branch_toggle", glyph, ImVec2(14.0f, 14.0f), false)) {
      if (toggle_children != nullptr) {
        *toggle_children = true;
      }
      action.select = false;
    }
  } else if (tree_depth > 0) {
    draw->AddText(ImVec2(min.x + depth_indent + ScaleUiLength(6.0f), min.y + title_y_offset),
                  ImGui::GetColorU32(ui::kTextMuted),
                  ".");
  }

  const std::string row_title = CompactPreview(Trim(chat.title).empty() ? chat.id : chat.title, title_limit);
  draw->AddText(ImVec2(min.x + title_x_offset, min.y + title_y_offset), ImGui::GetColorU32(ui::kTextPrimary), row_title.c_str());

  const bool running = ChatHasRunningGemini(app, chat.id);
  const bool has_unseen = !running && (app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end());
  if (running || has_unseen) {
    static constexpr const char* kSpinnerFrames[4] = {"-", "/", "-", "\\"};
    const int frame = static_cast<int>(ImGui::GetTime() * 8.0) & 3;
    if (running) {
      const float indicator_x = (hovered || selected) ? (max.x - indicator_running_active_offset) : (max.x - indicator_running_idle_offset);
      draw->AddText(ImVec2(indicator_x, min.y + title_y_offset), ImGui::GetColorU32(ui::kWarning), kSpinnerFrames[frame]);
    } else {
      const bool has_title_font = (g_font_title != nullptr);
      ImFont* glyph_font = has_title_font ? g_font_title : ImGui::GetFont();
      const float glyph_size = has_title_font ? (g_font_title->FontSize * 0.62f) : (ImGui::GetFontSize() * 1.20f);
      const char* unseen_glyph = has_title_font ? "\xE2\x97\x8F" : "@";
      const float indicator_x = (hovered || selected) ? (max.x - indicator_unseen_active_offset) : (max.x - indicator_unseen_idle_offset);
      const float unseen_y = std::max(min.y, min.y + title_y_offset - ScaleUiLength(2.5f));
      draw->AddText(glyph_font, glyph_size, ImVec2(indicator_x, unseen_y), ImGui::GetColorU32(ui::kAccent), unseen_glyph);
    }
  }

  if (hovered || selected) {
    ImGui::SetCursorScreenPos(ImVec2(max.x - options_x_offset, min.y + delete_y_offset));
    if (DrawMiniIconButton("chat_options_menu", "...", ImVec2(16.0f, 16.0f), true)) {
      action.request_open_options = true;
      action.select = false;
    }
    ImGui::SetCursorScreenPos(ImVec2(max.x - delete_x_offset, min.y + delete_y_offset));
    if (DrawMiniIconButton("delete_chat", "x", ImVec2(16.0f, 16.0f), true)) {
      action.request_delete = true;
      action.select = false;
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + row_bottom_gap));
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
  ImGui::PopID();
  return action;
}
