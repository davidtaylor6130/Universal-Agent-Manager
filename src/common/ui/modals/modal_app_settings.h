#pragma once

#include "common/ui/modals/modal_app_settings_appearance_section.h"
#include "common/ui/modals/modal_app_settings_behavior_section.h"
#include "common/ui/modals/modal_app_settings_commit_section.h"
#include "common/ui/modals/modal_app_settings_compatibility_section.h"
#include "common/ui/modals/modal_app_settings_diagnostics_section.h"
#include "common/ui/modals/modal_app_settings_imports_section.h"
#include "common/ui/modals/modal_app_settings_runtime_section.h"
#include "common/ui/modals/modal_app_settings_shortcuts_section.h"
#include "common/ui/modals/modal_app_settings_startup_section.h"
#include "common/ui/modals/modal_app_settings_templates_section.h"

/// <summary>
/// Draws the application settings modal using section-level UI components.
/// </summary>
static void DrawAppSettingsModal(AppState& app, const float platform_scale) {
  static AppSettings draft_settings{};
  static bool initialized = false;

  if (app.open_app_settings_popup) {
    draft_settings = app.settings;
    if (Trim(draft_settings.prompt_profile_root_path).empty()) {
      draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
    }
    draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
    app.app_settings_tab_index = 0;
    initialized = true;
    ImGui::OpenPopup("app_settings_popup");
    app.open_app_settings_popup = false;
    StartGeminiVersionCheck(app, true);
  }

  if (BeginCenteredPopupModal("Settings###app_settings_popup",
                              nullptr,
                              ImGuiWindowFlags_NoResize,
                              ImVec2(940.0f, 720.0f),
                              ImGuiCond_Appearing)) {
    if (!initialized) {
      draft_settings = app.settings;
      if (Trim(draft_settings.prompt_profile_root_path).empty()) {
        draft_settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
      }
      draft_settings.ui_theme = NormalizeThemeChoice(draft_settings.ui_theme);
      initialized = true;
      StartGeminiVersionCheck(app, false);
    }
    RefreshTemplateCatalog(app);

    ImGui::TextColored(ui::kTextPrimary, "Settings");
    ImGui::TextColored(ui::kTextMuted, "Runtime, imports, appearance, and diagnostics");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));
    DrawSoftDivider();
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace6));

    if (ImGui::BeginTabBar("app_settings_tabs", ImGuiTabBarFlags_None)) {
      if (ImGui::BeginTabItem("General",
                              nullptr,
                              (app.app_settings_tab_index == 0) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 0;
        DrawAppSettingsBehaviorSection(app, draft_settings);
        ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
        DrawSoftDivider();
        DrawAppSettingsTemplatesSection(app, draft_settings);
        ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
        DrawSoftDivider();
        DrawAppSettingsStartupSection(draft_settings);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Runtime",
                              nullptr,
                              (app.app_settings_tab_index == 1) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 1;
        DrawAppSettingsRuntimeSection(app, draft_settings);
        ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
        DrawSoftDivider();
        const ProviderProfile* active_profile =
            ProviderProfileStore::FindById(app.provider_profiles, draft_settings.active_provider_id);
        if (active_profile != nullptr && IsGeminiProviderId(active_profile->id)) {
          DrawAppSettingsCompatibilitySection(app);
        } else {
          ImGui::TextColored(ui::kTextSecondary, "Provider Compatibility");
          ImGui::TextColored(ui::kTextMuted,
                             "Gemini compatibility checks are shown when Gemini is the active provider.");
        }
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Imports",
                              nullptr,
                              (app.app_settings_tab_index == 2) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 2;
        DrawAppSettingsImportsSection(app);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Appearance",
                              nullptr,
                              (app.app_settings_tab_index == 3) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 3;
        DrawAppSettingsAppearanceSection(app, draft_settings, platform_scale);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Diagnostics",
                              nullptr,
                              (app.app_settings_tab_index == 4) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 4;
        DrawAppSettingsDiagnosticsSection(app, draft_settings);
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Shortcuts",
                              nullptr,
                              (app.app_settings_tab_index == 5) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
        app.app_settings_tab_index = 5;
        DrawAppSettingsShortcutsSection();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }

    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    DrawSoftDivider();
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
    DrawAppSettingsCommitSection(app, draft_settings, platform_scale, initialized);
    ImGui::EndPopup();
  }
}
