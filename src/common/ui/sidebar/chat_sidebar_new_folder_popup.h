#pragma once

/// <summary>
/// Draws the create-folder modal used by the sidebar.
/// </summary>
static void DrawSidebarNewFolderPopup(AppState& app) {
  if (ImGui::BeginPopupModal("new_folder_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(ui::kTextPrimary, "Create chat folder");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    ImGui::SetNextItemWidth(420.0f);
    PushInputChrome();
    ImGui::InputText("Title", &app.new_folder_title_input);
    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputText("Directory", &app.new_folder_directory_input);
    PopInputChrome();
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace8));
    if (DrawButton("Create", ImVec2(96.0f, 32.0f), ButtonKind::Primary)) {
      const std::string folder_title = Trim(app.new_folder_title_input);
      const std::string folder_dir = Trim(app.new_folder_directory_input);
      if (folder_title.empty()) {
        app.status_line = "Folder title is required.";
      } else if (folder_dir.empty()) {
        app.status_line = "Folder directory is required.";
      } else {
        ChatFolder folder;
        folder.id = NewFolderId();
        folder.title = folder_title;
        folder.directory = folder_dir;
        folder.collapsed = false;
        app.folders.push_back(std::move(folder));
        app.new_chat_folder_id = app.folders.back().id;
        SaveFolders(app);
        app.status_line = "Chat folder created.";
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
