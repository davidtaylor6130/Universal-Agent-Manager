#pragma once

/// <summary>
/// Folder settings modal renderer.
/// </summary>
static void DrawFolderSettingsModal(AppState& app) {
  if (app.open_folder_settings_popup) {
    ImGui::OpenPopup("folder_settings_popup");
    app.open_folder_settings_popup = false;
  }
  if (!ImGui::BeginPopupModal("folder_settings_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }

  const int folder_index = FindFolderIndexById(app, app.pending_folder_settings_id);
  if (folder_index < 0) {
    ImGui::TextColored(ui::kWarning, "Folder no longer exists.");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    if (DrawButton("Close", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
      app.pending_folder_settings_id.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return;
  }

  ChatFolder& folder = app.folders[folder_index];
  ImGui::TextColored(ui::kTextPrimary, "Folder Settings");
  ImGui::TextColored(ui::kTextMuted, "Edit the workspace directory used by chats in this folder.");
  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  DrawSoftDivider();

  PushInputChrome();
  ImGui::SetNextItemWidth(460.0f);
  ImGui::InputText("Title##folder_settings_title", &app.folder_settings_title_input);
  std::string browse_error;
  DrawPathInputWithBrowseButton(
      "Directory##folder_settings_directory",
      app.folder_settings_directory_input,
      "folder_settings_directory_picker",
      PathBrowseTarget::Directory,
      460.0f,
      nullptr,
      nullptr,
      &browse_error);
  if (!browse_error.empty()) {
    app.status_line = browse_error;
  }
  PopInputChrome();

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
  if (DrawButton("Open Directory", ImVec2(126.0f, 30.0f), ButtonKind::Ghost)) {
    std::string error;
    if (!OpenFolderInFileManager(ExpandLeadingTildePath(app.folder_settings_directory_input), &error)) {
      app.status_line = error;
    }
  }

  ImGui::Dummy(ImVec2(0.0f, ui::kSpace12));
  if (DrawButton("Save", ImVec2(96.0f, 32.0f), ButtonKind::Primary)) {
    const std::string title = Trim(app.folder_settings_title_input);
    const std::string directory = Trim(app.folder_settings_directory_input);
    if (title.empty()) {
      app.status_line = "Folder title is required.";
    } else if (directory.empty()) {
      app.status_line = "Folder directory is required.";
    } else {
      folder.title = title;
      folder.directory = directory;
      SaveFolders(app);
      app.status_line = "Folder settings saved.";
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (DrawButton("Delete Folder", ImVec2(122.0f, 32.0f), ButtonKind::Ghost)) {
    const std::string folder_id = folder.id;
    ImGui::CloseCurrentPopup();
    RequestDeleteFolder(app, folder_id);
  }
  ImGui::SameLine();
  if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}
