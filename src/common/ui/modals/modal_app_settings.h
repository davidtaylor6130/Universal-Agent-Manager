#pragma once

#include "common/ui/modals/modal_app_settings_appearance_section.h"
#include "common/ui/modals/modal_app_settings_behavior_section.h"
#include "common/ui/modals/modal_app_settings_commit_section.h"
#include "common/ui/modals/modal_app_settings_compatibility_section.h"
#include "common/ui/modals/modal_app_settings_diagnostics_section.h"
#include "common/ui/modals/modal_app_settings_shortcuts_section.h"
#include "common/ui/modals/modal_app_settings_startup_section.h"
#include "common/ui/modals/modal_app_settings_templates_section.h"

/// <summary>
/// Draws the application settings modal using section-level UI components.
/// </summary>
static void DrawAppSettingsModal(AppState& app, const float platform_scale) {
  static AppSettings draft_settings{};
  static CenterViewMode draft_center_mode = CenterViewMode::Structured;
  static bool initialized = false;

  if (app.open_app_settings_popup) {
    draft_settings = app.settings;
    if (Trim(draft_settings.gemini_global_root_path).empty()) {
      draft_settings.gemini_global_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
    }
    draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
    draft_center_mode = app.center_view_mode;
    initialized = true;
    ImGui::OpenPopup("app_settings_popup");
    app.open_app_settings_popup = false;
    StartGeminiVersionCheck(app, true);
  }

  if (ImGui::BeginPopupModal("app_settings_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!initialized) {
      draft_settings = app.settings;
      if (Trim(draft_settings.gemini_global_root_path).empty()) {
        draft_settings.gemini_global_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
      }
      draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
      draft_center_mode = app.center_view_mode;
      initialized = true;
      StartGeminiVersionCheck(app, false);
    }
    RefreshTemplateCatalog(app);

    ImGui::TextColored(ui::kTextPrimary, "Application Settings");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    DrawSoftDivider();

    DrawAppSettingsAppearanceSection(app, draft_settings, platform_scale);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsBehaviorSection(draft_settings);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsTemplatesSection(app, draft_settings);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsCompatibilitySection(app);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsStartupSection(draft_settings, draft_center_mode);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsDiagnosticsSection(app, draft_settings);

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    DrawSoftDivider();
    DrawAppSettingsShortcutsSection();

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
    DrawAppSettingsCommitSection(app, draft_settings, draft_center_mode, platform_scale, initialized);
    ImGui::EndPopup();
  }
}
