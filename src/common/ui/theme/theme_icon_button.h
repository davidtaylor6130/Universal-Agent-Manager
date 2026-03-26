#pragma once

/// <summary>
/// Draws a lightweight vector icon centered inside a mini icon button.
/// Returns false when glyph does not map to a vector icon token.
/// </summary>
static bool DrawMiniIconGlyph(ImDrawList* draw, const ImVec2& min, const ImVec2& max, const char* glyph, const ImU32 color) {
  if (draw == nullptr || glyph == nullptr) {
    return false;
  }

  const float w = max.x - min.x;
  const float h = max.y - min.y;
  const float icon_scale = std::max(0.72f, std::min(1.0f, std::min(w, h) / 20.0f));
  const float stroke = std::max(1.0f, std::round(1.4f * icon_scale));
  const ImVec2 center(min.x + (w * 0.5f), min.y + (h * 0.5f));

  if (std::strcmp(glyph, "icon:new_chat") == 0) {
    const float bubble_w = 10.0f * icon_scale;
    const float bubble_h = 7.0f * icon_scale;
    const ImVec2 bubble_min(center.x - bubble_w * 0.55f, center.y - bubble_h * 0.60f);
    const ImVec2 bubble_max(center.x + bubble_w * 0.45f, center.y + bubble_h * 0.40f);
    draw->AddRect(bubble_min, bubble_max, color, 1.8f * icon_scale, 0, stroke);
    draw->AddTriangleFilled(ImVec2(bubble_min.x + 1.6f * icon_scale, bubble_max.y),
                            ImVec2(bubble_min.x + 4.0f * icon_scale, bubble_max.y),
                            ImVec2(bubble_min.x + 2.0f * icon_scale, bubble_max.y + 2.2f * icon_scale),
                            color);
    const float plus_arm = 2.6f * icon_scale;
    const float plus_x = bubble_max.x + 1.6f * icon_scale;
    const float plus_y = bubble_min.y + 1.9f * icon_scale;
    draw->AddLine(ImVec2(plus_x - plus_arm, plus_y), ImVec2(plus_x + plus_arm, plus_y), color, stroke);
    draw->AddLine(ImVec2(plus_x, plus_y - plus_arm), ImVec2(plus_x, plus_y + plus_arm), color, stroke);
    return true;
  }

  if (std::strcmp(glyph, "icon:new_folder") == 0) {
    const float folder_w = 10.5f * icon_scale;
    const float folder_h = 7.0f * icon_scale;
    const ImVec2 folder_min(center.x - folder_w * 0.55f, center.y - folder_h * 0.35f);
    const ImVec2 folder_max(center.x + folder_w * 0.45f, center.y + folder_h * 0.55f);
    const ImVec2 tab_a(folder_min.x + 1.2f * icon_scale, folder_min.y - 2.1f * icon_scale);
    const ImVec2 tab_b(folder_min.x + 4.3f * icon_scale, folder_min.y - 2.1f * icon_scale);
    draw->AddRect(folder_min, folder_max, color, 1.6f * icon_scale, 0, stroke);
    draw->AddLine(tab_a, tab_b, color, stroke);
    draw->AddLine(ImVec2(tab_a.x, tab_a.y), ImVec2(tab_a.x, folder_min.y), color, stroke);
    draw->AddLine(ImVec2(tab_b.x, tab_b.y), ImVec2(tab_b.x, folder_min.y), color, stroke);
    const float plus_arm = 2.2f * icon_scale;
    const float plus_x = folder_max.x + 1.6f * icon_scale;
    const float plus_y = folder_min.y + 1.9f * icon_scale;
    draw->AddLine(ImVec2(plus_x - plus_arm, plus_y), ImVec2(plus_x + plus_arm, plus_y), color, stroke);
    draw->AddLine(ImVec2(plus_x, plus_y - plus_arm), ImVec2(plus_x, plus_y + plus_arm), color, stroke);
    return true;
  }

  if (std::strcmp(glyph, "icon:menu") == 0) {
    const float dot_r = std::max(1.1f, 1.35f * icon_scale);
    const float gap = 3.4f * icon_scale;
    draw->AddCircleFilled(ImVec2(center.x - gap, center.y), dot_r, color, 12);
    draw->AddCircleFilled(center, dot_r, color, 12);
    draw->AddCircleFilled(ImVec2(center.x + gap, center.y), dot_r, color, 12);
    return true;
  }

  if (std::strcmp(glyph, "icon:delete") == 0) {
    const float arm = 4.2f * icon_scale;
    draw->AddLine(ImVec2(center.x - arm, center.y - arm), ImVec2(center.x + arm, center.y + arm), color, stroke + 0.2f);
    draw->AddLine(ImVec2(center.x - arm, center.y + arm), ImVec2(center.x + arm, center.y - arm), color, stroke + 0.2f);
    return true;
  }

  return false;
}

/// <summary>
/// Compact icon button used in sidebar and row actions.
/// </summary>
static bool DrawMiniIconButton(const char* id, const char* glyph, const ImVec2& size = ImVec2(22.0f, 22.0f),
                               const bool danger = false) {
  const bool light = IsLightPaletteActive();
  const ImVec2 scaled_size = ScaleUiSize(size);
  const float corner = ScaleUiLength(ui::kRadiusSmall);
  ImGui::PushID(id);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##mini_icon", scaled_size);
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();
  const ImVec2 max(min.x + scaled_size.x, min.y + scaled_size.y);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec4 bg = hovered
                        ? (danger ? (light ? Rgb(220, 38, 38, 0.16f) : Rgb(255, 107, 107, 0.16f))
                                  : (light ? Rgb(9, 31, 63, 0.09f) : Rgb(255, 255, 255, 0.10f)))
                        : ui::kTransparent;
  const ImVec4 border = hovered ? (danger ? ui::kError : ui::kBorderStrong) : ui::kBorder;
  const ImVec4 text = danger ? ui::kError : ui::kTextSecondary;
  draw->AddRectFilled(min, max, ImGui::GetColorU32(bg), corner);
  draw->AddRect(min, max, ImGui::GetColorU32(border), corner);
  const ImU32 icon_color = ImGui::GetColorU32(text);
  if (!DrawMiniIconGlyph(draw, min, max, glyph, icon_color)) {
    const ImVec2 text_size = ImGui::CalcTextSize(glyph);
    draw->AddText(ImVec2(min.x + (scaled_size.x - text_size.x) * 0.5f, min.y + (scaled_size.y - text_size.y) * 0.5f), icon_color,
                  glyph);
  }
  ImGui::PopID();
  return clicked;
}

/// <summary>
/// Compact folder icon button used next to directory/file inputs.
/// </summary>
static bool DrawFolderIconButton(const char* id, const ImVec2& size = ImVec2(22.0f, 22.0f)) {
  const bool light = IsLightPaletteActive();
  const ImVec2 scaled_size = ScaleUiSize(size);
  const float corner = ScaleUiLength(ui::kRadiusSmall);
  ImGui::PushID(id);
  const ImVec2 min = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##folder_icon", scaled_size);
  const bool hovered = ImGui::IsItemHovered();
  const bool clicked = ImGui::IsItemClicked();
  const ImVec2 max(min.x + scaled_size.x, min.y + scaled_size.y);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec4 bg = hovered ? (light ? Rgb(9, 31, 63, 0.09f) : Rgb(255, 255, 255, 0.10f)) : ui::kTransparent;
  const ImVec4 border = hovered ? ui::kBorderStrong : ui::kBorder;
  const ImVec4 icon = hovered ? ui::kTextPrimary : ui::kTextSecondary;
  draw->AddRectFilled(min, max, ImGui::GetColorU32(bg), corner);
  draw->AddRect(min, max, ImGui::GetColorU32(border), corner);

  const float w = scaled_size.x;
  const float h = scaled_size.y;
  const float tab_left = min.x + (w * 0.20f);
  const float tab_right = min.x + (w * 0.54f);
  const float tab_top = min.y + (h * 0.24f);
  const float tab_bottom = min.y + (h * 0.42f);
  const float body_left = min.x + (w * 0.20f);
  const float body_right = min.x + (w * 0.80f);
  const float body_top = min.y + (h * 0.40f);
  const float body_bottom = min.y + (h * 0.76f);
  const float icon_rounding = ScaleUiLength(2.0f);
  draw->AddRectFilled(ImVec2(tab_left, tab_top), ImVec2(tab_right, tab_bottom), ImGui::GetColorU32(icon), icon_rounding);
  draw->AddRectFilled(ImVec2(body_left, body_top), ImVec2(body_right, body_bottom), ImGui::GetColorU32(icon), icon_rounding);
  ImGui::PopID();
  return clicked;
}
