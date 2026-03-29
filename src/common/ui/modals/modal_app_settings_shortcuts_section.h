#pragma once

/// <summary>
/// Draws shortcut hints in the app settings modal.
/// </summary>
static void DrawAppSettingsShortcutsSection() {
  ImGui::TextColored(ui::kTextSecondary, "Shortcuts");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+,  Open App Settings");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+N  Create chat");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+R  Refresh history");
  ImGui::TextColored(ui::kTextMuted, "Enter  Send message in Structured mode");
  ImGui::TextColored(ui::kTextMuted, "Shift+Enter  Insert newline in Structured mode");
  ImGui::TextColored(ui::kTextMuted, "Ctrl+Y  YOLO mode toggle (provider settings)");
}
