#pragma once

/// <summary>
/// Draws template-related controls in the app settings modal.
/// </summary>
static void DrawAppSettingsTemplatesSection(AppState& app, AppSettings& draft_settings) {
  ImGui::TextColored(ui::kTextSecondary, "Gemini Templates");
  PushInputChrome();
  ImGui::SetNextItemWidth(520.0f);
  ImGui::InputText("Global Root", &draft_settings.gemini_global_root_path);
  PopInputChrome();
  ImGui::TextColored(ui::kTextMuted, "Catalog folder: <root>/Markdown_Templates");
  const std::string current_default_template = TemplateLabelOrFallback(app, app.settings.default_gemini_template_id);
  ImGui::TextColored(ui::kTextMuted, "Current default: %s", current_default_template.c_str());
  if (DrawButton("Open Template Manager", ImVec2(164.0f, 30.0f), ButtonKind::Ghost)) {
    app.open_template_manager_popup = true;
  }
}
