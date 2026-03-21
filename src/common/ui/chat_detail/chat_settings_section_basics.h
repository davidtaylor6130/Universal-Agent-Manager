#pragma once

/// <summary>
/// Draws right-pane section title text with shared spacing.
/// </summary>
static void DrawSectionHeader(const char* title) {
  PushFontIfAvailable(g_font_ui);
  ImGui::TextColored(ui::kTextSecondary, "%s", title);
  PopFontIfAvailable(g_font_ui);
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
}

/// <summary>
/// Begins an elevated right-pane settings section card.
/// </summary>
static bool BeginSectionCard(const char* id, const float height = 0.0f) {
  return BeginPanel(id, ImVec2(0.0f, height), PanelTone::Elevated, true, 0, ImVec2(ui::kSpace12, ui::kSpace12));
}
