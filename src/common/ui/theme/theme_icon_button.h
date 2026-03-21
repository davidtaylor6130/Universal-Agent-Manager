#pragma once

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
  const ImVec2 text_size = ImGui::CalcTextSize(glyph);
  draw->AddText(ImVec2(min.x + (scaled_size.x - text_size.x) * 0.5f, min.y + (scaled_size.y - text_size.y) * 0.5f),
                ImGui::GetColorU32(text), glyph);
  ImGui::PopID();
  return clicked;
}
