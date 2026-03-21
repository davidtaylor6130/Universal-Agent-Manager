#pragma once

/// <summary>
/// Draws save/cancel actions and applies app settings draft values.
/// </summary>
static void DrawAppSettingsCommitSection(AppState& app,
                                         AppSettings& draft_settings,
                                         CenterViewMode& draft_center_mode,
                                         const float platform_scale,
                                         bool& initialized) {
  if (DrawButton("Save Preferences", ImVec2(138.0f, 34.0f), ButtonKind::Primary)) {
    const std::string previous_global_root = app.settings.gemini_global_root_path;
    draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
    if (Trim(draft_settings.gemini_global_root_path).empty()) {
      draft_settings.gemini_global_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
    }
    ClampWindowSettings(draft_settings);
    app.settings = draft_settings;
    app.center_view_mode = draft_center_mode;
    if (app.center_view_mode == CenterViewMode::CliConsole) {
      MarkSelectedCliTerminalForLaunch(app);
    }
    ApplyThemeFromSettings(app);
    g_platform_layout_scale = std::clamp(platform_scale, 1.0f, 2.25f);
    if (platform_scale > 1.01f) {
      ImGui::GetStyle().ScaleAllSizes(platform_scale);
    }
    CaptureUiScaleBaseStyle();
    ApplyUserUiScale(ImGui::GetIO(), app.settings.ui_scale_multiplier);
    SaveSettings(app);
    if (previous_global_root != app.settings.gemini_global_root_path) {
      MarkTemplateCatalogDirty(app);
      RefreshTemplateCatalog(app, true);
    }
    app.status_line = "Preferences saved.";
    initialized = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (DrawButton("Cancel", ImVec2(96.0f, 34.0f), ButtonKind::Ghost)) {
    initialized = false;
    ImGui::CloseCurrentPopup();
  }
}
