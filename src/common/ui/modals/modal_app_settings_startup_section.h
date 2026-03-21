#pragma once

/// <summary>
/// Draws startup mode and window controls in the app settings modal.
/// </summary>
static void DrawAppSettingsStartupSection(AppSettings& draft_settings, CenterViewMode& draft_center_mode) {
  ImGui::TextColored(ui::kTextSecondary, "Startup / Window");
  bool start_structured = (draft_center_mode == CenterViewMode::Structured);
  if (ImGui::RadioButton("Structured mode", start_structured)) {
    draft_center_mode = CenterViewMode::Structured;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Terminal mode", !start_structured)) {
    draft_center_mode = CenterViewMode::CliConsole;
  }
  ImGui::SetNextItemWidth(140.0f);
  ImGui::InputInt("Window Width", &draft_settings.window_width);
  ImGui::SetNextItemWidth(140.0f);
  ImGui::InputInt("Window Height", &draft_settings.window_height);
  ImGui::Checkbox("Start maximized", &draft_settings.window_maximized);
}
