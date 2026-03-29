#pragma once

/// <summary>
/// Draws behavior toggles in the app settings modal.
/// </summary>
static void DrawAppSettingsBehaviorSection(AppState& app, AppSettings& draft_settings) {
  ImGui::TextColored(ui::kTextSecondary, "Behavior");
  ImGui::Checkbox("Confirm before deleting chat", &draft_settings.confirm_delete_chat);
  ImGui::Checkbox("Confirm before deleting folder", &draft_settings.confirm_delete_folder);
  ImGui::Checkbox("Remember last selected chat", &draft_settings.remember_last_chat);
  draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);
  ImGui::SetNextItemWidth(140.0f);
  ImGui::InputInt("CLI idle timeout (sec)", &draft_settings.cli_idle_timeout_seconds);
  draft_settings.cli_idle_timeout_seconds = std::clamp(draft_settings.cli_idle_timeout_seconds, 30, 3600);
  draft_settings.cli_max_columns = std::clamp(draft_settings.cli_max_columns, 40, 512);
  ImGui::SetNextItemWidth(140.0f);
  ImGui::InputInt("Max CLI columns", &draft_settings.cli_max_columns);
  draft_settings.cli_max_columns = std::clamp(draft_settings.cli_max_columns, 40, 512);
  ImGui::TextColored(ui::kTextMuted, "Embedded provider terminals clamp reported width to this value.");
}
