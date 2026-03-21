#pragma once

/// <summary>
/// Draws shortcut hints in the app settings modal.
/// </summary>
static void DrawAppSettingsShortcutsSection() {
  ImGui::TextColored(ui::kTextSecondary, "Shortcuts");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+,  Open App Settings");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+N  Create chat");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+R  Refresh history");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+M  Toggle Structured/Terminal mode");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+Enter  Send message");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+Y  YOLO mode toggle (provider settings)");
}
