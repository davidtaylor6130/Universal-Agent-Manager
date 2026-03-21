#pragma once

/// <summary>
/// Delete-folder confirmation modal renderer.
/// </summary>
static void DrawDeleteFolderConfirmationModal(AppState& app) {
  if (app.open_delete_folder_popup) {
    ImGui::OpenPopup("confirm_delete_folder_popup");
    app.open_delete_folder_popup = false;
  }
  if (ImGui::BeginPopupModal("confirm_delete_folder_popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const ChatFolder* folder = FindFolderById(app, app.pending_delete_folder_id);
    const std::string folder_name = (folder != nullptr) ? FolderTitleOrFallback(*folder) : "Unknown folder";
    ImGui::TextColored(ui::kTextPrimary, "Delete folder?");
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace4));
    ImGui::TextWrapped("Chats in this folder will be moved to General.");
    ImGui::TextColored(ui::kTextMuted, "Target: %s", folder_name.c_str());
    ImGui::Dummy(ImVec2(0.0f, ui::kSpace10));
    if (DrawButton("Delete Folder", ImVec2(118.0f, 32.0f), ButtonKind::Primary)) {
      DeleteFolderById(app, app.pending_delete_folder_id);
      app.pending_delete_folder_id.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (DrawButton("Cancel", ImVec2(96.0f, 32.0f), ButtonKind::Ghost)) {
      app.pending_delete_folder_id.clear();
      app.status_line = "Delete folder cancelled.";
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}
