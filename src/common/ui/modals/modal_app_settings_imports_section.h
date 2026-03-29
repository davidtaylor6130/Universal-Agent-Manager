#pragma once

#include "common/import_service.h"

/// <summary>
/// Draws legacy import actions in the app settings modal.
/// </summary>
static void DrawAppSettingsImportsSection(AppState& app) {
  ImGui::TextColored(ui::kTextSecondary, "Imports");
  ImGui::TextColored(ui::kTextMuted, "Import legacy Gemini session JSON files or a previous UAM data folder.");

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  ImGui::TextColored(ui::kTextPrimary, "Import Gemini Chats");
  std::string browse_error;
  DrawPathInputWithBrowseButton(
      "Gemini source",
      app.import_gemini_path_input,
      "app_settings_import_gemini_picker",
      PathBrowseTarget::File,
      -1.0f,
      nullptr,
      nullptr,
      &browse_error);
  if (!browse_error.empty()) {
    app.status_line = browse_error;
  }
  ImGui::SameLine();
  if (DrawButton("Pick Folder", ImVec2(104.0f, 30.0f), ButtonKind::Ghost)) {
    std::string selected_path;
    std::string folder_error;
    if (BrowsePathWithNativeDialog(PathBrowseTarget::Directory, app.import_gemini_path_input, &selected_path, &folder_error)) {
      app.import_gemini_path_input = selected_path;
    } else if (!folder_error.empty()) {
      app.status_line = folder_error;
    }
  }
  ImGui::TextColored(ui::kTextMuted, "Select one `.json` file or a folder that contains Gemini chat JSON.");
  if (DrawButton("Import Gemini Chats", ImVec2(162.0f, 32.0f), ButtonKind::Primary)) {
    const uam::ImportSummary summary = uam::ImportGeminiChatsIntoLocalData(
        fs::path(Trim(app.import_gemini_path_input)),
        app.data_root,
        "gemini-structured");
    app.import_status_message = summary.message;
    if (summary.ok) {
      ReloadPersistedAppData(app);
    }
    app.status_line = summary.message;
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  DrawSoftDivider();
  ImGui::TextColored(ui::kTextPrimary, "Import UAM V1 Data Folder");
  DrawPathInputWithBrowseButton(
      "Legacy data root",
      app.import_legacy_data_root_input,
      "app_settings_import_legacy_picker",
      PathBrowseTarget::Directory,
      -1.0f,
      nullptr,
      nullptr,
      &browse_error);
  if (!browse_error.empty()) {
    app.status_line = browse_error;
  }
  ImGui::TextColored(ui::kTextMuted, "Select the previous app data folder that contains chats, folders, and settings.");
  if (DrawButton("Import UAM V1 Data Folder", ImVec2(208.0f, 32.0f), ButtonKind::Primary)) {
    const uam::ImportSummary summary = uam::ImportLegacyUamDataIntoLocalData(
        fs::path(Trim(app.import_legacy_data_root_input)),
        app.data_root,
        ResolveGeminiGlobalRootPath(app.settings));
    app.import_status_message = summary.message;
    if (summary.ok) {
      ReloadPersistedAppData(app);
    }
    app.status_line = summary.message;
  }

  if (!app.import_status_message.empty()) {
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    ImGui::TextWrapped("%s", app.import_status_message.c_str());
  }
}
