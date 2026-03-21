#pragma once

/// <summary>
/// Draws behavior toggles in the app settings modal.
/// </summary>
static void DrawAppSettingsBehaviorSection(AppSettings& draft_settings) {
  ImGui::TextColored(ui::kTextSecondary, "Behavior");
  ImGui::Checkbox("Confirm before deleting chat", &draft_settings.confirm_delete_chat);
  ImGui::Checkbox("Confirm before deleting folder", &draft_settings.confirm_delete_folder);
  ImGui::Checkbox("Remember last selected chat", &draft_settings.remember_last_chat);
}
